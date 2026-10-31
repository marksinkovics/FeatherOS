#include "command_line.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

constexpr char Help[] = "usage: host [-4 | -6] name | host address\n"
                        "Looks up a name's IPv4 and IPv6 addresses, or only one kind, or the name "
                        "of an address.";

// The name /etc/hosts or a PTR record gives an address.
int LookUpAddress(const char* text)
{
    sockaddr_storage address = {};
    auto* ipv4 = reinterpret_cast<sockaddr_in*>(&address);
    auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&address);
    if (inet_pton(AF_INET, text, &ipv4->sin_addr) == 1)
    {
        ipv4->sin_family = AF_INET;
    }
    else if (inet_pton(AF_INET6, text, &ipv6->sin6_addr) == 1)
    {
        ipv6->sin6_family = AF_INET6;
    }
    else
    {
        return -1;
    }
    char name[NI_MAXHOST];
    const int error = getnameinfo(reinterpret_cast<const sockaddr*>(&address), sizeof(address),
                                  name, sizeof(name), nullptr, 0, NI_NAMEREQD);
    if (error != 0)
    {
        printf("host: %s: %s\n", text, gai_strerror(error));
        return 1;
    }
    printf("%s has name %s\n", text, name);
    return 0;
}

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    int family = AF_UNSPEC;
    int first = 1;
    if (first < argc && strcmp(argv[first], "-4") == 0)
    {
        family = AF_INET;
        ++first;
    }
    else if (first < argc && strcmp(argv[first], "-6") == 0)
    {
        family = AF_INET6;
        ++first;
    }
    if (argc != first + 1)
    {
        puts("usage: host [-4 | -6] name | host address");
        return 2;
    }
    const char* name = argv[first];
    if (first == 1)
    {
        const int status = LookUpAddress(name);
        if (status >= 0)
        {
            return status;
        }
    }
    addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    const int error = getaddrinfo(name, nullptr, &hints, &results);
    if (error != 0)
    {
        printf("host: %s: %s\n", name, gai_strerror(error));
        return 1;
    }
    for (const addrinfo* result = results; result != nullptr; result = result->ai_next)
    {
        char text[INET6_ADDRSTRLEN];
        const bool ipv6 = result->ai_family == AF_INET6;
        const void* address =
            ipv6 ? static_cast<const void*>(
                       &reinterpret_cast<const sockaddr_in6*>(result->ai_addr)->sin6_addr)
                 : &reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr;
        inet_ntop(result->ai_family, address, text, sizeof(text));
        printf("%s has %saddress %s\n", name, ipv6 ? "IPv6 " : "", text);
    }
    freeaddrinfo(results);
    return 0;
}
