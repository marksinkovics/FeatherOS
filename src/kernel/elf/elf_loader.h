#ifndef FEATHEROS_ELF_ELF_LOADER_H
#define FEATHEROS_ELF_ELF_LOADER_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::ELF
{

constexpr uint16_t MachineX86_64 = 62;
constexpr uint16_t MachineAArch64 = 183;
constexpr uint32_t SegmentFlagExecute = 1;
constexpr uint32_t SegmentFlagWrite = 2;
constexpr uint32_t SegmentFlagRead = 4;

// The file an image is read from, a piece at a time: read fills the whole buffer or fails.
struct Source
{
    void* context;
    bool (*read)(void* context, uint64_t offset, void* buffer, uint64_t size);
};

// A Source over bytes already in memory, such as an initramfs file; MemoryFile is its context.
struct MemoryFile
{
    const uint8_t* data;
    uint64_t size;
};
bool ReadMemoryFile(void* context, uint64_t offset, void* buffer, uint64_t size);

struct LoadSegment
{
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
    uint32_t flags;
};

// Linkers write four or five; more is rejected.
constexpr uint16_t MaximumLoadSegments = 8;

struct Image
{
    uint64_t size;
    uint64_t entry_point;
    uint16_t load_segment_count;
    uint16_t machine;
    // ET_DYN: link addresses start near zero and the loader picks a base; ET_EXEC loads as linked.
    bool position_independent;
    uint64_t dynamic_offset;
    uint64_t dynamic_size;
    // The PT_TLS template at its link address, inside a load segment: each thread's block starts
    // as its first tls_file_size bytes followed by zeros, tls_memory_size bytes in all.
    bool has_tls;
    uint64_t tls_virtual_address;
    uint64_t tls_file_size;
    uint64_t tls_memory_size;
    uint64_t tls_alignment;
    LoadSegment segments[MaximumLoadSegments];
};

// The largest thread-local block a program may have.
constexpr uint64_t MaximumThreadLocalSize = 16 * 1024;

struct LoadOperations
{
    void* context;
    bool (*map)(void* context, uint64_t virtual_address, uint64_t memory_size, uint32_t flags);
    bool (*copy)(void* context, uint64_t virtual_address, const uint8_t* source, uint64_t size);
    bool (*zero)(void* context, uint64_t virtual_address, uint64_t size);
    bool (*protect)(void* context, uint64_t virtual_address, uint64_t memory_size, uint32_t flags);
};

// The smallest buffer LoadSegments accepts; a larger one means fewer reads.
constexpr uint64_t MinimumLoadBufferSize = 64;

// size is the file's size.
bool Parse(const Source& source, uint64_t size, uint16_t expected_machine, Image* image);
bool GetLoadSegment(const Image& image, uint16_t index, LoadSegment* segment);
// File bytes pass through buffer on their way to copy.
bool LoadSegments(const Image& image, const Source& source, uint64_t load_bias,
                  const LoadOperations& operations, uint8_t* buffer, uint64_t buffer_size);

} // namespace FeatherOS::ELF

#endif
