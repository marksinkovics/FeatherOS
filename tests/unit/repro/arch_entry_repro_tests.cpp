#include <catch2/catch_test_macros.hpp>

#include "kernel/signals.h"

// R3-03: the architecture's EnterHandler is not host-built, so the reachable layer is the
// sigaction path that feeds it, which refuses a handler outside user space.
TEST_CASE("R3-03 sigaction refuses a non-canonical handler address", "[R3-03]")
{
    constexpr uint64_t NonCanonical = 0x0000800000000000;
    FeatherOS::Signals::State state;
    FeatherOS::Signals::Reset(&state);
    const FeatherOS::User::SignalAction action = {NonCanonical, 0, 0, 0x400100};
    REQUIRE(FeatherOS::Signals::SetAction(&state, FeatherOS::User::SignalTerminate, &action,
                                          nullptr) != 0);
}
