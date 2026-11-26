#include "command_line.h"
#include "graphics/display.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace
{

using FeatherOS::Graphics::Display;
using FeatherOS::Graphics::Status;

constexpr uint32_t Rounds = 200;
constexpr uint32_t QuickRounds = 3;
constexpr uint32_t Widths[] = {0, 640, 32};
constexpr uint32_t Heights[] = {0, 480, 32};
constexpr char Usage[] = "usage: fbbench [--quick]\n";
constexpr char Help[] =
    "usage: fbbench [--quick]\n"
    "Takes the screen of the console it runs on and times FB_PRESENT for the whole screen, a\n"
    "640x480 rectangle and a 32x32 one, printing 'fbbench: <W>x<H> <count> presents <us> us\n"
    "each' for each. --quick does 3 presents of each instead of 200.";

uint64_t NowNanoseconds()
{
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    bool quick = false;
    if (argc == 2 && strcmp(argv[1], "--quick") == 0)
    {
        quick = true;
    }
    else if (argc != 1)
    {
        fputs(Usage, stderr);
        return 1;
    }

    Display display;
    const Status opened = display.Open();
    if (opened == Status::OpenFailed)
    {
        printf("fbbench: /dev/fb0: %s\n", strerror(display.Error()));
        return 1;
    }
    if (opened == Status::AcquireFailed)
    {
        printf("fbbench: FB_ACQUIRE: %s\n", strerror(display.Error()));
        return 1;
    }
    if (opened != Status::Ok)
    {
        puts("fbbench: unsupported screen");
        return 1;
    }

    const int32_t screen_width = display.Width();
    const int32_t screen_height = display.Height();
    uint32_t* pixels = display.Screen()->Pixels();
    for (int32_t index = 0; index < screen_width * screen_height; ++index)
    {
        pixels[index] = 0x203040U + static_cast<uint32_t>(index);
    }

    const uint32_t rounds = quick ? QuickRounds : Rounds;
    for (uint32_t size = 0; size < 3; ++size)
    {
        int32_t width = Widths[size] == 0 ? screen_width : static_cast<int32_t>(Widths[size]);
        int32_t height = Heights[size] == 0 ? screen_height : static_cast<int32_t>(Heights[size]);
        width = width < screen_width ? width : screen_width;
        height = height < screen_height ? height : screen_height;
        const uint64_t start = NowNanoseconds();
        for (uint32_t round = 0; round < rounds; ++round)
        {
            display.Invalidate(0, 0, width, height);
            if (display.Present() != Status::Ok)
            {
                printf("fbbench: FB_PRESENT: %s\n", strerror(display.Error()));
                return 1;
            }
        }
        const uint64_t microseconds = (NowNanoseconds() - start) / 1000ULL / rounds;
        printf("fbbench: %dx%d %u presents %llu us each\n", width, height, rounds,
               static_cast<unsigned long long>(microseconds));
    }
    puts("fbbench: done");
    return 0;
}
