#ifndef FEATHEROS_LIBC_SYS_RESOURCE_H
#define FEATHEROS_LIBC_SYS_RESOURCE_H

#include <sys/cdefs.h>

__BEGIN_DECLS

typedef unsigned long rlim_t;

#define RLIM_INFINITY (~0UL)
#define RLIMIT_NOFILE 7

struct rlimit
{
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

// RLIMIT_NOFILE only: EINVAL for any other resource, and from setrlimit for a soft limit above
// the hard one; EPERM for a hard limit above 1024, RLIM_INFINITY included; EFAULT for a bad
// pointer. The limits are the process's, shared by its threads and inherited by its children.
int getrlimit(int resource, struct rlimit* limits);
int setrlimit(int resource, const struct rlimit* limits);

__END_DECLS

#endif
