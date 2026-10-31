#ifndef FEATHEROS_LIBC_POLL_H
#define FEATHEROS_LIBC_POLL_H

#include <sys/cdefs.h>

typedef unsigned long nfds_t;

struct pollfd
{
    int fd;
    short events;
    short revents;
};

// Linux's values. POLLERR, POLLHUP and POLLNVAL are reported without being asked for.
#define POLLIN 0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM POLLIN
#define POLLWRNORM POLLOUT

__BEGIN_DECLS

// At most 64 descriptors. A negative timeout waits until a descriptor is ready.
int poll(struct pollfd* descriptors, nfds_t count, int timeout);

__END_DECLS

#endif
