#ifndef FEATHEROS_LIBC_UNISTD_H
#define FEATHEROS_LIBC_UNISTD_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

// All three are the console: reading returns one line at a time, and only the foreground
// program may read.
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

__BEGIN_DECLS

extern char** environ;

ssize_t read(int descriptor, void* buffer, size_t count);
ssize_t write(int descriptor, const void* buffer, size_t count);
int close(int descriptor);
pid_t getpid(void);
pid_t getppid(void);
__attribute__((__noreturn__)) void _exit(int status);

__END_DECLS

#endif
