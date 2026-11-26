#ifndef FEATHEROS_LIBC_FEATHEROS_FRAMEBUFFER_H
#define FEATHEROS_LIBC_FEATHEROS_FRAMEBUFFER_H

#include <stdint.h>
#include <sys/cdefs.h>

// /dev/fb0 is the framebuffer of the caller's virtual console, driven through ioctl. The open
// file is close-on-exec, and only one at a time may own a console's screen.
//   FB_GET_INFO  (struct fb_info*) works at any time: the screen size and the caller's console
//                number from 1 (0 on a pseudo-terminal or without one).
//   FB_ACQUIRE   (unsigned long flags, 0 or any of FB_ACQUIRE_RAW_KEYS and
//                FB_ACQUIRE_KEEP_SCREEN) takes the screen: the console draws neither text nor
//                status bar, and is blank, until the file is released. With
//                FB_ACQUIRE_RAW_KEYS, Ctrl+C is only queued as key events and sends no SIGINT;
//                with FB_ACQUIRE_KEEP_SCREEN the screen is not blanked and keeps its pixels until
//                the first FB_PRESENT. EINVAL for another bit, EBUSY if it or this file is taken
//                (a repeated acquire by the owner returns 0 and keeps the first flags), ENOTTY
//                without a virtual console as the controlling terminal; a background process gets
//                SIGTTOU first. Alt+F1 to Alt+F4 switch consoles whatever the flags.
//   FB_PRESENT   (const struct fb_present*) copies a rectangle of 0x00RRGGBB pixels, row after
//                row at stride pixels, to the screen at x, y, clipped to it; EINVAL for flags
//                other than 0, a stride below the width, a dimension above 16384, or when this
//                file does not own the screen; EFAULT for unreadable pixels.
//   FB_RELEASE   (no argument) gives the screen back, and the console repaints whole.
// The last close of the file, and the process's exit, release it too.
//
// The file that owns a console also reads its input, as whole 8-byte struct fb_events, from the
// first event after FB_ACQUIRE, which is FB_EVENT_SHOWN or FB_EVENT_HIDDEN, on:
//   read    returns 8 * n bytes for 1 <= n <= min(length / 8, 64): at least one event, waiting
//           for it unless the file is O_NONBLOCK (open with it, or fcntl F_SETFL), when an empty
//           queue gives EAGAIN. 0 for length 0; EINVAL for a length of 1 to 7, and when the file
//           owns no console, which includes a read that was waiting when another thread released;
//           EINTR, with no event taken, for a signal; EFAULT for an unwritable buffer. A close
//           does not wake a blocked read, which keeps the file and the console until it returns.
//   poll    POLLIN while events wait; POLLERR when the file owns no console.
// Keys arrive only while the console is shown; the program is told when it is not:
//   FB_EVENT_PRESS / FB_EVENT_REPEAT / FB_EVENT_RELEASE carry the key code (KEY_*, Linux's
//   numbers) and the modifiers after the event. A key's events are one press, repeats, one
//   release; the second press the keyboard reports for a held key is a repeat. Alt+F1 to Alt+F4
//   switch consoles and are not queued, but the Alt press is.
//   FB_EVENT_HIDDEN follows a release for every key that is down, and FB_EVENT_SHOWN comes when
//   the console is shown again. Keys held then start again with a press.
//   Ctrl+C, even with ISIG off, also sends SIGINT to the console's foreground group; the key is
//   queued too. Ctrl+Z is only queued. No key reaches the shell's terminal meanwhile.
// The queue holds 64 events, two of them kept for FB_EVENT_SHOWN and FB_EVENT_HIDDEN. Events that
// do not fit are dropped, never overwritten, and counted in fb_info.dropped_events since the
// acquire: when it grows, treat every key as released. time_ms is CLOCK_MONOTONIC in
// milliseconds modulo 2^32.
#define FB_GET_INFO 0x4600UL
#define FB_ACQUIRE 0x4601UL
#define FB_PRESENT 0x4602UL
#define FB_RELEASE 0x4603UL

#define FB_EVENT_RELEASE 0
#define FB_EVENT_PRESS 1
#define FB_EVENT_REPEAT 2
#define FB_EVENT_SHOWN 3
#define FB_EVENT_HIDDEN 4
#define FB_MOD_SHIFT 1
#define FB_MOD_CONTROL 2
#define FB_MOD_ALT 4
#define FB_MOD_CAPS_LOCK 8
#define FB_MOD_SUPER 16
#define FB_EVENT_CAPACITY 64
#define FB_ACQUIRE_RAW_KEYS 1UL
#define FB_ACQUIRE_KEEP_SCREEN 2UL

// Linux's key codes, for every key the keyboards report.
#define KEY_ESC 1
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_4 5
#define KEY_5 6
#define KEY_6 7
#define KEY_7 8
#define KEY_8 9
#define KEY_9 10
#define KEY_0 11
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_BACKSPACE 14
#define KEY_TAB 15
#define KEY_Q 16
#define KEY_W 17
#define KEY_E 18
#define KEY_R 19
#define KEY_T 20
#define KEY_Y 21
#define KEY_U 22
#define KEY_I 23
#define KEY_O 24
#define KEY_P 25
#define KEY_LEFTBRACE 26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER 28
#define KEY_LEFTCTRL 29
#define KEY_A 30
#define KEY_S 31
#define KEY_D 32
#define KEY_F 33
#define KEY_G 34
#define KEY_H 35
#define KEY_J 36
#define KEY_K 37
#define KEY_L 38
#define KEY_SEMICOLON 39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE 41
#define KEY_LEFTSHIFT 42
#define KEY_BACKSLASH 43
#define KEY_Z 44
#define KEY_X 45
#define KEY_C 46
#define KEY_V 47
#define KEY_B 48
#define KEY_N 49
#define KEY_M 50
#define KEY_COMMA 51
#define KEY_DOT 52
#define KEY_SLASH 53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK 55
#define KEY_LEFTALT 56
#define KEY_SPACE 57
#define KEY_CAPSLOCK 58
#define KEY_F1 59
#define KEY_F2 60
#define KEY_F3 61
#define KEY_F4 62
#define KEY_F5 63
#define KEY_F6 64
#define KEY_F7 65
#define KEY_F8 66
#define KEY_F9 67
#define KEY_F10 68
#define KEY_NUMLOCK 69
#define KEY_SCROLLLOCK 70
#define KEY_KP7 71
#define KEY_KP8 72
#define KEY_KP9 73
#define KEY_KPMINUS 74
#define KEY_KP4 75
#define KEY_KP5 76
#define KEY_KP6 77
#define KEY_KPPLUS 78
#define KEY_KP1 79
#define KEY_KP2 80
#define KEY_KP3 81
#define KEY_KP0 82
#define KEY_KPDOT 83
#define KEY_F11 87
#define KEY_F12 88
#define KEY_KPENTER 96
#define KEY_RIGHTCTRL 97
#define KEY_RIGHTALT 100
#define KEY_HOME 102
#define KEY_UP 103
#define KEY_PAGEUP 104
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_END 107
#define KEY_DOWN 108
#define KEY_PAGEDOWN 109
#define KEY_INSERT 110
#define KEY_DELETE 111
#define KEY_LEFTMETA 125
#define KEY_RIGHTMETA 126

__BEGIN_DECLS

struct fb_info
{
    uint32_t width;
    uint32_t height;
    uint32_t console;
    uint32_t dropped_events;
};

struct fb_event
{
    uint16_t code;
    uint8_t kind;
    uint8_t modifiers;
    uint32_t time_ms;
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
