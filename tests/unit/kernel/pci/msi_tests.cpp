#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/msi.h"

#include <array>
#include <cstdint>
#include <vector>

namespace
{

using FeatherOS::Pci::Address;
using FeatherOS::Pci::Function;
using FeatherOS::Pci::MsiMessage;

constexpr uint16_t Capability = 0x40;
constexpr uint32_t StatusCapabilityList = 1U << 20;
constexpr uint32_t MsiEnable = 1U << 0;
constexpr uint32_t MsiMultipleEnable = 7U << 4;
constexpr uint32_t MsixEnable = 1U << 15;
constexpr uint32_t MsixFunctionMask = 1U << 14;

struct Write
{
    uint16_t offset;
    uint32_t value;
};

class FakeFunction final : public FeatherOS::Pci::ConfigSpace
{
public:
    FakeFunction()
    {
        registers[1] = StatusCapabilityList;
        registers[0x34 / 4] = Capability;
    }

    uint32_t Read32(Address, uint16_t offset) override
    {
        return registers[offset / 4];
    }

    void Write32(Address, uint16_t offset, uint32_t value) override
    {
        log.push_back({offset, value});
        if (offset == FeatherOS::Pci::CommandRegister)
        {
            registers[1] = (registers[1] & 0xFFFF0000U) | (value & 0xFFFFU);
        }
        else if (offset == Capability)
        {
            const bool enabling =
                (watched_enable & (value >> 16)) != 0 && enable_log_index == SIZE_MAX;
            registers[offset / 4] = (registers[offset / 4] & 0xFFFFU) | (value & 0xFFFF0000U);
            if (enabling)
            {
                enable_log_index = log.size() - 1;
                command_at_enable = registers[1] & 0xFFFFU;
                if (table != nullptr)
                {
                    table_at_enable.assign(table, table + table_words);
                }
                registers_at_enable = registers;
            }
        }
        else
        {
            registers[offset / 4] = value;
        }
    }

    void SetMsi(bool wide, bool per_vector_mask, uint32_t initial_control = 0)
    {
        uint32_t control = (wide ? 1U << 7 : 0U) | (per_vector_mask ? 1U << 8 : 0U) |
                           (1U << 1) /* two vectors offered */ | initial_control;
        registers[Capability / 4] = FeatherOS::Pci::CapabilityMsi | (control << 16);
        watched_enable = MsiEnable;
    }

    void SetMsix(uint32_t table_size_minus_one)
    {
        registers[Capability / 4] = FeatherOS::Pci::CapabilityMsix | (table_size_minus_one << 16);
        watched_enable = MsixEnable;
    }

    void Watch(const uint32_t* words, size_t count)
    {
        table = words;
        table_words = count;
    }

    std::array<uint32_t, 64> registers{};
    std::array<uint32_t, 64> registers_at_enable{};
    std::vector<Write> log;
    std::vector<uint32_t> table_at_enable;
    size_t enable_log_index = SIZE_MAX;
    uint32_t command_at_enable = 0;
    uint32_t watched_enable = 0;
    const uint32_t* table = nullptr;
    size_t table_words = 0;
};

Function TestFunction()
{
    Function function{};
    function.address = {0, 3, 0};
    function.vendor = 0x1234;
    function.device = 0x11E8;
    function.header_type = 0;
    return function;
}

} // namespace

TEST_CASE("MSI-X: the entry holds address, data and an unmasked vector before the enable bit")
{
    FakeFunction config;
    config.SetMsix(3);
    std::array<uint32_t, 16> table{};
    for (size_t entry = 0; entry < 4; ++entry)
    {
        table[entry * 4 + 3] = 1;
    }
    config.Watch(table.data(), table.size());
    const MsiMessage message = {0x0000000FFEE03000ULL, 0x33};

    REQUIRE(FeatherOS::Pci::EnableMsiX(config, TestFunction(),
                                       reinterpret_cast<uintptr_t>(table.data()), 2, message));

    REQUIRE(config.enable_log_index != SIZE_MAX);
    REQUIRE(config.table_at_enable.size() == 16);
    CHECK(config.table_at_enable[8] == 0xFEE03000U);
    CHECK(config.table_at_enable[9] == 0xFU);
    CHECK(config.table_at_enable[10] == 0x33U);
    CHECK(config.table_at_enable[11] == 0);
    CHECK((config.command_at_enable & FeatherOS::Pci::CommandInterruptDisable) != 0);
    // The other entries stay masked and untouched.
    CHECK(table[3] == 1);
    CHECK(table[7] == 1);
    CHECK(table[15] == 1);
    CHECK(table[0] == 0);
    CHECK(table[4] == 0);

    const uint32_t control = config.registers[Capability / 4] >> 16;
    CHECK((control & MsixEnable) != 0);
    CHECK((control & MsixFunctionMask) == 0);
    // The first control write masks the function with the enable bit still clear.
    REQUIRE_FALSE(config.log.empty());
    CHECK(config.log.front().offset == Capability);
    CHECK(((config.log.front().value >> 16) & MsixEnable) == 0);
    CHECK(((config.log.front().value >> 16) & MsixFunctionMask) != 0);
}

