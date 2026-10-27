#include <catch2/catch_test_macros.hpp>

#include "kernel/elf/elf_loader.h"

#include <array>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace
{

constexpr size_t HeaderSize = 64;
constexpr size_t ProgramHeaderOffset = HeaderSize;
constexpr size_t ProgramHeaderSize = 56;
constexpr size_t SegmentFileOffset = 4096;
constexpr uint64_t SegmentVirtualAddress = 0x400000;

struct LoadTarget
{
    std::array<uint8_t, 64> memory;
    uint32_t flags = 0;
    uint32_t final_flags = 0;
    bool mapped = false;
    bool finalized = false;
    bool fail_map = false;
    bool fail_copy = false;
    bool fail_zero = false;
    bool fail_protect = false;
};

bool MapSegment(void* context, uint64_t address, uint64_t size, uint32_t flags)
{
    auto* target = static_cast<LoadTarget*>(context);
    target->mapped = true;
    target->flags = flags;
    return !target->fail_map && address == SegmentVirtualAddress &&
           size <= target->memory.size();
}

bool CopySegment(void* context, uint64_t address, const uint8_t* source, uint64_t size)
{
    auto* target = static_cast<LoadTarget*>(context);
    if (target->fail_copy || address != SegmentVirtualAddress || size > target->memory.size())
    {
        return false;
    }
    std::memcpy(target->memory.data(), source, size);
    return true;
}

bool ZeroSegment(void* context, uint64_t address, uint64_t size)
{
    auto* target = static_cast<LoadTarget*>(context);
    const uint64_t offset = address - SegmentVirtualAddress;
    if (target->fail_zero || address < SegmentVirtualAddress ||
        offset > target->memory.size() || size > target->memory.size() - offset)
    {
        return false;
    }
    std::memset(target->memory.data() + offset, 0, size);
    return true;
}

bool ProtectSegment(void* context, uint64_t address, uint64_t size, uint32_t flags)
{
    auto* target = static_cast<LoadTarget*>(context);
    target->finalized = true;
    target->final_flags = flags;
    return !target->fail_protect && address == SegmentVirtualAddress &&
           size <= target->memory.size();
}

FeatherOS::ELF::LoadOperations OperationsFor(LoadTarget* target)
{
    return {target, MapSegment, CopySegment, ZeroSegment, ProtectSegment};
}

void Write16(std::vector<uint8_t>& image, size_t offset, uint16_t value)
{
    image[offset] = static_cast<uint8_t>(value);
    image[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void Write32(std::vector<uint8_t>& image, size_t offset, uint32_t value)
{
    for (uint32_t index = 0; index < 4; ++index)
    {
        image[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

void Write64(std::vector<uint8_t>& image, size_t offset, uint64_t value)
{
    for (uint32_t index = 0; index < 8; ++index)
    {
        image[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

std::vector<uint8_t> MakeExecutable(uint16_t machine)
{
    std::vector<uint8_t> image(SegmentFileOffset + 4, 0);
    image[0] = 0x7F;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2;
    image[5] = 1;
    image[6] = 1;
    Write16(image, 16, 2);
    Write16(image, 18, machine);
    Write32(image, 20, 1);
    Write64(image, 24, SegmentVirtualAddress);
    Write64(image, 32, ProgramHeaderOffset);
    Write16(image, 52, HeaderSize);
    Write16(image, 54, ProgramHeaderSize);
    Write16(image, 56, 1);

    Write32(image, ProgramHeaderOffset, 1);
    Write32(image, ProgramHeaderOffset + 4, 5);
    Write64(image, ProgramHeaderOffset + 8, SegmentFileOffset);
    Write64(image, ProgramHeaderOffset + 16, SegmentVirtualAddress);
    Write64(image, ProgramHeaderOffset + 24, SegmentVirtualAddress);
    Write64(image, ProgramHeaderOffset + 32, 4);
    Write64(image, ProgramHeaderOffset + 40, 16);
    Write64(image, ProgramHeaderOffset + 48, 4096);
    image[SegmentFileOffset] = 0xAA;
    image[SegmentFileOffset + 1] = 0xBB;
    image[SegmentFileOffset + 2] = 0xCC;
    image[SegmentFileOffset + 3] = 0xDD;
    return image;
}

struct PieTarget
{
    std::map<uint64_t, uint8_t> memory;
    std::vector<std::pair<uint64_t, uint32_t>> protections;
};

bool MapPie(void*, uint64_t, uint64_t, uint32_t)
{
    return true;
}

bool CopyPie(void* context, uint64_t address, const uint8_t* source, uint64_t size)
{
    auto* target = static_cast<PieTarget*>(context);
    for (uint64_t index = 0; index < size; ++index)
    {
        target->memory[address + index] = source[index];
    }
    return true;
}

bool ZeroPie(void* context, uint64_t address, uint64_t size)
{
    auto* target = static_cast<PieTarget*>(context);
    for (uint64_t index = 0; index < size; ++index)
    {
        target->memory[address + index] = 0;
    }
    return true;
}

bool ProtectPie(void* context, uint64_t address, uint64_t, uint32_t flags)
{
    static_cast<PieTarget*>(context)->protections.emplace_back(address, flags);
    return true;
}

uint64_t Read64(const PieTarget& target, uint64_t address)
{
    uint64_t value = 0;
    for (uint32_t index = 0; index < 8; ++index)
    {
        value |= static_cast<uint64_t>(target.memory.at(address + index)) << (index * 8);
    }
    return value;
}

constexpr uint64_t PieDataAddress = 0x1000;
constexpr uint64_t PieRelaAddress = 0x1040;
constexpr size_t PieRelaOffset = 0x2040;
constexpr size_t PieDynamicOffset = 0x2080;

// A text segment at 0, a data segment at 0x1000 holding one pointer slot, a RELATIVE
// relocation for that slot, and the dynamic table that points at it.
std::vector<uint8_t> MakePositionIndependent(uint64_t relocation_info)
{
    std::vector<uint8_t> image(0x3000, 0);
    image[0] = 0x7F;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2;
    image[5] = 1;
    image[6] = 1;
    Write16(image, 16, 3);
    Write16(image, 18, FeatherOS::ELF::MachineX86_64);
    Write32(image, 20, 1);
    Write64(image, 24, 0);
    Write64(image, 32, ProgramHeaderOffset);
    Write16(image, 52, HeaderSize);
    Write16(image, 54, ProgramHeaderSize);
    Write16(image, 56, 3);

    const struct
    {
        uint32_t type;
        uint32_t flags;
        uint64_t offset;
        uint64_t address;
        uint64_t file_size;
        uint64_t memory_size;
    } headers[] = {
        {1, FeatherOS::ELF::SegmentFlagRead | FeatherOS::ELF::SegmentFlagExecute, 0x1000, 0, 16,
         16},
        {1, FeatherOS::ELF::SegmentFlagRead | FeatherOS::ELF::SegmentFlagWrite, 0x2000,
         PieDataAddress, 0x100, 0x200},
        {2, FeatherOS::ELF::SegmentFlagRead | FeatherOS::ELF::SegmentFlagWrite,
         PieDynamicOffset, PieDataAddress + 0x80, 0x40, 0x40},
    };
    for (size_t index = 0; index < 3; ++index)
    {
        const size_t header = ProgramHeaderOffset + index * ProgramHeaderSize;
        Write32(image, header, headers[index].type);
        Write32(image, header + 4, headers[index].flags);
        Write64(image, header + 8, headers[index].offset);
        Write64(image, header + 16, headers[index].address);
        Write64(image, header + 24, headers[index].address);
        Write64(image, header + 32, headers[index].file_size);
        Write64(image, header + 40, headers[index].memory_size);
        Write64(image, header + 48, headers[index].type == 1 ? 0x1000 : 8);
    }

    Write64(image, PieRelaOffset, PieDataAddress);
    Write64(image, PieRelaOffset + 8, relocation_info);
    Write64(image, PieRelaOffset + 16, 0x10);
    const uint64_t dynamic[][2] = {{7, PieRelaAddress}, {8, 24}, {9, 24}, {0, 0}};
    for (size_t index = 0; index < 4; ++index)
    {
        Write64(image, PieDynamicOffset + index * 16, dynamic[index][0]);
        Write64(image, PieDynamicOffset + index * 16 + 8, dynamic[index][1]);
    }
    return image;
}

bool LoadPie(const std::vector<uint8_t>& bytes, uint64_t bias, PieTarget* target)
{
    FeatherOS::ELF::Image image{};
    if (!FeatherOS::ELF::Parse(bytes.data(), bytes.size(), FeatherOS::ELF::MachineX86_64,
                               &image))
    {
        return false;
    }
    const FeatherOS::ELF::LoadOperations operations = {target, MapPie, CopyPie, ZeroPie,
                                                       ProtectPie};
    return FeatherOS::ELF::LoadSegments(image, bias, operations);
}

} // namespace

TEST_CASE("ELF64 loader relocates a position-independent executable at its load bias")
{
    const std::vector<uint8_t> bytes = MakePositionIndependent(8);
    FeatherOS::ELF::Image image{};
    REQUIRE(FeatherOS::ELF::Parse(bytes.data(), bytes.size(), FeatherOS::ELF::MachineX86_64,
                                  &image));
    REQUIRE(image.position_independent);

    constexpr uint64_t Bias = 0x0000100000000000;
    PieTarget target;
    REQUIRE(LoadPie(bytes, Bias, &target));
    REQUIRE(Read64(target, Bias + PieDataAddress) == Bias + 0x10);
    REQUIRE(target.protections.size() == 2);
    REQUIRE(target.protections[0].first == Bias);
    REQUIRE(target.protections[1].first == Bias + PieDataAddress);
    REQUIRE(target.protections[1].second ==
            (FeatherOS::ELF::SegmentFlagRead | FeatherOS::ELF::SegmentFlagWrite));
}

TEST_CASE("ELF64 loader rejects relocations a static PIE must not need")
{
    constexpr uint64_t Bias = 0x0000100000000000;
    PieTarget target;
    // R_X86_64_64 needs a symbol table.
    REQUIRE_FALSE(LoadPie(MakePositionIndependent(1), Bias, &target));
    REQUIRE_FALSE(LoadPie(MakePositionIndependent((1ULL << 32) | 8), Bias, &target));

    auto into_text = MakePositionIndependent(8);
    Write64(into_text, PieRelaOffset, 0);
    REQUIRE_FALSE(LoadPie(into_text, Bias, &target));

    auto with_plt = MakePositionIndependent(8);
    Write64(with_plt, PieDynamicOffset + 2 * 16, 23);
    REQUIRE_FALSE(LoadPie(with_plt, Bias, &target));

    auto table_outside_file = MakePositionIndependent(8);
    Write64(table_outside_file, PieDynamicOffset + 8, 0x5000);
    REQUIRE_FALSE(LoadPie(table_outside_file, Bias, &target));
}

TEST_CASE("ELF64 loader keeps fixed-address executables at their link address")
{
    const std::vector<uint8_t> bytes = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    FeatherOS::ELF::Image image{};
    REQUIRE(FeatherOS::ELF::Parse(bytes.data(), bytes.size(), FeatherOS::ELF::MachineX86_64,
                                  &image));
    REQUIRE_FALSE(image.position_independent);
    LoadTarget target;
    REQUIRE_FALSE(FeatherOS::ELF::LoadSegments(image, 0x1000, OperationsFor(&target)));
}

TEST_CASE("ELF64 parser accepts matching x86-64 and AArch64 executables")
{
    for (const uint16_t machine : {FeatherOS::ELF::MachineX86_64, FeatherOS::ELF::MachineAArch64})
    {
        const std::vector<uint8_t> bytes = MakeExecutable(machine);
        FeatherOS::ELF::Image image{};
        REQUIRE(FeatherOS::ELF::Parse(bytes.data(), bytes.size(), machine, &image));
        REQUIRE(image.entry_point == SegmentVirtualAddress);
        REQUIRE(image.load_segment_count == 1);

        FeatherOS::ELF::LoadSegment segment{};
        REQUIRE(FeatherOS::ELF::GetLoadSegment(image, 0, &segment));
        REQUIRE(segment.file_data == bytes.data() + SegmentFileOffset);
        REQUIRE(segment.file_size == 4);
        REQUIRE(segment.memory_size == 16);
        REQUIRE(segment.alignment == 4096);
        REQUIRE(segment.flags == 5);
        REQUIRE(segment.file_data[0] == 0xAA);
        REQUIRE_FALSE(FeatherOS::ELF::GetLoadSegment(image, 1, &segment));
    }
}

TEST_CASE("ELF64 parser rejects invalid identity and target machine")
{
    auto image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    FeatherOS::ELF::Image parsed{};
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size() - 1,
                                        FeatherOS::ELF::MachineX86_64, &parsed));

    image[0] = 0;
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineX86_64, &parsed));
    image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    image[4] = 1;
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineX86_64, &parsed));
    image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    image[5] = 2;
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineX86_64, &parsed));
    image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineAArch64, &parsed));
}

