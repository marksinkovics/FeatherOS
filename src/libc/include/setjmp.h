#ifndef FEATHEROS_LIBC_SETJMP_H
#define FEATHEROS_LIBC_SETJMP_H

#include <sys/cdefs.h>

// The callee-saved registers, the stack pointer, and the return address.
#if defined(__x86_64__)
typedef unsigned long jmp_buf[8];
#elif defined(__aarch64__)
// x19-x30 and sp, with room for d8-d15 once user programs may use floating point.
typedef unsigned long jmp_buf[21];
#else
#error "Unsupported architecture"
#endif

__BEGIN_DECLS

__attribute__((__returns_twice__)) int setjmp(jmp_buf environment);
__attribute__((__noreturn__)) void longjmp(jmp_buf environment, int value);

__END_DECLS

#endif
