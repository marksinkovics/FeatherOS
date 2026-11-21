#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/pci.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace
{

using FeatherOS::Pci::Address;
using FeatherOS::Pci::Bar;
using FeatherOS::Pci::Function;

struct FakeFunction
{
    std::array<uint32_t, 64> registers{};
    std::array<uint32_t, 6> bar_mask{};
    std::array<uint32_t, 6> bar_fixed{};
};

struct WriteRecord
{
    Address address;
    uint16_t offset;
    uint32_t value;
};

class FakeConfig final : public FeatherOS::Pci::ConfigSpace
{
public:
    FakeFunction& Add(uint8_t bus, uint8_t device, uint8_t function, uint16_t vendor,
                      uint16_t device_id, uint32_t class_word, uint8_t header_type)
    {
        FakeFunction& fake = functions[Key(bus, device, function)];
        fake.registers[0] = vendor | (static_cast<uint32_t>(device_id) << 16);
        fake.registers[2] = class_word;
        fake.registers[3] = static_cast<uint32_t>(header_type) << 16;
        return fake;
    }

    uint32_t Read32(Address address, uint16_t offset) override
    {
        const auto found = functions.find(Key(address.bus, address.device, address.function));
        if (found == functions.end())
        {
            return 0xFFFFFFFFU;
        }
        return found->second.registers[offset / 4];
    }

    void Write32(Address address, uint16_t offset, uint32_t value) override
    {
        writes.push_back({address, offset, value});
        const auto found = functions.find(Key(address.bus, address.device, address.function));
        if (found == functions.end())
        {
            return;
        }
        FakeFunction& fake = found->second;
        const uint16_t index = offset / 4;
        if (offset == FeatherOS::Pci::CommandRegister)
        {
            fake.registers[index] = (fake.registers[index] & 0xFFFF0000U) | (value & 0xFFFFU);
        }
        else if (offset >= 0x10 && offset <= 0x24)
        {
            const uint16_t bar = static_cast<uint16_t>((offset - 0x10) / 4);
            fake.registers[index] = (value & fake.bar_mask[bar]) | fake.bar_fixed[bar];
        }
        else
        {
            fake.registers[index] = value;
        }
    }

    std::vector<WriteRecord> writes;

private:
    static uint32_t Key(uint8_t bus, uint8_t device, uint8_t function)
    {
        return (static_cast<uint32_t>(bus) << 16) | (static_cast<uint32_t>(device) << 8) | function;
    }

    std::map<uint32_t, FakeFunction> functions;
};

struct Collected
{
    std::vector<Function> functions;
    size_t stop_after = SIZE_MAX;
};

bool Collect(const Function& function, void* context)
{
    auto* collected = static_cast<Collected*>(context);
    collected->functions.push_back(function);
    return collected->functions.size() < collected->stop_after;
}

Function FunctionAt(FakeConfig& config, uint8_t bus, uint8_t device, uint8_t function)
{
    Collected collected;
    FeatherOS::Pci::Enumerate(config, Collect, &collected);
    for (const Function& entry : collected.functions)
    {
        if (entry.address.bus == bus && entry.address.device == device &&
            entry.address.function == function)
        {
            return entry;
        }
    }
    FAIL("function not enumerated");
    return {};
}

void SetBar32(FakeFunction& fake, uint32_t index, uint32_t size, uint32_t address, bool prefetch)
{
    fake.bar_mask[index] = ~(size - 1);
    fake.bar_fixed[index] = prefetch ? 8U : 0U;
    fake.registers[4 + index] = address | fake.bar_fixed[index];
}

void SetBar64(FakeFunction& fake, uint32_t index, uint64_t size, uint64_t address, bool prefetch)
{
    const uint64_t mask = ~(size - 1);
    fake.bar_mask[index] = static_cast<uint32_t>(mask);
    fake.bar_mask[index + 1] = static_cast<uint32_t>(mask >> 32);
    fake.bar_fixed[index] = 4U | (prefetch ? 8U : 0U);
    fake.bar_fixed[index + 1] = 0;
    fake.registers[4 + index] = static_cast<uint32_t>(address) | fake.bar_fixed[index];
    fake.registers[5 + index] = static_cast<uint32_t>(address >> 32);
}

void SetBarIo(FakeFunction& fake, uint32_t index, uint32_t mask, uint32_t address)
{
    fake.bar_mask[index] = mask;
    fake.bar_fixed[index] = 1;
    fake.registers[4 + index] = address | 1U;
}

} // namespace

