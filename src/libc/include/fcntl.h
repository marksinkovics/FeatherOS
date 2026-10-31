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
// Calls that would wait fail with EAGAIN instead, or a socket's connect with EINPROGRESS. Every
// descriptor that shares the open file, through dup or posix_spawn, shares this flag.
#define O_NONBLOCK 04000
#define O_NDELAY O_NONBLOCK
// The descriptor is not passed to programs this one starts.
#define O_CLOEXEC 02000000

// fcntl commands, as on Linux. F_SETFL changes O_APPEND and O_NONBLOCK, except on the console,
// and ignores other flags. Record locks are not supported.
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

__BEGIN_DECLS

// O_CREAT and O_TRUNC need write access. The mode argument is accepted and ignored, since there
// are no permissions yet.
int open(const char* path, int flags, ...);
int creat(const char* path, mode_t mode);
int fcntl(int descriptor, int command, ...);

__END_DECLS

#endif
