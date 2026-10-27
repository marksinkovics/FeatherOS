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

struct Image
{
    const uint8_t* data;
    uint64_t size;
    uint64_t entry_point;
    uint64_t program_header_offset;
    uint16_t program_header_count;
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
};

// The largest thread-local block a program may have.
constexpr uint64_t MaximumThreadLocalSize = 16 * 1024;

struct LoadSegment
{
    const uint8_t* file_data;
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
    uint32_t flags;
};

struct LoadOperations
{
    void* context;
    bool (*map)(void* context, uint64_t virtual_address, uint64_t memory_size, uint32_t flags);
    bool (*copy)(void* context, uint64_t virtual_address, const uint8_t* source, uint64_t size);
    bool (*zero)(void* context, uint64_t virtual_address, uint64_t size);
    bool (*protect)(void* context, uint64_t virtual_address, uint64_t memory_size, uint32_t flags);
};

bool Parse(const void* data, uint64_t size, uint16_t expected_machine, Image* image);
bool GetLoadSegment(const Image& image, uint16_t index, LoadSegment* segment);
bool LoadSegments(const Image& image, uint64_t load_bias, const LoadOperations& operations);

} // namespace FeatherOS::ELF

#endif