TEST_CASE("MSI-X refuses an entry past the table and a function without the capability")
{
    FakeFunction config;
    config.SetMsix(3);
    std::array<uint32_t, 16> table{};
    const MsiMessage message = {0xFEE00000, 0x40};
    CHECK_FALSE(FeatherOS::Pci::EnableMsiX(config, TestFunction(),
                                           reinterpret_cast<uintptr_t>(table.data()), 4, message));
    CHECK(config.log.empty());
    for (const uint32_t word : table)
    {
        CHECK(word == 0);
    }

    FakeFunction without;
    CHECK_FALSE(FeatherOS::Pci::EnableMsiX(without, TestFunction(),
                                           reinterpret_cast<uintptr_t>(table.data()), 0, message));
    CHECK_FALSE(FeatherOS::Pci::EnableMsiX(config, TestFunction(), 0, 0, message));
    CHECK(without.log.empty());
}

TEST_CASE("MSI: address, data and mask come before the enable bit, with INTx disabled")
{
    struct Layout
    {
        bool wide;
        bool per_vector_mask;
        uint16_t data_offset;
    };
    for (const Layout layout : {Layout{false, false, 8}, Layout{true, false, 12},
                                Layout{true, true, 12}, Layout{false, true, 8}})
    {
        CAPTURE(layout.wide, layout.per_vector_mask);
        FakeFunction config;
        // A firmware that left MSI on with several vectors.
        config.SetMsi(layout.wide, layout.per_vector_mask, MsiEnable | MsiMultipleEnable);
        config.registers[(Capability + layout.data_offset + 4) / 4] = 0xFFFFFFFFU;
        const MsiMessage message = {layout.wide ? 0x0000000012345678ULL : 0x08020040ULL, 0x55};
        REQUIRE(FeatherOS::Pci::EnableMsi(config, TestFunction(), message));

        REQUIRE(config.enable_log_index != SIZE_MAX);
        CHECK(config.registers_at_enable[(Capability + 4) / 4] ==
              static_cast<uint32_t>(message.address));
        CHECK(config.registers_at_enable[(Capability + layout.data_offset) / 4] == 0x55U);
        if (layout.wide)
        {
            CHECK(config.registers_at_enable[(Capability + 8) / 4] ==
                  static_cast<uint32_t>(message.address >> 32));
        }
        if (layout.per_vector_mask)
        {
            CHECK(config.registers_at_enable[(Capability + layout.data_offset + 4) / 4] == 0);
        }
        else
        {
            CHECK(config.registers_at_enable[(Capability + layout.data_offset + 4) / 4] ==
                  0xFFFFFFFFU);
        }
        CHECK((config.command_at_enable & FeatherOS::Pci::CommandInterruptDisable) != 0);
        CHECK(config.enable_log_index == config.log.size() - 1);

        const uint32_t control = config.registers[Capability / 4] >> 16;
        CHECK((control & MsiEnable) != 0);
        CHECK((control & MsiMultipleEnable) == 0);
        // Enable was switched off before anything was programmed.
        REQUIRE_FALSE(config.log.empty());
        CHECK(config.log.front().offset == Capability);
        CHECK(((config.log.front().value >> 16) & MsiEnable) == 0);
    }
}

TEST_CASE("MSI refuses a 64-bit address on a 32-bit function and a function without the capability")
{
    FakeFunction config;
    config.SetMsi(false, false);
    CHECK_FALSE(FeatherOS::Pci::EnableMsi(config, TestFunction(), {0x100000000ULL, 1}));
    CHECK_FALSE(FeatherOS::Pci::EnableMsi(config, TestFunction(), {0xFEE00000, 0x10000}));
    CHECK(config.log.empty());

    FakeFunction without;
    CHECK_FALSE(FeatherOS::Pci::EnableMsi(without, TestFunction(), {0xFEE00000, 1}));
    CHECK(without.log.empty());
}
