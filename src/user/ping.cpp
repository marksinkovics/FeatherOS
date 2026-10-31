#include "command_line.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

constexpr char Help[] = "usage: ping [-6] [-c count] host\n"
                        "Sends an ICMP echo request every second, until count replies or Ctrl+C.\n"
                        "The host is an address or a name; an address with a colon, or -6, means\n"
                        "IPv6, and a name takes its first address of either version without -6.";

namespace
{

constexpr size_t DataSize = 56;
constexpr long IntervalMilliseconds = 1000;

volatile sig_atomic_t stopped = 0;

void Stop(int)
{
    stopped = 1;
}

long Milliseconds()
{
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

struct Statistics
{
    unsigned long sent;
    unsigned long received;
    long minimum;
    long maximum;
    long total;
};

// Waits out the rest of the interval for this sequence number's reply; true if it came.
bool AwaitReply(int socket, int family, uint16_t sequence, long sent_at, Statistics* statistics)
{
    for (;;)
    {
        uint8_t reply[sizeof(icmphdr) + DataSize];
        sockaddr_storage source = {};
        socklen_t source_length = sizeof(source);
        const ssize_t length = recvfrom(socket, reply, sizeof(reply), 0,
                                        reinterpret_cast<sockaddr*>(&source), &source_length);
        if (length < 0)
        {
            if (errno == EAGAIN)
            {
                printf("Request timeout for icmp_seq %u\n", sequence);
            }
            else if (errno != EINTR)
            {
                printf("ping: recvfrom: %s\n", strerror(errno));
            }
            return false;
        }
        if (static_cast<size_t>(length) < sizeof(icmphdr))
        {
            continue;
        }
        bool matches;
        const void* address;
        if (family == AF_INET6)
        {
            icmp6_hdr header;
            memcpy(&header, reply, sizeof(header));
            matches = header.icmp6_type == ICMP6_ECHO_REPLY && ntohs(header.icmp6_seq) == sequence;
            address = &reinterpret_cast<const sockaddr_in6*>(&source)->sin6_addr;
        }
        else
        {
            icmphdr header;
            memcpy(&header, reply, sizeof(header));
            matches = header.type == ICMP_ECHOREPLY && ntohs(header.un.echo.sequence) == sequence;
            address = &reinterpret_cast<const sockaddr_in*>(&source)->sin_addr;
        }
        // A late reply to an earlier request.
        if (!matches)
        {
            continue;
        }
        const long elapsed = Milliseconds() - sent_at;
        char from[INET6_ADDRSTRLEN];
        inet_ntop(family, address, from, sizeof(from));
        printf("%zd bytes from %s: icmp_seq=%u time=%ld ms\n", length, from, sequence, elapsed);
        if (statistics->received == 0 || elapsed < statistics->minimum)
        {
            statistics->minimum = elapsed;
        }
        if (elapsed > statistics->maximum)
        {
            statistics->maximum = elapsed;
        }
        statistics->total += elapsed;
        ++statistics->received;
        return true;
    }
}

// Fills target from an address or a name; prints why and returns false when there is none.
bool ResolveTarget(const char* text, bool ipv6, sockaddr_storage* target, socklen_t* length)
{
    uint8_t ipv4_address[4];
    if (ipv6 && inet_pton(AF_INET, text, ipv4_address) == 1)
    {
        printf("ping: %s: not an IPv6 address\n", text);
        return false;
    }
    addrinfo hints = {};
    hints.ai_family = ipv6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    const int error = getaddrinfo(text, nullptr, &hints, &results);
    if (error != 0)
    {
        printf("ping: %s: %s\n", text, gai_strerror(error));
        return false;
    }
    memcpy(target, results->ai_addr, results->ai_addrlen);
    *length = results->ai_addrlen;
    freeaddrinfo(results);
    return true;
}

void BuildRequest(uint8_t (&request)[sizeof(icmphdr) + DataSize], bool ipv6, uint16_t sequence)
{
    if (ipv6)
    {
        icmp6_hdr header = {};
        header.icmp6_type = ICMP6_ECHO_REQUEST;
        header.icmp6_seq = htons(sequence);
        memcpy(request, &header, sizeof(header));
    }
    else
    {
        icmphdr header = {};
        header.type = ICMP_ECHO;
        header.un.echo.sequence = htons(sequence);
        memcpy(request, &header, sizeof(header));
    }
    for (size_t index = 0; index < DataSize; ++index)
    {
        request[sizeof(icmphdr) + index] = static_cast<uint8_t>(index);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    unsigned long long count = 0;
    bool ipv6 = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first)
    {
        if (strcmp(argv[first], "-6") == 0)
        {
            ipv6 = true;
        }
        else if (strcmp(argv[first], "-c") == 0 && first + 1 < argc)
        {
            ++first;
            if (!FeatherOS::User::ParseUnsigned(argv[first], &count) || count == 0 ||
                count > 0xFFFF)
            {
                puts("ping: count must be from 1 to 65535");
                return 2;
            }
        }
        else
        {
            break;
        }
    }
    if (argc != first + 1)
    {
        puts("usage: ping [-6] [-c count] host");
        return 2;
    }
    const char* host = argv[first];
    sockaddr_storage target = {};
    socklen_t target_length = 0;
    if (!ResolveTarget(host, ipv6 || strchr(host, ':') != nullptr, &target, &target_length))
    {
        return 2;
    }
    const int family = target.ss_family;
    ipv6 = family == AF_INET6;

    const int socket_descriptor = socket(family, SOCK_DGRAM, ipv6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP);
    if (socket_descriptor < 0)
    {
        printf("ping: socket: %s\n", strerror(errno));
        return 2;
    }
    const timeval timeout = {.tv_sec = IntervalMilliseconds / 1000, .tv_usec = 0};
    setsockopt(socket_descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    signal(SIGINT, Stop);

    char address[INET6_ADDRSTRLEN];
    inet_ntop(family,
              ipv6 ? static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(&target)->sin6_addr)
                   : &reinterpret_cast<sockaddr_in*>(&target)->sin_addr,
              address, sizeof(address));
    if (strcmp(address, host) == 0)
    {
        printf("PING %s: %zu data bytes\n", host, DataSize);
    }
    else
    {
        printf("PING %s (%s): %zu data bytes\n", host, address, DataSize);
    }
    Statistics statistics = {};
    for (unsigned long sequence = 1; !stopped && (count == 0 || sequence <= count); ++sequence)
    {
        uint8_t request[sizeof(icmphdr) + DataSize];
        BuildRequest(request, ipv6, static_cast<uint16_t>(sequence));

        const long sent_at = Milliseconds();
        if (sendto(socket_descriptor, request, sizeof(request), 0,
                   reinterpret_cast<const sockaddr*>(&target), target_length) < 0)
        {
            printf("ping: sendto: %s\n", strerror(errno));
        }
        else
        {
            ++statistics.sent;
            AwaitReply(socket_descriptor, family, static_cast<uint16_t>(sequence), sent_at,
                       &statistics);
        }
        const long remaining = IntervalMilliseconds - (Milliseconds() - sent_at);
        if (!stopped && (count == 0 || sequence < count) && remaining > 0)
        {
            usleep(static_cast<unsigned>(remaining) * 1000);
        }
    }
    close(socket_descriptor);

    const unsigned long lost = statistics.sent - statistics.received;
    printf("--- %s ping statistics ---\n", host);
    printf("%lu packets transmitted, %lu received, %lu%% packet loss\n", statistics.sent,
           statistics.received, statistics.sent == 0 ? 0 : lost * 100 / statistics.sent);
    if (statistics.received > 0)
    {
        printf("round-trip min/avg/max = %ld/%ld/%ld ms\n", statistics.minimum,
               statistics.total / static_cast<long>(statistics.received), statistics.maximum);
    }
    return statistics.received > 0 ? 0 : 1;
}
