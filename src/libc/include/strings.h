#ifndef FEATHEROS_LIBC_STRINGS_H
#define FEATHEROS_LIBC_STRINGS_H

#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// Compare as strcmp and strncmp, ignoring ASCII case.
int strcasecmp(const char* left, const char* right);
int strncasecmp(const char* left, const char* right, size_t count);

__END_DECLS

#endif
