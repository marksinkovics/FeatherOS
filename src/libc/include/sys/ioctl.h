#ifndef FEATHEROS_LIBC_SYS_IOCTL_H
#define FEATHEROS_LIBC_SYS_IOCTL_H

#include <sys/cdefs.h>
#include <termios.h>

#define TIOCGWINSZ 1
#define TIOCSWINSZ 2

__BEGIN_DECLS

// Only TIOCGWINSZ and TIOCSWINSZ, as tcgetwinsize and tcsetwinsize; on a terminal, other requests
// fail with EINVAL.
int ioctl(int descriptor, unsigned long request, ...);

__END_DECLS

#endif
