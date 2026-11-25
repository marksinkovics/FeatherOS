#ifndef FEATHEROS_MEMORY_USER_COPY_H
#define FEATHEROS_MEMORY_USER_COPY_H

#include "kernel/sync/spinlock.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Memory::UserCopy
{

// Resolves one user page to its kernel (direct-map) address; nullptr when it is not mapped for the
// user, or not writable and write is set. Called with *Target::lock held: no sleep, no wait for
// another CPU, no kernel-heap allocation; it may map a page.
using ResolvePage = volatile uint8_t* (*)(void* context, uint64_t page_address, bool write);

// The lock is the address space's memory lock. Every page is resolved and copied inside one hold,
// so a concurrent unmap, which clears the entry under the same lock, either comes before the hold
// (the page is missing) or after it (nothing points at the frame any more).
struct Target
{
    Sync::SpinLock* lock;
    void* context;
    ResolvePage resolve;
};

// One lookup per page, for a check before data is consumed; a later unmap still fails the copy.
bool RangeMapped(const Target& target, uint64_t address, uint64_t size, bool write);

// False when a page is missing (or read-only for CopyOut); earlier pages may already be copied.
bool CopyIn(const Target& target, uint64_t address, void* destination, size_t size);
bool CopyOut(const Target& target, uint64_t address, const void* source, size_t size);

// Copies the longest mapped prefix, at most capacity bytes, and returns its length.
size_t CopyInPrefix(const Target& target, uint64_t address, void* destination, size_t capacity);

// 0, User::SyscallErrorBadAddress, or User::SyscallErrorInvalidArgument when no terminator fits.
uint64_t CopyStringIn(const Target& target, uint64_t address, char* buffer, size_t capacity);

} // namespace FeatherOS::Memory::UserCopy

#endif
