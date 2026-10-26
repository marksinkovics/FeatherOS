#ifndef FEATHEROS_LIBC_ENVIRONMENT_TABLE_H
#define FEATHEROS_LIBC_ENVIRONMENT_TABLE_H

#include "user/syscall_abi.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

enum class EnvironmentResult : uint8_t
{
    Done,
    InvalidName,
    Full,
};

// The environment in a fixed buffer the size of the block Spawn accepts, so whatever a program
// sets can be passed to its children. Entries are stored back to back in order, which keeps the
// storage in Spawn's format with no copying.
class EnvironmentTable
{
public:
    // Must be called first. block is "NAME=value" strings and an empty string, as the kernel passes
    // it.
    void Load(const char* block);
    // A null-terminated array of "NAME=value" pointers, which environ points to.
    char** Entries();
    // Spawn's format: the entries followed by an empty string.
    const char* Block() const;

    const char* Find(const char* name) const;
    EnvironmentResult Set(const char* name, const char* value, bool overwrite);
    EnvironmentResult Remove(const char* name);

private:
    static size_t Length(const char* text);
    static void Copy(char* destination, const char* source, size_t count);
    // POSIX leaves names otherwise open; '=' would end the name early.
    static bool ValidName(const char* name);

    int64_t IndexOf(const char* name) const;
    // Closes the gap the entry leaves, so later entries and their pointers move down.
    void RemoveAt(size_t index);
    // No member initializers, so a global table needs no constructor and can be loaded before
    // the program's constructors run.
    char storage[User::MaximumEnvironmentSize];
    char* entries[User::MaximumEnvironmentVariables + 1];
    size_t used;
    size_t count;
};

} // namespace FeatherOS::LibC

#endif
