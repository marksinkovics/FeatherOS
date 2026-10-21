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
    void Load(const char* block)
    {
        used = 0;
        count = 0;
        entries[0] = nullptr;
        storage[0] = '\0';
        if (block == nullptr)
        {
            return;
        }
        while (block[used] != '\0' && count < User::MaximumEnvironmentVariables)
        {
            const size_t length = Length(block + used);
            if (used + length + 2 > sizeof(storage))
            {
                break;
            }
            Copy(storage + used, block + used, length + 1);
            entries[count++] = storage + used;
            used += length + 1;
        }
        storage[used] = '\0';
        entries[count] = nullptr;
    }

    // A null-terminated array of "NAME=value" pointers, which environ points to.
    char** Entries()
    {
        return entries;
    }

    // Spawn's format: the entries followed by an empty string.
    const char* Block() const
    {
        return storage;
    }

    const char* Find(const char* name) const
    {
        const int64_t index = IndexOf(name);
        return index < 0 ? nullptr : entries[index] + Length(name) + 1;
    }

    EnvironmentResult Set(const char* name, const char* value, bool overwrite)
    {
        if (!ValidName(name))
        {
            return EnvironmentResult::InvalidName;
        }
        const int64_t existing = IndexOf(name);
        if (existing >= 0 && !overwrite)
        {
            return EnvironmentResult::Done;
        }
        const size_t name_length = Length(name);
        const size_t value_length = Length(value);
        // Room for "NAME=value", its terminator, and the block's final empty string, counting
        // the space an entry being replaced frees.
        const size_t needed = name_length + 1 + value_length + 1;
        const size_t freed = existing >= 0 ? Length(entries[existing]) + 1 : 0;
        const size_t added_entries = existing >= 0 ? 0 : 1;
        if (used - freed + needed + 1 > sizeof(storage) ||
            count + added_entries > User::MaximumEnvironmentVariables)
        {
            return EnvironmentResult::Full;
        }
        // The value may be the old entry's own text, as in setenv(name, getenv(name), 1), which
        // removing the entry would move.
        char saved_value[User::MaximumEnvironmentSize];
        if (value >= storage && value < storage + sizeof(storage))
        {
            Copy(saved_value, value, value_length + 1);
            value = saved_value;
        }
        if (existing >= 0)
        {
            RemoveAt(static_cast<size_t>(existing));
        }
        char* entry = storage + used;
        Copy(entry, name, name_length);
        entry[name_length] = '=';
        Copy(entry + name_length + 1, value, value_length + 1);
        entries[count++] = entry;
        entries[count] = nullptr;
        used += needed;
        storage[used] = '\0';
        return EnvironmentResult::Done;
    }

    EnvironmentResult Remove(const char* name)
    {
        if (!ValidName(name))
        {
            return EnvironmentResult::InvalidName;
        }
        const int64_t index = IndexOf(name);
        if (index >= 0)
        {
            RemoveAt(static_cast<size_t>(index));
        }
        return EnvironmentResult::Done;
    }

private:
    static size_t Length(const char* text)
    {
        size_t length = 0;
        while (text[length] != '\0')
        {
            ++length;
        }
        return length;
    }

    static void Copy(char* destination, const char* source, size_t count)
    {
        for (size_t index = 0; index < count; ++index)
        {
            destination[index] = source[index];
        }
    }

    // POSIX leaves names otherwise open; '=' would end the name early.
    static bool ValidName(const char* name)
    {
        if (name == nullptr || *name == '\0')
        {
            return false;
        }
        for (; *name != '\0'; ++name)
        {
            if (*name == '=')
            {
                return false;
            }
        }
        return true;
    }

    int64_t IndexOf(const char* name) const
    {
        if (name == nullptr)
        {
            return -1;
        }
        for (size_t index = 0; index < count; ++index)
        {
            const char* entry = entries[index];
            size_t offset = 0;
            while (name[offset] != '\0' && entry[offset] == name[offset])
            {
                ++offset;
            }
            if (name[offset] == '\0' && entry[offset] == '=')
            {
                return static_cast<int64_t>(index);
            }
        }
        return -1;
    }

    // Closes the gap the entry leaves, so later entries and their pointers move down.
    void RemoveAt(size_t index)
    {
        char* entry = entries[index];
        const size_t length = Length(entry) + 1;
        const size_t tail = static_cast<size_t>(storage + used - (entry + length));
        for (size_t offset = 0; offset < tail; ++offset)
        {
            entry[offset] = entry[length + offset];
        }
        for (size_t later = index; later + 1 < count; ++later)
        {
            entries[later] = entries[later + 1] - length;
        }
        --count;
        entries[count] = nullptr;
        used -= length;
        storage[used] = '\0';
    }

    // No member initializers, so a global table needs no constructor and can be loaded before
    // the program's constructors run.
    char storage[User::MaximumEnvironmentSize];
    char* entries[User::MaximumEnvironmentVariables + 1];
    size_t used;
    size_t count;
};

} // namespace FeatherOS::LibC

#endif
