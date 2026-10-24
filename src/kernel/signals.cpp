#include "signals.h"

namespace FeatherOS::Signals
{
namespace
{

// Signals 19-22 stop a process on other systems; FeatherOS cannot stop one, so they are refused.
constexpr uint64_t StopSignals = Bit(19) | Bit(20) | Bit(21) | Bit(22);
// What other systems also ignore by default: a child's exit, continue, urgent data, and a
// window size change.
constexpr uint64_t IgnoredByDefault =
    Bit(User::SignalChild) | Bit(User::SignalContinue) | Bit(23) | Bit(28);
constexpr uint64_t Unblockable = Bit(User::SignalKill);

bool Ignores(const State& state, int signal)
{
    const uint64_t handler = state.actions[signal].handler;
    return signal != User::SignalKill &&
           (handler == User::SignalIgnore ||
            (handler == User::SignalDefault && (IgnoredByDefault & Bit(signal)) != 0));
}

} // namespace

bool IsValid(int signal)
{
    return signal > 0 && signal < User::SignalCount && (StopSignals & Bit(signal)) == 0;
}

void Reset(State* state)
{
    state->pending = 0;
    state->blocked = 0;
    for (User::SignalAction& action : state->actions)
    {
        action = {User::SignalDefault, 0, 0, 0};
    }
}

void Inherit(State* child, const State& parent)
{
    Reset(child);
    child->blocked = parent.blocked;
    for (int signal = 1; signal < User::SignalCount; ++signal)
    {
        if (parent.actions[signal].handler == User::SignalIgnore)
        {
            child->actions[signal].handler = User::SignalIgnore;
        }
    }
}

void Send(State* state, int signal)
{
    if (!IsValid(signal) || Ignores(*state, signal))
    {
        return;
    }
    __atomic_fetch_or(&state->pending, Bit(signal), __ATOMIC_SEQ_CST);
}

uint64_t Deliverable(const State& state)
{
    const uint64_t pending = __atomic_load_n(&state.pending, __ATOMIC_SEQ_CST);
    return pending & (~state.blocked | Unblockable);
}

Decision TakeNext(State* state)
{
    for (;;)
    {
        const uint64_t deliverable = Deliverable(*state);
        if (deliverable == 0)
        {
            return {DecisionKind::None, 0, {}, 0};
        }
        const int signal = __builtin_ctzll(deliverable);
        __atomic_fetch_and(&state->pending, ~Bit(signal), __ATOMIC_SEQ_CST);
        // A signal pending since before its action changed to ignore is dropped now.
        if (Ignores(*state, signal))
        {
            continue;
        }
        const User::SignalAction action = state->actions[signal];
        if (signal == User::SignalKill || action.handler == User::SignalDefault)
        {
            return {DecisionKind::Terminate, signal, action, 0};
        }
        const Decision decision = {DecisionKind::Handle, signal, action, state->blocked};
        state->blocked |= action.mask & ~Unblockable;
        if ((action.flags & User::SignalNoDefer) == 0)
        {
            state->blocked |= Bit(signal);
        }
        if ((action.flags & User::SignalResetHandler) != 0)
        {
            state->actions[signal] = {User::SignalDefault, 0, 0, 0};
        }
        return decision;
    }
}

uint64_t SetAction(State* state, int signal, const User::SignalAction* new_action,
                   User::SignalAction* old_action)
{
    if (!IsValid(signal))
    {
        return User::SyscallErrorInvalidArgument;
    }
    constexpr uint64_t KnownFlags = User::SignalResetHandler | User::SignalNoDefer;
    if (new_action != nullptr &&
        (signal == User::SignalKill || (new_action->flags & ~KnownFlags) != 0 ||
         (new_action->handler > User::SignalIgnore && new_action->restorer == 0)))
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (old_action != nullptr)
    {
        *old_action = state->actions[signal];
    }
    if (new_action != nullptr)
    {
        state->actions[signal] = *new_action;
        if (Ignores(*state, signal))
        {
            __atomic_fetch_and(&state->pending, ~Bit(signal), __ATOMIC_SEQ_CST);
        }
    }
    return 0;
}

uint64_t ChangeMask(State* state, uint64_t how, uint64_t set)
{
    const uint64_t previous = state->blocked;
    if (how == User::SignalMaskBlock)
    {
        state->blocked |= set;
    }
    else if (how == User::SignalMaskUnblock)
    {
        state->blocked &= ~set;
    }
    else if (how == User::SignalMaskSet)
    {
        state->blocked = set;
    }
    else
    {
        return User::SyscallErrorInvalidArgument;
    }
    state->blocked &= ~(Unblockable | 1);
    return previous;
}

void RestoreMask(State* state, uint64_t mask)
{
    state->blocked = mask & ~(Unblockable | 1);
}

} // namespace FeatherOS::Signals
