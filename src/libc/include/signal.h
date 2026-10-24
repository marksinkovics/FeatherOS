#ifndef FEATHEROS_LIBC_SIGNAL_H
#define FEATHEROS_LIBC_SIGNAL_H

#include <sys/cdefs.h>
#include <sys/types.h>

// Linux's numbers. The stop signals (SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU) are left out, since
// processes cannot be stopped.
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define NSIG 32

typedef int sig_atomic_t;
// Bit n stands for signal n.
typedef unsigned long sigset_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t) - 1)

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// Linux's values. SA_RESTART is accepted, but an interrupted call still fails with EINTR.
// SA_SIGINFO is not supported.
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000

struct sigaction
{
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    // Filled in by sigaction; the kernel returns from a handler through it.
    void (*sa_restorer)(void);
};

__BEGIN_DECLS

// Handlers run when the program next returns from the kernel: after a syscall, or when a timer
// tick preempts it. A blocked syscall gives up with EINTR first. CPU faults still end the
// program with SIGSEGV without running a handler.
int sigaction(int signal, const struct sigaction* action, struct sigaction* old_action);
// Sets a handler that stays installed and runs with the signal blocked, as BSD and glibc do.
sighandler_t signal(int signal, sighandler_t handler);
// pid as POSIX: a process, 0 for the caller's group, -1 for every process but init and the
// caller, or -group. init cannot be signaled (EPERM).
int kill(pid_t pid, int signal);
int raise(int signal);
int sigprocmask(int how, const sigset_t* set, sigset_t* old_set);
int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int signal);
int sigdelset(sigset_t* set, int signal);
int sigismember(const sigset_t* set, int signal);

__END_DECLS

#endif
