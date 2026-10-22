#ifndef FEATHEROS_LIBC_STDLIB_H
#define FEATHEROS_LIBC_STDLIB_H

#include <stddef.h>
#include <sys/cdefs.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFFFFFF

__BEGIN_DECLS

typedef struct
{
    int quot;
    int rem;
} div_t;

typedef struct
{
    long quot;
    long rem;
} ldiv_t;

typedef struct
{
    long long quot;
    long long rem;
} lldiv_t;

__attribute__((__noreturn__)) void exit(int status);
__attribute__((__noreturn__)) void _Exit(int status);
__attribute__((__noreturn__)) void abort(void);
int atexit(void (*function)(void));

// Allocations are 16-byte aligned. Freed memory is reused, and once more than 256 KiB is free
// at the end of the heap, all but 64 KiB of it goes back to the kernel.
void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* pointer, size_t size);
void free(void* pointer);

int abs(int value);
long labs(long value);
long long llabs(long long value);
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);

// Correctly rounded; hexadecimal ("0x1.8p3"), "inf", "infinity" and "nan" are accepted.
double strtod(const char* text, char** end);
float strtof(const char* text, char** end);
long double strtold(const char* text, char** end);
double atof(const char* text);
int atoi(const char* text);
long atol(const char* text);
long long atoll(const char* text);
long strtol(const char* text, char** end, int base);
long long strtoll(const char* text, char** end, int base);
unsigned long strtoul(const char* text, char** end, int base);
unsigned long long strtoull(const char* text, char** end, int base);

void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t count, size_t size,
              int (*compare)(const void*, const void*));

char* getenv(const char* name);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);

int rand(void);
void srand(unsigned int seed);

__END_DECLS

#endif
