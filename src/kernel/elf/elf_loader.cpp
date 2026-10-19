#include "elf_loader.h"

namespace FeatherOS::ELF
{
namespace
{

constexpr uint64_t ElfHeaderSize = 64;
constexpr uint64_t ProgramHeaderSize = 56;
constexpr uint32_t ProgramLoad = 1;
constexpr uint32_t SegmentExecutable = 1;

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

} // namespace

bool Parse(const void* input, uint64_t size, uint16_t expected_machine, Image* image)
{
    if (input == nullptr || image == nullptr || size < ElfHeaderSize)
    {
        return false;
    }
    const auto* data = static_cast<const uint8_t*>(input);
    if (data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' || data[4] != 2 ||
        data[5] != 1 || data[6] != 1 || ReadLittleEndian16(data + 16) != 2 ||
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
        data, size, ReadLittleEndian64(data + 24), program_header_offset, program_header_count, 0,
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

bool LoadSegments(const Image& image, const LoadOperations& operations)
{
    if (operations.map == nullptr || operations.copy == nullptr || operations.zero == nullptr ||
        operations.protect == nullptr || image.load_segment_count == 0)
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
        if (!operations.map(operations.context, segment.virtual_address, segment.memory_size,
                            segment.flags | SegmentFlagWrite))
        {
            return false;
        }
        if (segment.file_size > 0 && !operations.copy(operations.context, segment.virtual_address,
                                                      segment.file_data, segment.file_size))
        {
            return false;
        }
        const uint64_t zero_size = segment.memory_size - segment.file_size;
        if (zero_size > 0 &&
            !operations.zero(operations.context, segment.virtual_address + segment.file_size,
                             zero_size))
        {
            return false;
        }
        if (!operations.protect(operations.context, segment.virtual_address, segment.memory_size,
                                segment.flags))
        {
            return false;
        }
    }
    return true;
}

} // namespace FeatherOS::ELF