#ifndef FEATHEROS_LIBC_STDIO_H
#define FEATHEROS_LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 512
// Streams that can be open at once, counting stdin, stdout and stderr. Each has a 512-byte
// buffer, so this is kept below the kernel's 32 descriptors.
#define FOPEN_MAX 19
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define __FEATHEROS_PRINTF(format_index, first_argument)                                           \
    __attribute__((__format__(__printf__, format_index, first_argument)))

#define __FEATHEROS_SCANF(format_index, first_argument)                                            \
    __attribute__((__format__(__scanf__, format_index, first_argument)))

__BEGIN_DECLS

typedef struct FeatherFile FILE;

// stdout is line-buffered and stderr unbuffered. Reading from the console first flushes stdout,
// so a prompt without a newline still appears.
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

// Modes "r", "w", "a", and the same with '+'; a 'b' is accepted and ignored. A disk file can
// have only one writer at a time, so opening it for writing twice fails with EBUSY.
FILE* fopen(const char* path, const char* mode);
FILE* fdopen(int descriptor, const char* mode);
int fclose(FILE* stream);
int fflush(FILE* stream);
// Only the mode is used; the stream keeps its own buffer.
int setvbuf(FILE* stream, char* buffer, int mode, size_t size);
void setbuf(FILE* stream, char* buffer);

size_t fread(void* buffer, size_t size, size_t count, FILE* stream);
size_t fwrite(const void* buffer, size_t size, size_t count, FILE* stream);
int fgetc(FILE* stream);
int getc(FILE* stream);
int getchar(void);
int ungetc(int character, FILE* stream);
char* fgets(char* buffer, int size, FILE* stream);
ssize_t getline(char** line, size_t* capacity, FILE* stream);
int fputc(int character, FILE* stream);
int putc(int character, FILE* stream);
int putchar(int character);
int fputs(const char* text, FILE* stream);
int puts(const char* text);

// Offsets past the end of a file fail with EINVAL; the console cannot seek (ESPIPE).
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
void rewind(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
void clearerr(FILE* stream);
int fileno(FILE* stream);
// Holds the stream's lock across several calls, so another thread's output cannot come between
// them; the lock is recursive, so the holder's own calls still work.
void flockfile(FILE* stream);
int ftrylockfile(FILE* stream);
void funlockfile(FILE* stream);

int remove(const char* path);
// Within one filesystem (EXDEV between the disk and /tmp). A file at to is replaced; a directory
// there fails with EEXIST. Open files cannot be renamed (EBUSY).
int rename(const char* from, const char* to);
void perror(const char* prefix);

int printf(const char* format, ...) __FEATHEROS_PRINTF(1, 2);
int fprintf(FILE* stream, const char* format, ...) __FEATHEROS_PRINTF(2, 3);
int dprintf(int descriptor, const char* format, ...) __FEATHEROS_PRINTF(2, 3);
int sprintf(char* buffer, const char* format, ...) __FEATHEROS_PRINTF(2, 3);
int snprintf(char* buffer, size_t size, const char* format, ...) __FEATHEROS_PRINTF(3, 4);
int vprintf(const char* format, va_list arguments) __FEATHEROS_PRINTF(1, 0);
int vfprintf(FILE* stream, const char* format, va_list arguments) __FEATHEROS_PRINTF(2, 0);
int vdprintf(int descriptor, const char* format, va_list arguments) __FEATHEROS_PRINTF(2, 0);
int vsprintf(char* buffer, const char* format, va_list arguments) __FEATHEROS_PRINTF(2, 0);
int vsnprintf(char* buffer, size_t size, const char* format, va_list arguments)
    __FEATHEROS_PRINTF(3, 0);

// Integers (with the same length modifiers as printf), floating point, %c, %s, %[...], %n, %*,
// and widths.
int scanf(const char* format, ...) __FEATHEROS_SCANF(1, 2);
int fscanf(FILE* stream, const char* format, ...) __FEATHEROS_SCANF(2, 3);
int sscanf(const char* text, const char* format, ...) __FEATHEROS_SCANF(2, 3);
int vscanf(const char* format, va_list arguments) __FEATHEROS_SCANF(1, 0);
int vfscanf(FILE* stream, const char* format, va_list arguments) __FEATHEROS_SCANF(2, 0);
int vsscanf(const char* text, const char* format, va_list arguments) __FEATHEROS_SCANF(2, 0);

__END_DECLS

#endif
