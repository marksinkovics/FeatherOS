#include <stdint.h>

#include "kernel/arch/serial.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS
{
namespace Serial
{

namespace
{

Sync::SpinLock write_lock;

const uint64_t UartPhysicalAddress = 0x09000000;

volatile uint32_t* Register(uint32_t offset)
{
    uintptr_t address = (uintptr_t)(UartPhysicalAddress + offset);
    return (volatile uint32_t*)address;
}

void WriteCharacter(char character)
{
    for (uint32_t attempt = 0; attempt < 100000; ++attempt)
    {
        if ((*Register(0x18) & (1U << 5)) == 0)
        {
            *Register(0x00) = (uint32_t)character;
            return;
        }
    }
}

} // namespace

void Init(uint64_t memory_offset)
{
    (void)memory_offset;

    *Register(0x30) = 0;
    *Register(0x44) = 0x7ff;
    *Register(0x24) = 39;
    *Register(0x28) = 4;
    *Register(0x2c) = (3U << 5) | (1U << 4);
    *Register(0x30) = (1U << 0) | (1U << 8) | (1U << 9);
}

void Write(const char* text)
{
    if (text == nullptr)
    {
        return;
    }

    Sync::SpinLockGuard guard(write_lock);
    while (*text != '\0')
    {
        if (*text == '\n')
        {
            WriteCharacter('\r');
        }
        WriteCharacter(*text++);
    }
}

void ForceUnlock()
{
    write_lock.Unlock();
}

bool ReadCharacter(char* character)
{
    if (character == nullptr || (*Register(0x18) & (1U << 4)) != 0)
    {
        return false;
    }
    *character = static_cast<char>(*Register(0x00));
    return true;
}

} // namespace Serial
} // namespace FeatherOS
