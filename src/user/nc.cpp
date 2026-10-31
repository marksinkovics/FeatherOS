#include "command_line.h"
#include "tcp_client.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr char Usage[] = "usage: nc [-4 | -6] [-u] host port\n"
                         "       nc -l [-4 | -6] port";
constexpr char Help[] =
    "Connects to a TCP port, or with -l waits for one connection on a port of every local\n"
    "address, then copies standard input to the connection and the connection to standard\n"
    "output. At the end of the input it stops sending, and it exits once the peer does.\n"
    "With -u it sends each read of the input as a UDP datagram instead, and after the end of\n"
    "the input waits a second for more replies.";
// UDP has no end of stream, so nc stops once replies stop for this long.
constexpr int DatagramIdleMs = 1000;

namespace
{

int AcceptOne(const char* port_text, int family)
{
    char* end = nullptr;
    const long port = strtol(port_text, &end, 10);
    if (end == port_text || *end != '\0' || port <= 0 || port > 0xFFFF)
    {
        printf("nc: bad port %s\n", port_text);
        return -1;
    }
    const bool ipv6 = family == AF_INET6;
    const int listener = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    sockaddr_storage local = {};
    socklen_t local_length;
    if (ipv6)
    {
        auto* address = reinterpret_cast<sockaddr_in6*>(&local);
        address->sin6_family = AF_INET6;
        address->sin6_port = htons(static_cast<uint16_t>(port));
        local_length = sizeof(sockaddr_in6);
    }
    else
    {
        auto* address = reinterpret_cast<sockaddr_in*>(&local);
        address->sin_family = AF_INET;
        address->sin_port = htons(static_cast<uint16_t>(port));
        local_length = sizeof(sockaddr_in);
    }
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&local), local_length) != 0 ||
        listen(listener, 1) != 0)
    {
        printf("nc: port %ld: %s\n", port, strerror(errno));
        if (listener >= 0)
        {
            close(listener);
        }
        return -1;
    }
    const int connection = accept(listener, nullptr, nullptr);
    if (connection < 0)
    {
        printf("nc: accept: %s\n", strerror(errno));
    }
    close(listener);
    return connection;
}

// peer names the other end in errors, or is null.
int Relay(int connection, bool datagram, const char* peer)
{
    pollfd descriptors[2] = {{STDIN_FILENO, POLLIN, 0}, {connection, POLLIN, 0}};
    bool input_open = true;
    bool output_open = true;
    // A terminal never ends its input, so there the peer's end is enough.
    const bool interactive = isatty(STDIN_FILENO) != 0;
    char buffer[1024];
    while (output_open || (input_open && !interactive))
    {
        descriptors[0].fd = input_open ? STDIN_FILENO : -1;
        descriptors[1].fd = output_open ? connection : -1;
        const int ready = poll(descriptors, 2, datagram && !input_open ? DatagramIdleMs : -1);
        if (ready < 0)
        {
            printf("nc: poll: %s\n", strerror(errno));
            return 1;
        }
        if (ready == 0)
        {
            return 0;
        }
        if (output_open && descriptors[1].revents != 0)
        {
            const ssize_t received = recv(connection, buffer, sizeof(buffer), 0);
            if (received < 0)
            {
                if (peer != nullptr)
                {
                    printf("nc: %s: %s\n", peer, strerror(errno));
                }
                else
                {
                    printf("nc: %s\n", strerror(errno));
                }
                return 1;
            }
            output_open = datagram || received != 0;
            FeatherOS::User::WriteAll(STDOUT_FILENO, buffer, static_cast<size_t>(received));
        }
        if (input_open && descriptors[0].revents != 0)
        {
            const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count <= 0)
            {
                input_open = false;
                if (!datagram)
                {
                    shutdown(connection, SHUT_WR);
                }
            }
            else if (!FeatherOS::User::SendAll(connection, buffer, static_cast<size_t>(count)))
            {
                // A peer that already closed may refuse the rest of the input.
                if (!output_open)
                {
                    return 0;
                }
                printf("nc: %s\n", strerror(errno));
                return 1;
            }
        }
    }
    return 0;
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
    bool listening = false;
    bool datagram = false;
    int family = AF_UNSPEC;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first)
    {
        if (strcmp(argv[first], "-l") == 0)
        {
            listening = true;
        }
        else if (strcmp(argv[first], "-u") == 0)
        {
            datagram = true;
        }
        else if (strcmp(argv[first], "-4") == 0)
        {
            family = AF_INET;
        }
        else if (strcmp(argv[first], "-6") == 0)
        {
            family = AF_INET6;
        }
        else
        {
            puts(Usage);
            return 2;
        }
    }
    if (argc - first != (listening ? 1 : 2) || (listening && datagram))
    {
        puts(Usage);
        return 2;
    }
    const int connection =
        listening ? AcceptOne(argv[first], family)
                  : FeatherOS::User::Connect("nc", argv[first], argv[first + 1], family,
                                             datagram ? SOCK_DGRAM : SOCK_STREAM);
    if (connection < 0)
    {
        return 1;
    }
    char peer[96];
    if (!listening)
    {
        snprintf(peer, sizeof(peer), "%s %s", argv[first], argv[first + 1]);
    }
    const int status = Relay(connection, datagram, listening ? nullptr : peer);
    close(connection);
    return status;
}
