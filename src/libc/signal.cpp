#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

extern "C" void __featheros_signal_return();

using FeatherOS::LibC::Check;

namespace
{

// The restorer stubs in arch/<arch>/signal_return.cpp make this syscall by number.
static_assert(static_cast<uint64_t>(FeatherOS::User::SyscallNumber::SignalReturn) == 34,
              "signal_return.cpp uses the SignalReturn syscall number");

constexpr int KnownFlags = SA_RESTART | SA_NODEFER | SA_RESETHAND;

bool ValidSignal(int signal)
{
    return signal > 0 && signal < NSIG;
}

FeatherOS::User::SignalAction ToKernel(const struct sigaction& action)
{
    const auto handler = reinterpret_cast<uint64_t>(action.sa_handler);
    uint64_t flags = 0;
    flags |= (action.sa_flags & SA_RESETHAND) != 0 ? FeatherOS::User::SignalResetHandler : 0;
    flags |= (action.sa_flags & SA_NODEFER) != 0 ? FeatherOS::User::SignalNoDefer : 0;
    return {handler, action.sa_mask, flags,
            handler > FeatherOS::User::SignalIgnore
                ? reinterpret_cast<uint64_t>(&__featheros_signal_return)
                : 0};
}

struct sigaction FromKernel(const FeatherOS::User::SignalAction& action)
{
    struct sigaction result = {};
    result.sa_handler = reinterpret_cast<sighandler_t>(action.handler);
    result.sa_mask = action.mask;
    result.sa_flags =
        ((action.flags & FeatherOS::User::SignalResetHandler) != 0 ? SA_RESETHAND : 0) |
        ((action.flags & FeatherOS::User::SignalNoDefer) != 0 ? SA_NODEFER : 0);
    result.sa_restorer = reinterpret_cast<void (*)()>(action.restorer);
    return result;
}

} // namespace

int sigaction(int signal, const struct sigaction* action, struct sigaction* old_action)
{
    if (!ValidSignal(signal) || (action != nullptr && (action->sa_flags & ~KnownFlags) != 0))
    {
        errno = EINVAL;
        return -1;
    }
    FeatherOS::User::SignalAction requested = {};
    FeatherOS::User::SignalAction previous = {};
    if (action != nullptr)
    {
        requested = ToKernel(*action);
    }
    const int result = Check<int>(FeatherOS::User::Syscall(
        FeatherOS::User::SyscallNumber::SignalAction, static_cast<uint64_t>(signal),
        action != nullptr ? reinterpret_cast<uint64_t>(&requested) : 0,
        reinterpret_cast<uint64_t>(&previous)));
    if (result == 0 && old_action != nullptr)
    {
        *old_action = FromKernel(previous);
    }
    return result;
}

sighandler_t signal(int signal, sighandler_t handler)
{
    struct sigaction action = {};
    struct sigaction previous = {};
    action.sa_handler = handler;
    return sigaction(signal, &action, &previous) == 0 ? previous.sa_handler : SIG_ERR;
}

int kill(pid_t pid, int signal)
{
    if (signal < 0 || signal >= NSIG)
    {
        errno = EINVAL;
        return -1;
    }
    const uint64_t result = FeatherOS::User::Syscall(
        FeatherOS::User::SyscallNumber::Kill, static_cast<uint64_t>(static_cast<int64_t>(pid)),
        static_cast<uint64_t>(signal));
    if (result == FeatherOS::User::SyscallErrorNotFound)
    {
        errno = ESRCH;
        return -1;
    }
    return Check<int>(result);
}

// A deliverable signal runs its handler as the Kill syscall returns, so before raise does.
int raise(int signal)
{
    return kill(getpid(), signal);
}

int sigprocmask(int how, const sigset_t* set, sigset_t* old_set)
{
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK)
    {
        errno = EINVAL;
        return -1;
    }
    // Without a set, blocking nothing only reads the mask.
    const uint64_t operation = set == nullptr       ? FeatherOS::User::SignalMaskBlock
                               : how == SIG_BLOCK   ? FeatherOS::User::SignalMaskBlock
                               : how == SIG_UNBLOCK ? FeatherOS::User::SignalMaskUnblock
                                                    : FeatherOS::User::SignalMaskSet;
    const uint64_t previous = FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::SignalMask,
                                                       operation, set != nullptr ? *set : 0);
    if (FeatherOS::User::IsSyscallError(previous))
    {
        return Check<int>(previous);
    }
    if (old_set != nullptr)
    {
        *old_set = previous;
    }
    return 0;
}

int sigemptyset(sigset_t* set)
{
    *set = 0;
    return 0;
}

int sigfillset(sigset_t* set)
{
    *set = ~0UL & ~1UL;
    return 0;
}

int sigaddset(sigset_t* set, int signal)
{
    if (!ValidSignal(signal))
    {
        errno = EINVAL;
        return -1;
    }
    *set |= 1UL << signal;
    return 0;
}

int sigdelset(sigset_t* set, int signal)
{
    if (!ValidSignal(signal))
    {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1UL << signal);
    return 0;
}

int sigismember(const sigset_t* set, int signal)
{
    if (!ValidSignal(signal))
    {
        errno = EINVAL;
        return -1;
    }
    return (*set & (1UL << signal)) != 0 ? 1 : 0;
}

int pause()
{
    return Check<int>(FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::Pause));
}
