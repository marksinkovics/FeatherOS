#ifndef FEATHEROS_LIBC_SYS_IOCTL_H
#define FEATHEROS_LIBC_SYS_IOCTL_H

#include <sys/cdefs.h>

// The pixel sizes are reported as 0.
#define TIOCGWINSZ 1

struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

__BEGIN_DECLS

// Only TIOCGWINSZ on the console, the text area below the status bar; other descriptors fail
// with ENOTTY, and other requests with EINVAL.
int ioctl(int descriptor, unsigned long request, ...);

__END_DECLS

#endif
