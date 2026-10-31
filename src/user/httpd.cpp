#include "command_line.h"
#include "http_message.h"
#include "tcp_client.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace Http = FeatherOS::User::Http;

constexpr char Usage[] = "usage: httpd [-4 | -6] [-p port] [directory]";
constexpr char Help[] =
    "Serves the files under a directory, the current one by default, with HTTP/1.1 on port 80\n"
    "or the one -p names, over IPv4 and IPv6 unless -4 or -6 picks one. Answers GET and HEAD,\n"
    "sends a directory's index.html or else a listing of it, and logs each request on standard\n"
    "output as: client method target status body-bytes.";

namespace
{

constexpr int MaximumConnections = 8;
constexpr int ListenBacklog = 16;
constexpr long IdleTimeoutSeconds = 10;
// Closing a connection with unread data resets it, which can cut off the response, so a closing
// connection reads what the client still sends, for a while and up to a limit.
constexpr long DrainTimeoutSeconds = 1;
constexpr size_t MaximumDrainBytes = 64 * 1024;
constexpr char IndexFile[] = "index.html";
constexpr char HtmlType[] = "text/html; charset=utf-8";

// The served directory as an absolute path without a trailing '/', so "" for the root.
char root[Http::MaximumPathLength + 1];

pthread_mutex_t connection_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t connection_closed = PTHREAD_COND_INITIALIZER;
int connection_count = 0;

struct Client
{
    int connection;
    char address[INET6_ADDRSTRLEN];
};

// One request's response, with what the log reports about it.
struct Exchange
{
    int connection;
    bool head_only;
    // The response says the connection closes after it.
    bool close;
    // The connection broke, or a file ended before its length.
    bool failed;
    int status;
    unsigned long long bytes;
};

class Text
{
public:
    Text() = default;
    ~Text()
    {
        free(m_data);
    }
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;

    void Append(const char* text)
    {
        Append(text, strlen(text));
    }

    void Append(const char* text, size_t length)
    {
        if (m_failed || length == 0)
        {
            return;
        }
        if (m_length + length > m_capacity)
        {
            size_t capacity = m_capacity == 0 ? 1024 : m_capacity;
            while (capacity < m_length + length)
            {
                capacity *= 2;
            }
            auto* data = static_cast<char*>(realloc(m_data, capacity));
            if (data == nullptr)
            {
                m_failed = true;
                return;
            }
            m_data = data;
            m_capacity = capacity;
        }
        memcpy(m_data + m_length, text, length);
        m_length += length;
    }

    void AppendHtml(const char* text)
    {
        for (; *text != '\0'; ++text)
        {
            switch (*text)
            {
            case '&':
                Append("&amp;");
                break;
            case '<':
                Append("&lt;");
                break;
            case '>':
                Append("&gt;");
                break;
            case '"':
                Append("&quot;");
                break;
            default:
                Append(text, 1);
                break;
            }
        }
    }

    bool Failed() const
    {
        return m_failed;
    }

    const char* Data() const
    {
        return m_data;
    }

