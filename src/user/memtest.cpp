#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

namespace
{

// Each call keeps 1 KiB live on the stack, so a deep chain spans many stack pages.
uint64_t FillStack(uint64_t depth)
{
    volatile uint8_t frame[1024];
    for (uint64_t index = 0; index < sizeof(frame); ++index)
    {
        frame[index] = static_cast<uint8_t>(depth + index);
    }
    const uint64_t below = depth == 0 ? 0 : FillStack(depth - 1);
    uint64_t sum = below;
    for (uint64_t index = 0; index < sizeof(frame); ++index)
    {
        sum += frame[index];
    }
    return sum;
}

bool HeapWorks()
{
    constexpr uint64_t HeapBytes = 3 * 4096 + 100;
    const uint64_t start = ExtendHeap(HeapBytes);
    if (IsSyscallError(start) || ExtendHeap(0) != start + HeapBytes)
    {
        return false;
    }
    auto* bytes = reinterpret_cast<volatile uint8_t*>(start);
    for (uint64_t index = 0; index < HeapBytes; ++index)
    {
        bytes[index] = static_cast<uint8_t>(index * 7);
    }
    for (uint64_t index = 0; index < HeapBytes; ++index)
    {
        if (bytes[index] != static_cast<uint8_t>(index * 7))
        {
            return false;
        }
    }
    return true;
}

} // namespace

// memtest [overflow]: grows the stack and heap, or overflows the stack into its guard page.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    if (arguments != nullptr && arguments->argument_count == 2 &&
        StringEquals(arguments->argument_values[1], "overflow"))
    {
        FillStack(1000);
        WriteLine("memtest: stack overflow was not caught");
        Exit(1);
    }
    uint64_t expected = 0;
    for (uint64_t depth = 0; depth <= 32; ++depth)
    {
        for (uint64_t index = 0; index < 1024; ++index)
        {
            expected += static_cast<uint8_t>(depth + index);
        }
    }
    if (FillStack(32) != expected || !HeapWorks())
    {
        WriteLine("memtest: failed");
        Exit(1);
    }
    WriteLine("memtest: stack and heap ok");
    Exit(0);
}
