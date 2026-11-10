#include <catch2/catch_test_macros.hpp>

#include "kernel/kernel_test_mode.h"

using FeatherOS::KernelTest::Mode;
using FeatherOS::KernelTest::ParseCommandLine;

TEST_CASE("Kernel test mode comes from the command line")
{
    Mode mode = Mode::Panic;
    REQUIRE(ParseCommandLine(nullptr, &mode));
    REQUIRE(mode == Mode::None);
    REQUIRE(ParseCommandLine("", &mode));
    REQUIRE(mode == Mode::None);
    REQUIRE(ParseCommandLine("test=memory", &mode));
    REQUIRE(mode == Mode::Memory);
    REQUIRE(ParseCommandLine("quiet  test=user-preemption verbose", &mode));
    REQUIRE(mode == Mode::UserPreemption);
    REQUIRE(ParseCommandLine("test=input ", &mode));
    REQUIRE(mode == Mode::Input);
    // Other options, and words that only start like one, are not modes.
    REQUIRE(ParseCommandLine("attest=smp testing=smp", &mode));
    REQUIRE(mode == Mode::None);
}

TEST_CASE("Kernel test mode rejects unknown names")
{
    Mode mode = Mode::None;
    REQUIRE_FALSE(ParseCommandLine("test=", &mode));
    REQUIRE_FALSE(ParseCommandLine("test=mem", &mode));
    REQUIRE_FALSE(ParseCommandLine("test=memoryx", &mode));
    REQUIRE_FALSE(ParseCommandLine("test=memory test=nothing", &mode));
}

TEST_CASE("Kernel test mode selection")
{
    FeatherOS::KernelTest::SelectMode(Mode::Network);
    REQUIRE(FeatherOS::KernelTest::Selected(Mode::Network));
    REQUIRE_FALSE(FeatherOS::KernelTest::Selected(Mode::Smp));
    FeatherOS::KernelTest::SelectMode(Mode::None);
    REQUIRE(FeatherOS::KernelTest::SelectedMode() == Mode::None);
}
