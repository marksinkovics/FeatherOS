#include <catch2/catch_test_macros.hpp>

#include "user/init_config.h"

#include <string>

namespace
{

using FeatherOS::User::Init::LineKind;
using FeatherOS::User::Init::Policy;
using FeatherOS::User::Init::Service;

LineKind Parse(const std::string& line, Service* service)
{
    return FeatherOS::User::Init::ParseLine(line.data(), line.size(), service);
}

} // namespace

TEST_CASE("init reads a policy and a command from each service line")
{
    Service service{};
    REQUIRE(Parse("console shell", &service) == LineKind::Service);
    REQUIRE(service.policy == Policy::Console);
    REQUIRE(std::string(service.command) == "shell");
    REQUIRE(Parse("  once\techo hello   world \r", &service) == LineKind::Service);
    REQUIRE(service.policy == Policy::Once);
    REQUIRE(std::string(service.command) == "echo hello   world");
    REQUIRE(Parse("respawn sleep 100", &service) == LineKind::Service);
    REQUIRE(service.policy == Policy::Respawn);
    REQUIRE_FALSE(service.running);
}

TEST_CASE("init skips blank and comment lines and rejects malformed ones")
{
    Service service{};
    REQUIRE(Parse("", &service) == LineKind::Empty);
    REQUIRE(Parse("   \r", &service) == LineKind::Empty);
    REQUIRE(Parse("# console shell", &service) == LineKind::Empty);
    REQUIRE(Parse("forever shell", &service) == LineKind::Invalid);
    REQUIRE(Parse("once", &service) == LineKind::Invalid);
    REQUIRE(Parse("onceshell", &service) == LineKind::Invalid);
    REQUIRE(Parse("once " + std::string(FeatherOS::User::MaximumCommandLineLength, 'x'),
                  &service) == LineKind::Invalid);
}

TEST_CASE("init disables a service that restarts too often")
{
    Service service{};
    REQUIRE(Parse("respawn crashy", &service) == LineKind::Service);
    for (uint32_t restart = 0; restart < FeatherOS::User::Init::MaximumRestarts; ++restart)
    {
        REQUIRE(FeatherOS::User::Init::AllowRestart(&service, 1000 + restart * 100));
    }
    REQUIRE_FALSE(FeatherOS::User::Init::AllowRestart(&service, 1600));

    // Restarts spread over more than the window keep being allowed.
    Service steady{};
    REQUIRE(Parse("respawn steady", &steady) == LineKind::Service);
    for (uint64_t restart = 0; restart < 20; ++restart)
    {
        REQUIRE(FeatherOS::User::Init::AllowRestart(&steady, restart * 3000));
    }
}
