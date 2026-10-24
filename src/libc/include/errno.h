#ifndef FEATHEROS_LIBC_ERRNO_H
#define FEATHEROS_LIBC_ERRNO_H

#include <sys/cdefs.h>

// The values match Linux, so they look familiar in other tools and documentation.
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define EOVERFLOW 75
#define EILSEQ 84
#define EWOULDBLOCK EAGAIN

__BEGIN_DECLS

// A function rather than a variable, so each thread can have its own errno later.
int* __errno_location(void);

__END_DECLS

#define errno (*__errno_location())

#endif
