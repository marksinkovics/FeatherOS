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
// Offsets past the end of the file fail with EINVAL.
off_t lseek(int descriptor, off_t offset, int whence);
int unlink(const char* path);
int rmdir(const char* path);
int chdir(const char* path);
char* getcwd(char* buffer, size_t size);
int access(const char* path, int mode);
int isatty(int descriptor);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int microseconds);
// Moves the end of the heap that malloc also uses, and returns the old end. A negative increment
// returns the whole pages above the new end to the kernel; going below the heap's start fails
// with EINVAL. While a program's own sbrk memory lies above malloc's, malloc cannot shrink.
void* sbrk(intptr_t increment);
pid_t getpid(void);
pid_t getppid(void);
__attribute__((__noreturn__)) void _exit(int status);

__END_DECLS

#endif
