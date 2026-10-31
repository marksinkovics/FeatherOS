#ifndef FEATHEROS_LIBC_TERMIOS_H
#define FEATHEROS_LIBC_TERMIOS_H

#include <sys/cdefs.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 8

// c_cc indices. Only VMIN and VTIME can be changed; the others report the fixed keys.
#define VINTR 0
#define VERASE 1
#define VEOF 2
#define VMIN 3
#define VTIME 4

struct termios
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

// Only ICRNL, ICANON, ECHO, ISIG and FEATHEROS_WAKE_ON_OUTPUT have an effect. The other flags exist
// for programs that clear them, as cfmakeraw does; the console never changes output, so OPOST is
// always reported.
#define ICRNL 0x0001
#define BRKINT 0x0002
#define IGNBRK 0x0004
#define IGNCR 0x0008
#define INLCR 0x0010
#define INPCK 0x0020
#define ISTRIP 0x0040
#define IXON 0x0080
#define PARMRK 0x0100

#define OPOST 0x0001

#define CS5 0x0000
#define CS6 0x0001
#define CS7 0x0002
#define CS8 0x0003
#define CSIZE 0x0003
#define PARENB 0x0004

#define ICANON 0x0001
#define ECHO 0x0002
#define ISIG 0x0004
#define IEXTEN 0x0008
#define ECHONL 0x0010
// Not POSIX: a non-canonical read with VMIN > 0 returns 0 bytes once output from other programs or
// the kernel reached the console and paused, with the cursor at the start of a row, so a line
// editor can draw its prompt again below it.
#define FEATHEROS_WAKE_ON_OUTPUT 0x0100

// All three apply the settings at once; console output is never buffered and TCSAFLUSH keeps
// input already typed.
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

__BEGIN_DECLS

// The console only; other descriptors fail with ENOTTY. Only the foreground group may change the
// settings (EIO otherwise), and they return to the defaults when the program that changed them
// exits.
int tcgetattr(int descriptor, struct termios* settings);
int tcsetattr(int descriptor, int when, const struct termios* settings);
void cfmakeraw(struct termios* settings);

__END_DECLS

#endif
