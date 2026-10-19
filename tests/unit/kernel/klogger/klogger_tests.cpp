#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/serial.h"
#include "kernel/klogger.h"

#include <string>

namespace FeatherOS::Test
{

std::string serial_output;

} // namespace FeatherOS::Test

namespace FeatherOS::Serial
{

void Init(uint64_t)
{
    Test::serial_output.clear();
}

void Write(const char* text)
{
    if (text != nullptr)
    {
        Test::serial_output += text;
    }
}

} // namespace FeatherOS::Serial

TEST_CASE("logger prefixes levels and ignores null messages")
{
    FeatherOS::KLogger logger;
    logger.Init(0);
    logger.Info("ready");
    logger.Warning(nullptr);
    logger.Error("failure");

    REQUIRE(FeatherOS::Test::serial_output == "KERN/I: ready\nKERN/E: failure\n");
}

TEST_CASE("logger truncates long messages and terminates the line")
{
    FeatherOS::KLogger logger;
    logger.Init(0);
    const std::string message(400, 'x');
    logger.Debug(message.c_str());

    REQUIRE(FeatherOS::Test::serial_output.size() == 256);
    REQUIRE(FeatherOS::Test::serial_output.substr(0, 8) == "KERN/D: ");
    REQUIRE(FeatherOS::Test::serial_output.substr(252, 4) == "...\n");
}