TEST_CASE("ELF64 parser rejects invalid program tables and load segments")
{
    FeatherOS::ELF::Image parsed{};

    auto image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    Write64(image, 32, image.size() - 1);
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineX86_64, &parsed));

    image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    Write64(image, ProgramHeaderOffset + 32, 20);
    Write64(image, ProgramHeaderOffset + 40, 16);
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineX86_64, &parsed));

    image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    Write64(image, ProgramHeaderOffset + 8, UINT64_MAX - 1);
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineX86_64, &parsed));

    image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    Write64(image, ProgramHeaderOffset + 48, 3);
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineX86_64, &parsed));
}

TEST_CASE("ELF64 entry point must lie inside an executable load segment")
{
    auto image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    Write64(image, 24, SegmentVirtualAddress + 16);
    FeatherOS::ELF::Image parsed{};
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineX86_64, &parsed));

    image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    Write32(image, ProgramHeaderOffset + 4, 4);
    REQUIRE_FALSE(FeatherOS::ELF::Parse(image.data(), image.size(),
                                        FeatherOS::ELF::MachineX86_64, &parsed));
}

TEST_CASE("ELF64 segment loader maps, copies file bytes, and zeroes BSS")
{
    const std::vector<uint8_t> bytes = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    FeatherOS::ELF::Image image{};
    REQUIRE(FeatherOS::ELF::Parse(bytes.data(), bytes.size(), FeatherOS::ELF::MachineX86_64,
                                  &image));

    LoadTarget target;
    target.memory.fill(0xCC);
    REQUIRE(FeatherOS::ELF::LoadSegments(image, 0, OperationsFor(&target)));
    REQUIRE(target.mapped);
    REQUIRE(target.flags == (FeatherOS::ELF::SegmentFlagRead | FeatherOS::ELF::SegmentFlagWrite |
                             FeatherOS::ELF::SegmentFlagExecute));
    REQUIRE(target.finalized);
    REQUIRE(target.final_flags ==
            (FeatherOS::ELF::SegmentFlagRead | FeatherOS::ELF::SegmentFlagExecute));
    REQUIRE(target.memory[0] == 0xAA);
    REQUIRE(target.memory[1] == 0xBB);
    REQUIRE(target.memory[2] == 0xCC);
    REQUIRE(target.memory[3] == 0xDD);
    for (size_t index = 4; index < 16; ++index)
    {
        REQUIRE(target.memory[index] == 0);
    }
    REQUIRE(target.memory[16] == 0xCC);
}

