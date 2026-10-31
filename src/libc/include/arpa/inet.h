#ifndef FEATHEROS_LIBC_ARPA_INET_H
#define FEATHEROS_LIBC_ARPA_INET_H

#include <netinet/in.h>
#include <sys/cdefs.h>
#include <sys/socket.h>

__BEGIN_DECLS

// AF_INET and AF_INET6; other families fail with EAFNOSUPPORT.
int inet_pton(int family, const char* text, void* address);
const char* inet_ntop(int family, const void* address, char* text, socklen_t size);

__END_DECLS

#endif
