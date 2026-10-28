#include "kernel_stack.h"

#include "kernel/memory/page_allocator.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Memory
{
namespace
{

// Each slot is an unmapped guard page followed by the stack, so an overflow faults instead of
// silently overwriting the neighbouring stack.
constexpr uint64_t StackRegionBase = 0xFFFFFC0000000000;
constexpr uint64_t StackSlotSize = PageSize + KernelStackSize;
constexpr uint64_t MaximumStackSlots = 65536;
constexpr uint64_t StackPageCount = KernelStackSize / PageSize;

struct FreeStack
{
    FreeStack* next;
};

// Freed stacks stay mapped for reuse. Unmapping one would need a TLB shootdown, because
// another CPU may still cache its translation.
constinit Sync::SpinLock stack_lock;
FreeStack* free_stacks = nullptr;
uint64_t next_slot = 0;

// No other CPU has touched a stack that is still being set up, so unmapping needs no shootdown.
void ReleasePartialStack(uint64_t base, const uint64_t* frames, uint64_t mapped_pages)
{
    for (uint64_t page = 0; page < mapped_pages; ++page)
    {
        UnmapPage(base + page * PageSize);
        FreePageFrame(frames[page]);
    }
}

} // namespace

uint8_t* AllocateKernelStack()
{
    uint64_t slot;
    {
        Sync::SpinLockGuard guard(stack_lock);
        if (free_stacks != nullptr)
        {
            FreeStack* stack = free_stacks;
            free_stacks = stack->next;
            return reinterpret_cast<uint8_t*>(stack);
        }
        if (next_slot == MaximumStackSlots)
        {
            return nullptr;
        }
        slot = next_slot++;
    }

    const uint64_t base = StackRegionBase + slot * StackSlotSize + PageSize;
    uint64_t frames[StackPageCount];
    for (uint64_t page = 0; page < StackPageCount; ++page)
    {
        if (!AllocatePageFrame(&frames[page]))
        {
            ReleasePartialStack(base, frames, page);
            return nullptr;
        }
        if (!MapPage(base + page * PageSize, frames[page], PageFlags::Read | PageFlags::Write))
        {
            FreePageFrame(frames[page]);
            ReleasePartialStack(base, frames, page);
            return nullptr;
        }
    }
    return reinterpret_cast<uint8_t*>(base);
}

void FreeKernelStack(uint8_t* stack)
{
    if (stack == nullptr)
    {
        return;
    }
    auto* free_stack = reinterpret_cast<FreeStack*>(stack);
    Sync::SpinLockGuard guard(stack_lock);
    free_stack->next = free_stacks;
    free_stacks = free_stack;
}

} // namespace FeatherOS::Memory
