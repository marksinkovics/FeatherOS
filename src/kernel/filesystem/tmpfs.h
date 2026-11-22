#ifndef FEATHEROS_FILESYSTEM_TMPFS_H
#define FEATHEROS_FILESYSTEM_TMPFS_H

#include "kernel/filesystem/filesystem.h"

#include <stdint.h>

// The RAM filesystem mounted at /tmp and /etc, and at / when the machine has no disk. Files,
// directories and symbolic links live on the kernel heap until they are removed or the system
// restarts. Paths are relative to a root, such as "notes/a.txt" ("" is the root), and names are
// case-sensitive. Links are never followed here; the VFS walk does that. Hard links are not
// supported: a node holds its own name.
namespace FeatherOS::Tmpfs
{

enum class Root : uint8_t
{
    Tmp,
    Etc,
    Ram,
};

// Files, directories and links under all the roots together.
constexpr uint32_t MaximumEntries = 128;
// All file data and link targets together; writes beyond it are short.
constexpr uint64_t MaximumBytes = 4 * 1024 * 1024;

// Node indices stay valid until the node is removed, and are unique across the roots. Names are
// at most User::MaximumNameLength bytes.
// Any file, directory or link, "" for the root.
bool Find(Root root, const char* path, uint32_t* node);
bool IsDirectoryNode(uint32_t node);
bool IsLinkNode(uint32_t node);
// A node's number for stat and directory entries, never 0.
uint64_t SerialNumber(uint32_t node);
bool Open(Root root, const char* path, uint32_t* node);
bool Create(Root root, const char* path, uint32_t* node);
// A file's size, or the length of a link's target.
uint64_t Size(uint32_t node);
bool Read(uint32_t node, uint64_t offset, void* buffer, uint64_t length, uint64_t* bytes_read);
// offset may be at most the file size. Returns false if not every byte fit; *bytes_written says
// how many did.
bool Write(uint32_t node, uint64_t offset, const void* buffer, uint64_t length,
           uint64_t* bytes_written);
// Shrinking frees memory; growing is not supported.
bool Truncate(uint32_t node, uint64_t size);
// A file or a link.
bool Remove(Root root, const char* path);
bool MakeDirectory(Root root, const char* path);
// Only an empty directory can be removed, and never the root.
bool RemoveDirectory(Root root, const char* path);
// A name onto itself changes nothing. An existing to of the same kind (a directory only if empty)
// is freed once every check has passed. Fails if the move would put a directory inside itself.
// Open files keep their node.
bool Rename(Root root, const char* from, const char* to);
bool IsDirectory(Root root, const char* path);
bool ReadDirectory(Root root, const char* path, uint32_t index, Filesystem::DirectoryEntry* entry);
// A new link at path that holds target, at most User::MaximumPathLength - 1 bytes; its bytes count
// toward MaximumBytes.
bool SymbolicLink(Root root, const char* path, const char* target, uint32_t* node);
// Copies as much of the target as capacity holds; *length receives its whole length.
bool ReadLink(uint32_t node, char* buffer, uint32_t capacity, uint32_t* length);
// As Filesystem::Driver::FindLink.
bool FindLink(Root root, const char* path, uint32_t* link_end);
uint64_t UsedBytes();
// Removes everything, for tests.
void Reset();

} // namespace FeatherOS::Tmpfs

#endif
