#ifndef FEATHEROS_FILESYSTEM_TMPFS_H
#define FEATHEROS_FILESYSTEM_TMPFS_H

#include "kernel/filesystem/fat16.h"

#include <stdint.h>

// The RAM filesystem mounted at /tmp and /etc. Files and directories live on the kernel heap until
// they are removed or the system restarts. Paths are relative to a root, such as "notes/a.txt"
// ("" is the root), and names are case-sensitive.
namespace FeatherOS::Tmpfs
{

enum class Root : uint8_t
{
    Tmp,
    Etc,
};

// Files and directories under both roots together.
constexpr uint32_t MaximumEntries = 128;
// All file data together; writes beyond it are short.
constexpr uint64_t MaximumBytes = 4 * 1024 * 1024;

// Node indices stay valid until the node is removed, and are unique across the roots.
bool Open(Root root, const char* path, uint32_t* node);
bool Create(Root root, const char* path, uint32_t* node);
uint64_t Size(uint32_t node);
bool Read(uint32_t node, uint64_t offset, void* buffer, uint64_t length, uint64_t* bytes_read);
// offset may be at most the file size. Returns false if not every byte fit; *bytes_written says
// how many did.
bool Write(uint32_t node, uint64_t offset, const void* buffer, uint64_t length,
           uint64_t* bytes_written);
// Shrinking frees memory; growing is not supported.
bool Truncate(uint32_t node, uint64_t size);
bool Remove(Root root, const char* path);
bool MakeDirectory(Root root, const char* path);
// Only an empty directory can be removed, and never the root.
bool RemoveDirectory(Root root, const char* path);
// Fails if to exists, or would put a directory inside itself. Open files keep their node.
bool Rename(Root root, const char* from, const char* to);
bool IsDirectory(Root root, const char* path);
bool ReadDirectory(Root root, const char* path, uint32_t index, FAT16::DirectoryEntry* entry);
uint64_t UsedBytes();
// Removes everything, for tests.
void Reset();

} // namespace FeatherOS::Tmpfs

#endif
