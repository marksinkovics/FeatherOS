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
// NONE is 0 on both machines.
constexpr uint32_t RelocationNone = 0;
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

bool ReadProgramHeader(const Source& source, uint64_t table_offset, uint16_t index,
                       LoadSegment* segment, uint32_t* type)
{
    uint8_t header[ProgramHeaderSize];
    if (!source.read(source.context,
                     table_offset + static_cast<uint64_t>(index) * ProgramHeaderSize, header,
                     sizeof(header)))
    {
        return false;
    }
    *type = ReadLittleEndian32(header);
    segment->flags = ReadLittleEndian32(header + 4);
    segment->file_offset = ReadLittleEndian64(header + 8);
    segment->virtual_address = ReadLittleEndian64(header + 16);
    segment->file_size = ReadLittleEndian64(header + 32);
    segment->memory_size = ReadLittleEndian64(header + 40);
    segment->alignment = ReadLittleEndian64(header + 48);
    return true;
}

bool IsValidLoadSegment(uint64_t file_size, const LoadSegment& segment)
{
    if (segment.file_size > segment.memory_size ||
        ((segment.flags & SegmentFlagWrite) != 0 && (segment.flags & SegmentFlagExecute) != 0))
    {
        return false;
    }

    const uint64_t offset = segment.file_offset;
    if (offset > file_size || segment.file_size > file_size - offset ||
        segment.memory_size > UINT64_MAX - segment.virtual_address)
    {
        return false;
    }

    // An empty segment maps nothing, so linkers may emit it with an arbitrary file offset.
    if (segment.alignment > 1 && segment.memory_size > 0)
    {
        if ((segment.alignment & (segment.alignment - 1)) != 0 ||
            segment.virtual_address % segment.alignment != offset % segment.alignment)
        {
            return false;
        }
    }
    return true;
}

// The file offset of size bytes at a link address, if a load segment holds them in the file.
bool FileOffsetAt(const Image& image, uint64_t address, uint64_t size, uint64_t* offset)
{
    for (uint16_t index = 0; index < image.load_segment_count; ++index)
    {
        const LoadSegment& segment = image.segments[index];
        if (address >= segment.virtual_address &&
            address - segment.virtual_address <= segment.file_size &&
            size <= segment.file_size - (address - segment.virtual_address))
        {
            *offset = segment.file_offset + (address - segment.virtual_address);
            return true;
        }
    }
    return false;
}

bool InWritableSegment(const Image& image, uint64_t address, uint64_t size)
{
    for (uint16_t index = 0; index < image.load_segment_count; ++index)
    {
        const LoadSegment& segment = image.segments[index];
        if ((segment.flags & SegmentFlagWrite) != 0 && address >= segment.virtual_address &&
            address - segment.virtual_address <= segment.memory_size &&
            size <= segment.memory_size - (address - segment.virtual_address))
        {
            return true;
        }
    }
    return false;
}

struct RelocationTable
{
    uint64_t address;
    uint64_t size;
    uint64_t entry_size;
    bool present;
};

bool ReadDynamicTable(const Image& image, const Source& source, uint8_t* buffer,
                      uint64_t buffer_size, RelocationTable* table)
{
    const uint64_t chunk_size = buffer_size - buffer_size % DynamicEntrySize;
    const uint64_t table_size = image.dynamic_size - image.dynamic_size % DynamicEntrySize;
    for (uint64_t done = 0; done < table_size;)
    {
        const uint64_t chunk = table_size - done < chunk_size ? table_size - done : chunk_size;
        if (!source.read(source.context, image.dynamic_offset + done, buffer, chunk))
        {
            return false;
        }
        for (uint64_t offset = 0; offset < chunk; offset += DynamicEntrySize)
        {
            const uint64_t tag = ReadLittleEndian64(buffer + offset);
            const uint64_t value = ReadLittleEndian64(buffer + offset + 8);
            if (tag == DynamicNull)
            {
                return true;
            }
            if (tag == DynamicRel || tag == DynamicJumpRelocations)
            {
                return false;
            }
            if (tag == DynamicRela)
            {
                table->address = value;
                table->present = true;
            }
            else if (tag == DynamicRelaSize)
            {
                table->size = value;
            }
            else if (tag == DynamicRelaEntrySize)
            {
                table->entry_size = value;
            }
        }
        done += chunk;
    }
    return true;
}

