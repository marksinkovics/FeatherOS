#include "environment_table.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

void EnvironmentTable::Load(const char* block)
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

char** EnvironmentTable::Entries()
{
    return entries;
}

const char* EnvironmentTable::Block() const
{
    return storage;
}

const char* EnvironmentTable::Find(const char* name) const
{
    const int64_t index = IndexOf(name);
    return index < 0 ? nullptr : entries[index] + Length(name) + 1;
}

EnvironmentResult EnvironmentTable::Set(const char* name, const char* value, bool overwrite)
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

EnvironmentResult EnvironmentTable::Remove(const char* name)
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

size_t EnvironmentTable::Length(const char* text)
{
    size_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

void EnvironmentTable::Copy(char* destination, const char* source, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        destination[index] = source[index];
    }
}

bool EnvironmentTable::ValidName(const char* name)
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

int64_t EnvironmentTable::IndexOf(const char* name) const
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

void EnvironmentTable::RemoveAt(size_t index)
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

} // namespace FeatherOS::LibC
