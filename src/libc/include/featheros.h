#ifndef FEATHEROS_LIBC_FEATHEROS_H
#define FEATHEROS_LIBC_FEATHEROS_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

// FeatherOS calls with no POSIX equivalent.

__BEGIN_DECLS

// Shows prompt and reads one console line with editing, without the newline. Ctrl+C discards
// the typed text and shows the prompt again, which printing the prompt and calling fgets cannot
// do. Flushes stdout first; input buffered in stdin is not seen. Returns the line's length, or
// -1 with errno (EIO when the caller is not in the foreground).
ssize_t featheros_read_line(const char* prompt, char* buffer, size_t size);
// The read-only page where the kernel placed the program's arguments and environment. The C
// library copies both out before main, so only its address is of interest, for example to see
// address-space layout randomization.
const void* featheros_arguments_page(void);

__END_DECLS

#endif
