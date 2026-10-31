#include "elf_loader.h"

namespace FeatherOS::ELF
{
namespace
{

constexpr uint64_t ElfHeaderSize = 64;
constexpr uint64_t ProgramHeaderSize = 56;
constexpr uint32_t ProgramLoad = 1;
constexpr uint32_t ProgramDynamic = 2;
constexpr uint32_t ProgramThreadLocal = 7;
constexpr uint32_t SegmentExecutable = 1;
constexpr uint16_t TypeExecutable = 2;
constexpr uint16_t TypeSharedObject = 3;
constexpr uint64_t DynamicNull = 0;
constexpr uint64_t DynamicRela = 7;
constexpr uint64_t DynamicRelaSize = 8;
constexpr uint64_t DynamicRelaEntrySize = 9;
constexpr uint64_t DynamicRel = 17;
constexpr uint64_t DynamicJumpRelocations = 23;
constexpr uint64_t DynamicEntrySize = 16;
constexpr uint64_t RelaEntrySize = 24;
constexpr uint32_t RelocationX86_64Relative = 8;
constexpr uint32_t RelocationAArch64Relative = 1027;

uint16_t ReadLittleEndian16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t ReadLittleEndian32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t ReadLittleEndian64(const uint8_t* bytes)
{
    return static_cast<uint64_t>(ReadLittleEndian32(bytes)) |
           (static_cast<uint64_t>(ReadLittleEndian32(bytes + 4)) << 32);
}

bool ReadProgramHeader(const Image& image, uint16_t index, LoadSegment* segment, uint32_t* type)
{
    if (index >= image.program_header_count || segment == nullptr || type == nullptr)
    {
        return false;
    }
    const uint64_t offset =
        image.program_header_offset + static_cast<uint64_t>(index) * ProgramHeaderSize;
    const uint8_t* header = image.data + offset;
    *type = ReadLittleEndian32(header);
    segment->flags = ReadLittleEndian32(header + 4);
    segment->file_offset = ReadLittleEndian64(header + 8);
    segment->virtual_address = ReadLittleEndian64(header + 16);
    segment->file_size = ReadLittleEndian64(header + 32);
    segment->memory_size = ReadLittleEndian64(header + 40);
    segment->alignment = ReadLittleEndian64(header + 48);
    segment->file_data = nullptr;
    return true;
}

bool IsValidLoadSegment(const Image& image, uint16_t index, LoadSegment* segment)
{
    uint32_t type;
    if (!ReadProgramHeader(image, index, segment, &type) || type != ProgramLoad ||
        segment->file_size > segment->memory_size ||
        ((segment->flags & SegmentFlagWrite) != 0 && (segment->flags & SegmentFlagExecute) != 0))
    {
        return false;
    }

    const uint64_t offset = segment->file_offset;
    if (offset > image.size || segment->file_size > image.size - offset ||
        segment->memory_size > UINT64_MAX - segment->virtual_address)
    {
        return false;
    }

    // An empty segment maps nothing, so linkers may emit it with an arbitrary file offset.
    if (segment->alignment > 1 && segment->memory_size > 0)
    {
        if ((segment->alignment & (segment->alignment - 1)) != 0 ||
            segment->virtual_address % segment->alignment != offset % segment->alignment)
        {
            return false;
        }
    }
    segment->file_data = image.data + offset;
    return true;
}

const uint8_t* FileBytesAt(const Image& image, uint64_t address, uint64_t size)
{
    for (uint16_t index = 0; index < image.load_segment_count; ++index)
    {
        LoadSegment segment;
        if (!GetLoadSegment(image, index, &segment))
        {
            return nullptr;
        }
        if (address >= segment.virtual_address &&
            address - segment.virtual_address <= segment.file_size &&
            size <= segment.file_size - (address - segment.virtual_address))
        {
            return segment.file_data + (address - segment.virtual_address);
        }
    }
    return nullptr;
}

bool InWritableSegment(const Image& image, uint64_t address, uint64_t size)
{
    for (uint16_t index = 0; index < image.load_segment_count; ++index)
    {
        LoadSegment segment;
        if (!GetLoadSegment(image, index, &segment))
        {
            return false;
        }
        if ((segment.flags & SegmentFlagWrite) != 0 && address >= segment.virtual_address &&
            address - segment.virtual_address <= segment.memory_size &&
            size <= segment.memory_size - (address - segment.virtual_address))
        {
            return true;
        }
    }
    return false;
}

// A static PIE carries only RELATIVE relocations: each adds the load bias to an addend and
// stores the result in a writable segment. Anything that needs symbols or a PLT is rejected.
bool ApplyRelocations(const Image& image, uint64_t load_bias, const LoadOperations& operations)
{
    if (!image.position_independent || image.dynamic_size == 0)
    {
        return true;
    }
    uint64_t rela_address = 0;
    uint64_t rela_size = 0;
    uint64_t rela_entry_size = RelaEntrySize;
    bool has_rela = false;
    for (uint64_t offset = 0; offset + DynamicEntrySize <= image.dynamic_size;
         offset += DynamicEntrySize)
    {
        const uint8_t* entry = image.data + image.dynamic_offset + offset;
        const uint64_t tag = ReadLittleEndian64(entry);
        const uint64_t value = ReadLittleEndian64(entry + 8);
        if (tag == DynamicNull)
        {
            break;
        }
        if (tag == DynamicRel || tag == DynamicJumpRelocations)
        {
            return false;
        }
        if (tag == DynamicRela)
        {
            rela_address = value;
            has_rela = true;
        }
        else if (tag == DynamicRelaSize)
        {
            rela_size = value;
        }
        else if (tag == DynamicRelaEntrySize)
        {
            rela_entry_size = value;
        }
    }
    if (!has_rela || rela_size == 0)
    {
        return rela_size == 0;
    }
    if (rela_entry_size != RelaEntrySize || rela_size % RelaEntrySize != 0)
    {
        return false;
    }
    const uint8_t* table = FileBytesAt(image, rela_address, rela_size);
    if (table == nullptr)
    {
        return false;
    }

    const uint32_t relative_type =
        image.machine == MachineX86_64 ? RelocationX86_64Relative : RelocationAArch64Relative;
    for (uint64_t offset = 0; offset < rela_size; offset += RelaEntrySize)
    {
        const uint64_t target = ReadLittleEndian64(table + offset);
        const uint64_t info = ReadLittleEndian64(table + offset + 8);
        const uint64_t addend = ReadLittleEndian64(table + offset + 16);
        if ((info >> 32) != 0 || static_cast<uint32_t>(info) != relative_type ||
            !InWritableSegment(image, target, sizeof(uint64_t)))
        {
            return false;
        }
        const uint64_t value = load_bias + addend;
        uint8_t bytes[sizeof(uint64_t)];
        for (uint32_t index = 0; index < sizeof(bytes); ++index)
        {
            bytes[index] = static_cast<uint8_t>(value >> (index * 8));
        }
        if (!operations.copy(operations.context, target + load_bias, bytes, sizeof(bytes)))
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool Parse(const void* input, uint64_t size, uint16_t expected_machine, Image* image)
{
    if (input == nullptr || image == nullptr || size < ElfHeaderSize)
    {
        return false;
    }
    const auto* data = static_cast<const uint8_t*>(input);
    const uint16_t type = ReadLittleEndian16(data + 16);
    if (data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' || data[4] != 2 ||
        data[5] != 1 || data[6] != 1 || (type != TypeExecutable && type != TypeSharedObject) ||
        ReadLittleEndian16(data + 18) != expected_machine || ReadLittleEndian32(data + 20) != 1 ||
        ReadLittleEndian16(data + 52) != ElfHeaderSize ||
        ReadLittleEndian16(data + 54) != ProgramHeaderSize)
    {
        return false;
    }

    const uint64_t program_header_offset = ReadLittleEndian64(data + 32);
    const uint16_t program_header_count = ReadLittleEndian16(data + 56);
    if (program_header_count == 0 || program_header_offset > size ||
        program_header_count > (size - program_header_offset) / ProgramHeaderSize)
    {
        return false;
    }

    Image parsed = {
        .data = data,
        .size = size,
        .entry_point = ReadLittleEndian64(data + 24),
        .program_header_offset = program_header_offset,
        .program_header_count = program_header_count,
        .load_segment_count = 0,
        .machine = expected_machine,
        .position_independent = type == TypeSharedObject,
        .dynamic_offset = 0,
        .dynamic_size = 0,
        .has_tls = false,
        .tls_virtual_address = 0,
        .tls_file_size = 0,
        .tls_memory_size = 0,
        .tls_alignment = 1,
    };
    bool entry_is_executable = false;
    for (uint16_t index = 0; index < program_header_count; ++index)
    {
        LoadSegment segment;
        uint32_t type;
        if (!ReadProgramHeader(parsed, index, &segment, &type))
        {
            return false;
        }
        if (type == ProgramDynamic)
        {
            if (segment.file_offset > size || segment.file_size > size - segment.file_offset)
            {
                return false;
            }
            parsed.dynamic_offset = segment.file_offset;
            parsed.dynamic_size = segment.file_size;
            continue;
        }
        if (type == ProgramThreadLocal)
        {
            const uint64_t alignment = segment.alignment == 0 ? 1 : segment.alignment;
            if (parsed.has_tls || segment.file_size > segment.memory_size ||
                segment.memory_size > MaximumThreadLocalSize ||
                (alignment & (alignment - 1)) != 0 || alignment > 4096)
            {
                return false;
            }
            parsed.has_tls = true;
            parsed.tls_virtual_address = segment.virtual_address;
            parsed.tls_file_size = segment.file_size;
            parsed.tls_memory_size = segment.memory_size;
            parsed.tls_alignment = alignment;
            continue;
        }
        if (type != ProgramLoad)
        {
            continue;
        }
        if (!IsValidLoadSegment(parsed, index, &segment))
        {
            return false;
        }
        ++parsed.load_segment_count;
        if ((segment.flags & SegmentExecutable) != 0 &&
            parsed.entry_point >= segment.virtual_address &&
            parsed.entry_point - segment.virtual_address < segment.memory_size)
        {
            entry_is_executable = true;
        }
    }
    if (parsed.load_segment_count == 0 || !entry_is_executable)
    {
        return false;
    }
    // New threads copy the template from the program's memory, so it must be loaded.
    bool template_loaded = !parsed.has_tls;
    for (uint16_t index = 0; index < program_header_count && !template_loaded; ++index)
    {
        LoadSegment segment;
        uint32_t type;
        template_loaded =
            ReadProgramHeader(parsed, index, &segment, &type) && type == ProgramLoad &&
            parsed.tls_virtual_address >= segment.virtual_address &&
            parsed.tls_virtual_address - segment.virtual_address <= segment.memory_size &&
            parsed.tls_file_size <=
                segment.memory_size - (parsed.tls_virtual_address - segment.virtual_address);
    }
    if (!template_loaded)
    {
        return false;
    }

    *image = parsed;
    return true;
}

bool GetLoadSegment(const Image& image, uint16_t index, LoadSegment* segment)
{
    if (segment == nullptr || index >= image.load_segment_count)
    {
        return false;
    }
    uint16_t load_index = 0;
    for (uint16_t program_index = 0; program_index < image.program_header_count; ++program_index)
    {
        LoadSegment candidate;
        uint32_t type;
        if (!ReadProgramHeader(image, program_index, &candidate, &type))
        {
            return false;
        }
        if (type != ProgramLoad)
        {
            continue;
        }
        if (!IsValidLoadSegment(image, program_index, &candidate))
        {
            return false;
        }
        if (load_index++ == index)
        {
            *segment = candidate;
            return true;
        }
    }
    return false;
}

bool LoadSegments(const Image& image, uint64_t load_bias, const LoadOperations& operations)
{
    if (operations.map == nullptr || operations.copy == nullptr || operations.zero == nullptr ||
        operations.protect == nullptr || image.load_segment_count == 0 ||
        (!image.position_independent && load_bias != 0))
    {
        return false;
    }

    for (uint16_t index = 0; index < image.load_segment_count; ++index)
    {
        LoadSegment segment;
        if (!GetLoadSegment(image, index, &segment))
        {
            return false;
        }
        if (segment.memory_size == 0)
        {
            continue;
        }
        if (segment.virtual_address > UINT64_MAX - load_bias - segment.memory_size)
        {
            return false;
        }
        const uint64_t address = segment.virtual_address + load_bias;
        if (!operations.map(operations.context, address, segment.memory_size,
                            segment.flags | SegmentFlagWrite))
        {
            return false;
        }
        if (segment.file_size > 0 &&
            !operations.copy(operations.context, address, segment.file_data, segment.file_size))
        {
            return false;
        }
        const uint64_t zero_size = segment.memory_size - segment.file_size;
        if (zero_size > 0 &&
            !operations.zero(operations.context, address + segment.file_size, zero_size))
        {
            return false;
        }
    }

    // Relocations go in while every segment is still writable; final permissions come after.
    if (!ApplyRelocations(image, load_bias, operations))
    {
        return false;
    }
    for (uint16_t index = 0; index < image.load_segment_count; ++index)
    {
        LoadSegment segment;
        if (!GetLoadSegment(image, index, &segment))
        {
            return false;
        }
        if (segment.memory_size > 0 &&
            !operations.protect(operations.context, segment.virtual_address + load_bias,
                                segment.memory_size, segment.flags))
        {
            return false;
        }
    }
    return true;
}

} // namespace FeatherOS::ELF