TEST_CASE("PCI enumeration finds multifunction devices, bridges and skips absent functions")
{
    FakeConfig config;
    config.Add(0, 0, 0, 0x8086, 0x1237, 0x06000000, 0x00);
    // Function 1 is absent while 0 and 2 exist: a multifunction device may be sparse.
    config.Add(0, 1, 0, 0x1B36, 0x000D, 0x0C033000, 0x80);
    config.Add(0, 1, 2, 0x1B36, 0x000D, 0x0C033000, 0x00);
    // Functions of a single-function device are never probed.
    config.Add(0, 2, 3, 0xDEAD, 0xBEEF, 0, 0x00);
    FakeFunction& bridge = config.Add(0, 3, 0, 0x1B36, 0x000E, 0x06040000, 0x01);
    bridge.registers[6] = 0x00020200;
    config.Add(2, 4, 0, 0x1AF4, 0x1001, 0x01000000, 0x00);
    config.Add(1, 5, 0, 0x1111, 0x2222, 0, 0x00);

    Collected collected;
    FeatherOS::Pci::Enumerate(config, Collect, &collected);

    REQUIRE(collected.functions.size() == 5);
    CHECK(collected.functions[0].vendor == 0x8086);
    CHECK(collected.functions[1].address.device == 1);
    CHECK(collected.functions[1].address.function == 0);
    CHECK(collected.functions[1].class_code == 0x0C);
    CHECK(collected.functions[1].subclass == 0x03);
    CHECK(collected.functions[1].prog_if == 0x30);
    CHECK(collected.functions[2].address.function == 2);
    CHECK(collected.functions[3].header_type == 0x01);
    CHECK(collected.functions[4].address.bus == 2);
    CHECK(collected.functions[4].address.device == 4);
    CHECK(collected.functions[4].device == 0x1001);
}

TEST_CASE("PCI enumeration stops when the visitor says so and ignores looping bridges")
{
    FakeConfig config;
    FakeFunction& first = config.Add(0, 0, 0, 0x1000, 1, 0x06040000, 0x01);
    first.registers[6] = 0x00000100;
    FakeFunction& second = config.Add(0, 1, 0, 0x1000, 2, 0x06040000, 0x01);
    second.registers[6] = 0x00000000;
    FakeFunction& loop = config.Add(1, 0, 0, 0x1000, 3, 0x06040000, 0x01);
    loop.registers[6] = 0x00000100;
    config.Add(0, 2, 0, 0x1000, 4, 0, 0x00);

    Collected collected;
    FeatherOS::Pci::Enumerate(config, Collect, &collected);
    CHECK(collected.functions.size() == 4);

    Collected limited;
    limited.stop_after = 2;
    FeatherOS::Pci::Enumerate(config, Collect, &limited);
    CHECK(limited.functions.size() == 2);
}

TEST_CASE("PCI BAR sizing handles 32-bit, 64-bit, prefetchable and I/O BARs and restores state")
{
    FakeConfig config;
    FakeFunction& fake = config.Add(0, 4, 0, 0x1B36, 0x000D, 0x0C033000, 0x00);
    fake.registers[1] = 0x02100007;
    SetBar32(fake, 0, 0x1000, 0xFEB00000, false);
    SetBar64(fake, 1, 0x200000, 0x80000000ULL << 4, true);
    SetBar32(fake, 3, 0x4000, 0xFE000000, true);
    SetBarIo(fake, 4, 0xFFFFFFE0, 0xC040);
    SetBarIo(fake, 5, 0x0000FFE0, 0xC080);
    const std::array<uint32_t, 64> before = fake.registers;
    const Function function = FunctionAt(config, 0, 4, 0);
    config.writes.clear();

    Bar bar;
    REQUIRE(FeatherOS::Pci::ReadBar(config, function, 0, &bar));
    CHECK(bar.size == 0x1000);
    CHECK(bar.address == 0xFEB00000);
    CHECK_FALSE(bar.is_io);
    CHECK_FALSE(bar.is_64bit);
    CHECK_FALSE(bar.prefetchable);

    REQUIRE(FeatherOS::Pci::ReadBar(config, function, 1, &bar));
    CHECK(bar.size == 0x200000);
    CHECK(bar.address == (0x80000000ULL << 4));
    CHECK(bar.is_64bit);
    CHECK(bar.prefetchable);

    // The upper half of the 64-bit BAR is not a BAR of its own.
    CHECK_FALSE(FeatherOS::Pci::ReadBar(config, function, 2, &bar));

    REQUIRE(FeatherOS::Pci::ReadBar(config, function, 3, &bar));
    CHECK(bar.size == 0x4000);
    CHECK(bar.prefetchable);

    REQUIRE(FeatherOS::Pci::ReadBar(config, function, 4, &bar));
    CHECK(bar.is_io);
    CHECK(bar.size == 0x20);
    CHECK(bar.address == 0xC040);

    REQUIRE(FeatherOS::Pci::ReadBar(config, function, 5, &bar));
    CHECK(bar.is_io);
    CHECK(bar.size == 0x20);

    CHECK_FALSE(FeatherOS::Pci::ReadBar(config, function, 6, &bar));
    CHECK(fake.registers == before);

    // Decoding is off while the BAR holds all ones.
    uint32_t command = fake.registers[1];
    for (const WriteRecord& write : config.writes)
    {
        if (write.offset == FeatherOS::Pci::CommandRegister)
        {
            command = write.value;
        }
        else if (write.value == 0xFFFFFFFFU)
        {
            CHECK((command & 3U) == 0);
        }
    }
}

