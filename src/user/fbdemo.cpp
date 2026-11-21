#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <featheros/framebuffer.h>
#include <poll.h>
#include <signal.h>
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
constexpr int DrainMilliseconds = 200;
constexpr int InterruptedStatus = 130;
constexpr unsigned PauseSeconds = 3;
constexpr char Usage[] = "usage: fbdemo [--keys] [seconds]\n";
constexpr char Help[] =
    "usage: fbdemo [--keys] [seconds]\n"
    "Takes the screen of the console it runs on and draws a test pattern: eight color bars\n"
    "over a gradient, with two rectangles clipped at the corners. Type r and Enter to draw\n"
    "it again, q and Enter to quit; with no input for the given seconds (default 5) it gives\n"
    "the screen back.\n"
    "With --keys it also prints the console's key events as 'fbdemo: event <kind> <code>\n"
    "<modifiers>' and draws again when the console is shown. Type p and Enter to stop reading\n"
    "events for 3 seconds; Ctrl+C ends it with status 130.";

volatile sig_atomic_t Interrupted = 0;

void OnInterrupt(int)
{
    Interrupted = 1;
}

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

const char* KindName(uint8_t kind)
{
    switch (kind)
    {
    case FB_EVENT_PRESS:
        return "press";
    case FB_EVENT_REPEAT:
        return "repeat";
    case FB_EVENT_RELEASE:
        return "release";
    case FB_EVENT_SHOWN:
        return "shown";
    case FB_EVENT_HIDDEN:
        return "hidden";
    default:
        return "unknown";
    }
}

struct EventReader
{
    int descriptor;
    const uint32_t* screen;
    uint32_t last_time = 0;
    bool failed = false;

    void Read()
    {
        fb_event events[16];
        const ssize_t length = read(descriptor, events, sizeof(events));
        if (length < 0)
        {
            failed = errno != EAGAIN && errno != EINTR;
            return;
        }
        for (size_t index = 0; index < static_cast<size_t>(length) / sizeof(fb_event); ++index)
        {
            Handle(events[index]);
        }
    }

    void Handle(const fb_event& event)
    {
        if (event.time_ms < last_time)
        {
            puts("fbdemo: time went backwards");
        }
        last_time = event.time_ms;
        printf("fbdemo: event %s %u %u\n", KindName(event.kind), event.code, event.modifiers);
        if (event.kind == FB_EVENT_SHOWN)
        {
            if (PresentAll(descriptor, screen))
            {
                puts("fbdemo: presented");
            }
            else
            {
                failed = true;
            }
        }
    }

    void Drain()
    {
        for (;;)
        {
            pollfd events = {descriptor, POLLIN, 0};
            if (poll(&events, 1, DrainMilliseconds) <= 0 || failed)
            {
                return;
            }
            Read();
        }
    }
};

bool ParseArguments(int argc, char** argv, bool* keys, unsigned long long* seconds)
{
    using FeatherOS::User::ParseUnsigned;
    int next = 1;
    if (next < argc && strcmp(argv[next], "--keys") == 0)
    {
        *keys = true;
        ++next;
    }
    if (next < argc)
    {
        if (!ParseUnsigned(argv[next], seconds) || *seconds == 0 || *seconds > MaximumSeconds)
        {
            return false;
        }
        ++next;
    }
    return next == argc;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    bool keys = false;
    unsigned long long seconds = DefaultSeconds;
    if (!ParseArguments(argc, argv, &keys, &seconds))
    {
        fputs(Usage, stderr);
        return 1;
    }

    const int descriptor = open("/dev/fb0", O_RDWR | (keys ? O_NONBLOCK : 0));
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
    if (keys)
    {
        struct sigaction action = {};
        action.sa_handler = OnInterrupt;
        sigaction(SIGINT, &action, nullptr);
    }
    EventReader reader = {descriptor, screen};
    bool failed = !PresentAll(descriptor, screen);
    if (!failed)
    {
        puts("fbdemo: presented");
    }
    while (!failed && !reader.failed && Interrupted == 0)
    {
        pollfd inputs[2] = {{STDIN_FILENO, POLLIN, 0}, {descriptor, POLLIN, 0}};
        const int ready = poll(inputs, keys ? 2 : 1, static_cast<int>(seconds * 1000));
        if (ready < 0 && errno == EINTR)
        {
            continue;
        }
        if (ready <= 0)
        {
            break;
        }
        if (keys && (inputs[1].revents & (POLLIN | POLLERR)) != 0)
        {
            reader.Read();
        }
        if ((inputs[0].revents & (POLLIN | POLLHUP)) == 0)
        {
            continue;
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
        else if (keys && line[0] == 'p')
        {
            puts("fbdemo: paused");
            sleep(PauseSeconds);
            puts("fbdemo: resumed");
        }
    }
    failed = failed || reader.failed;
    if (keys && Interrupted != 0 && !failed)
    {
        reader.Drain();
        failed = reader.failed;
        puts("fbdemo: interrupted");
    }
    if (keys)
    {
        fb_info final = {};
        if (ioctl(descriptor, FB_GET_INFO, &final) == 0)
        {
            printf("fbdemo: dropped %u\n", final.dropped_events);
        }
    }
    free(screen);
    ioctl(descriptor, FB_RELEASE);
    if (failed)
    {
        printf("fbdemo: FB_PRESENT: %s\n", strerror(errno));
        return 1;
    }
    if (Interrupted != 0)
    {
        return InterruptedStatus;
    }
    puts("fbdemo: done");
    return 0;
}
