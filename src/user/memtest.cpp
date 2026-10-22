#include "command_line.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

void* const HeapFailed = reinterpret_cast<void*>(-1);

bool HeapWorks()
{
    constexpr uint64_t HeapBytes = 3 * 4096 + 100;
    auto* start = static_cast<uint8_t*>(sbrk(HeapBytes));
    if (start == HeapFailed || sbrk(0) != start + HeapBytes)
    {
        return false;
    }
    volatile uint8_t* bytes = start;
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

constexpr uint64_t PageSize = 4096;

uint64_t GrowUntilRefused()
{
    uint64_t pages = 0;
    while (sbrk(PageSize) != HeapFailed)
    {
        ++pages;
    }
    return pages;
}

// Grows the heap until the per-process page limit refuses more, then checks that the memory it
// already has still works. Shrinking the heap must then give every page back, so it grows as far
// a second time.
[[noreturn]] void ExhaustHeap()
{
    auto* start = static_cast<uint8_t*>(sbrk(0));
    const uint64_t pages = GrowUntilRefused();
    volatile uint8_t* bytes = start;
    for (uint64_t page = 0; page < pages; ++page)
    {
        bytes[page * PageSize] = static_cast<uint8_t>(page);
    }
    for (uint64_t page = 0; page < pages; ++page)
    {
        if (bytes[page * PageSize] != static_cast<uint8_t>(page))
        {
            puts("memtest: heap corrupted after exhaustion");
            exit(1);
        }
    }
    if (sbrk(-static_cast<intptr_t>(pages * PageSize)) == HeapFailed || sbrk(0) != start ||
        GrowUntilRefused() != pages)
    {
        puts("memtest: shrinking the heap did not return its pages");
        exit(1);
    }
    printf("memtest: page limit reached after %llu heap pages, twice\n",
           static_cast<unsigned long long>(pages));
    exit(0);
}

} // namespace

constexpr char Help[] =
    "usage: memtest [overflow|exhaust]\n"
    "Grows and checks the stack and heap. overflow recurses into the stack guard\n"
    "page and is terminated; exhaust grows the heap until the page limit refuses more,\n"
    "gives it all back, and checks that it grows as far again.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "overflow") == 0)
    {
        FillStack(1000);
        puts("memtest: stack overflow was not caught");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "exhaust") == 0)
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
        puts("memtest: failed");
        return 1;
    }
    puts("memtest: stack and heap ok");
    return 0;
}
