#include "command_line.h"
#include "ntp_message.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace Ntp = FeatherOS::User::Ntp;

constexpr char Help[] = "usage: ntp [-4 | -6] [-n] [-p port] [server]\n"
                        "Asks an NTP server, pool.ntp.org unless named, for the time and sets\n"
                        "the clock to it; -n only shows how far the clock is off.";
constexpr char Usage[] = "usage: ntp [-4 | -6] [-n] [-p port] [server]";

namespace
{

constexpr char DefaultServer[] = "pool.ntp.org";
constexpr int Attempts = 3;
constexpr long ReplyTimeoutMilliseconds = 2000;
constexpr uint64_t NanosecondsPerSecond = 1000000000;

struct Options
{
    int family = AF_UNSPEC;
    bool query_only = false;
    const char* port = "123";
    const char* server = DefaultServer;
};

enum class Outcome
{
    Synchronized,
    NoReply,
    Refused,
};

bool ReadClock(uint64_t* nanoseconds)
{
    timespec now = {};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    {
        return false;
    }
    *nanoseconds = static_cast<uint64_t>(now.tv_sec) * NanosecondsPerSecond +
                   static_cast<uint64_t>(now.tv_nsec);
    return true;
}

long MonotonicMilliseconds()
{
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

bool ParseOptions(int argc, char** argv, Options* options)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first)
    {
        if (strcmp(argv[first], "-4") == 0)
        {
            options->family = AF_INET;
        }
        else if (strcmp(argv[first], "-6") == 0)
        {
            options->family = AF_INET6;
        }
        else if (strcmp(argv[first], "-n") == 0)
        {
            options->query_only = true;
        }
        else if (strcmp(argv[first], "-p") == 0 && first + 1 < argc)
        {
            options->port = argv[++first];
        }
        else
        {
            return false;
        }
    }
    if (first + 1 < argc)
    {
        return false;
    }
    if (first < argc)
    {
        options->server = argv[first];
    }
    return true;
}

void FormatAddress(const addrinfo& address, char (&text)[INET6_ADDRSTRLEN])
{
    const void* bytes =
        address.ai_family == AF_INET6
            ? static_cast<const void*>(
                  &reinterpret_cast<const sockaddr_in6*>(address.ai_addr)->sin6_addr)
            : &reinterpret_cast<const sockaddr_in*>(address.ai_addr)->sin_addr;
    inet_ntop(address.ai_family, bytes, text, sizeof(text));
}

// Skips late replies to earlier attempts and anything else that does not echo transmit.
Ntp::ReplyStatus AwaitReply(int connection, uint64_t transmit, Ntp::Reply* reply, uint64_t* arrived)
{
    const long deadline = MonotonicMilliseconds() + ReplyTimeoutMilliseconds;
    for (;;)
    {
        const long remaining = deadline - MonotonicMilliseconds();
        pollfd entry = {connection, POLLIN, 0};
        if (remaining <= 0 || poll(&entry, 1, static_cast<int>(remaining)) <= 0)
        {
            return Ntp::ReplyStatus::Malformed;
        }
        uint8_t packet[Ntp::PacketSize * 2];
        const ssize_t received = recv(connection, packet, sizeof(packet), 0);
        if (received < 0 || !ReadClock(arrived))
        {
            return Ntp::ReplyStatus::Malformed;
        }
        const Ntp::ReplyStatus status =
            Ntp::ParseReply(packet, static_cast<size_t>(received), transmit, reply);
        if (status != Ntp::ReplyStatus::Malformed)
        {
            return status;
        }
    }
}

void PrintOffset(const char* address, const Ntp::Sample& sample, uint8_t stratum)
{
    printf("ntp: %s offset %+.3f s, delay %.3f s, stratum %u\n", address,
           static_cast<double>(sample.offset) / NanosecondsPerSecond,
           static_cast<double>(sample.round_trip_delay) / NanosecondsPerSecond, stratum);
}

bool StepClock(int64_t offset)
{
    uint64_t now = 0;
    if (!ReadClock(&now))
    {
        return false;
    }
    const uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(now) + offset);
    const timespec time = {static_cast<time_t>(target / NanosecondsPerSecond),
                           static_cast<long>(target % NanosecondsPerSecond)};
    if (clock_settime(CLOCK_REALTIME, &time) != 0)
    {
        fprintf(stderr, "ntp: cannot set the clock: %s\n", strerror(errno));
        return false;
    }
    char text[32];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S UTC", gmtime(&time.tv_sec));
    printf("ntp: clock set to %s\n", text);
    return true;
}

Outcome QueryServer(const addrinfo& address, const Options& options)
{
    char text[INET6_ADDRSTRLEN];
    FormatAddress(address, text);
    const int connection = socket(address.ai_family, SOCK_DGRAM, 0);
    if (connection < 0 || connect(connection, address.ai_addr, address.ai_addrlen) != 0)
    {
        fprintf(stderr, "ntp: %s: %s\n", text, strerror(errno));
        if (connection >= 0)
        {
            close(connection);
        }
        return Outcome::NoReply;
    }
    Outcome outcome = Outcome::NoReply;
    for (int attempt = 0; attempt < Attempts && outcome == Outcome::NoReply; ++attempt)
    {
        uint64_t sent = 0;
        if (!ReadClock(&sent))
        {
            fputs("ntp: clock unavailable\n", stderr);
            outcome = Outcome::Refused;
            break;
        }
        const uint64_t transmit = Ntp::TimestampFromUnixNanoseconds(sent);
        uint8_t request[Ntp::PacketSize];
        Ntp::BuildRequest(request, transmit);
        if (send(connection, request, sizeof(request), 0) < 0)
        {
            fprintf(stderr, "ntp: %s: %s\n", text, strerror(errno));
            break;
        }
        Ntp::Reply reply = {};
        uint64_t arrived = 0;
        switch (AwaitReply(connection, transmit, &reply, &arrived))
        {
        case Ntp::ReplyStatus::Valid:
        {
            const Ntp::Sample sample = Ntp::Measure(sent, reply, arrived);
            PrintOffset(text, sample, reply.stratum);
            outcome = options.query_only || StepClock(sample.offset) ? Outcome::Synchronized
                                                                     : Outcome::Refused;
            break;
        }
        case Ntp::ReplyStatus::Kiss:
            fprintf(stderr, "ntp: %s refused with kiss code %s\n", text, reply.kiss_code);
            outcome = Outcome::Refused;
            break;
        case Ntp::ReplyStatus::Unsynchronized:
            fprintf(stderr, "ntp: %s is not synchronized\n", text);
            outcome = Outcome::Refused;
            break;
        case Ntp::ReplyStatus::Malformed:
            break;
        }
    }
    if (outcome == Outcome::NoReply)
    {
        fprintf(stderr, "ntp: no reply from %s\n", text);
    }
    close(connection);
    return outcome;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    Options options;
    if (!ParseOptions(argc, argv, &options))
    {
        puts(Usage);
        return 2;
    }
    addrinfo hints = {};
    hints.ai_family = options.family;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    const int error = getaddrinfo(options.server, options.port, &hints, &results);
    if (error != 0)
    {
        fprintf(stderr, "ntp: %s: %s\n", options.server, gai_strerror(error));
        return 1;
    }
    // A server that refuses or is unsynchronized may have peers under the same name that are not.
    bool synchronized = false;
    for (const addrinfo* result = results; result != nullptr && !synchronized;
         result = result->ai_next)
    {
        synchronized = QueryServer(*result, options) == Outcome::Synchronized;
    }
    freeaddrinfo(results);
    return synchronized ? 0 : 1;
}