TEST_CASE("PCI BAR sizing reports an unimplemented BAR as empty")
{
    FakeConfig config;
    config.Add(0, 1, 0, 0x1234, 0x5678, 0, 0x00);
    const Function function = FunctionAt(config, 0, 1, 0);
    Bar bar;
    REQUIRE(FeatherOS::Pci::ReadBar(config, function, 0, &bar));
    CHECK(bar.size == 0);
}

TEST_CASE("PCI BAR assignment writes a 64-bit address with its flags and keeps the command")
{
    FakeConfig config;
    FakeFunction& fake = config.Add(0, 1, 0, 0x1234, 0x5678, 0, 0x00);
    fake.registers[1] = 0x00100003;
    SetBar64(fake, 0, 0x10000, 0, true);
    const Function function = FunctionAt(config, 0, 1, 0);
    Bar bar;
    REQUIRE(FeatherOS::Pci::ReadBar(config, function, 0, &bar));
    REQUIRE(FeatherOS::Pci::AssignBar(config, function, 0, bar, 0x1'2000'0000ULL));
    REQUIRE(FeatherOS::Pci::ReadBar(config, function, 0, &bar));
    CHECK(bar.address == 0x1'2000'0000ULL);
    CHECK(bar.is_64bit);
    CHECK(bar.prefetchable);
    CHECK((fake.registers[1] & 0xFFFFU) == 3);
    CHECK_FALSE(FeatherOS::Pci::AssignBar(config, function, 5, bar, 0));
}

TEST_CASE("PCI capability walk finds a capability and ends on cycles and bad pointers")
{
    FakeConfig config;
    FakeFunction& fake = config.Add(0, 1, 0, 0x1234, 0x5678, 0, 0x00);
    fake.registers[1] = 0x00100000;
    fake.registers[13] = 0x40;
    fake.registers[0x40 / 4] = 0x00005009;
    fake.registers[0x50 / 4] = 0x00006011;
    fake.registers[0x60 / 4] = 0x00004005;
    fake.registers[0x70 / 4] = 0x00000000;
    const Function function = FunctionAt(config, 0, 1, 0);

    CHECK(FeatherOS::Pci::FindCapability(config, function, 0x11) == 0x50);
    CHECK(FeatherOS::Pci::FindCapability(config, function, 0x05) == 0x60);
    CHECK(FeatherOS::Pci::FindCapability(config, function, 0x09) == 0x40);

    // 0x60 points back to 0x40: the walk ends after 48 steps instead of looping.
    CHECK(FeatherOS::Pci::FindCapability(config, function, 0x10) == 0);

    fake.registers[0x60 / 4] = 0x00000005;
    CHECK(FeatherOS::Pci::FindCapability(config, function, 0x10) == 0);

    fake.registers[0x60 / 4] = 0x00003005;
    fake.registers[0x30 / 4] = 0x00006001;
    CHECK(FeatherOS::Pci::FindCapability(config, function, 0x10) == 0);

    fake.registers[1] = 0;
    CHECK(FeatherOS::Pci::FindCapability(config, function, 0x11) == 0);
}