TEST_CASE("ELF64 loader accepts and skips an empty unaligned load segment")
{
    auto bytes = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    const size_t second = ProgramHeaderOffset + ProgramHeaderSize;
    Write16(bytes, 56, 2);
    // GNU ld emits an empty RW segment like this when a program has no .data or .bss.
    Write32(bytes, second, 1);
    Write32(bytes, second + 4, FeatherOS::ELF::SegmentFlagRead | FeatherOS::ELF::SegmentFlagWrite);
    Write64(bytes, second + 8, 0xE8);
    Write64(bytes, second + 16, 0);
    Write64(bytes, second + 24, 0);
    Write64(bytes, second + 32, 0);
    Write64(bytes, second + 40, 0);
    Write64(bytes, second + 48, 4096);

    FeatherOS::ELF::Image image{};
    REQUIRE(FeatherOS::ELF::Parse(bytes.data(), bytes.size(), FeatherOS::ELF::MachineX86_64,
                                  &image));
    REQUIRE(image.load_segment_count == 2);

    LoadTarget target;
    REQUIRE(FeatherOS::ELF::LoadSegments(image, 0, OperationsFor(&target)));
    REQUIRE(target.memory[0] == 0xAA);
    REQUIRE(target.final_flags ==
            (FeatherOS::ELF::SegmentFlagRead | FeatherOS::ELF::SegmentFlagExecute));
}

