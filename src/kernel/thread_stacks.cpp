#include "thread_stacks.h"

namespace FeatherOS::ThreadStacks
{

uint64_t Top(uint64_t stack_top, uint64_t region_pages, uint32_t slot)
{
    return stack_top - slot * region_pages * PageSize;
}

uint64_t Bottom(uint64_t stack_top, uint64_t region_pages, uint32_t slot_count)
{
    return Top(stack_top, region_pages, slot_count);
}

bool Find(uint64_t stack_top, uint64_t region_pages, uint32_t slot_count, uint64_t address,
          uint32_t* slot)
{
    const uint64_t region_size = region_pages * PageSize;
    if (address >= stack_top || address < Bottom(stack_top, region_pages, slot_count))
    {
        return false;
    }
    const uint64_t below_top = stack_top - 1 - address;
    const auto found = static_cast<uint32_t>(below_top / region_size);
    const uint64_t guard_page = Top(stack_top, region_pages, found + 1);
    if (address < guard_page + PageSize)
    {
        return false;
    }
    *slot = found;
    return true;
}

} // namespace FeatherOS::ThreadStacks
