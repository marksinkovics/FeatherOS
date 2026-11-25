#include "kernel/memory/user_copy.h"

#include "kernel/memory/page_allocator.h"
#include "kernel/string/memory_functions.h"
#include "user/syscall_abi.h"

namespace FeatherOS::Memory::UserCopy
{

namespace
{

constexpr uint64_t PageMask = PageSize - 1;

bool RangeWraps(uint64_t address, uint64_t size)
{
    return size != 0 && address > UINT64_MAX - (size - 1);
}

size_t ChunkAt(uint64_t address, uint64_t remaining)
{
    const uint64_t room = PageSize - (address & PageMask);
    return static_cast<size_t>(remaining < room ? remaining : room);
}

} // namespace

bool RangeMapped(const Target& target, uint64_t address, uint64_t size, bool write)
{
    if (RangeWraps(address, size))
    {
        return false;
    }
    while (size > 0)
    {
        const size_t chunk = ChunkAt(address, size);
        {
            Sync::SpinLockGuard guard(*target.lock);
            if (target.resolve(target.context, address & ~PageMask, write) == nullptr)
            {
                return false;
            }
        }
        address += chunk;
        size -= chunk;
    }
    return true;
}

bool CopyIn(const Target& target, uint64_t address, void* destination, size_t size)
{
    if (RangeWraps(address, size))
    {
        return false;
    }
    auto* bytes = static_cast<uint8_t*>(destination);
    while (size > 0)
    {
        const size_t chunk = ChunkAt(address, size);
        {
            Sync::SpinLockGuard guard(*target.lock);
            volatile uint8_t* page = target.resolve(target.context, address & ~PageMask, false);
            if (page == nullptr)
            {
                return false;
            }
            memcpy(bytes, const_cast<const uint8_t*>(page + (address & PageMask)), chunk);
        }
        bytes += chunk;
        address += chunk;
        size -= chunk;
    }
    return true;
}

bool CopyOut(const Target& target, uint64_t address, const void* source, size_t size)
{
    if (RangeWraps(address, size))
    {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(source);
    while (size > 0)
    {
        const size_t chunk = ChunkAt(address, size);
        {
            Sync::SpinLockGuard guard(*target.lock);
            volatile uint8_t* page = target.resolve(target.context, address & ~PageMask, true);
            if (page == nullptr)
            {
                return false;
            }
            memcpy(const_cast<uint8_t*>(page + (address & PageMask)), bytes, chunk);
        }
        bytes += chunk;
        address += chunk;
        size -= chunk;
    }
    return true;
}

size_t CopyInPrefix(const Target& target, uint64_t address, void* destination, size_t capacity)
{
    // A range that would wrap past the top of the address space ends there.
    if (RangeWraps(address, capacity))
    {
        capacity = static_cast<size_t>(UINT64_MAX - address) + 1;
    }
    auto* bytes = static_cast<uint8_t*>(destination);
    size_t copied = 0;
    while (copied < capacity)
    {
        const size_t chunk = ChunkAt(address, capacity - copied);
        {
            Sync::SpinLockGuard guard(*target.lock);
            volatile uint8_t* page = target.resolve(target.context, address & ~PageMask, false);
            if (page == nullptr)
            {
                return copied;
            }
            memcpy(bytes + copied, const_cast<const uint8_t*>(page + (address & PageMask)), chunk);
        }
        copied += chunk;
        address += chunk;
    }
    return copied;
}

uint64_t CopyStringIn(const Target& target, uint64_t address, char* buffer, size_t capacity)
{
    size_t index = 0;
    while (index < capacity)
    {
        const size_t chunk = ChunkAt(address, capacity - index);
        {
            Sync::SpinLockGuard guard(*target.lock);
            volatile uint8_t* page = target.resolve(target.context, address & ~PageMask, false);
            if (page == nullptr)
            {
                return User::SyscallErrorBadAddress;
            }
            volatile const uint8_t* source = page + (address & PageMask);
            for (size_t offset = 0; offset < chunk; ++offset)
            {
                const char character = static_cast<char>(source[offset]);
                buffer[index + offset] = character;
                if (character == '\0')
                {
                    return 0;
                }
            }
        }
        index += chunk;
        if (address > UINT64_MAX - chunk)
        {
            return User::SyscallErrorBadAddress;
        }
        address += chunk;
    }
    return User::SyscallErrorInvalidArgument;
}

} // namespace FeatherOS::Memory::UserCopy
