#include <catch2/catch_test_macros.hpp>

#include "kernel/signals.h"

namespace
{

using FeatherOS::Signals::Bit;
using FeatherOS::Signals::DecisionKind;
using FeatherOS::User::SignalAction;

constexpr uint64_t Handler = 0x400000;
constexpr uint64_t Restorer = 0x400100;

FeatherOS::Signals::State Fresh()
{
    FeatherOS::Signals::State state;
    FeatherOS::Signals::Reset(&state);
    return state;
}

void Handle(FeatherOS::Signals::State* state, int signal, uint64_t mask = 0, uint64_t flags = 0)
{
    const SignalAction action = {Handler, mask, flags, Restorer};
    REQUIRE(FeatherOS::Signals::SetAction(state, signal, &action, nullptr) == 0);
}

} // namespace

TEST_CASE("signals with the default action end the program, lowest first")
{
    FeatherOS::Signals::State state = Fresh();
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalTerminate);
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalInterrupt);
    REQUIRE(FeatherOS::Signals::Deliverable(state) ==
            (Bit(FeatherOS::User::SignalInterrupt) | Bit(FeatherOS::User::SignalTerminate)));
    FeatherOS::Signals::Decision decision = FeatherOS::Signals::TakeNext(&state);
    REQUIRE(decision.kind == DecisionKind::Terminate);
    REQUIRE(decision.signal == FeatherOS::User::SignalInterrupt);
    decision = FeatherOS::Signals::TakeNext(&state);
    REQUIRE(decision.signal == FeatherOS::User::SignalTerminate);
    REQUIRE(FeatherOS::Signals::TakeNext(&state).kind == DecisionKind::None);
}

TEST_CASE("ignored signals never become pending")
{
    FeatherOS::Signals::State state = Fresh();
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalChild);
    REQUIRE(state.pending == 0);
    const SignalAction ignore = {FeatherOS::User::SignalIgnore, 0, 0, 0};
    REQUIRE(FeatherOS::Signals::SetAction(&state, FeatherOS::User::SignalUser1, &ignore,
                                          nullptr) == 0);
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalUser1);
    REQUIRE(state.pending == 0);
    // Ignoring a signal that is already pending drops it.
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalUser2);
    REQUIRE(FeatherOS::Signals::SetAction(&state, FeatherOS::User::SignalUser2, &ignore,
                                          nullptr) == 0);
    REQUIRE(state.pending == 0);
    // SIGKILL cannot be ignored, and signals 0 and 19-22 do not exist here.
    FeatherOS::Signals::Send(&state, 0);
    FeatherOS::Signals::Send(&state, 19);
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalCount);
    REQUIRE(state.pending == 0);
}

TEST_CASE("blocked signals wait, but SIGKILL cannot be blocked")
{
    FeatherOS::Signals::State state = Fresh();
    REQUIRE(FeatherOS::Signals::ChangeMask(&state, FeatherOS::User::SignalMaskBlock,
                                           Bit(FeatherOS::User::SignalTerminate) |
                                               Bit(FeatherOS::User::SignalKill) | 1) == 0);
    REQUIRE(state.blocked == Bit(FeatherOS::User::SignalTerminate));
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalTerminate);
    REQUIRE(FeatherOS::Signals::Deliverable(state) == 0);
    REQUIRE(FeatherOS::Signals::TakeNext(&state).kind == DecisionKind::None);
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalKill);
    REQUIRE(FeatherOS::Signals::TakeNext(&state).signal == FeatherOS::User::SignalKill);
    REQUIRE(FeatherOS::Signals::ChangeMask(&state, FeatherOS::User::SignalMaskUnblock,
                                           Bit(FeatherOS::User::SignalTerminate)) ==
            Bit(FeatherOS::User::SignalTerminate));
    REQUIRE(FeatherOS::Signals::TakeNext(&state).signal == FeatherOS::User::SignalTerminate);
    REQUIRE(FeatherOS::Signals::ChangeMask(&state, FeatherOS::User::SignalMaskSet, ~0ULL) == 0);
    REQUIRE(state.blocked == (~0ULL & ~Bit(FeatherOS::User::SignalKill) & ~1ULL));
    REQUIRE(FeatherOS::Signals::ChangeMask(&state, 7, 0) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
}

