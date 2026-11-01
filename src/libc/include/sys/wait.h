#ifndef FEATHEROS_LIBC_SYS_WAIT_H
#define FEATHEROS_LIBC_SYS_WAIT_H

#include <sys/cdefs.h>
#include <sys/types.h>

// Linux's values. WUNTRACED also reports a child that stopped, and WCONTINUED one that a
// SIGCONT continued, each once.
#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8

// The Linux encoding. A program the kernel ended after a CPU exception reports signal 11
// (SIGSEGV).
#define WIFEXITED(status) (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0 && ((status) & 0x7F) != 0x7F)
#define WTERMSIG(status) ((status) & 0x7F)
#define WIFSTOPPED(status) (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status) WEXITSTATUS(status)
#define WIFCONTINUED(status) ((status) == 0xFFFF)

__BEGIN_DECLS

// pid -1 waits for any child posix_spawn started, 0 for one in the caller's process group, and
// -n for one in group n, by polling every 10 ms; a positive pid names one child.
pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait(int* status);

__END_DECLS

#endif
