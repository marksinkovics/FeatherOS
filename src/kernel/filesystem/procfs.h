#ifndef FEATHEROS_FILESYSTEM_PROCFS_H
#define FEATHEROS_FILESYSTEM_PROCFS_H

#include "kernel/filesystem/fat16.h"

#include <stddef.h>
#include <stdint.h>

// The generated files mounted at /proc: "meminfo", "stat", and a directory per process holding
// "status". Paths are relative to the mount ("" is the root). A file's text is made when it is
// opened, so a reader sees one consistent snapshot.
namespace FeatherOS::Procfs
{

constexpr size_t MaximumFileSize = 1024;
constexpr uint32_t MaximumCpus = 16;

struct ProcessStatus
{
    char name[40];
    // A Linux-style state letter: R running or ready, S sleeping or blocked, Z exited.
    char state;
    uint64_t process_id;
    uint64_t parent_id;
    uint64_t group;
    uint64_t pages;
    uint64_t page_limit;
    uint64_t open_files;
    // Time spent running, on any CPU, the CPU it last ran on, and when it started, as uptime.
    uint64_t cpu_milliseconds;
    uint32_t last_cpu;
    uint64_t start_milliseconds;
    // Threads still running, the main one included; CPU time counts every thread.
    uint32_t threads;
};

struct MemoryStatus
{
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t kernel_heap_bytes;
    uint64_t tmp_bytes;
};

struct SystemStatus
{
    uint64_t uptime_milliseconds;
    uint32_t cpu_count;
    // How long each CPU ran tasks rather than idling.
    uint64_t busy_milliseconds[MaximumCpus];
};

// The kernel's process table and memory counters, which the host tests replace.
struct Source
{
    // The index-th live process; false past the last one.
    bool (*process_at)(uint32_t index, uint64_t* process_id);
    bool (*process_status)(uint64_t process_id, ProcessStatus* status);
    void (*memory_status)(MemoryStatus* status);
    void (*system_status)(SystemStatus* status);
};

void SetSource(const Source& source);
// Writes the file's text into buffer (MaximumFileSize bytes) and its length into *length.
bool Generate(const char* path, char* buffer, size_t* length);
bool IsDirectory(const char* path);
bool ReadDirectory(const char* path, uint32_t index, FAT16::DirectoryEntry* entry);

} // namespace FeatherOS::Procfs

#endif