TEST_CASE("PCI capability pointers below 0x40 or unaligned are not followed")
{
    FakeConfig config;
    FakeFunction& fake = config.Add(0, 1, 0, 0x1234, 0x5678, 0, 0x00);
    fake.registers[1] = 0x00100000;
    fake.registers[13] = 0x20;
    fake.registers[0x20 / 4] = 0x00000011;
    const Function function = FunctionAt(config, 0, 1, 0);
    CHECK(FeatherOS::Pci::FindCapability(config, function, 0x11) == 0);

    fake.registers[13] = 0x53;
    fake.registers[0x50 / 4] = 0x00000011;
    CHECK(FeatherOS::Pci::FindCapability(config, function, 0x11) == 0x50);
}

TEST_CASE("PCI EnableDevice sets memory space and bus master and optionally masks INTx")
{
    FakeConfig config;
    FakeFunction& fake = config.Add(0, 1, 0, 0x1234, 0x5678, 0, 0x00);
    fake.registers[1] = 0x00100001;
    const Function function = FunctionAt(config, 0, 1, 0);

    FeatherOS::Pci::EnableDevice(config, function, false);
    CHECK((fake.registers[1] & 0xFFFFU) == 0x0007);
    CHECK((fake.registers[1] >> 16) == 0x0010);

    FeatherOS::Pci::EnableDevice(config, function, true);
    CHECK((fake.registers[1] & 0xFFFFU) == 0x0407);
    // The status half is written as zero, which clears nothing.
    CHECK(config.writes.back().value >> 16 == 0);
}

TEST_CASE("PCI swizzle walks up the bridges to the bus-0 slot")
{
    FakeConfig config;
    FakeFunction& bridge = config.Add(0, 3, 0, 0x1B36, 0x000E, 0x06040000, 0x01);
    bridge.registers[6] = 0x00030100;
    bridge.registers[15] = 0x00000100;
    FakeFunction& inner = config.Add(1, 2, 0, 0x1B36, 0x000E, 0x06040000, 0x01);
    inner.registers[6] = 0x00030201;
    inner.registers[15] = 0x00000100;
    FakeFunction& endpoint = config.Add(2, 1, 0, 0x1B36, 0x000D, 0x0C033000, 0x00);
    endpoint.registers[15] = 0x00000200;
    FakeFunction& direct = config.Add(0, 5, 0, 0x1B36, 0x000D, 0x0C033000, 0x00);
    direct.registers[15] = 0x00000300;
    FakeFunction& none = config.Add(0, 6, 0, 0x1B36, 0x000D, 0x0C033000, 0x00);
    none.registers[15] = 0;

    Address root;
    uint8_t pin;
    REQUIRE(FeatherOS::Pci::SwizzleToRoot(config, FunctionAt(config, 0, 5, 0), &root, &pin));
    CHECK(root.device == 5);
    CHECK(pin == 3);

    // Pin B behind device 1, then device 2, ends as ((2-1+1)%4+1)=3, then ((3-1+2)%4+1)=1.
    REQUIRE(FeatherOS::Pci::SwizzleToRoot(config, FunctionAt(config, 2, 1, 0), &root, &pin));
    CHECK(root.bus == 0);
    CHECK(root.device == 3);
    CHECK(pin == 1);

    CHECK_FALSE(FeatherOS::Pci::SwizzleToRoot(config, FunctionAt(config, 0, 6, 0), &root, &pin));
}

TEST_CASE("PCI boot line format")
{
    Function function{};
    function.address = {0, 4, 0};
    function.vendor = 0x1B36;
    function.device = 0x000D;
    function.class_code = 0x0C;
    function.subclass = 0x03;
    function.prog_if = 0x30;
    char line[48];
    REQUIRE(FeatherOS::Pci::Describe(function, line, sizeof(line)));
    CHECK(std::string(line) == "PCI: 00:04.0 1b36:000d class 0c0330");

    function.address = {0xAB, 31, 7};
    REQUIRE(FeatherOS::Pci::Describe(function, line, sizeof(line)));
    CHECK(std::string(line) == "PCI: ab:1f.7 1b36:000d class 0c0330");

    char small[35];
    CHECK_FALSE(FeatherOS::Pci::Describe(function, small, sizeof(small)));
}
