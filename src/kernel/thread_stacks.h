#ifndef FEATHEROS_THREAD_STACKS_H
#define FEATHEROS_THREAD_STACKS_H

#include <stdint.h>

// Where each thread's stack lives. Slot 0 is the main thread's stack region, ending at the
// layout's stack top; slot n is the region n regions below it. A region is region_pages pages,
// grown on demand from the top, and its lowest page always stays unmapped as a guard.
namespace FeatherOS::ThreadStacks
{

constexpr uint64_t PageSize = 4096;

uint64_t Top(uint64_t stack_top, uint64_t region_pages, uint32_t slot);
// The lowest address of slot_count regions, which the heap must stay below.
uint64_t Bottom(uint64_t stack_top, uint64_t region_pages, uint32_t slot_count);
// True when address lies in one of slot_count regions above its guard page; *slot says which.
bool Find(uint64_t stack_top, uint64_t region_pages, uint32_t slot_count, uint64_t address,
          uint32_t* slot);

} // namespace FeatherOS::ThreadStacks

#endif
