#ifndef FEATHEROS_LIBC_SYS_WAIT_H
#define FEATHEROS_LIBC_SYS_WAIT_H

#include <sys/cdefs.h>
#include <sys/types.h>

#define WNOHANG 1

// The Linux encoding. With no signals yet, a program the kernel ended after a CPU exception
// reports signal 11 (SIGSEGV), and one Ctrl+C interrupted reports signal 2 (SIGINT).
#define WIFEXITED(status) (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0)
#define WTERMSIG(status) ((status) & 0x7F)

__BEGIN_DECLS

// pid -1 waits for any child posix_spawn started, by polling every 10 ms; pid must otherwise
// name a child. Process groups (0 and negative pids) are not supported.
pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait(int* status);

__END_DECLS

#endif
