#ifndef FEATHEROS_LIBC_SCHED_H
#define FEATHEROS_LIBC_SCHED_H

#include <sys/cdefs.h>

__BEGIN_DECLS

// The glibc extension: the CPU the caller ran on, which may change right after it returns.
int sched_getcpu(void);

__END_DECLS

#endif
