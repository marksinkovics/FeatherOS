#ifndef FEATHEROS_LIBC_FCNTL_H
#define FEATHEROS_LIBC_FCNTL_H

#include <sys/cdefs.h>
#include <sys/types.h>

// The values match Linux.
#define O_RDONLY 00
#define O_WRONLY 01
#define O_RDWR 02
#define O_ACCMODE 03
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
// Accepted and ignored: there is no exec to close descriptors across.
#define O_CLOEXEC 02000000

__BEGIN_DECLS

// O_CREAT and O_TRUNC need write access. The mode argument is accepted and ignored, since there
// are no permissions yet.
int open(const char* path, int flags, ...);
int creat(const char* path, mode_t mode);

__END_DECLS

#endif
