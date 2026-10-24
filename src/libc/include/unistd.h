#ifndef FEATHEROS_LIBC_UNISTD_H
#define FEATHEROS_LIBC_UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>

// All three are the console: reading returns one line at a time, and only the foreground
// program may read.
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

// access modes. With no users, R_OK and X_OK hold for every file; W_OK fails only for the
// read-only initramfs.
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

__BEGIN_DECLS

extern char** environ;

ssize_t read(int descriptor, void* buffer, size_t count);
ssize_t write(int descriptor, const void* buffer, size_t count);
int close(int descriptor);
// descriptors[0] gets the read end and descriptors[1] the write end. A pipe holds 4 KiB; writes
// of up to PIPE_BUF bytes are never split. Reading returns 0 once every write end is closed,
// and writing fails with EPIPE once every read end is.
int pipe(int descriptors[2]);
// flags may hold O_CLOEXEC.
int pipe2(int descriptors[2], int flags);
// Copies share the file's offset, and are not close-on-exec.
int dup(int descriptor);
int dup2(int descriptor, int target);
// Offsets past the end of the file fail with EINVAL.
off_t lseek(int descriptor, off_t offset, int whence);
int unlink(const char* path);
int rmdir(const char* path);
int chdir(const char* path);
char* getcwd(char* buffer, size_t size);
int access(const char* path, int mode);
int isatty(int descriptor);
// Both end early, failing with EINTR or returning the seconds left, when a signal arrives.
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int microseconds);
// Moves the end of the heap that malloc also uses, and returns the old end. A negative increment
// returns the whole pages above the new end to the kernel; going below the heap's start fails
// with EINVAL. While a program's own sbrk memory lies above malloc's, malloc cannot shrink.
void* sbrk(intptr_t increment);
pid_t getpid(void);
pid_t getppid(void);
pid_t getpgrp(void);
// Fails with ESRCH for a process that does not exist; groups of other sessions are visible too.
pid_t getpgid(pid_t pid);
// The group that may read the console. Only init and session leaders (the shell) may change it,
// to their own group or one holding one of their children; others get EPERM.
pid_t tcgetpgrp(int descriptor);
int tcsetpgrp(int descriptor, pid_t group);
__attribute__((__noreturn__)) void _exit(int status);
// Waits for a signal; returns -1 with EINTR once its handler has run.
int pause(void);

__END_DECLS

#endif
