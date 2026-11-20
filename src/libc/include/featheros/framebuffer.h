#ifndef FEATHEROS_LIBC_FEATHEROS_FRAMEBUFFER_H
#define FEATHEROS_LIBC_FEATHEROS_FRAMEBUFFER_H

#include <stdint.h>
#include <sys/cdefs.h>

// /dev/fb0 is the framebuffer of the caller's virtual console, driven through ioctl. The open
// file is close-on-exec, and only one at a time may own a console's screen.
//   FB_GET_INFO  (struct fb_info*) works at any time: the screen size and the caller's console
//                number from 1 (0 on a pseudo-terminal or without one).
//   FB_ACQUIRE   (no argument) takes the screen: the console draws neither text nor status bar,
//                and is blank, until the file is released. EBUSY if it or this file is taken,
//                ENOTTY without a virtual console as the controlling terminal; a background
//                process gets SIGTTOU first.
//   FB_PRESENT   (const struct fb_present*) copies a rectangle of 0x00RRGGBB pixels, row after
//                row at stride pixels, to the screen at x, y, clipped to it; EINVAL for flags
//                other than 0, a stride below the width, a dimension above 16384, or when this
//                file does not own the screen; EFAULT for unreadable pixels.
//   FB_RELEASE   (no argument) gives the screen back, and the console repaints whole.
// The last close of the file, and the process's exit, release it too.
#define FB_GET_INFO 0x4600UL
#define FB_ACQUIRE 0x4601UL
#define FB_PRESENT 0x4602UL
#define FB_RELEASE 0x4603UL

__BEGIN_DECLS

struct fb_info
{
    uint32_t width;
    uint32_t height;
    uint32_t console;
    uint32_t dropped_events;
};

struct fb_present
{
    const uint32_t* pixels;
    uint32_t stride;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
};

__END_DECLS

#endif
