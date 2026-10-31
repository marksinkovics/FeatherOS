#include "command_line.h"
#include "http_message.h"
#include "tcp_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace Http = FeatherOS::User::Http;

constexpr char Usage[] = "usage: fetch [-4 | -6] [-o file] url";
constexpr char Help[] =
    "Gets an http URL with HTTP/1.1 and writes the body to standard output, or with -o to a\n"
    "file. A URL without a scheme means http. Follows up to 5 redirects, and fails on a status\n"
    "outside 2xx.";

namespace
{

constexpr int MaximumRedirects = 5;
constexpr long ReceiveTimeoutSeconds = 30;

enum class Outcome
{
    Saved,
    Redirected,
    Failed,
};

bool IsRedirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

const char* UrlStatusText(Http::UrlStatus status)
{
    switch (status)
    {
    case Http::UrlStatus::Ok:
        return "no error";
    case Http::UrlStatus::UnsupportedScheme:
        return "only http URLs are supported";
    case Http::UrlStatus::Malformed:
        return "malformed URL";
    case Http::UrlStatus::TooLong:
        return "URL too long";
    }
    return "bad URL";
}

// Opens output only once a successful response arrives, so a failure leaves an existing file
// alone.
class Output
{
public:
    explicit Output(const char* path) : m_path(path), m_descriptor(-1), m_bytes(0) {}
    ~Output()
    {
        if (m_descriptor > STDERR_FILENO)
        {
            close(m_descriptor);
        }
    }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    bool Open()
    {
        if (m_path == nullptr)
        {
            m_descriptor = STDOUT_FILENO;
            return true;
        }
        m_descriptor = open(m_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (m_descriptor < 0)
        {
            fprintf(stderr, "fetch: %s: %s\n", m_path, strerror(errno));
            return false;
        }
        return true;
    }

    bool IsOpen() const
    {
        return m_descriptor >= 0;
    }

    bool Write(const char* data, size_t length)
    {
        if (!FeatherOS::User::WriteAll(m_descriptor, data, length))
        {
            fprintf(stderr, "fetch: %s: %s\n", m_path != nullptr ? m_path : "output",
                    strerror(errno));
            return false;
        }
        m_bytes += length;
        return true;
    }

    const char* Path() const
    {
        return m_path;
    }

    unsigned long long Bytes() const
    {
        return m_bytes;
    }

private:
    const char* m_path;
    int m_descriptor;
    unsigned long long m_bytes;
};

// One request and its response. A redirect leaves its target in next.
Outcome Get(const Http::Url& url, int family, Output* output, Http::Url* next)
{
    char buffer[4096];
    const size_t request_length = Http::BuildRequest(url, buffer, sizeof(buffer));
    if (request_length == 0)
    {
        fprintf(stderr, "fetch: URL too long\n");
        return Outcome::Failed;
    }
    const int connection =
        FeatherOS::User::Connect("fetch", url.host, url.port, family, SOCK_STREAM);
    if (connection < 0)
    {
        return Outcome::Failed;
    }
    const timeval timeout = {.tv_sec = ReceiveTimeoutSeconds, .tv_usec = 0};
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (!FeatherOS::User::SendAll(connection, buffer, request_length))
    {
        fprintf(stderr, "fetch: %s\n", strerror(errno));
        close(connection);
        return Outcome::Failed;
    }

    Http::ResponseParser parser;
    Outcome outcome = Outcome::Failed;
    for (;;)
    {
        const ssize_t received = recv(connection, buffer, sizeof(buffer), 0);
        if (received < 0)
        {
            fprintf(stderr, "fetch: %s\n", errno == EAGAIN ? "timed out" : strerror(errno));
            break;
        }
        const size_t body = received == 0 ? 0 : parser.Feed(buffer, static_cast<size_t>(received));
        if (received == 0 ? !parser.Finish() : parser.Error() != Http::ResponseError::None)
        {
            fprintf(stderr, "fetch: %s\n", Http::ResponseErrorText(parser.Error()));
            break;
        }
        if (parser.HeadersComplete() && !output->IsOpen())
        {
            const int status = parser.Status();
            if (IsRedirect(status) && parser.Location()[0] != '\0')
            {
                const Http::UrlStatus resolved = Http::ResolveUrl(url, parser.Location(), next);
                if (resolved == Http::UrlStatus::Ok)
                {
                    outcome = Outcome::Redirected;
                }
                else
                {
                    fprintf(stderr, "fetch: redirect to %s: %s\n", parser.Location(),
                            UrlStatusText(resolved));
                }
                break;
            }
            if (status < 200 || status > 299)
            {
                fprintf(stderr, "fetch: %d %s\n", status, parser.Reason());
                break;
            }
            if (!output->Open())
            {
                break;
            }
        }
        if (body > 0 && !output->Write(buffer, body))
        {
            break;
        }
        if (parser.Done())
        {
            outcome = Outcome::Saved;
            break;
        }
    }
    close(connection);
    return outcome;
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
    int family = AF_UNSPEC;
    const char* output_path = nullptr;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first)
    {
        if (strcmp(argv[first], "-4") == 0)
        {
            family = AF_INET;
        }
        else if (strcmp(argv[first], "-6") == 0)
        {
            family = AF_INET6;
        }
        else if (strcmp(argv[first], "-o") == 0 && first + 1 < argc)
        {
            output_path = argv[++first];
        }
        else
        {
            puts(Usage);
            return 2;
        }
    }
    if (argc - first != 1)
    {
        puts(Usage);
        return 2;
    }

    char text[Http::MaximumHostLength + Http::MaximumPathLength + 32];
    const char* argument = argv[first];
    const bool has_scheme = strstr(argument, "://") != nullptr;
    if (snprintf(text, sizeof(text), "%s%s", has_scheme ? "" : "http://", argument) >=
        static_cast<int>(sizeof(text)))
    {
        fprintf(stderr, "fetch: %s\n", UrlStatusText(Http::UrlStatus::TooLong));
        return 1;
    }
    Http::Url url;
    const Http::UrlStatus status = Http::ParseUrl(text, &url);
    if (status != Http::UrlStatus::Ok)
    {
        fprintf(stderr, "fetch: %s: %s\n", argument, UrlStatusText(status));
        return 1;
    }

    Output output(output_path);
    for (int redirects = 0;; ++redirects)
    {
        if (redirects > MaximumRedirects)
        {
            fprintf(stderr, "fetch: too many redirects\n");
            return 1;
        }
        Http::Url next;
        const Outcome outcome = Get(url, family, &output, &next);
        if (outcome == Outcome::Failed)
        {
            return 1;
        }
        if (outcome == Outcome::Saved)
        {
            break;
        }
        url = next;
    }
    if (output.Path() != nullptr)
    {
        printf("fetch: saved %llu bytes to %s\n", output.Bytes(), output.Path());
    }
    return 0;
}
