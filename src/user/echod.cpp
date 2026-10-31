#include "command_line.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr char Usage[] = "usage: echod [-4 | -6] [-p port]";
constexpr char Help[] =
    "Sends back every byte its clients send, on port 7 (the echo service) or the one -p names,\n"
    "over IPv4 and IPv6 unless -4 or -6 picks one. One thread serves up to 16 clients at once\n"
    "with non-blocking sockets and poll, and logs each connection on standard output.";

namespace
{

constexpr int MaximumClients = 16;
constexpr int ListenBacklog = 16;
constexpr size_t BufferSize = 4096;

// Bytes received and not yet sent back. A client is read from only while there is room, so a
// client that never reads stalls itself, not the others.
struct Client
{
    int connection;
    bool input_ended;
    size_t start;
    size_t length;
    unsigned long long total;
    char address[INET6_ADDRSTRLEN];
    char buffer[BufferSize];
};

Client clients[MaximumClients];
int client_count = 0;

int Listen(int family, uint16_t port)
{
    const int listener = socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
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
        fprintf(stderr, "echod: port %u over %s: %s\n", port, family == AF_INET6 ? "IPv6" : "IPv4",
                strerror(errno));
        if (listener >= 0)
        {
            close(listener);
        }
        return -1;
    }
    return listener;
}

void AddressText(const sockaddr_storage& peer, char* text)
{
    const void* address =
        peer.ss_family == AF_INET6
            ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(&peer)->sin6_addr)
            : static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(&peer)->sin_addr);
    if (inet_ntop(peer.ss_family, address, text, INET6_ADDRSTRLEN) == nullptr)
    {
        strcpy(text, "?");
    }
}

// Takes every connection waiting on listener, until accept would block.
void AcceptClients(int listener)
{
    for (;;)
    {
        sockaddr_storage peer = {};
        socklen_t peer_length = sizeof(peer);
        const int connection =
            accept4(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length, SOCK_NONBLOCK);
        if (connection < 0)
        {
            if (errno != EAGAIN && errno != EINTR)
            {
                fprintf(stderr, "echod: accept: %s\n", strerror(errno));
            }
            return;
        }
        if (client_count == MaximumClients)
        {
            close(connection);
            continue;
        }
        Client& client = clients[client_count++];
        client.connection = connection;
        client.input_ended = false;
        client.start = 0;
        client.length = 0;
        client.total = 0;
        AddressText(peer, client.address);
        printf("echod: %s connected, %d open\n", client.address, client_count);
        fflush(stdout);
    }
}

// False once the client is done with: its input ended and all of it went back, or it failed.
bool Receive(Client& client)
{
    if (client.start != 0)
    {
        memmove(client.buffer, client.buffer + client.start, client.length);
        client.start = 0;
    }
    const ssize_t count =
        recv(client.connection, client.buffer + client.length, BufferSize - client.length, 0);
    if (count < 0)
    {
        return errno == EAGAIN || errno == EINTR;
    }
    if (count == 0)
    {
        client.input_ended = true;
        return client.length != 0;
    }
    client.length += static_cast<size_t>(count);
    client.total += static_cast<unsigned long long>(count);
    return true;
}

bool Send(Client& client)
{
    const ssize_t count =
        send(client.connection, client.buffer + client.start, client.length, MSG_NOSIGNAL);
    if (count < 0)
    {
        return errno == EAGAIN || errno == EINTR;
    }
    client.start += static_cast<size_t>(count);
    client.length -= static_cast<size_t>(count);
    return client.length != 0 || !client.input_ended;
}

void Drop(int index)
{
    Client& client = clients[index];
    close(client.connection);
    printf("echod: %s closed after %llu bytes, %d open\n", client.address, client.total,
           client_count - 1);
    fflush(stdout);
    clients[index] = clients[--client_count];
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
    unsigned long long port = 7;
    for (int index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "-4") == 0)
        {
            ipv6 = false;
        }
        else if (strcmp(argv[index], "-6") == 0)
        {
            ipv4 = false;
        }
        else if (strcmp(argv[index], "-p") == 0 && index + 1 < argc)
        {
            if (!FeatherOS::User::ParseUnsigned(argv[++index], &port) || port == 0 || port > 0xFFFF)
            {
                fprintf(stderr, "echod: bad port %s\n", argv[index]);
                return 2;
            }
        }
        else
        {
            puts(Usage);
            return 2;
        }
    }
    if (!ipv4 && !ipv6)
    {
        puts(Usage);
        return 2;
    }

    int listeners[2];
    int listener_count = 0;
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
        listeners[listener_count++] = listener;
    }
    printf("echod: listening on port %llu\n", port);
    fflush(stdout);

    pollfd entries[2 + MaximumClients];
    for (;;)
    {
        int count = 0;
        for (int index = 0; index < listener_count; ++index)
        {
            entries[count++] = {listeners[index], POLLIN, 0};
        }
        for (int index = 0; index < client_count; ++index)
        {
            const Client& client = clients[index];
            short events = 0;
            if (!client.input_ended && client.length < BufferSize)
            {
                events |= POLLIN;
            }
            if (client.length != 0)
            {
                events |= POLLOUT;
            }
            entries[count++] = {client.connection, events, 0};
        }
        if (poll(entries, static_cast<nfds_t>(count), -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            fprintf(stderr, "echod: poll: %s\n", strerror(errno));
            return 1;
        }
        // Clients first, from the last, since dropping one moves the last into its place;
        // accepting appends behind the ones polled.
        for (int index = client_count - 1; index >= 0; --index)
        {
            Client& client = clients[index];
            const short events = entries[listener_count + index].revents;
            bool keep = true;
            if ((events & POLLOUT) != 0)
            {
                keep = Send(client);
            }
            if (keep && (events & (POLLIN | POLLHUP | POLLERR)) != 0 && !client.input_ended &&
                client.length < BufferSize)
            {
                keep = Receive(client);
            }
            if (!keep)
            {
                Drop(index);
            }
        }
        for (int index = 0; index < listener_count; ++index)
        {
            if (entries[index].revents != 0)
            {
                AcceptClients(listeners[index]);
            }
        }
    }
}
