#ifndef FEATHEROS_LIBC_SYS_REBOOT_H
#define FEATHEROS_LIBC_SYS_REBOOT_H

#include <sys/cdefs.h>

// Linux's values.
#define RB_AUTOBOOT 0x01234567
#define RB_POWER_OFF 0x4321FEDC

__BEGIN_DECLS

// Only init and session leaders may call it, and the kernel stops every process and flushes the
// disk first. Returns -1 with EPERM if refused; otherwise it does not return.
int reboot(int command);

__END_DECLS

#endif
