#ifndef FEATHEROS_LIBC_SYS_SOCKET_H
#define FEATHEROS_LIBC_SYS_SOCKET_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};

// Large and aligned enough for any address the library knows.
struct sockaddr_storage
{
    sa_family_t ss_family;
    char __ss_padding[118];
    unsigned long __ss_align;
};

// Linux's values.
#define AF_UNSPEC 0
#define AF_INET 2
#define AF_INET6 10
#define PF_UNSPEC AF_UNSPEC
#define PF_INET AF_INET
#define PF_INET6 AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000

#define SOL_SOCKET 1
// Accepted and ignored: TCP ports are always bound as if it were set.
#define SO_REUSEADDR 2
// getsockopt only: the error a non-blocking connect ended with, or a connected UDP socket's from
// an ICMP message, which reading clears; 0 for none.
#define SO_ERROR 4
#define SO_RCVTIMEO 20

#define SOMAXCONN 16
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

__BEGIN_DECLS

int socket(int family, int type, int protocol);
int bind(int socket, const struct sockaddr* address, socklen_t length);
int connect(int socket, const struct sockaddr* address, socklen_t length);
int listen(int socket, int backlog);
int accept(int socket, struct sockaddr* address, socklen_t* length);
int accept4(int socket, struct sockaddr* address, socklen_t* length, int flags);
int shutdown(int socket, int how);
int getsockname(int socket, struct sockaddr* address, socklen_t* length);
int getpeername(int socket, struct sockaddr* address, socklen_t* length);
ssize_t send(int socket, const void* buffer, size_t length, int flags);
ssize_t recv(int socket, void* buffer, size_t length, int flags);
ssize_t sendto(int socket, const void* buffer, size_t length, int flags,
               const struct sockaddr* address, socklen_t address_length);
ssize_t recvfrom(int socket, void* buffer, size_t length, int flags, struct sockaddr* address,
                 socklen_t* address_length);
int setsockopt(int socket, int level, int option, const void* value, socklen_t length);
int getsockopt(int socket, int level, int option, void* value, socklen_t* length);

__END_DECLS

#endif
