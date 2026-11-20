#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <featheros/framebuffer.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace
{

constexpr uint32_t ScreenWidth = 1024;
constexpr uint32_t ScreenHeight = 768;
constexpr uint32_t BarHeight = ScreenHeight / 2;
constexpr uint32_t BarWidth = ScreenWidth / 8;
constexpr uint32_t Bars[8] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF,
                              0xFFFF00, 0x00FFFF, 0xFF00FF, 0x808080};
constexpr uint32_t CornerColor = 0x654321;
constexpr uint32_t CornerSize = 20;
constexpr int32_t CornerOrigin = -10;
constexpr uint32_t PatchColor = 0x123456;
constexpr uint32_t PatchSize = 100;
constexpr int32_t PatchX = 1000;
constexpr int32_t PatchY = 700;
constexpr unsigned long long DefaultSeconds = 5;
constexpr unsigned long long MaximumSeconds = 3600;
constexpr char Help[] =
    "usage: fbdemo [seconds]\n"
    "Takes the screen of the console it runs on and draws a test pattern: eight color bars\n"
    "over a gradient, with two rectangles clipped at the corners. Type r and Enter to draw\n"
    "it again, q and Enter to quit; with no input for the given seconds (default 5) it gives\n"
    "the screen back.";

bool Present(int descriptor, const uint32_t* pixels, uint32_t stride, int32_t x, int32_t y,
             uint32_t width, uint32_t height)
{
    const fb_present request = {pixels, stride, x, y, width, height, 0};
    return ioctl(descriptor, FB_PRESENT, &request) == 0;
}

void FillPattern(uint32_t* screen)
{
    for (uint32_t y = 0; y < ScreenHeight; ++y)
    {
        for (uint32_t x = 0; x < ScreenWidth; ++x)
        {
            screen[y * ScreenWidth + x] =
                y < BarHeight ? Bars[x / BarWidth] : ((x / 4) << 16) | (((y - BarHeight) / 2) << 8);
        }
    }
}

bool PresentAll(int descriptor, const uint32_t* screen)
{
    static uint32_t corner[CornerSize * CornerSize];
    static uint32_t patch[PatchSize * PatchSize];
    for (uint32_t& pixel : corner)
    {
        pixel = CornerColor;
    }
    for (uint32_t& pixel : patch)
    {
        pixel = PatchColor;
    }
    return Present(descriptor, screen, ScreenWidth, 0, 0, ScreenWidth, ScreenHeight) &&
           Present(descriptor, corner, CornerSize, CornerOrigin, CornerOrigin, CornerSize,
                   CornerSize) &&
           Present(descriptor, patch, PatchSize, PatchX, PatchY, PatchSize, PatchSize);
}

} // namespace

int main(int argc, char** argv)
{
    using FeatherOS::User::ParseUnsigned;
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    unsigned long long seconds = DefaultSeconds;
    if (argc > 2 || (argc == 2 && (!ParseUnsigned(argv[1], &seconds) || seconds == 0 ||
                                   seconds > MaximumSeconds)))
    {
        fputs("usage: fbdemo [seconds]\n", stderr);
        return 1;
    }

    const int descriptor = open("/dev/fb0", O_RDWR);
    if (descriptor < 0)
    {
        printf("fbdemo: /dev/fb0: %s\n", strerror(errno));
        return 1;
    }
    fb_info info = {};
    if (ioctl(descriptor, FB_GET_INFO, &info) != 0 || info.width != ScreenWidth ||
        info.height != ScreenHeight)
    {
        puts("fbdemo: unsupported screen");
        return 1;
    }
    if (ioctl(descriptor, FB_ACQUIRE) != 0)
    {
        printf("fbdemo: FB_ACQUIRE: %s\n", strerror(errno));
        return 1;
    }
    auto* screen = static_cast<uint32_t*>(
        malloc(static_cast<size_t>(ScreenWidth) * ScreenHeight * sizeof(uint32_t)));
    if (screen == nullptr)
    {
        ioctl(descriptor, FB_RELEASE);
        puts("fbdemo: out of memory");
        return 1;
    }
    FillPattern(screen);

    printf("fbdemo: tty%u %ux%u\n", info.console, info.width, info.height);
    bool failed = !PresentAll(descriptor, screen);
    if (!failed)
    {
        puts("fbdemo: presented");
    }
    while (!failed)
    {
        pollfd input = {STDIN_FILENO, POLLIN, 0};
        if (poll(&input, 1, static_cast<int>(seconds * 1000)) <= 0)
        {
            break;
        }
        char line[16];
        const ssize_t length = read(STDIN_FILENO, line, sizeof(line));
        if (length <= 0 || line[0] == 'q')
        {
            break;
        }
        if (line[0] == 'r')
        {
            failed = !PresentAll(descriptor, screen);
            if (!failed)
            {
                puts("fbdemo: presented");
            }
        }
    }
    free(screen);
    ioctl(descriptor, FB_RELEASE);
    if (failed)
    {
        printf("fbdemo: FB_PRESENT: %s\n", strerror(errno));
        return 1;
    }
    puts("fbdemo: done");
    return 0;
}
