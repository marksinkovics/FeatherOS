#include "command_line.h"
#include "dhcp_lease.h"
#include "dhcp_message.h"
#include "process_table.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <featheros.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace Dhcp = FeatherOS::User::Dhcp;

constexpr char Help[] =
    "usage: dhcp [-r]\n"
    "Leases an IPv4 address, gateway and name servers from a DHCP server, configures the\n"
    "network with them and keeps running to hold the lease: it renews with the server at\n"
    "half the lease, asks any server at seven eighths, and drops the address if the lease\n"
    "runs out. init runs it as a service. -r asks the running client to renew now, as\n"
    "SIGUSR1 does. When the address goes, as after 'net down', it leases again as soon\n"
    "as the link is up.";

namespace
{

// Attempts before the client reports that no server answers; it keeps trying.
constexpr unsigned QuietAttempts = 4;
// How often a wait looks at the link and address, which change without a message to the client.
constexpr uint64_t NetworkCheckInterval = 1000;

// A signal handler writes to the pipe, so a renewal asked for just before poll is not missed.
int wake_pipe[2] = {-1, -1};

enum class Event
{
    Timeout,
    Datagram,
    Renew,
};

enum class Outcome
{
    Answered,
    TimedOut,
    Renew,
};

struct Client
{
    int socket;
    uint8_t mac[6];
};

void AskToRenew(int /*signal_number*/)
{
    const int saved = errno;
    const char byte = 0;
    write(wake_pipe[1], &byte, 1);
    errno = saved;
}

uint64_t Now()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

uint32_t RandomTransaction()
{
    uint32_t value = 0;
    const int descriptor = open("/dev/random", O_RDONLY);
    if (descriptor < 0 || read(descriptor, &value, sizeof(value)) != sizeof(value))
    {
        value = static_cast<uint32_t>(Now());
    }
    if (descriptor >= 0)
    {
        close(descriptor);
    }
    return value;
}

struct NetworkState
{
    bool link_up;
    in_addr ipv4_address;
};

NetworkState ReadNetworkState()
{
    featheros_network current;
    if (featheros_network_settings(nullptr, &current, 0) < 0)
    {
        return {.link_up = true, .ipv4_address = {}};
    }
    return {.link_up = current.link_up != 0, .ipv4_address = current.ipv4_address};
}

bool SameNetworkState(const NetworkState& first, const NetworkState& second)
{
    return first.link_up == second.link_up &&
           first.ipv4_address.s_addr == second.ipv4_address.s_addr;
}

// 'net down', 'net up' or an address set by hand counts as a request to renew, so the client
// leases again as soon as the link is back.
Event Wait(uint64_t until, const Client& client)
{
    const NetworkState before = ReadNetworkState();
    for (;;)
    {
        const uint64_t now = Now();
        if (now >= until)
        {
            return Event::Timeout;
        }
        const uint64_t wait =
            until - now < NetworkCheckInterval ? until - now : NetworkCheckInterval;
        pollfd entries[2] = {{.fd = client.socket, .events = POLLIN, .revents = 0},
                             {.fd = wake_pipe[0], .events = POLLIN, .revents = 0}};
        if (poll(entries, 2, static_cast<int>(wait)) <= 0)
        {
            if (!SameNetworkState(before, ReadNetworkState()))
            {
                return Event::Renew;
            }
            continue;
        }
        if ((entries[1].revents & POLLIN) != 0)
        {
            char bytes[16];
            read(wake_pipe[0], bytes, sizeof(bytes));
            return Event::Renew;
        }
        if ((entries[0].revents & POLLIN) != 0)
        {
            return Event::Datagram;
        }
    }
}

bool Receive(const Client& client, uint32_t transaction, Dhcp::Reply* reply)
{
    uint8_t received[Dhcp::MessageSize + 128];
    const ssize_t count = recvfrom(client.socket, received, sizeof(received), 0, nullptr, nullptr);
    return count > 0 &&
           Dhcp::ParseReply(received, static_cast<size_t>(count), transaction, client.mac, reply);
}

bool Send(const Client& client, const uint8_t* message, size_t length, const uint8_t* server)
{
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(Dhcp::ServerPort);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (server != nullptr)
    {
        memcpy(&destination.sin_addr, server, 4);
    }
    if (sendto(client.socket, message, length, 0, reinterpret_cast<const sockaddr*>(&destination),
               sizeof(destination)) < 0)
    {
        // A link that is down is waited out quietly.
        if (errno != ENETDOWN)
        {
            printf("dhcp: sendto: %s\n", strerror(errno));
        }
        return false;
    }
    return true;
}

// Broadcasts the message and waits until timeout for a reply of the wanted type, or a NAK. A
// failed send still waits, so that retries keep their backoff while the link is down.
Outcome Exchange(const Client& client, const uint8_t* message, size_t length, uint32_t transaction,
                 uint8_t wanted, uint64_t timeout, Dhcp::Reply* reply)
{
    const uint64_t until = Now() + timeout;
    Send(client, message, length, nullptr);
    for (;;)
    {
        const Event event = Wait(until, client);
        if (event == Event::Timeout)
        {
            return Outcome::TimedOut;
        }
        if (event == Event::Renew)
        {
            return Outcome::Renew;
        }
        if (Receive(client, transaction, reply) &&
            (reply->type == wanted || reply->type == Dhcp::Nak))
        {
            return Outcome::Answered;
        }
    }
}

// Discovers servers and requests an offer until one acknowledges; returns when the request was
// sent, where the lease starts. A request to renew starts over without waiting.
uint64_t Acquire(const Client& client, Dhcp::Reply* ack)
{
    unsigned attempt = 0;
    for (;;)
    {
        if (attempt == QuietAttempts)
        {
            puts("dhcp: no answer from a DHCP server; still trying");
        }
        const uint64_t timeout = Dhcp::DiscoverTimeout(attempt);
        ++attempt;
        const uint32_t transaction = RandomTransaction();
        uint8_t message[Dhcp::MessageSize];
        size_t length =
            Dhcp::BuildMessage(message, Dhcp::Discover, transaction, client.mac, nullptr, nullptr);
        Dhcp::Reply offer;
        Outcome outcome =
            Exchange(client, message, length, transaction, Dhcp::Offer, timeout, &offer);
        if (outcome == Outcome::Answered && offer.type == Dhcp::Offer)
        {
            length = Dhcp::BuildMessage(message, Dhcp::Request, transaction, client.mac,
                                        offer.your_address, offer.server);
            const uint64_t requested_at = Now();
            outcome = Exchange(client, message, length, transaction, Dhcp::Ack, timeout, ack);
            if (outcome == Outcome::Answered && ack->type == Dhcp::Ack)
            {
                return requested_at;
            }
            if (outcome == Outcome::Answered)
            {
                puts("dhcp: the server refused the request");
            }
        }
        if (outcome == Outcome::Renew)
        {
            attempt = 0;
        }
    }
}

// A netmask with gaps falls back to the address's class.
unsigned PrefixLength(const Dhcp::Reply& reply)
{
    const int length = reply.has_netmask ? Dhcp::NetmaskPrefixLength(reply.netmask) : -1;
    if (length >= 0)
    {
        return static_cast<unsigned>(length);
    }
    return reply.your_address[0] < 128 ? 8 : reply.your_address[0] < 192 ? 16 : 24;
}

void Configure(const Dhcp::Reply& ack, const char* verb)
{
    featheros_network settings = {};
    settings.ipv4_prefix_length = static_cast<unsigned char>(PrefixLength(ack));
    memcpy(&settings.ipv4_address, ack.your_address, 4);
    if (ack.has_router)
    {
        memcpy(&settings.ipv4_gateway, ack.router, 4);
    }
    settings.name_server_count = static_cast<unsigned>(ack.name_server_count);
    for (size_t index = 0; index < ack.name_server_count; ++index)
    {
        auto* server = reinterpret_cast<sockaddr_in*>(&settings.name_servers[index]);
        server->sin_family = AF_INET;
        memcpy(&server->sin_addr, ack.name_servers[index], 4);
    }
    if (featheros_network_settings(&settings, nullptr,
                                   FEATHEROS_NETWORK_IPV4 | FEATHEROS_NETWORK_NAME_SERVERS) < 0)
    {
        printf("dhcp: cannot configure the network: %s\n", strerror(errno));
        return;
    }
    char address[INET_ADDRSTRLEN];
    char gateway[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &settings.ipv4_address, address, sizeof(address));
    inet_ntop(AF_INET, &settings.ipv4_gateway, gateway, sizeof(gateway));
    printf("dhcp: %s %s/%u via %s for %lu s\n", verb, address, settings.ipv4_prefix_length, gateway,
           static_cast<unsigned long>(ack.lease_seconds));
}

void Drop(const char* reason)
{
    printf("dhcp: %s; dropping the address\n", reason);
    featheros_network settings = {};
    featheros_network_settings(&settings, nullptr, FEATHEROS_NETWORK_IPV4);
}

bool IsZero(const uint8_t (&address)[4])
{
    return (address[0] | address[1] | address[2] | address[3]) == 0;
}

// False once something else replaced or removed the leased address, such as 'net down'.
bool AddressHeld(const Dhcp::Reply& ack)
{
    featheros_network current;
    return featheros_network_settings(nullptr, &current, 0) == 0 &&
           memcmp(&current.ipv4_address, ack.your_address, 4) == 0;
}

// Renews the lease until a server refuses it, it runs out or the address is gone.
void Hold(const Client& client, Dhcp::Reply ack, uint64_t requested_at)
{
    Configure(ack, "leased");
    Dhcp::LeaseTimes times = Dhcp::ScheduleLease(requested_at, ack.lease_seconds,
                                                 ack.renewal_seconds, ack.rebinding_seconds);
    uint64_t send_at = times.renew_at;
    uint32_t transaction = 0;
    bool waiting = false;
    for (;;)
    {
        const uint64_t now = Now();
        const Dhcp::LeasePhase phase = Dhcp::PhaseAt(times, now);
        if (phase == Dhcp::LeasePhase::Expired)
        {
            Drop("the lease ran out");
            return;
        }
        if (now >= send_at)
        {
            if (!AddressHeld(ack))
            {
                puts("dhcp: the address is gone; leasing again");
                return;
            }
            transaction = RandomTransaction();
            uint8_t message[Dhcp::MessageSize];
            const size_t length =
                Dhcp::BuildRenewal(message, transaction, client.mac, ack.your_address);
            const bool unicast = phase != Dhcp::LeasePhase::Rebinding && !IsZero(ack.server);
            waiting = Send(client, message, length, unicast ? ack.server : nullptr);
            requested_at = now;
            send_at = Dhcp::RetryAt(times, now);
        }
        const Event event = Wait(send_at < times.expire_at ? send_at : times.expire_at, client);
        Dhcp::Reply reply;
        if (event == Event::Renew)
        {
            send_at = Now();
        }
        else if (event == Event::Datagram && Receive(client, transaction, &reply) && waiting)
        {
            if (reply.type == Dhcp::Nak)
            {
                Drop("the server refused the renewal");
                return;
            }
            if (reply.type == Dhcp::Ack)
            {
                if (IsZero(reply.server))
                {
                    memcpy(reply.server, ack.server, sizeof(reply.server));
                }
                ack = reply;
                Configure(ack, "renewed");
                times = Dhcp::ScheduleLease(requested_at, ack.lease_seconds, ack.renewal_seconds,
                                            ack.rebinding_seconds);
                send_at = times.renew_at;
                waiting = false;
            }
        }
    }
}

int SignalRunningClient()
{
    FeatherOS::User::ProcessInfo table[FeatherOS::User::MaximumListedProcesses];
    const size_t count = FeatherOS::User::ReadProcessTable(table, sizeof(table) / sizeof(table[0]));
    const auto self = static_cast<unsigned long long>(getpid());
    int signalled = 0;
    for (size_t index = 0; index < count; ++index)
    {
        if (table[index].pid != self && strcmp(table[index].name, "dhcp") == 0 &&
            kill(static_cast<pid_t>(table[index].pid), SIGUSR1) == 0)
        {
            ++signalled;
        }
    }
    if (signalled == 0)
    {
        puts("dhcp: no client is running");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-r") == 0)
    {
        return SignalRunningClient();
    }
    if (argc != 1)
    {
        puts("usage: dhcp [-r]");
        return 2;
    }
    if (pipe(wake_pipe) < 0)
    {
        printf("dhcp: pipe: %s\n", strerror(errno));
        return 1;
    }
    struct sigaction action = {};
    action.sa_handler = AskToRenew;
    sigaction(SIGUSR1, &action, nullptr);

    featheros_network current;
    if (featheros_network_settings(nullptr, &current, 0) < 0)
    {
        printf("dhcp: %s\n", strerror(errno));
        return 1;
    }
    Client client = {};
    memcpy(client.mac, current.mac, sizeof(client.mac));
    client.socket = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(Dhcp::ClientPort);
    if (client.socket < 0 ||
        bind(client.socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
    {
        printf("dhcp: cannot use port %u: %s\n", Dhcp::ClientPort, strerror(errno));
        return 1;
    }
    for (;;)
    {
        Dhcp::Reply ack;
        const uint64_t requested_at = Acquire(client, &ack);
        Hold(client, ack, requested_at);
    }
}
