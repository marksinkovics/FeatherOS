#include "devices.h"

#include "kernel/random.h"
#include "kernel/terminal/pseudo_terminal.h"
#include "user/syscall_abi.h"

namespace FeatherOS::Devices
{
namespace
{

struct Entry
{
    const char* name;
    Device device;
};

constexpr Entry Entries[] = {
    {"null", Device::Null},
    {"zero", Device::Zero},
    {"random", Device::Random},
    {"tty", Device::ControllingTerminal},
    {"ptmx", Device::PseudoTerminalMaster},
};
constexpr uint32_t EntryCount = sizeof(Entries) / sizeof(Entries[0]);
constexpr char SlaveDirectory[] = "pts";
constexpr char ConsolePrefix[] = "tty";
constexpr uint32_t PairShift = 8;
constexpr uint32_t MaximumConsoles = 9;

Terminals::Terminal* const* consoles = nullptr;
uint32_t console_count = 0;

bool Equals(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

// After "pts/": a pair number in decimal, without leading zeros.
bool ParsePair(const char* text, uint32_t* number)
{
    uint32_t value = 0;
    uint32_t length = 0;
    for (; text[length] >= '0' && text[length] <= '9'; ++length)
    {
        if (length == 2 || (length == 1 && value == 0))
        {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(text[length] - '0');
    }
    *number = value;
    return length > 0 && text[length] == '\0';
}

void SetEntry(FAT16::DirectoryEntry* entry, const char* name, bool directory)
{
    uint32_t length = 0;
    for (; name[length] != '\0'; ++length)
    {
        entry->name[length] = name[length];
    }
    entry->name[length] = '\0';
    entry->directory = directory;
    entry->size = 0;
}

// "ttyN" for console N, from 1.
bool ParseConsole(const char* path, uint32_t* index)
{
    uint32_t prefix = 0;
    while (ConsolePrefix[prefix] != '\0' && path[prefix] == ConsolePrefix[prefix])
    {
        ++prefix;
    }
    const char digit = path[prefix];
    if (ConsolePrefix[prefix] != '\0' || digit < '1' ||
        static_cast<uint32_t>(digit - '0') > console_count || path[prefix + 1] != '\0')
    {
        return false;
    }
    *index = static_cast<uint32_t>(digit - '1');
    return true;
}

} // namespace

void SetConsoles(Terminals::Terminal* const* terminals, uint32_t count)
{
    consoles = terminals;
    console_count = count < MaximumConsoles ? count : MaximumConsoles;
}

bool Find(const char* path, uint32_t* node)
{
    for (const Entry& entry : Entries)
    {
        if (Equals(path, entry.name))
        {
            *node = static_cast<uint32_t>(entry.device);
            return true;
        }
    }
    uint32_t console;
    if (ParseConsole(path, &console))
    {
        *node = static_cast<uint32_t>(Device::Console) | (console << PairShift);
        return true;
    }
    uint32_t prefix = 0;
    while (SlaveDirectory[prefix] != '\0' && path[prefix] == SlaveDirectory[prefix])
    {
        ++prefix;
    }
    uint32_t number;
    if (SlaveDirectory[prefix] != '\0' || path[prefix] != '/' ||
        !ParsePair(path + prefix + 1, &number) || !PseudoTerminals::InUse(number))
    {
        return false;
    }
    *node = static_cast<uint32_t>(Device::PseudoTerminalSlave) | (number << PairShift);
    return true;
}

Device DeviceOf(uint32_t node)
{
    return static_cast<Device>(node & ((1U << PairShift) - 1));
}

uint32_t PairOf(uint32_t node)
{
    return node >> PairShift;
}

Terminals::Terminal* ConsoleOf(uint32_t node)
{
    const uint32_t index = node >> PairShift;
    return index < console_count ? consoles[index] : nullptr;
}

bool IsDirectory(const char* path)
{
    return *path == '\0' || Equals(path, SlaveDirectory);
}

void Read(Device device, void* buffer, uint64_t length, uint64_t* bytes_read)
{
    auto* bytes = static_cast<uint8_t*>(buffer);
    if (device != Device::Zero && device != Device::Random)
    {
        *bytes_read = 0;
        return;
    }
    uint64_t random = 0;
    for (uint64_t index = 0; index < length; ++index)
    {
        if (device == Device::Zero)
        {
            bytes[index] = 0;
            continue;
        }
        if (index % sizeof(random) == 0)
        {
            random = Random::Next();
        }
        bytes[index] = static_cast<uint8_t>(random >> (8 * (index % sizeof(random))));
    }
    *bytes_read = length;
}

bool ReadDirectory(const char* path, uint32_t index, FAT16::DirectoryEntry* entry)
{
    if (*path == '\0')
    {
        if (index < EntryCount)
        {
            SetEntry(entry, Entries[index].name, false);
            return true;
        }
        if (index < EntryCount + console_count)
        {
            const char name[] = {'t', 't', 'y', static_cast<char>('1' + index - EntryCount), '\0'};
            SetEntry(entry, name, false);
            return true;
        }
        if (index == EntryCount + console_count)
        {
            SetEntry(entry, SlaveDirectory, true);
            return true;
        }
        return false;
    }
    if (!Equals(path, SlaveDirectory))
    {
        return false;
    }
    for (uint32_t number = 0; number < User::MaximumPseudoTerminals; ++number)
    {
        if (!PseudoTerminals::InUse(number))
        {
            continue;
        }
        if (index-- == 0)
        {
            const char name[] = {number < 10 ? static_cast<char>('0' + number) : '1',
                                 number < 10 ? '\0' : static_cast<char>('0' + number - 10), '\0'};
            SetEntry(entry, name, false);
            return true;
        }
    }
    return false;
}

} // namespace FeatherOS::Devices
