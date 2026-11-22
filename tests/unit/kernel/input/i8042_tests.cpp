#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/x86_64/i8042.h"

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace
{

namespace I8042 = FeatherOS::Arch::X86_64::I8042;

class FakeController final : public I8042::Ports
{
public:
    uint8_t Read8(uint16_t port) override
    {
        if (port == I8042::CommandPort)
        {
            ++status_reads;
            if (absent)
            {
                return 0xFF;
            }
            uint8_t status = 0;
            if (stuck_output || !output.empty())
            {
                status |= I8042::StatusOutputFull;
            }
            if (input_never_empties || input_busy_reads > 0)
            {
                status |= I8042::StatusInputFull;
                if (input_busy_reads > 0)
                {
                    --input_busy_reads;
                }
            }
            return status;
        }
        ++data_reads;
        if (stuck_output)
        {
            return 0xAA;
        }
        if (output.empty())
        {
            return 0;
        }
        const uint8_t value = output.front();
        output.pop_front();
        return value;
    }

    void Write8(uint16_t port, uint8_t value) override
    {
        writes.emplace_back(port, value);
        if (port == I8042::CommandPort && value == 0x20 && answers)
        {
            output.push_back(config);
        }
        else if (port == I8042::CommandPort && value == 0x60)
        {
            next_data_is_config = true;
        }
        else if (port == I8042::DataPort && next_data_is_config)
        {
            config = value;
            next_data_is_config = false;
        }
    }

    bool absent = false;
    bool stuck_output = false;
    bool input_never_empties = false;
    bool answers = true;
    uint32_t input_busy_reads = 0;
    uint8_t config = 0;
    std::deque<uint8_t> output;
    std::vector<std::pair<uint16_t, uint8_t>> writes;
    uint32_t status_reads = 0;
    uint32_t data_reads = 0;
    bool next_data_is_config = false;
};

} // namespace

TEST_CASE("i8042 setup turns on IRQ 1 and translation and the keyboard clock", "[i8042]")
{
    FakeController controller;
    controller.config = 0x00;
    const auto setup = I8042::Configure(controller);
    REQUIRE(setup.status == I8042::Status::Ready);
    CHECK(setup.original == 0x00);
    CHECK(setup.config == 0x41);
    CHECK(controller.config == 0x41);
}

TEST_CASE("i8042 setup clears the keyboard clock disable bit and keeps the other bits", "[i8042]")
{
    FakeController controller;
    controller.config = 0x10 | 0x20 | 0x04 | 0x02;
    const auto setup = I8042::Configure(controller);
    REQUIRE(setup.status == I8042::Status::Ready);
    CHECK(controller.config == (0x20 | 0x04 | 0x02 | 0x40 | 0x01));
    CHECK((controller.config & I8042::ConfigFirstPortClockDisabled) == 0);
    CHECK((controller.config & I8042::ConfigTranslation) != 0);
    CHECK((controller.config & I8042::ConfigFirstPortInterrupt) != 0);
}

TEST_CASE("i8042 setup leaves a correct configuration byte as it is", "[i8042]")
{
    FakeController controller;
    controller.config = 0x47;
    const auto setup = I8042::Configure(controller);
    REQUIRE(setup.status == I8042::Status::Ready);
    CHECK(controller.config == 0x47);
}

TEST_CASE("i8042 setup flushes leftover bytes before reading the configuration", "[i8042]")
{
    FakeController controller;
    controller.config = 0x44;
    controller.output = {0xAA, 0xFA, 0x1E, 0x9E, 0x2A};
    const auto setup = I8042::Configure(controller);
    REQUIRE(setup.status == I8042::Status::Ready);
    CHECK(setup.flushed == 5);
    CHECK(setup.original == 0x44);
    CHECK(controller.output.empty());
}

TEST_CASE("i8042 setup sends read, write command and byte in order", "[i8042]")
{
    FakeController controller;
    controller.config = 0x04;
    REQUIRE(I8042::Configure(controller).status == I8042::Status::Ready);
    const std::vector<std::pair<uint16_t, uint8_t>> expected = {
        {I8042::CommandPort, 0x20}, {I8042::CommandPort, 0x60}, {I8042::DataPort, 0x45}};
    CHECK(controller.writes == expected);
}

TEST_CASE("i8042 setup waits out a busy input buffer", "[i8042]")
{
    FakeController controller;
    controller.config = 0x04;
    controller.input_busy_reads = 50;
    const auto setup = I8042::Configure(controller);
    REQUIRE(setup.status == I8042::Status::Ready);
    CHECK(controller.config == 0x45);
}

TEST_CASE("i8042 setup finds no controller when the status port reads 0xFF", "[i8042]")
{
    FakeController controller;
    controller.absent = true;
    const auto setup = I8042::Configure(controller);
    CHECK(setup.status == I8042::Status::NoController);
    CHECK(controller.writes.empty());
    CHECK(controller.data_reads == 0);
    CHECK(controller.status_reads == 1);
}

TEST_CASE("i8042 flush stops after a bounded number of reads", "[i8042]")
{
    FakeController controller;
    controller.stuck_output = true;
    uint32_t flushed = 0;
    CHECK_FALSE(I8042::Flush(controller, &flushed));
    CHECK(flushed == I8042::FlushLimit);
    CHECK(controller.data_reads == I8042::FlushLimit);
}

TEST_CASE("i8042 setup gives up when the output buffer never empties", "[i8042]")
{
    FakeController controller;
    controller.stuck_output = true;
    const auto setup = I8042::Configure(controller);
    CHECK(setup.status == I8042::Status::TimedOut);
    CHECK(controller.writes.empty());
}

TEST_CASE("i8042 setup times out when the input buffer never empties", "[i8042]")
{
    FakeController controller;
    controller.input_never_empties = true;
    const auto setup = I8042::Configure(controller);
    CHECK(setup.status == I8042::Status::TimedOut);
    CHECK(controller.writes.empty());
    CHECK(controller.status_reads <= I8042::PollLimit + 2);
}

TEST_CASE("i8042 setup times out when the controller never answers the read", "[i8042]")
{
    FakeController controller;
    controller.answers = false;
    const auto setup = I8042::Configure(controller);
    CHECK(setup.status == I8042::Status::TimedOut);
    REQUIRE(controller.writes.size() == 1);
    CHECK(controller.writes[0].second == 0x20);
    CHECK(controller.status_reads <= I8042::PollLimit + 10);
}
