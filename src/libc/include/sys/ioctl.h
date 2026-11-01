#ifndef FEATHEROS_LIBC_SYS_IOCTL_H
#define FEATHEROS_LIBC_SYS_IOCTL_H

#include <sys/cdefs.h>

// The pixel sizes are reported as 0 and ignored when set.
#define TIOCGWINSZ 1
#define TIOCSWINSZ 2

struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

__BEGIN_DECLS

// Only TIOCGWINSZ and TIOCSWINSZ, on a terminal or a pty master, which stands for its slave; other
// descriptors fail with ENOTTY, and other requests with EINVAL. The console's size is the text
// area below the status bar.
int ioctl(int descriptor, unsigned long request, ...);

__END_DECLS

#endif
