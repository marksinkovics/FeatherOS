#ifndef FEATHEROS_FILESYSTEM_FAT16_H
#define FEATHEROS_FILESYSTEM_FAT16_H

#include <stdint.h>

namespace FeatherOS::FAT16
{

struct File
{
    uint32_t first_cluster;
    uint32_t size;
    // Where the directory entry is, so writes can update the size and first cluster: the
    // directory's first cluster (0 for the root) and the entry's index in it.
    uint32_t directory_cluster;
    uint32_t entry_index;
};

struct FileInfo
{
    uint32_t size;
};

struct DirectoryEntry
{
    // The long name if the entry has one, otherwise "NAME.EXT", and a terminator.
    char name[64];
    bool directory;
    uint32_t size;
};

// Paths are relative to the volume root, such as "docs/My Notes.txt". Each component matches a
// long or short name, ignoring case; "." and ".." work as usual. Updates are ordered so that an
// interrupted operation can leak clusters but never leaves a file pointing at unowned data:
// data and FAT entries are written before the directory entry that references them, and a
// directory entry is cleared before its clusters are freed.
bool Mount();
bool Open(const char* path, File* file);
bool Stat(const char* path, FileInfo* info);
bool Read(const File* file, uint64_t offset, void* buffer, uint64_t length, uint64_t* bytes_read);
// Fails if the name is invalid or exists, or the root directory is full. Subdirectories grow.
bool Create(const char* path, File* file);
bool MakeDirectory(const char* path);
// Only an empty directory can be removed.
bool RemoveDirectory(const char* path);
// offset may be at most the file size, so files grow only at their end. Returns false if not
// every byte was written (for example when the disk is full); *bytes_written says how many were.
bool Write(File* file, uint64_t offset, const void* buffer, uint64_t length,
           uint64_t* bytes_written);
// Shrinks the file and frees the clusters it no longer needs.
bool Truncate(File* file, uint64_t size);
bool Remove(const char* path);
// Moves an entry to another name or directory. Fails if to exists (unless it is from under
// another case), or would put a directory inside itself. Open files must be closed first.
bool Rename(const char* from, const char* to);
uint32_t FreeClusterCount();
bool IsDirectory(const char* path);
// The index-th entry of the directory at path ("" for the root), without "." and "..";
// false past the last one.
bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry);

} // namespace FeatherOS::FAT16

#endif