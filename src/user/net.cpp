#include "command_line.h"

#include <arpa/inet.h>
#include <errno.h>
#include <featheros.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

constexpr char Help[] = "usage: net\n"
                        "       net dns [address[#port]...]\n"
                        "       net ipv4 address/prefix [gateway] | none\n"
                        "       net down | up\n"
                        "Shows the network settings, or replaces the name servers or the IPv4\n"
                        "address. Name servers use port 53 unless one follows a '#'. down cuts\n"
                        "the link, dropping the addresses and every connection to another host,\n"
                        "while loopback keeps working; up brings it back, with IPv6 at once and\n"
                        "IPv4 once 'dhcp -r' or 'net ipv4' sets it.";

namespace
{

constexpr char Usage[] = "usage: net [dns [address[#port]...] | ipv4 address/prefix [gateway] | "
                         "ipv4 none | down | up]";

bool IsZero(const in6_addr& address)
{
    for (unsigned char byte : address.s6_addr)
    {
        if (byte != 0)
        {
            return false;
        }
    }
    return true;
}

void PrintNameServer(const sockaddr_storage& server)
{
    char text[INET6_ADDRSTRLEN];
    unsigned port;
    if (server.ss_family == AF_INET6)
    {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&server);
        inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text));
        port = ntohs(ipv6->sin6_port);
    }
    else
    {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&server);
        inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text));
        port = ntohs(ipv4->sin_port);
    }
    if (port == 53)
    {
        printf("dns %s\n", text);
    }
    else
    {
        printf("dns %s#%u\n", text, port);
    }
}

void Show(const featheros_network& settings)
{
    printf("mac %02x:%02x:%02x:%02x:%02x:%02x\n", settings.mac[0], settings.mac[1], settings.mac[2],
           settings.mac[3], settings.mac[4], settings.mac[5]);
    puts(settings.link_up != 0 ? "link up" : "link down");
    char address[INET6_ADDRSTRLEN];
    char gateway[INET6_ADDRSTRLEN];
    if (settings.ipv4_address.s_addr == 0)
    {
        puts("ipv4 none");
    }
    else
    {
        inet_ntop(AF_INET, &settings.ipv4_address, address, sizeof(address));
        inet_ntop(AF_INET, &settings.ipv4_gateway, gateway, sizeof(gateway));
        printf("ipv4 %s/%u via %s\n", address, settings.ipv4_prefix_length, gateway);
    }
    if (!IsZero(settings.ipv6_link_local))
    {
        inet_ntop(AF_INET6, &settings.ipv6_link_local, address, sizeof(address));
        printf("ipv6 %s\n", address);
    }
    if (!IsZero(settings.ipv6_global))
    {
        inet_ntop(AF_INET6, &settings.ipv6_global, address, sizeof(address));
        inet_ntop(AF_INET6, &settings.ipv6_router, gateway, sizeof(gateway));
        printf("ipv6 %s/%u via %s\n", address, settings.ipv6_prefix_length,
               IsZero(settings.ipv6_router) ? "none" : gateway);
    }
    for (unsigned index = 0; index < settings.name_server_count; ++index)
    {
        PrintNameServer(settings.name_servers[index]);
    }
}

// "10.0.2.3", "fec0::3" or either with "#port".
bool ParseNameServer(const char* text, sockaddr_storage* server)
{
    char address[INET6_ADDRSTRLEN];
    const char* hash = strchr(text, '#');
    const size_t length = hash == nullptr ? strlen(text) : static_cast<size_t>(hash - text);
    unsigned long long port = 53;
    if (length >= sizeof(address) ||
        (hash != nullptr &&
         (!FeatherOS::User::ParseUnsigned(hash + 1, &port) || port == 0 || port > 0xFFFF)))
    {
        return false;
    }
    memcpy(address, text, length);
    address[length] = '\0';
    *server = {};
    auto* ipv4 = reinterpret_cast<sockaddr_in*>(server);
    auto* ipv6 = reinterpret_cast<sockaddr_in6*>(server);
    if (inet_pton(AF_INET, address, &ipv4->sin_addr) == 1)
    {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(static_cast<uint16_t>(port));
        return true;
    }
    if (inet_pton(AF_INET6, address, &ipv6->sin6_addr) == 1)
    {
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(static_cast<uint16_t>(port));
        return true;
    }
    return false;
}

int SetNameServers(int count, char** servers)
{
    if (count > FEATHEROS_NAME_SERVERS)
    {
        printf("net: at most %d name servers\n", FEATHEROS_NAME_SERVERS);
        return 2;
    }
    featheros_network settings = {};
    settings.name_server_count = static_cast<unsigned>(count);
    for (int index = 0; index < count; ++index)
    {
        if (!ParseNameServer(servers[index], &settings.name_servers[index]))
        {
            printf("net: %s: not an address with an optional #port\n", servers[index]);
            return 2;
        }
    }
    if (featheros_network_settings(&settings, nullptr, FEATHEROS_NETWORK_NAME_SERVERS) < 0)
    {
        printf("net: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

int SetIpv4(int count, char** words)
{
    featheros_network settings = {};
    if (count == 1 && strcmp(words[0], "none") == 0)
    {
        if (featheros_network_settings(&settings, nullptr, FEATHEROS_NETWORK_IPV4) < 0)
        {
            printf("net: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }
    char address[INET_ADDRSTRLEN];
    const char* slash = count >= 1 ? strchr(words[0], '/') : nullptr;
    unsigned long long prefix = 0;
    if (count < 1 || count > 2 || slash == nullptr ||
        static_cast<size_t>(slash - words[0]) >= sizeof(address) ||
        !FeatherOS::User::ParseUnsigned(slash + 1, &prefix) || prefix > 32)
    {
        puts(Usage);
        return 2;
    }
    memcpy(address, words[0], static_cast<size_t>(slash - words[0]));
    address[slash - words[0]] = '\0';
    settings.ipv4_prefix_length = static_cast<unsigned char>(prefix);
    if (inet_pton(AF_INET, address, &settings.ipv4_address) != 1 ||
        (count == 2 && inet_pton(AF_INET, words[1], &settings.ipv4_gateway) != 1))
    {
        puts("net: not an IPv4 address");
        return 2;
    }
    if (featheros_network_settings(&settings, nullptr, FEATHEROS_NETWORK_IPV4) < 0)
    {
        printf("net: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

int SetLink(bool up)
{
    featheros_network settings = {};
    settings.link_up = up ? 1 : 0;
    if (featheros_network_settings(&settings, nullptr, FEATHEROS_NETWORK_LINK) < 0)
    {
        printf("net: %s\n", strerror(errno));
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
    if (argc >= 2 && strcmp(argv[1], "dns") == 0)
    {
        return SetNameServers(argc - 2, argv + 2);
    }
    if (argc >= 2 && strcmp(argv[1], "ipv4") == 0)
    {
        return SetIpv4(argc - 2, argv + 2);
    }
    if (argc == 2 && (strcmp(argv[1], "down") == 0 || strcmp(argv[1], "up") == 0))
    {
        return SetLink(strcmp(argv[1], "up") == 0);
    }
    if (argc != 1)
    {
        puts(Usage);
        return 2;
    }
    featheros_network settings;
    if (featheros_network_settings(nullptr, &settings, 0) < 0)
    {
        printf("net: %s\n", strerror(errno));
        return 1;
    }
    Show(settings);
    return 0;
}