TEST_CASE("ELF64 segment loader rejects missing or failed memory callbacks")
{
    const std::vector<uint8_t> bytes = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    FeatherOS::ELF::Image image{};
    REQUIRE(FeatherOS::ELF::Parse(bytes.data(), bytes.size(), FeatherOS::ELF::MachineX86_64,
                                  &image));

    LoadTarget target;
    target.memory.fill(0xCC);
    FeatherOS::ELF::LoadOperations missing_operations{};
    REQUIRE_FALSE(FeatherOS::ELF::LoadSegments(image, 0, missing_operations));

    target.fail_map = true;
    REQUIRE_FALSE(FeatherOS::ELF::LoadSegments(image, 0, OperationsFor(&target)));
    target.fail_map = false;
    target.fail_copy = true;
    REQUIRE_FALSE(FeatherOS::ELF::LoadSegments(image, 0, OperationsFor(&target)));
    target.fail_copy = false;
    target.fail_zero = true;
    REQUIRE_FALSE(FeatherOS::ELF::LoadSegments(image, 0, OperationsFor(&target)));
    target.fail_zero = false;
    target.fail_protect = true;
    REQUIRE_FALSE(FeatherOS::ELF::LoadSegments(image, 0, OperationsFor(&target)));
}
TEST_CASE("ELF64 parser records a TLS template inside a load segment")
{
    const auto with_tls = [](uint64_t address, uint64_t file_size, uint64_t memory_size,
                             uint64_t alignment)
    {
        std::vector<uint8_t> image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
        Write16(image, 56, 2);
        const size_t header = ProgramHeaderOffset + ProgramHeaderSize;
        Write32(image, header, 7);
        Write32(image, header + 4, 4);
        Write64(image, header + 8, SegmentFileOffset);
        Write64(image, header + 16, address);
        Write64(image, header + 32, file_size);
        Write64(image, header + 40, memory_size);
        Write64(image, header + 48, alignment);
        return image;
    };
    FeatherOS::ELF::Image parsed{};
    std::vector<uint8_t> image = MakeExecutable(FeatherOS::ELF::MachineX86_64);
    REQUIRE(FeatherOS::ELF::Parse(image.data(), image.size(), FeatherOS::ELF::MachineX86_64,
                                  &parsed));
    REQUIRE_FALSE(parsed.has_tls);
    REQUIRE(parsed.tls_alignment == 1);

    image = with_tls(SegmentVirtualAddress + 2, 2, 64, 8);
    REQUIRE(FeatherOS::ELF::Parse(image.data(), image.size(), FeatherOS::ELF::MachineX86_64,
                                  &parsed));
    REQUIRE(parsed.has_tls);
    REQUIRE(parsed.tls_virtual_address == SegmentVirtualAddress + 2);
    REQUIRE(parsed.tls_file_size == 2);
    REQUIRE(parsed.tls_memory_size == 64);
    REQUIRE(parsed.tls_alignment == 8);

    // The template must be loaded, smaller than the block, small and aligned to a power of two.
    for (const auto& bad : {with_tls(SegmentVirtualAddress + 15, 4, 8, 8),
                            with_tls(0x900000, 2, 8, 8), with_tls(SegmentVirtualAddress, 8, 4, 8),
                            with_tls(SegmentVirtualAddress, 2, 32 * 1024, 8),
                            with_tls(SegmentVirtualAddress, 2, 8, 12)})
    {
        REQUIRE_FALSE(FeatherOS::ELF::Parse(bad.data(), bad.size(), FeatherOS::ELF::MachineX86_64,
                                            &parsed));
    }
}
