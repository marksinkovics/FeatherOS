#include "libc/address_text.h"
#include "libc/dns_message.h"
#include "libc/hosts_file.h"
#include "libc/resolv_conf.h"

#include <fcntl.h>
#include <featheros.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace Dns = FeatherOS::LibC::Dns;
namespace Hosts = FeatherOS::LibC::Hosts;
namespace ResolvConf = FeatherOS::LibC::ResolvConf;

namespace
{

constexpr int KnownNameFlags = NI_NUMERICHOST | NI_NUMERICSERV | NI_NOFQDN | NI_NAMEREQD | NI_DGRAM;
constexpr int KnownFlags = AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_V4MAPPED | AI_ALL |
                           AI_ADDRCONFIG | AI_NUMERICSERV;
constexpr int DefaultAttempts = 2;
constexpr int DefaultTimeoutSeconds = 2;
// Both record types' worth.
constexpr size_t MaximumCandidates = 2 * Dns::MaximumAddresses;
constexpr off_t MaximumHostsFileSize = 64 * 1024;
constexpr off_t MaximumResolvConfSize = 16 * 1024;
// A domain name of 253 bytes, its final dot and the terminator.
constexpr size_t CanonicalNameCapacity = 255;

// Where and how to ask: the name servers in /etc/resolv.conf, or else the kernel's.
struct Resolver
{
    size_t name_server_count;
    sockaddr_storage name_servers[ResolvConf::MaximumNameServers];
    int timeout_seconds;
    int attempts;
};

struct Candidate
{
    int family;
    uint8_t bytes[16];
};

struct Candidates
{
    size_t count;
    Candidate list[MaximumCandidates];
    // Empty unless a source other than the node itself names it.
    char canonical_name[CanonicalNameCapacity];
};

// What a query asked for: addresses for A and AAAA, a name for PTR.
struct Reply
{
    Dns::Addresses addresses;
    char name[Dns::NameCapacity];
};

// One result and the address it points to, freed together.
struct Entry
{
    addrinfo info;
    sockaddr_in6 address;
};

void Add(Candidates* candidates, int family, const uint8_t* bytes)
{
    if (candidates->count == MaximumCandidates)
    {
        return;
    }
    Candidate& candidate = candidates->list[candidates->count++];
    candidate.family = family;
    memset(candidate.bytes, 0, sizeof(candidate.bytes));
    memcpy(candidate.bytes, bytes, family == AF_INET6 ? 16 : 4);
}

bool WantsIpv4(int family)
{
    return family != AF_INET6;
}

bool WantsIpv6(int family)
{
    return family != AF_INET;
}

// Without a services database, only numbers name ports.
int ParseService(const char* service, int flags, uint16_t* port)
{
    *port = 0;
    if (service == nullptr)
    {
        return 0;
    }
    unsigned long value = 0;
    const char* digit = service;
    for (; *digit >= '0' && *digit <= '9' && value <= 0xFFFF; ++digit)
    {
        value = value * 10 + static_cast<unsigned long>(*digit - '0');
    }
    if (digit == service || *digit != '\0' || value > 0xFFFF)
    {
        return (flags & AI_NUMERICSERV) != 0 ? EAI_NONAME : EAI_SERVICE;
    }
    *port = static_cast<uint16_t>(value);
    return 0;
}

// RFC 6761 6.3: these names are the loopback addresses and never go to a name server.
bool IsLocalhost(const char* name)
{
    size_t length = strlen(name);
    if (length > 0 && name[length - 1] == '.')
    {
        --length;
    }
    constexpr char Localhost[] = "localhost";
    constexpr size_t LocalhostLength = sizeof(Localhost) - 1;
    return length >= LocalhostLength &&
           strncasecmp(name + length - LocalhostLength, Localhost, LocalhostLength) == 0 &&
           (length == LocalhostLength || name[length - LocalhostLength - 1] == '.');
}

// The whole file in a buffer to free, or nullptr when it is missing, empty or too large.
char* ReadWholeFile(const char* path, off_t limit, size_t* length)
{
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
    {
        return nullptr;
    }
    struct stat status = {};
    char* text = nullptr;
    if (fstat(descriptor, &status) == 0 && status.st_size > 0 && status.st_size <= limit)
    {
        const auto size = static_cast<size_t>(status.st_size);
        text = static_cast<char*>(malloc(size));
        size_t filled = 0;
        while (text != nullptr && filled < size)
        {
            const ssize_t count = read(descriptor, text + filled, size - filled);
            if (count <= 0)
            {
                break;
            }
            filled += static_cast<size_t>(count);
        }
        *length = filled;
    }
    close(descriptor);
    return text;
}

// Every address /etc/hosts lists for name in the wanted family, in the file's order.
void LookUpHosts(const char* name, int family, Candidates* candidates)
{
    size_t length = 0;
    char* text = ReadWholeFile(_PATH_HOSTS, MaximumHostsFileSize, &length);
    if (text == nullptr)
    {
        return;
    }
    const char* cursor = text;
    Hosts::Line line;
    while (Hosts::NextLine(&cursor, text + length, &line))
    {
        const int line_family = line.address_length == 16 ? AF_INET6 : AF_INET;
        if ((family != AF_UNSPEC && family != line_family) || !Hosts::Names(line, name))
        {
            continue;
        }
        if (candidates->canonical_name[0] == '\0' &&
            line.names[0].length < sizeof(candidates->canonical_name))
        {
            memcpy(candidates->canonical_name, line.names[0].text, line.names[0].length);
            candidates->canonical_name[line.names[0].length] = '\0';
        }
        Add(candidates, line_family, line.address);
    }
    free(text);
}

// The canonical name of the first line in /etc/hosts with the address, or false.
bool LookUpHostsName(const uint8_t* address, size_t address_length, char* name, size_t capacity)
{
    size_t length = 0;
    char* text = ReadWholeFile(_PATH_HOSTS, MaximumHostsFileSize, &length);
    if (text == nullptr)
    {
        return false;
    }
    const char* cursor = text;
    Hosts::Line line;
    bool found = false;
    while (!found && Hosts::NextLine(&cursor, text + length, &line))
    {
        if (line.address_length == address_length &&
            memcmp(line.address, address, address_length) == 0 && line.names[0].length < capacity)
        {
            memcpy(name, line.names[0].text, line.names[0].length);
            name[line.names[0].length] = '\0';
            found = true;
        }
    }
    free(text);
    return found;
}

// A top-level domain is never all digits (RFC 1123 2.1), so such a name is a mistyped address.
bool LooksNumeric(const char* name)
{
    for (; *name != '\0'; ++name)
    {
        if ((*name < '0' || *name > '9') && *name != '.')
        {
            return false;
        }
    }
    return true;
}

uint16_t RandomId()
{
    uint16_t value = 0;
    const int descriptor = open("/dev/random", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0 || read(descriptor, &value, sizeof(value)) != sizeof(value))
    {
        timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        value = static_cast<uint16_t>(now.tv_nsec ^ now.tv_sec);
    }
    if (descriptor >= 0)
    {
        close(descriptor);
    }
    return value;
}

socklen_t AddressLength(int family)
{
    return family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
}

bool SameEndpoint(const sockaddr_storage& left, const sockaddr_storage& right)
{
    if (left.ss_family != right.ss_family)
    {
        return false;
    }
    if (left.ss_family == AF_INET6)
    {
        const auto& first = reinterpret_cast<const sockaddr_in6&>(left);
        const auto& second = reinterpret_cast<const sockaddr_in6&>(right);
        return first.sin6_port == second.sin6_port &&
               memcmp(&first.sin6_addr, &second.sin6_addr, sizeof(in6_addr)) == 0;
    }
    const auto& first = reinterpret_cast<const sockaddr_in&>(left);
    const auto& second = reinterpret_cast<const sockaddr_in&>(right);
    return first.sin_port == second.sin_port && first.sin_addr.s_addr == second.sin_addr.s_addr;
}

Dns::Outcome Parse(const uint8_t* response, size_t length, uint16_t id, uint16_t type, Reply* reply)
{
    if (type == Dns::TypePtr)
    {
        return Dns::ParsePointerResponse(response, length, id, reply->name, sizeof(reply->name));
    }
    return Dns::ParseResponse(response, length, id, type, &reply->addresses);
}

void SetReceiveTimeout(int socket_descriptor, int timeout_seconds)
{
    const timeval timeout = {.tv_sec = timeout_seconds, .tv_usec = 0};
    setsockopt(socket_descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

Dns::Outcome AskOverUdp(const sockaddr_storage& server, int timeout_seconds, const uint8_t* query,
                        size_t length, uint16_t id, uint16_t type, Reply* reply)
{
    const int socket_descriptor = socket(server.ss_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_descriptor < 0)
    {
        return Dns::Outcome::Failed;
    }
    SetReceiveTimeout(socket_descriptor, timeout_seconds);
    Dns::Outcome outcome = Dns::Outcome::Failed;
    if (sendto(socket_descriptor, query, length, 0, reinterpret_cast<const sockaddr*>(&server),
               AddressLength(server.ss_family)) >= 0)
    {
        for (;;)
        {
            uint8_t response[Dns::MaximumMessageSize];
            sockaddr_storage source = {};
            socklen_t source_length = sizeof(source);
            const ssize_t received = recvfrom(socket_descriptor, response, sizeof(response), 0,
                                              reinterpret_cast<sockaddr*>(&source), &source_length);
            if (received < 0)
            {
                break;
            }
            if (!SameEndpoint(source, server))
            {
                continue;
            }
            outcome = Parse(response, static_cast<size_t>(received), id, type, reply);
            if (outcome != Dns::Outcome::Unrelated)
            {
                break;
            }
            outcome = Dns::Outcome::Failed;
        }
    }
    close(socket_descriptor);
    return outcome;
}

bool SendAll(int socket_descriptor, const uint8_t* data, size_t length)
{
    while (length > 0)
    {
        const ssize_t sent = send(socket_descriptor, data, length, MSG_NOSIGNAL);
        if (sent <= 0)
        {
            return false;
        }
        data += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

bool ReceiveAll(int socket_descriptor, uint8_t* data, size_t length)
{
    while (length > 0)
    {
        const ssize_t received = recv(socket_descriptor, data, length, 0);
        if (received <= 0)
        {
            return false;
        }
        data += received;
        length -= static_cast<size_t>(received);
    }
    return true;
}

// Over TCP each message follows its length in two bytes (RFC 7766 8), sent in one write so both
// leave in one segment. The answer can reach 65535 bytes, so it goes on the heap.
Dns::Outcome AskOverTcp(const sockaddr_storage& server, int timeout_seconds, const uint8_t* query,
                        size_t length, uint16_t id, uint16_t type, Reply* reply)
{
    const int socket_descriptor = socket(server.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_descriptor < 0)
    {
        return Dns::Outcome::Failed;
    }
    SetReceiveTimeout(socket_descriptor, timeout_seconds);
    uint8_t frame[2 + Dns::MaximumMessageSize];
    frame[0] = static_cast<uint8_t>(length >> 8);
    frame[1] = static_cast<uint8_t>(length);
    memcpy(frame + 2, query, length);
    uint8_t prefix[2];
    Dns::Outcome outcome = Dns::Outcome::Failed;
    if (connect(socket_descriptor, reinterpret_cast<const sockaddr*>(&server),
                AddressLength(server.ss_family)) == 0 &&
        SendAll(socket_descriptor, frame, length + 2) &&
        ReceiveAll(socket_descriptor, prefix, sizeof(prefix)))
    {
        const size_t response_length = (static_cast<size_t>(prefix[0]) << 8) | prefix[1];
        auto* response = static_cast<uint8_t*>(malloc(response_length > 0 ? response_length : 1));
        if (response != nullptr && ReceiveAll(socket_descriptor, response, response_length))
        {
            outcome = Parse(response, response_length, id, type, reply);
        }
        free(response);
    }
    close(socket_descriptor);
    // The connection carries only this answer, and it is whole.
    if (outcome == Dns::Outcome::Unrelated || outcome == Dns::Outcome::Truncated)
    {
        return Dns::Outcome::Failed;
    }
    return outcome;
}

Dns::Outcome Ask(const sockaddr_storage& server, int timeout_seconds, const uint8_t* query,
                 size_t length, uint16_t id, uint16_t type, Reply* reply)
{
    const Dns::Outcome outcome =
        AskOverUdp(server, timeout_seconds, query, length, id, type, reply);
    if (outcome != Dns::Outcome::Truncated)
    {
        return outcome;
    }
    return AskOverTcp(server, timeout_seconds, query, length, id, type, reply);
}

// Asks each name server in turn, for each attempt, until one answers.
Dns::Outcome Query(const Resolver& resolver, const char* name, uint16_t type, Reply* reply)
{
    uint8_t query[Dns::MaximumMessageSize];
    const uint16_t id = RandomId();
    const size_t length = Dns::BuildQuery(query, sizeof(query), id, name, type);
    if (length == 0)
    {
        return Dns::Outcome::NoName;
    }
    for (int attempt = 0; attempt < resolver.attempts; ++attempt)
    {
        for (size_t index = 0; index < resolver.name_server_count; ++index)
        {
            const Dns::Outcome outcome = Ask(resolver.name_servers[index], resolver.timeout_seconds,
                                             query, length, id, type, reply);
            if (outcome == Dns::Outcome::Answered || outcome == Dns::Outcome::NoName)
            {
                return outcome;
            }
        }
    }
    return Dns::Outcome::Failed;
}

sockaddr_storage SocketAddress(const ResolvConf::NameServer& server)
{
    sockaddr_storage address = {};
    if (server.address_length == 16)
    {
        auto& ipv6 = reinterpret_cast<sockaddr_in6&>(address);
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(server.port);
        memcpy(&ipv6.sin6_addr, server.address, 16);
    }
    else
    {
        auto& ipv4 = reinterpret_cast<sockaddr_in&>(address);
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(server.port);
        memcpy(&ipv4.sin_addr, server.address, 4);
    }
    return address;
}

bool LoadResolver(Resolver* resolver)
{
    ResolvConf::Config config;
    config.name_server_count = 0;
    config.timeout_seconds = DefaultTimeoutSeconds;
    config.attempts = DefaultAttempts;
    size_t length = 0;
    char* text = ReadWholeFile(_PATH_RESCONF, MaximumResolvConfSize, &length);
    if (text != nullptr)
    {
        ResolvConf::Parse(text, length, &config);
        free(text);
    }
    resolver->timeout_seconds = config.timeout_seconds;
    resolver->attempts = config.attempts;
    resolver->name_server_count = config.name_server_count;
    for (size_t index = 0; index < config.name_server_count; ++index)
    {
        resolver->name_servers[index] = SocketAddress(config.name_servers[index]);
    }
    if (resolver->name_server_count > 0)
    {
        return true;
    }
    featheros_network settings;
    if (featheros_network_settings(nullptr, &settings, 0) < 0)
    {
        return false;
    }
    for (unsigned index = 0;
         index < settings.name_server_count && index < ResolvConf::MaximumNameServers; ++index)
    {
        resolver->name_servers[resolver->name_server_count++] = settings.name_servers[index];
    }
    return resolver->name_server_count > 0;
}

int LookUp(const char* name, int family, Candidates* candidates)
{
    Resolver resolver;
    if (!LoadResolver(&resolver))
    {
        return EAI_AGAIN;
    }
    bool no_name = false;
    bool failed = false;
    const uint16_t types[] = {Dns::TypeA, Dns::TypeAaaa};
    for (const uint16_t type : types)
    {
        if ((type == Dns::TypeA && !WantsIpv4(family)) ||
            (type == Dns::TypeAaaa && !WantsIpv6(family)))
        {
            continue;
        }
        Reply reply;
        const Dns::Outcome outcome = Query(resolver, name, type, &reply);
        if (outcome == Dns::Outcome::NoName)
        {
            no_name = true;
            break;
        }
        if (outcome != Dns::Outcome::Answered)
        {
            failed = true;
            continue;
        }
        for (size_t index = 0; index < reply.addresses.count; ++index)
        {
            Add(candidates, type == Dns::TypeA ? AF_INET : AF_INET6, reply.addresses.bytes[index]);
        }
    }
    if (no_name)
    {
        return EAI_NONAME;
    }
    if (candidates->count > 0)
    {
        return 0;
    }
    return failed ? EAI_AGAIN : EAI_NODATA;
}

// Numeric addresses, the loopback or wildcard addresses for a missing node, /etc/hosts,
// localhost, or a lookup.
int FindAddresses(const char* node, int family, int flags, Candidates* candidates)
{
    if (node == nullptr)
    {
        const bool passive = (flags & AI_PASSIVE) != 0;
        const uint8_t ipv4[4] = {static_cast<uint8_t>(passive ? 0 : 127), 0, 0,
                                 static_cast<uint8_t>(passive ? 0 : 1)};
        const in6_addr& ipv6 = passive ? in6addr_any : in6addr_loopback;
        if (WantsIpv4(family))
        {
            Add(candidates, AF_INET, ipv4);
        }
        if (WantsIpv6(family))
        {
            Add(candidates, AF_INET6, ipv6.s6_addr);
        }
        return 0;
    }
    uint8_t ipv4[4];
    uint8_t ipv6[16];
    if (FeatherOS::LibC::ParseIpv4Text(node, ipv4))
    {
        if (!WantsIpv4(family))
        {
            return EAI_ADDRFAMILY;
        }
        Add(candidates, AF_INET, ipv4);
        return 0;
    }
    if (FeatherOS::LibC::ParseIpv6Text(node, ipv6))
    {
        if (!WantsIpv6(family))
        {
            return EAI_ADDRFAMILY;
        }
        Add(candidates, AF_INET6, ipv6);
        return 0;
    }
    if ((flags & AI_NUMERICHOST) != 0 || *node == '\0' || LooksNumeric(node))
    {
        return EAI_NONAME;
    }
    LookUpHosts(node, family, candidates);
    if (candidates->count > 0)
    {
        return 0;
    }
    if (IsLocalhost(node))
    {
        return FindAddresses(nullptr, family, 0, candidates);
    }
    return LookUp(node, family, candidates);
}

// /etc/hosts, then a PTR lookup. EAI_NONAME when neither names the address, and EAI_AGAIN when
// no name server answered, as glibc does.
int FindName(const uint8_t* address, size_t address_length, char* name, size_t capacity)
{
    if (LookUpHostsName(address, address_length, name, capacity))
    {
        return 0;
    }
    char reverse_name[Dns::NameCapacity];
    if (Dns::BuildReverseName(address, address_length, reverse_name, sizeof(reverse_name)) == 0)
    {
        return EAI_NONAME;
    }
    Resolver resolver;
    if (!LoadResolver(&resolver))
    {
        return EAI_AGAIN;
    }
    Reply reply;
    const Dns::Outcome outcome = Query(resolver, reverse_name, Dns::TypePtr, &reply);
    if (outcome == Dns::Outcome::Failed)
    {
        return EAI_AGAIN;
    }
    if (outcome != Dns::Outcome::Answered || reply.name[0] == '\0')
    {
        return EAI_NONAME;
    }
    if (strlen(reply.name) >= capacity)
    {
        return EAI_OVERFLOW;
    }
    strcpy(name, reply.name);
    return 0;
}

int FormatHost(const uint8_t* address, size_t address_length, int flags, char* host,
               size_t capacity)
{
    if ((flags & NI_NUMERICHOST) == 0)
    {
        const int error = FindName(address, address_length, host, capacity);
        if (error != EAI_NONAME)
        {
            return error;
        }
        if ((flags & NI_NAMEREQD) != 0)
        {
            return EAI_NONAME;
        }
    }
    uint8_t ipv4[4];
    uint8_t ipv6[16];
    size_t written = 0;
    if (address_length == 16)
    {
        memcpy(ipv6, address, sizeof(ipv6));
        written = FeatherOS::LibC::FormatIpv6Text(ipv6, host, capacity);
    }
    else
    {
        memcpy(ipv4, address, sizeof(ipv4));
        written = FeatherOS::LibC::FormatIpv4Text(ipv4, host, capacity);
    }
    return written == 0 ? EAI_OVERFLOW : 0;
}

// Without a services database, ports stay numbers.
int FormatService(uint16_t port, char* service, size_t capacity)
{
    char digits[6];
    size_t count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + port % 10);
        port /= 10;
    } while (port != 0);
    if (count >= capacity)
    {
        return EAI_OVERFLOW;
    }
    for (size_t index = 0; index < count; ++index)
    {
        service[index] = digits[count - 1 - index];
    }
    service[count] = '\0';
    return 0;
}

addrinfo* MakeEntry(const Candidate& candidate, uint16_t port, int socket_type, int protocol)
{
    auto* entry = static_cast<Entry*>(calloc(1, sizeof(Entry)));
    if (entry == nullptr)
    {
        return nullptr;
    }
    entry->info.ai_family = candidate.family;
    entry->info.ai_socktype = socket_type;
    entry->info.ai_protocol = protocol != 0                ? protocol
                              : socket_type == SOCK_STREAM ? IPPROTO_TCP
                                                           : IPPROTO_UDP;
    entry->info.ai_addr = reinterpret_cast<sockaddr*>(&entry->address);
    entry->info.ai_addrlen = AddressLength(candidate.family);
    if (candidate.family == AF_INET6)
    {
        entry->address.sin6_family = AF_INET6;
        entry->address.sin6_port = htons(port);
        memcpy(&entry->address.sin6_addr, candidate.bytes, 16);
    }
    else
    {
        auto* ipv4 = reinterpret_cast<sockaddr_in*>(&entry->address);
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(port);
        memcpy(&ipv4->sin_addr, candidate.bytes, 4);
    }
    return &entry->info;
}

} // namespace

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                struct addrinfo** result)
{
    const int flags = hints == nullptr ? 0 : hints->ai_flags;
    const int family = hints == nullptr ? AF_UNSPEC : hints->ai_family;
    const int socket_type = hints == nullptr ? 0 : hints->ai_socktype;
    const int protocol = hints == nullptr ? 0 : hints->ai_protocol;
    if (result == nullptr || (flags & ~KnownFlags) != 0 ||
        ((flags & AI_CANONNAME) != 0 && node == nullptr))
    {
        return EAI_BADFLAGS;
    }
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
    {
        return EAI_FAMILY;
    }
    if (socket_type != 0 && socket_type != SOCK_STREAM && socket_type != SOCK_DGRAM)
    {
        return EAI_SOCKTYPE;
    }
    if (node == nullptr && service == nullptr)
    {
        return EAI_NONAME;
    }
    uint16_t port;
    const int service_error = ParseService(service, flags, &port);
    if (service_error != 0)
    {
        return service_error;
    }
    Candidates candidates;
    candidates.count = 0;
    candidates.canonical_name[0] = '\0';
    const int error = FindAddresses(node, family, flags, &candidates);
    if (error != 0)
    {
        return error;
    }

    const int socket_types[] = {SOCK_STREAM, SOCK_DGRAM};
    addrinfo* first = nullptr;
    addrinfo** link = &first;
    for (size_t index = 0; index < candidates.count; ++index)
    {
        for (const int type : socket_types)
        {
            if (socket_type != 0 && type != socket_type)
            {
                continue;
            }
            *link = MakeEntry(candidates.list[index], port, type, protocol);
            if (*link == nullptr)
            {
                freeaddrinfo(first);
                return EAI_MEMORY;
            }
            link = &(*link)->ai_next;
        }
    }
    if ((flags & AI_CANONNAME) != 0)
    {
        first->ai_canonname =
            strdup(candidates.canonical_name[0] != '\0' ? candidates.canonical_name : node);
        if (first->ai_canonname == nullptr)
        {
            freeaddrinfo(first);
            return EAI_MEMORY;
        }
    }
    *result = first;
    return 0;
}

void freeaddrinfo(struct addrinfo* list)
{
    while (list != nullptr)
    {
        addrinfo* next = list->ai_next;
        free(list->ai_canonname);
        // The entry starts with its addrinfo.
        free(list);
        list = next;
    }
}

int getnameinfo(const struct sockaddr* address, socklen_t address_length, char* host,
                socklen_t host_length, char* service, socklen_t service_length, int flags)
{
    if ((flags & ~KnownNameFlags) != 0)
    {
        return EAI_BADFLAGS;
    }
    if (address == nullptr || address_length < sizeof(sa_family_t))
    {
        return EAI_FAMILY;
    }
    const uint8_t* bytes = nullptr;
    size_t bytes_length = 0;
    uint16_t port = 0;
    if (address->sa_family == AF_INET && address_length >= sizeof(sockaddr_in))
    {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        bytes = reinterpret_cast<const uint8_t*>(&ipv4->sin_addr);
        bytes_length = 4;
        port = ntohs(ipv4->sin_port);
    }
    else if (address->sa_family == AF_INET6 && address_length >= sizeof(sockaddr_in6))
    {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        bytes = ipv6->sin6_addr.s6_addr;
        bytes_length = 16;
        port = ntohs(ipv6->sin6_port);
    }
    else
    {
        return EAI_FAMILY;
    }
    const bool wants_host = host != nullptr && host_length > 0;
    const bool wants_service = service != nullptr && service_length > 0;
    if (!wants_host && !wants_service)
    {
        return EAI_NONAME;
    }
    if (wants_host)
    {
        const int error = FormatHost(bytes, bytes_length, flags, host, host_length);
        if (error != 0)
        {
            return error;
        }
    }
    return wants_service ? FormatService(port, service, service_length) : 0;
}

const char* gai_strerror(int error)
{
    switch (error)
    {
    case 0:
        return "Success";
    case EAI_BADFLAGS:
        return "Bad value for ai_flags";
    case EAI_NONAME:
        return "Name or service not known";
    case EAI_AGAIN:
        return "Temporary failure in name resolution";
    case EAI_FAIL:
        return "Non-recoverable failure in name resolution";
    case EAI_NODATA:
        return "No address associated with hostname";
    case EAI_FAMILY:
        return "ai_family not supported";
    case EAI_SOCKTYPE:
        return "ai_socktype not supported";
    case EAI_SERVICE:
        return "Servname not supported for ai_socktype";
    case EAI_ADDRFAMILY:
        return "Address family for hostname not supported";
    case EAI_MEMORY:
        return "Memory allocation failure";
    case EAI_SYSTEM:
        return "System error";
    case EAI_OVERFLOW:
        return "Argument buffer overflow";
    default:
        return "Unknown error";
    }
}
