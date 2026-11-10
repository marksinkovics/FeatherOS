#ifndef FEATHEROS_LIBC_MALLOC_H
#define FEATHEROS_LIBC_MALLOC_H

#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// The bytes of an allocation from malloc and its relatives that may be used, at least what was
// asked for; 0 for nullptr.
size_t malloc_usable_size(void* pointer);

__END_DECLS

#endif