// A static PIE carries only RELATIVE relocations: each adds the load bias to an addend and
// stores the result in a writable segment. Anything that needs symbols or a PLT is rejected.
// GNU ld can also leave NONE entries, from relocations it resolved after sizing the table.
bool ApplyRelocations(const Image& image, const Source& source, uint64_t load_bias,
                      const LoadOperations& operations, uint8_t* buffer, uint64_t buffer_size)
{
    if (!image.position_independent || image.dynamic_size == 0)
    {
        return true;
    }
    RelocationTable table = {
        .address = 0, .size = 0, .entry_size = RelaEntrySize, .present = false};
    if (!ReadDynamicTable(image, source, buffer, buffer_size, &table))
    {
        return false;
    }
    if (!table.present || table.size == 0)
    {
        return table.size == 0;
    }
    uint64_t table_offset;
    if (table.entry_size != RelaEntrySize || table.size % RelaEntrySize != 0 ||
        !FileOffsetAt(image, table.address, table.size, &table_offset))
    {
        return false;
    }

    const uint32_t relative_type =
        image.machine == MachineX86_64 ? RelocationX86_64Relative : RelocationAArch64Relative;
    const uint64_t chunk_size = buffer_size - buffer_size % RelaEntrySize;
    for (uint64_t done = 0; done < table.size;)
    {
        const uint64_t chunk = table.size - done < chunk_size ? table.size - done : chunk_size;
        if (!source.read(source.context, table_offset + done, buffer, chunk))
        {
            return false;
        }
        for (uint64_t offset = 0; offset < chunk; offset += RelaEntrySize)
        {
            const uint64_t target = ReadLittleEndian64(buffer + offset);
            const uint64_t info = ReadLittleEndian64(buffer + offset + 8);
            const uint64_t addend = ReadLittleEndian64(buffer + offset + 16);
            if (info == RelocationNone)
            {
                continue;
            }
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
        done += chunk;
    }
    return true;
}

bool CopyFileBytes(const Source& source, uint64_t file_offset, uint64_t address, uint64_t size,
                   const LoadOperations& operations, uint8_t* buffer, uint64_t buffer_size)
{
    for (uint64_t done = 0; done < size;)
    {
        const uint64_t chunk = size - done < buffer_size ? size - done : buffer_size;
        if (!source.read(source.context, file_offset + done, buffer, chunk) ||
            !operations.copy(operations.context, address + done, buffer, chunk))
        {
            return false;
        }
        done += chunk;
    }
    return true;
}

} // namespace

bool ReadMemoryFile(void* context, uint64_t offset, void* buffer, uint64_t size)
{
    const auto* file = static_cast<const MemoryFile*>(context);
    if (file == nullptr || offset > file->size || size > file->size - offset)
    {
        return false;
    }
    auto* destination = static_cast<uint8_t*>(buffer);
    for (uint64_t index = 0; index < size; ++index)
    {
        destination[index] = file->data[offset + index];
    }
    return true;
}

bool Parse(const Source& source, uint64_t size, uint16_t expected_machine, Image* image)
{
    uint8_t data[ElfHeaderSize];
    if (source.read == nullptr || image == nullptr || size < ElfHeaderSize ||
        !source.read(source.context, 0, data, sizeof(data)))
    {
        return false;
    }
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
        .size = size,
        .entry_point = ReadLittleEndian64(data + 24),
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
        .segments = {},
    };
    bool entry_is_executable = false;
    for (uint16_t index = 0; index < program_header_count; ++index)
    {
        LoadSegment segment;
        uint32_t segment_type;
        if (!ReadProgramHeader(source, program_header_offset, index, &segment, &segment_type))
        {
            return false;
        }
        if (segment_type == ProgramDynamic)
        {
            if (segment.file_offset > size || segment.file_size > size - segment.file_offset)
            {
                return false;
            }
            parsed.dynamic_offset = segment.file_offset;
            parsed.dynamic_size = segment.file_size;
            continue;
        }
        if (segment_type == ProgramThreadLocal)
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
        if (segment_type != ProgramLoad)
        {
            continue;
        }
        if (parsed.load_segment_count == MaximumLoadSegments || !IsValidLoadSegment(size, segment))
        {
            return false;
        }
        parsed.segments[parsed.load_segment_count++] = segment;
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
    for (uint16_t index = 0; index < parsed.load_segment_count && !template_loaded; ++index)
    {
        const LoadSegment& segment = parsed.segments[index];
        template_loaded =
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
    *segment = image.segments[index];
    return true;
}

bool LoadSegments(const Image& image, const Source& source, uint64_t load_bias,
                  const LoadOperations& operations, uint8_t* buffer, uint64_t buffer_size)
{
    if (operations.map == nullptr || operations.copy == nullptr || operations.zero == nullptr ||
        operations.protect == nullptr || source.read == nullptr || buffer == nullptr ||
        buffer_size < MinimumLoadBufferSize || image.load_segment_count == 0 ||
        image.load_segment_count > MaximumLoadSegments ||
        (!image.position_independent && load_bias != 0))
    {
        return false;
    }

    for (uint16_t index = 0; index < image.load_segment_count; ++index)
    {
        const LoadSegment& segment = image.segments[index];
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
                            segment.flags | SegmentFlagWrite) ||
            !CopyFileBytes(source, segment.file_offset, address, segment.file_size, operations,
                           buffer, buffer_size))
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
    if (!ApplyRelocations(image, source, load_bias, operations, buffer, buffer_size))
    {
        return false;
    }
    for (uint16_t index = 0; index < image.load_segment_count; ++index)
    {
        const LoadSegment& segment = image.segments[index];
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
