#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <featheros.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

using FeatherOS::LibC::Check;

namespace
{

bool ToKernelAddress(const struct sockaddr* address, socklen_t length,
                     FeatherOS::User::SocketAddress* result)
{
    *result = {};
    if (address == nullptr || length < sizeof(sa_family_t))
    {
        return false;
    }
    result->family = address->sa_family;
    if (address->sa_family == AF_INET)
    {
        if (length < sizeof(sockaddr_in))
        {
            return false;
        }
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        const auto* bytes = reinterpret_cast<const uint8_t*>(&ipv4->sin_addr.s_addr);
        result->port = ntohs(ipv4->sin_port);
        for (int index = 0; index < 4; ++index)
        {
            result->address[index] = bytes[index];
        }
    }
    else if (address->sa_family == AF_INET6)
    {
        if (length < sizeof(sockaddr_in6))
        {
            return false;
        }
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        result->port = ntohs(ipv6->sin6_port);
        result->scope = ipv6->sin6_scope_id;
        for (int index = 0; index < 16; ++index)
        {
            result->address[index] = ipv6->sin6_addr.s6_addr[index];
        }
    }
    return true;
}

// Truncates to the caller's length and reports the full one, as POSIX asks.
void FromKernelAddress(const FeatherOS::User::SocketAddress& source, struct sockaddr* address,
                       socklen_t* length)
{
    sockaddr_storage storage = {};
    socklen_t size = sizeof(sa_family_t);
    storage.ss_family = static_cast<sa_family_t>(source.family);
    if (source.family == AF_INET)
    {
        auto* ipv4 = reinterpret_cast<sockaddr_in*>(&storage);
        auto* bytes = reinterpret_cast<uint8_t*>(&ipv4->sin_addr.s_addr);
        ipv4->sin_port = htons(source.port);
        for (int index = 0; index < 4; ++index)
        {
            bytes[index] = source.address[index];
        }
        size = sizeof(sockaddr_in);
    }
    else if (source.family == AF_INET6)
    {
        auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&storage);
        ipv6->sin6_port = htons(source.port);
        ipv6->sin6_scope_id = source.scope;
        for (int index = 0; index < 16; ++index)
        {
            ipv6->sin6_addr.s6_addr[index] = source.address[index];
        }
        size = sizeof(sockaddr_in6);
    }
    const socklen_t copied = *length < size ? *length : size;
    const auto* from = reinterpret_cast<const char*>(&storage);
    auto* to = reinterpret_cast<char*>(address);
    for (socklen_t index = 0; index < copied; ++index)
    {
        to[index] = from[index];
    }
    *length = size;
}

uint64_t KernelSocketFlags(int flags)
{
    uint64_t kernel_flags = 0;
    if ((flags & SOCK_CLOEXEC) != 0)
    {
        kernel_flags |= FeatherOS::User::SocketCloseOnExec;
    }
    if ((flags & SOCK_NONBLOCK) != 0)
    {
        kernel_flags |= FeatherOS::User::SocketNonBlocking;
    }
    return kernel_flags;
}

} // namespace

int socket(int family, int type, int protocol)
{
    const uint64_t kernel_type =
        (static_cast<unsigned>(type) & ~static_cast<unsigned>(SOCK_CLOEXEC | SOCK_NONBLOCK)) |
        KernelSocketFlags(type);
    return Check<int>(FeatherOS::User::CreateSocket(static_cast<uint64_t>(family), kernel_type,
                                                    static_cast<uint64_t>(protocol)));
}

int bind(int socket, const struct sockaddr* address, socklen_t length)
{
    FeatherOS::User::SocketAddress kernel_address;
    if (!ToKernelAddress(address, length, &kernel_address))
    {
        errno = EINVAL;
        return -1;
    }
    return Check<int>(FeatherOS::User::Bind(static_cast<uint64_t>(socket), &kernel_address));
}

int connect(int socket, const struct sockaddr* address, socklen_t length)
{
    FeatherOS::User::SocketAddress kernel_address;
    if (!ToKernelAddress(address, length, &kernel_address))
    {
        errno = EINVAL;
        return -1;
    }
    return Check<int>(FeatherOS::User::Connect(static_cast<uint64_t>(socket), &kernel_address));
}

