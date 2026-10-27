#ifndef FEATHEROS_LIBC_LIMITS_H
#define FEATHEROS_LIBC_LIMITS_H

// GCC's own limits.h has the basic limits; in C++ it leaves the long long ones to the C library.
#include_next <limits.h>

#ifndef LLONG_MAX
#define LLONG_MAX __LONG_LONG_MAX__
#endif
#ifndef LLONG_MIN
#define LLONG_MIN (-LLONG_MAX - 1LL)
#endif
#ifndef ULLONG_MAX
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#endif

// The kernel's limits, as POSIX names them.
#define PIPE_BUF 512
#define OPEN_MAX 32
#define PATH_MAX 64

#endif
