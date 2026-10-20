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

// Grows the heap until the per-process page limit refuses more, then checks that the memory it
// already has still works.
[[noreturn]] void ExhaustHeap()
{
    constexpr uint64_t PageSize = 4096;
    const uint64_t start = ExtendHeap(0);
    uint64_t pages = 0;
    while (!IsSyscallError(ExtendHeap(PageSize)))
    {
        ++pages;
    }
    auto* bytes = reinterpret_cast<volatile uint8_t*>(start);
    for (uint64_t page = 0; page < pages; ++page)
    {
        bytes[page * PageSize] = static_cast<uint8_t>(page);
    }
    for (uint64_t page = 0; page < pages; ++page)
    {
        if (bytes[page * PageSize] != static_cast<uint8_t>(page))
        {
            WriteLine("memtest: heap corrupted after exhaustion");
            Exit(1);
        }
    }
    WriteText("memtest: page limit reached after ");
    WriteNumber(pages);
    WriteLine(" heap pages");
    Exit(0);
}

} // namespace

// memtest [overflow|exhaust]: grows the stack and heap, overflows the stack into its guard page,
// or grows the heap until the page limit refuses more.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    if (arguments != nullptr && arguments->argument_count == 2 &&
        StringEquals(arguments->argument_values[1], "overflow"))
    {
        FillStack(1000);
        WriteLine("memtest: stack overflow was not caught");
        Exit(1);
    }
    if (arguments != nullptr && arguments->argument_count == 2 &&
        StringEquals(arguments->argument_values[1], "exhaust"))
    {
        ExhaustHeap();
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
