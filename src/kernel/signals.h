#ifndef FEATHEROS_SIGNALS_H
#define FEATHEROS_SIGNALS_H

#include "user/syscall_abi.h"

#include <stdint.h>

// A process's signals: which are pending, which it blocks, and what each one does. Other CPUs
// may add pending signals at any time; everything else belongs to the process itself, which
// takes a signal when it is about to return to user mode.
namespace FeatherOS::Signals
{

struct State
{
    // Bit n stands for signal n.
    volatile uint64_t pending;
    uint64_t blocked;
    User::SignalAction actions[User::SignalCount];
};

enum class DecisionKind : uint8_t
{
    None,
    Terminate,
    // Run actions[signal].handler; saved_mask goes back in place when it returns.
    Handle,
};

struct Decision
{
    DecisionKind kind;
    int signal;
    User::SignalAction action;
    uint64_t saved_mask;
};

constexpr uint64_t Bit(int signal)
{
    return 1ULL << signal;
}

bool IsValid(int signal);
// A new program's state: every signal at its default action, none blocked or pending.
void Reset(State* state);
// What a child starts with, as for exec: handlers fall back to the default action, ignored
// signals stay ignored, and the blocked mask carries over.
void Inherit(State* child, const State& parent);
// Makes signal pending unless the process ignores it. Safe from any CPU.
void Send(State* state, int signal);
// Pending signals the process does not block; SIGKILL can never be blocked.
uint64_t Deliverable(const State& state);
// Takes the lowest deliverable signal, dropping ignored ones on the way. For a handler, blocks
// the signal (unless SignalNoDefer) and the action's mask until the handler returns.
Decision TakeNext(State* state);
// Returns 0 or a syscall error. old_action may be null; so may new_action, to only read.
uint64_t SetAction(State* state, int signal, const User::SignalAction* new_action,
                   User::SignalAction* old_action);
// how is a User::SignalMask* value; returns the previous mask.
uint64_t ChangeMask(State* state, uint64_t how, uint64_t set);
// Puts back the mask a handler's frame saved.
void RestoreMask(State* state, uint64_t mask);

} // namespace FeatherOS::Signals

#endif
