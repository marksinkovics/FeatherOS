#ifndef FEATHEROS_FILES_H
#define FEATHEROS_FILES_H

#include "kernel/filesystem/vfs.h"
#include "user/syscall_abi.h"

#include <stdint.h>

// Per-process file descriptors over the VFS. Functions return the value a syscall hands back:
// a result, or a User::SyscallError code. Buffers are kernel memory.
namespace FeatherOS::Files
{

struct OpenFile
{
    VFS::FileHandle handle;
    bool used;
    bool readable;
    bool writable;
};

// A zero-filled table has no open files.
struct Table
{
    OpenFile files[User::MaximumOpenFiles];
};

uint64_t Open(Table* table, const char* path, uint64_t flags);
uint64_t Read(Table* table, uint64_t descriptor, void* buffer, uint64_t length);
uint64_t Write(Table* table, uint64_t descriptor, const void* buffer, uint64_t length);
uint64_t Seek(Table* table, uint64_t descriptor, uint64_t offset);
uint64_t Close(Table* table, uint64_t descriptor);
uint64_t Remove(const char* path);
uint64_t ReadDirectory(const char* path, uint64_t index, User::DirectoryEntry* entry);
uint64_t MakeDirectory(const char* path);
// 0 for a directory, otherwise SyscallErrorNotFound.
uint64_t CheckDirectory(const char* path);
// Fails unless the directory is empty.
uint64_t RemoveDirectory(const char* path);
void CloseAll(Table* table);
// Waits for any filesystem call in progress, then blocks all later ones, for shutdown.
void Quiesce();

} // namespace FeatherOS::Files

#endif
