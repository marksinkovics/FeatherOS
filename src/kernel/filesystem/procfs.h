#ifndef FEATHEROS_FILESYSTEM_PROCFS_H
#define FEATHEROS_FILESYSTEM_PROCFS_H

#include "kernel/filesystem/fat16.h"

#include <stddef.h>
#include <stdint.h>

// The generated files mounted at /proc: "meminfo", and a directory per process holding
// "status". Paths are relative to the mount ("" is the root). A file's text is made when it is
// opened, so a reader sees one consistent snapshot.
namespace FeatherOS::Procfs
{

constexpr size_t MaximumFileSize = 1024;

struct ProcessStatus
{
    char name[40];
    // A Linux-style state letter: R running or ready, S sleeping or blocked, Z exited.
    char state;
    uint64_t process_id;
    uint64_t parent_id;
    uint64_t pages;
    uint64_t page_limit;
    uint64_t open_files;
};

struct MemoryStatus
{
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t kernel_heap_bytes;
    uint64_t tmp_bytes;
};

// The kernel's process table and memory counters, which the host tests replace.
struct Source
{
    // The index-th live process; false past the last one.
    bool (*process_at)(uint32_t index, uint64_t* process_id);
    bool (*process_status)(uint64_t process_id, ProcessStatus* status);
    void (*memory_status)(MemoryStatus* status);
};

void SetSource(const Source& source);
// Writes the file's text into buffer (MaximumFileSize bytes) and its length into *length.
bool Generate(const char* path, char* buffer, size_t* length);
bool IsDirectory(const char* path);
bool ReadDirectory(const char* path, uint32_t index, FAT16::DirectoryEntry* entry);

} // namespace FeatherOS::Procfs

#endif
