#include <catch2/catch_test_macros.hpp>

#include "kernel/signals.h"

// R3-03: the x86-64 EnterHandler (arch/x86_64/exceptions.cpp:402) is not host-built, so the
// reachable layer is the sigaction path that feeds it. A non-canonical handler must be refused
// there or in EnterHandler; this test pins the sigaction side, where the rejection is
// observable on the host. It fails today because SetAction accepts any handler above SignalIgnore.
TEST_CASE("R3-03 sigaction accepts a non-canonical handler address", "[.repro][R3-03]")
{
    constexpr uint64_t NonCanonical = 0x0000800000000000;
    FeatherOS::Signals::State state;
    FeatherOS::Signals::Reset(&state);
    const FeatherOS::User::SignalAction action = {NonCanonical, 0, 0, 0x400100};
    REQUIRE(FeatherOS::Signals::SetAction(&state, FeatherOS::User::SignalTerminate, &action,
                                          nullptr) != 0);
}