int listen(int socket, int backlog)
{
    return Check<int>(FeatherOS::User::Listen(static_cast<uint64_t>(socket),
                                              backlog < 0 ? 0 : static_cast<uint64_t>(backlog)));
}

int accept4(int socket, struct sockaddr* address, socklen_t* length, int flags)
{
    if ((address != nullptr && length == nullptr) || (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    FeatherOS::User::SocketAddress kernel_address = {};
    const int accepted = Check<int>(FeatherOS::User::Accept(
        static_cast<uint64_t>(socket), &kernel_address, KernelSocketFlags(flags)));
    if (accepted >= 0 && address != nullptr)
    {
        FromKernelAddress(kernel_address, address, length);
    }
    return accepted;
}

int accept(int socket, struct sockaddr* address, socklen_t* length)
{
    return accept4(socket, address, length, 0);
}

int shutdown(int socket, int how)
{
    return Check<int>(
        FeatherOS::User::ShutdownSocket(static_cast<uint64_t>(socket), static_cast<uint64_t>(how)));
}

namespace
{

int SocketName(int socket, struct sockaddr* address, socklen_t* length, bool peer)
{
    if (address == nullptr || length == nullptr)
    {
        errno = EFAULT;
        return -1;
    }
    FeatherOS::User::SocketAddress kernel_address = {};
    if (Check<int>(
            FeatherOS::User::SocketName(static_cast<uint64_t>(socket), &kernel_address, peer)) < 0)
    {
        return -1;
    }
    FromKernelAddress(kernel_address, address, length);
    return 0;
}

} // namespace

int getsockname(int socket, struct sockaddr* address, socklen_t* length)
{
    return SocketName(socket, address, length, false);
}

int getpeername(int socket, struct sockaddr* address, socklen_t* length)
{
    return SocketName(socket, address, length, true);
}

ssize_t send(int socket, const void* buffer, size_t length, int flags)
{
    return sendto(socket, buffer, length, flags, nullptr, 0);
}

ssize_t recv(int socket, void* buffer, size_t length, int flags)
{
    return recvfrom(socket, buffer, length, flags, nullptr, nullptr);
}

ssize_t sendto(int socket, const void* buffer, size_t length, int flags,
               const struct sockaddr* address, socklen_t address_length)
{
    FeatherOS::User::SocketAddress kernel_address;
    if (address != nullptr && !ToKernelAddress(address, address_length, &kernel_address))
    {
        errno = EINVAL;
        return -1;
    }
    const FeatherOS::User::SocketMessage message = {
        .buffer = reinterpret_cast<uint64_t>(buffer),
        .length = length,
        .address = address == nullptr ? 0 : reinterpret_cast<uint64_t>(&kernel_address),
    };
    return Check<ssize_t>(FeatherOS::User::SendTo(static_cast<uint64_t>(socket), &message,
                                                  static_cast<uint64_t>(flags)));
}

ssize_t recvfrom(int socket, void* buffer, size_t length, int flags, struct sockaddr* address,
                 socklen_t* address_length)
{
    if (address != nullptr && address_length == nullptr)
    {
        errno = EINVAL;
        return -1;
    }
    FeatherOS::User::SocketAddress kernel_address = {};
    const FeatherOS::User::SocketMessage message = {
        .buffer = reinterpret_cast<uint64_t>(buffer),
        .length = length,
        .address = reinterpret_cast<uint64_t>(&kernel_address),
    };
    const ssize_t received = Check<ssize_t>(FeatherOS::User::ReceiveFrom(
        static_cast<uint64_t>(socket), &message, static_cast<uint64_t>(flags)));
    if (received >= 0 && address != nullptr)
    {
        FromKernelAddress(kernel_address, address, address_length);
    }
    return received;
}

int setsockopt(int socket, int level, int option, const void* value, socklen_t length)
{
    if (level != SOL_SOCKET || (option != SO_RCVTIMEO && option != SO_REUSEADDR))
    {
        errno = ENOPROTOOPT;
        return -1;
    }
    if (option == SO_REUSEADDR)
    {
        if (value == nullptr || length < sizeof(int))
        {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    const auto* timeout = static_cast<const timeval*>(value);
    if (timeout == nullptr || length < sizeof(timeval) || timeout->tv_sec < 0 ||
        timeout->tv_usec < 0 || timeout->tv_usec >= 1000000)
    {
        errno = EINVAL;
        return -1;
    }
    // Rounded up, so a short timeout never turns into "wait forever".
    const uint64_t milliseconds =
        static_cast<uint64_t>(timeout->tv_sec) * 1000 + (timeout->tv_usec + 999) / 1000;
    return Check<int>(FeatherOS::User::SetSocketOption(
        static_cast<uint64_t>(socket), FeatherOS::User::SocketOptionReceiveTimeout, milliseconds));
}

int getsockopt(int socket, int level, int option, void* value, socklen_t* length)
{
    if (level != SOL_SOCKET || (option != SO_ERROR && option != SO_RCVTIMEO))
    {
        errno = ENOPROTOOPT;
        return -1;
    }
    const socklen_t size = option == SO_ERROR ? sizeof(int) : sizeof(timeval);
    if (value == nullptr || length == nullptr || *length < size)
    {
        errno = EINVAL;
        return -1;
    }
    uint64_t result = 0;
    if (Check<int>(FeatherOS::User::GetSocketOption(
            static_cast<uint64_t>(socket),
            option == SO_ERROR ? FeatherOS::User::SocketOptionError
                               : FeatherOS::User::SocketOptionReceiveTimeout,
            &result)) < 0)
    {
        return -1;
    }
    if (option == SO_ERROR)
    {
        *static_cast<int*>(value) = result == 0 ? 0 : FeatherOS::LibC::ErrorNumber(result);
    }
    else
    {
        *static_cast<timeval*>(value) = {.tv_sec = static_cast<time_t>(result / 1000),
                                         .tv_usec = static_cast<suseconds_t>(result % 1000 * 1000)};
    }
    *length = size;
    return 0;
}

int featheros_network_settings(const struct featheros_network* new_settings,
                               struct featheros_network* old_settings, int changes)
{
    FeatherOS::User::NetworkSettings request = {};
    if (changes != 0)
    {
        if (new_settings == nullptr ||
            new_settings->name_server_count > FeatherOS::User::MaximumNameServers)
        {
            errno = EINVAL;
            return -1;
        }
        request.link_up = new_settings->link_up != 0 ? 1 : 0;
        request.ipv4_prefix_length = new_settings->ipv4_prefix_length;
        memcpy(request.ipv4_address, &new_settings->ipv4_address, 4);
        memcpy(request.ipv4_gateway, &new_settings->ipv4_gateway, 4);
        request.name_server_count = new_settings->name_server_count;
        for (unsigned int index = 0; index < request.name_server_count; ++index)
        {
            if (!ToKernelAddress(
                    reinterpret_cast<const sockaddr*>(&new_settings->name_servers[index]),
                    sizeof(sockaddr_storage), &request.name_servers[index]))
            {
                errno = EINVAL;
                return -1;
            }
        }
    }
    FeatherOS::User::NetworkSettings current = {};
    if (Check<int>(FeatherOS::User::ExchangeNetworkSettings(
            changes == 0 ? nullptr : &request, old_settings == nullptr ? nullptr : &current,
            static_cast<uint64_t>(changes))) < 0)
    {
        return -1;
    }
    if (old_settings == nullptr)
    {
        return 0;
    }
    *old_settings = {};
    memcpy(old_settings->mac, current.mac, sizeof(current.mac));
    old_settings->ipv4_prefix_length = current.ipv4_prefix_length;
    old_settings->ipv6_prefix_length = current.ipv6_prefix_length;
    memcpy(&old_settings->ipv4_address, current.ipv4_address, 4);
    memcpy(&old_settings->ipv4_gateway, current.ipv4_gateway, 4);
    memcpy(&old_settings->ipv6_link_local, current.ipv6_link_local, 16);
    memcpy(&old_settings->ipv6_global, current.ipv6_global, 16);
    memcpy(&old_settings->ipv6_router, current.ipv6_router, 16);
    old_settings->link_up = current.link_up != 0 ? 1 : 0;
    old_settings->name_server_count = current.name_server_count;
    for (unsigned int index = 0; index < current.name_server_count; ++index)
    {
        socklen_t length = sizeof(sockaddr_storage);
        FromKernelAddress(current.name_servers[index],
                          reinterpret_cast<sockaddr*>(&old_settings->name_servers[index]), &length);
    }
    return 0;
}