TEST_CASE("a handler runs with its signal and mask blocked, and the old mask comes back")
{
    FeatherOS::Signals::State state = Fresh();
    state.blocked = Bit(FeatherOS::User::SignalHangUp);
    Handle(&state, FeatherOS::User::SignalUser1, Bit(FeatherOS::User::SignalUser2));
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalUser1);
    const FeatherOS::Signals::Decision decision = FeatherOS::Signals::TakeNext(&state);
    REQUIRE(decision.kind == DecisionKind::Handle);
    REQUIRE(decision.action.handler == Handler);
    REQUIRE(decision.action.restorer == Restorer);
    REQUIRE(decision.saved_mask == Bit(FeatherOS::User::SignalHangUp));
    REQUIRE(state.blocked ==
            (Bit(FeatherOS::User::SignalHangUp) | Bit(FeatherOS::User::SignalUser1) |
             Bit(FeatherOS::User::SignalUser2)));
    // Another SIGUSR1 waits until the handler is done.
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalUser1);
    REQUIRE(FeatherOS::Signals::Deliverable(state) == 0);
    FeatherOS::Signals::RestoreMask(&state, decision.saved_mask);
    REQUIRE(FeatherOS::Signals::TakeNext(&state).kind == DecisionKind::Handle);
}

TEST_CASE("SignalNoDefer and SignalResetHandler change how a handler runs")
{
    FeatherOS::Signals::State state = Fresh();
    Handle(&state, FeatherOS::User::SignalUser1, 0,
           FeatherOS::User::SignalNoDefer | FeatherOS::User::SignalResetHandler);
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalUser1);
    REQUIRE(FeatherOS::Signals::TakeNext(&state).kind == DecisionKind::Handle);
    REQUIRE(state.blocked == 0);
    SignalAction now = {};
    REQUIRE(FeatherOS::Signals::SetAction(&state, FeatherOS::User::SignalUser1, nullptr, &now) ==
            0);
    REQUIRE(now.handler == FeatherOS::User::SignalDefault);
    FeatherOS::Signals::Send(&state, FeatherOS::User::SignalUser1);
    REQUIRE(FeatherOS::Signals::TakeNext(&state).kind == DecisionKind::Terminate);
}

TEST_CASE("actions that cannot work are refused")
{
    FeatherOS::Signals::State state = Fresh();
    const SignalAction handler = {Handler, 0, 0, Restorer};
    const SignalAction no_restorer = {Handler, 0, 0, 0};
    const SignalAction unknown_flag = {Handler, 0, 1U << 5, Restorer};
    REQUIRE(FeatherOS::Signals::SetAction(&state, FeatherOS::User::SignalKill, &handler,
                                          nullptr) == FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Signals::SetAction(&state, FeatherOS::User::SignalUser1, &no_restorer,
                                          nullptr) == FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Signals::SetAction(&state, FeatherOS::User::SignalUser1, &unknown_flag,
                                          nullptr) == FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Signals::SetAction(&state, 20, &handler, nullptr) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Signals::SetAction(&state, 0, nullptr, nullptr) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    SignalAction old = {};
    REQUIRE(FeatherOS::Signals::SetAction(&state, FeatherOS::User::SignalKill, nullptr, &old) == 0);
    REQUIRE(old.handler == FeatherOS::User::SignalDefault);
}

TEST_CASE("a child keeps ignored signals and the mask, but not handlers or pending signals")
{
    FeatherOS::Signals::State parent = Fresh();
    Handle(&parent, FeatherOS::User::SignalUser1);
    const SignalAction ignore = {FeatherOS::User::SignalIgnore, 0, 0, 0};
    REQUIRE(FeatherOS::Signals::SetAction(&parent, FeatherOS::User::SignalPipe, &ignore,
                                          nullptr) == 0);
    parent.blocked = Bit(FeatherOS::User::SignalTerminate);
    FeatherOS::Signals::Send(&parent, FeatherOS::User::SignalUser2);
    FeatherOS::Signals::State child = Fresh();
    FeatherOS::Signals::Inherit(&child, parent);
    REQUIRE(child.pending == 0);
    REQUIRE(child.blocked == Bit(FeatherOS::User::SignalTerminate));
    REQUIRE(child.actions[FeatherOS::User::SignalUser1].handler ==
            FeatherOS::User::SignalDefault);
    REQUIRE(child.actions[FeatherOS::User::SignalPipe].handler == FeatherOS::User::SignalIgnore);
}
