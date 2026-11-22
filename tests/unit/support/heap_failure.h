#ifndef FEATHEROS_TESTS_HEAP_FAILURE_H
#define FEATHEROS_TESTS_HEAP_FAILURE_H

#include <cstddef>

namespace FeatherOS::Memory::Testing
{

// Every AllocateZeroed of exactly this many items of this size fails; a count of 0 lets them all
// through.
extern std::size_t failing_zeroed_count;
extern std::size_t failing_zeroed_size;

class FailZeroedAllocations
{
public:
    FailZeroedAllocations(std::size_t count, std::size_t size)
    {
        failing_zeroed_count = count;
        failing_zeroed_size = size;
    }
    ~FailZeroedAllocations()
    {
        failing_zeroed_count = 0;
    }
    FailZeroedAllocations(const FailZeroedAllocations&) = delete;
    FailZeroedAllocations& operator=(const FailZeroedAllocations&) = delete;
};

} // namespace FeatherOS::Memory::Testing

#endif