    size_t Length() const
    {
        return m_length;
    }

private:
    char* m_data = nullptr;
    size_t m_length = 0;
    size_t m_capacity = 0;
    bool m_failed = false;
};

struct Entry
{
    char name[sizeof(dirent::d_name)];
    bool directory;
};

int CompareEntries(const void* first, const void* second)
{
    return strcmp(static_cast<const Entry*>(first)->name, static_cast<const Entry*>(second)->name);
}

void SetReceiveTimeout(int connection, long seconds)
{
    const timeval timeout = {.tv_sec = seconds, .tv_usec = 0};
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

void SendHead(Exchange& exchange, int status, const char* type, unsigned long long length,
              const char* location = nullptr)
{
    exchange.status = status;
    char date[64] = "";
    const time_t now = time(nullptr);
    tm calendar;
    if (now >= 0 && gmtime_r(&now, &calendar) != nullptr)
    {
        strftime(date, sizeof(date), "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &calendar);
    }
    char head[Http::MaximumPathLength * 3 + 512];
    const int head_length =
        snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n%sServer: FeatherOS-httpd/1\r\nContent-Type: %s\r\n"
                 "Content-Length: %llu\r\n%s%s%s%s\r\n",
                 status, Http::StatusReason(status), date, type, length,
                 location != nullptr ? "Location: " : "", location != nullptr ? location : "",
                 location != nullptr ? "\r\n" : "", exchange.close ? "Connection: close\r\n" : "");
    exchange.failed =
        head_length < 0 || static_cast<size_t>(head_length) >= sizeof(head) ||
        !FeatherOS::User::SendAll(exchange.connection, head, static_cast<size_t>(head_length));
}

void SendBody(Exchange& exchange, const char* data, size_t length)
{
    if (exchange.head_only || exchange.failed)
    {
        return;
    }
    exchange.failed = !FeatherOS::User::SendAll(exchange.connection, data, length);
    if (!exchange.failed)
    {
        exchange.bytes += length;
    }
}

void SendStatus(Exchange& exchange, int status, const char* location = nullptr)
{
    char body[64];
    const int length = snprintf(body, sizeof(body), "%d %s\n", status, Http::StatusReason(status));
    SendHead(exchange, status, "text/plain; charset=utf-8", static_cast<unsigned long long>(length),
             location);
    SendBody(exchange, body, static_cast<size_t>(length));
}

int ErrorStatus(int error)
{
    return error == ENOENT || error == ENOTDIR ? 404 : 500;
}

void SendFile(Exchange& exchange, const char* path)
{
    const int descriptor = open(path, O_RDONLY);
    struct stat status;
    if (descriptor < 0 || fstat(descriptor, &status) != 0)
    {
        SendStatus(exchange, ErrorStatus(errno));
        if (descriptor >= 0)
        {
            close(descriptor);
        }
        return;
    }
    auto left = static_cast<unsigned long long>(status.st_size);
    SendHead(exchange, 200, Http::ContentType(path), left);
    char buffer[4096];
    while (!exchange.head_only && !exchange.failed && left > 0)
    {
        const ssize_t count = read(
            descriptor, buffer, left < sizeof(buffer) ? static_cast<size_t>(left) : sizeof(buffer));
        if (count <= 0)
        {
            // The length is already promised, so only closing the connection can tell the client.
            exchange.failed = true;
            break;
        }
        SendBody(exchange, buffer, static_cast<size_t>(count));
        left -= static_cast<unsigned long long>(count);
    }
    close(descriptor);
}

// Reads a directory's entries sorted by name; false when it cannot be read.
bool ReadEntries(const char* directory, Entry** entries, size_t* count)
{
    DIR* stream = opendir(directory);
    if (stream == nullptr)
    {
        return false;
    }
    size_t capacity = 0;
    *entries = nullptr;
    *count = 0;
    bool complete = true;
    while (const dirent* entry = readdir(stream))
    {
        if (*count == capacity)
        {
            capacity = capacity == 0 ? 16 : capacity * 2;
            auto* grown = static_cast<Entry*>(realloc(*entries, capacity * sizeof(Entry)));
            if (grown == nullptr)
            {
                complete = false;
                break;
            }
            *entries = grown;
        }
        Entry& copy = (*entries)[(*count)++];
        strcpy(copy.name, entry->d_name);
        copy.directory = entry->d_type == DT_DIR;
    }
    closedir(stream);
    if (!complete)
    {
        free(*entries);
        return false;
    }
    qsort(*entries, *count, sizeof(Entry), CompareEntries);
    return true;
}

void SendListing(Exchange& exchange, const char* directory, const char* path)
{
    Entry* entries = nullptr;
    size_t count = 0;
    if (!ReadEntries(directory, &entries, &count))
    {
        SendStatus(exchange, ErrorStatus(errno));
        return;
    }
    Text page;
    page.Append("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Index of ");
    page.AppendHtml(path);
    page.Append("</title></head>\n<body>\n<h1>Index of ");
    page.AppendHtml(path);
    page.Append("</h1>\n<ul>\n");
    if (strcmp(path, "/") != 0)
    {
        page.Append("<li><a href=\"../\">../</a></li>\n");
    }
    for (size_t index = 0; index < count; ++index)
    {
        const Entry& entry = entries[index];
        // Encoding also turns a ':' into %3A, so no name reads as a URL scheme.
        char link[sizeof(entry.name) * 3];
        Http::EncodePath(entry.name, link, sizeof(link));
        const char* suffix = entry.directory ? "/" : "";
        page.Append("<li><a href=\"");
        page.Append(link);
        page.Append(suffix);
        page.Append("\">");
        page.AppendHtml(entry.name);
        page.Append(suffix);
        page.Append("</a></li>\n");
    }
    page.Append("</ul>\n</body></html>\n");
    free(entries);
    if (page.Failed())
    {
        SendStatus(exchange, 500);
        return;
    }
    SendHead(exchange, 200, HtmlType, page.Length());
    SendBody(exchange, page.Data(), page.Length());
}

void Respond(Exchange& exchange, const Http::RequestParser& request)
{
    if (request.ErrorStatus() != 0)
    {
        SendStatus(exchange, request.ErrorStatus());
        return;
    }
    if (request.RequestMethod() == Http::Method::Other)
    {
        SendStatus(exchange, 501);
        return;
    }
    char path[Http::MaximumPathLength + 1];
    if (!Http::RequestPath(request.Target(), path, sizeof(path)))
    {
        SendStatus(exchange, 400);
        return;
    }
    char file[sizeof(root) + sizeof(path) + sizeof(IndexFile)];
    snprintf(file, sizeof(file), "%s%s", root, path);
    struct stat status;
    if (stat(file, &status) != 0)
    {
        SendStatus(exchange, ErrorStatus(errno));
        return;
    }
    if (S_ISREG(status.st_mode))
    {
        SendFile(exchange, file);
        return;
    }
    if (!S_ISDIR(status.st_mode))
    {
        SendStatus(exchange, 403);
        return;
    }
    const size_t length = strlen(path);
    if (path[length - 1] != '/')
    {
        // Relative links in the page resolve against the directory only with a trailing '/'.
        char location[Http::MaximumPathLength * 3 + 2];
        if (!Http::EncodePath(path, location, sizeof(location) - 1))
        {
            SendStatus(exchange, 500);
            return;
        }
        strcat(location, "/");
        SendStatus(exchange, 301, location);
        return;
    }
    snprintf(file + strlen(file), sizeof(file) - strlen(file), "%s", IndexFile);
    if (stat(file, &status) == 0 && S_ISREG(status.st_mode))
    {
        SendFile(exchange, file);
        return;
    }
    file[strlen(file) - strlen(IndexFile)] = '\0';
    SendListing(exchange, file, path);
}

void CloseConnection(int connection, bool peer_closed)
{
    shutdown(connection, SHUT_WR);
    if (!peer_closed)
    {
        SetReceiveTimeout(connection, DrainTimeoutSeconds);
        char discard[512];
        size_t drained = 0;
        ssize_t received;
        while (drained < MaximumDrainBytes &&
               (received = recv(connection, discard, sizeof(discard), 0)) > 0)
        {
            drained += static_cast<size_t>(received);
        }
    }
    close(connection);
}

void ReleaseConnectionSlot()
{
    pthread_mutex_lock(&connection_lock);
    --connection_count;
    pthread_cond_signal(&connection_closed);
    pthread_mutex_unlock(&connection_lock);
}

void* Serve(void* argument)
{
    auto* client = static_cast<Client*>(argument);
    const int connection = client->connection;
    SetReceiveTimeout(connection, IdleTimeoutSeconds);
    // Bytes past one request's head are the start of the next, when the client pipelines.
    char buffer[2048];
    size_t start = 0;
    size_t end = 0;
    bool open = true;
    bool peer_closed = false;
    while (open)
    {
        Http::RequestParser request;
        while (!request.Done())
        {
            if (start == end)
            {
                const ssize_t received = recv(connection, buffer, sizeof(buffer), 0);
                if (received <= 0)
                {
                    peer_closed = received == 0;
                    break;
                }
                start = 0;
                end = static_cast<size_t>(received);
            }
            start += request.Feed(buffer + start, end - start);
        }
        if (!request.Done())
        {
            break;
        }
        // A body is never read, so the connection cannot carry another request after it.
        Exchange exchange = {
            .connection = connection,
            .head_only = request.RequestMethod() == Http::Method::Head,
            .close = request.ErrorStatus() != 0 || !request.KeepAlive() || request.HasBody(),
            .failed = false,
            .status = 0,
            .bytes = 0,
        };
        Respond(exchange, request);
        printf("%s %s %s %d %llu\n", client->address,
               request.MethodName()[0] != '\0' ? request.MethodName() : "-",
               request.Target()[0] != '\0' ? request.Target() : "-", exchange.status,
               exchange.bytes);
        fflush(stdout);
        open = !exchange.close && !exchange.failed;
    }
    CloseConnection(connection, peer_closed);
    free(client);
    ReleaseConnectionSlot();
    return nullptr;
}

void AddressText(const sockaddr_storage& address, char* text, size_t size)
{
    const void* bytes =
        address.ss_family == AF_INET6
            ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr)
            : &reinterpret_cast<const sockaddr_in*>(&address)->sin_addr;
    if (inet_ntop(address.ss_family, bytes, text, static_cast<socklen_t>(size)) == nullptr)
    {
        snprintf(text, size, "?");
    }
}

void AcceptClient(int listener, const pthread_attr_t* attributes)
{
    pthread_mutex_lock(&connection_lock);
    while (connection_count >= MaximumConnections)
    {
        pthread_cond_wait(&connection_closed, &connection_lock);
    }
    ++connection_count;
    pthread_mutex_unlock(&connection_lock);

    sockaddr_storage peer = {};
    socklen_t peer_length = sizeof(peer);
    const int connection = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (connection < 0)
    {
        ReleaseConnectionSlot();
        return;
    }
    auto* client = static_cast<Client*>(malloc(sizeof(Client)));
    pthread_t thread;
    if (client == nullptr)
    {
        close(connection);
        ReleaseConnectionSlot();
        return;
    }
    client->connection = connection;
    AddressText(peer, client->address, sizeof(client->address));
    if (pthread_create(&thread, attributes, Serve, client) != 0)
    {
        close(connection);
        free(client);
        ReleaseConnectionSlot();
    }
}

int Listen(int family, uint16_t port)
{
    const int listener = socket(family, SOCK_STREAM, 0);
    sockaddr_storage local = {};
    socklen_t local_length;
    if (family == AF_INET6)
    {
        auto* address = reinterpret_cast<sockaddr_in6*>(&local);
        address->sin6_family = AF_INET6;
        address->sin6_port = htons(port);
        local_length = sizeof(sockaddr_in6);
    }
    else
    {
        auto* address = reinterpret_cast<sockaddr_in*>(&local);
        address->sin_family = AF_INET;
        address->sin_port = htons(port);
        local_length = sizeof(sockaddr_in);
    }
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&local), local_length) != 0 ||
        listen(listener, ListenBacklog) != 0)
    {
        fprintf(stderr, "httpd: port %u over %s: %s\n", port, family == AF_INET6 ? "IPv6" : "IPv4",
                strerror(errno));
        if (listener >= 0)
        {
            close(listener);
        }
        return -1;
    }
    return listener;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Usage);
        puts(Help);
        return 0;
    }
    bool ipv4 = true;
    bool ipv6 = true;
    const char* port_text = "80";
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first)
    {
        if (strcmp(argv[first], "-4") == 0)
        {
            ipv6 = false;
        }
        else if (strcmp(argv[first], "-6") == 0)
        {
            ipv4 = false;
        }
        else if (strcmp(argv[first], "-p") == 0 && first + 1 < argc)
        {
            port_text = argv[++first];
        }
        else
        {
            puts(Usage);
            return 2;
        }
    }
    if (argc - first > 1 || (!ipv4 && !ipv6))
    {
        puts(Usage);
        return 2;
    }
    char* end = nullptr;
    const long port = strtol(port_text, &end, 10);
    if (end == port_text || *end != '\0' || port <= 0 || port > 0xFFFF)
    {
        fprintf(stderr, "httpd: bad port %s\n", port_text);
        return 2;
    }
    if (argc - first == 1 && chdir(argv[first]) != 0)
    {
        fprintf(stderr, "httpd: %s: %s\n", argv[first], strerror(errno));
        return 1;
    }
    if (getcwd(root, sizeof(root)) == nullptr)
    {
        fprintf(stderr, "httpd: %s\n", strerror(errno));
        return 1;
    }
    const size_t root_length = strlen(root);
    if (root_length > 0 && root[root_length - 1] == '/')
    {
        root[root_length - 1] = '\0';
    }

    pollfd listeners[2];
    nfds_t listener_count = 0;
    constexpr int Families[] = {AF_INET, AF_INET6};
    for (const int family : Families)
    {
        if (family == AF_INET ? !ipv4 : !ipv6)
        {
            continue;
        }
        const int listener = Listen(family, static_cast<uint16_t>(port));
        if (listener < 0)
        {
            return 1;
        }
        listeners[listener_count++] = {listener, POLLIN, 0};
    }
    printf("httpd: serving %s on port %ld\n", root[0] != '\0' ? root : "/", port);
    fflush(stdout);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    for (;;)
    {
        if (poll(listeners, listener_count, -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            fprintf(stderr, "httpd: poll: %s\n", strerror(errno));
            return 1;
        }
        for (nfds_t index = 0; index < listener_count; ++index)
        {
            if (listeners[index].revents != 0)
            {
                AcceptClient(listeners[index].fd, &attributes);
            }
        }
    }
}
