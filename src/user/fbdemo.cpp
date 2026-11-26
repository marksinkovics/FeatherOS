#include "command_line.h"
#include "graphics/display.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace
{

using FeatherOS::Graphics::Display;
using FeatherOS::Graphics::Event;
using FeatherOS::Graphics::EventKind;
using FeatherOS::Graphics::Status;
using FeatherOS::Graphics::Surface;

constexpr uint32_t BarCount = 8;
constexpr uint32_t Bars[BarCount] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF,
                                     0xFFFF00, 0x00FFFF, 0xFF00FF, 0x808080};
constexpr uint32_t CornerColor = 0x654321;
constexpr uint32_t CornerSize = 20;
constexpr int32_t CornerOrigin = -10;
constexpr uint32_t PatchColor = 0x123456;
constexpr uint32_t PatchSize = 100;
constexpr int32_t PatchRightMargin = 24;
constexpr int32_t PatchBottomMargin = 68;
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

// The gradient's channels wrap at 256, so a screen wider than 1024 or taller than 768 stays valid.
void FillPattern(Surface* screen)
{
    const uint32_t width = static_cast<uint32_t>(screen->Width());
    const uint32_t height = static_cast<uint32_t>(screen->Height());
    const uint32_t bar_height = height / 2;
    const uint32_t bar_width = width / BarCount;
    uint32_t* pixels = screen->Pixels();
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t bar = x / bar_width < BarCount ? x / bar_width : BarCount - 1;
            pixels[y * width + x] =
                y < bar_height ? Bars[bar]
                               : (((x / 4) & 0xFF) << 16) | ((((y - bar_height) / 2) & 0xFF) << 8);
        }
    }
    screen->FillRectangle(CornerOrigin, CornerOrigin, CornerSize, CornerSize, CornerColor);
    screen->FillRectangle(screen->Width() - PatchRightMargin, screen->Height() - PatchBottomMargin,
                          PatchSize, PatchSize, PatchColor);
}

bool PresentAll(Display* display)
{
    display->InvalidateAll();
    return display->Present() == Status::Ok;
}

const char* KindName(EventKind kind)
{
    switch (kind)
    {
    case EventKind::Press:
        return "press";
    case EventKind::Repeat:
        return "repeat";
    case EventKind::Release:
        return "release";
    case EventKind::Shown:
        return "shown";
    case EventKind::Hidden:
        return "hidden";
    default:
        return "unknown";
    }
}

struct EventReader
{
    Display* display;
    uint32_t last_time = 0;
    bool failed = false;

    void Read()
    {
        Event event = {};
        Status status = Status::Timeout;
        while ((status = display->WaitEvent(0, &event)) == Status::Ok)
        {
            Handle(event);
        }
        failed = failed || (status != Status::Timeout && status != Status::Interrupted);
    }

    void Handle(const Event& event)
    {
        if (event.time_ms < last_time)
        {
            puts("fbdemo: time went backwards");
        }
        last_time = event.time_ms;
        printf("fbdemo: event %s %u %u\n", KindName(event.kind), event.code, event.modifiers);
        if (event.kind == EventKind::Shown)
        {
            if (PresentAll(display))
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
        Event event = {};
        Status status = Status::Timeout;
        while (!failed && (status = display->WaitEvent(DrainMilliseconds, &event)) == Status::Ok)
        {
            Handle(event);
        }
        failed = failed || (status != Status::Timeout && status != Status::Interrupted);
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

    Display display;
    const Status opened = display.Open();
    if (opened == Status::OpenFailed)
    {
        printf("fbdemo: /dev/fb0: %s\n", strerror(display.Error()));
        return 1;
    }
    if (opened == Status::AcquireFailed)
    {
        printf("fbdemo: FB_ACQUIRE: %s\n", strerror(display.Error()));
        return 1;
    }
    if (opened == Status::OutOfMemory)
    {
        puts("fbdemo: out of memory");
        return 1;
    }
    if (opened != Status::Ok || display.Width() < static_cast<int32_t>(BarCount))
    {
        puts("fbdemo: unsupported screen");
        return 1;
    }
    FillPattern(display.Screen());

    printf("fbdemo: tty%u %dx%d\n", display.Console(), display.Width(), display.Height());
    if (keys)
    {
        struct sigaction action = {};
        action.sa_handler = OnInterrupt;
        sigaction(SIGINT, &action, nullptr);
    }
    EventReader reader = {&display};
    bool failed = !PresentAll(&display);
    if (!failed)
    {
        puts("fbdemo: presented");
    }
    while (!failed && !reader.failed && Interrupted == 0)
    {
        pollfd inputs[2] = {{STDIN_FILENO, POLLIN, 0}, {display.Descriptor(), POLLIN, 0}};
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
            failed = !PresentAll(&display);
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
        uint32_t dropped = 0;
        if (display.DroppedEvents(&dropped) == Status::Ok)
        {
            printf("fbdemo: dropped %u\n", dropped);
        }
    }
    display.Close();
    if (failed)
    {
        printf("fbdemo: FB_PRESENT: %s\n", strerror(display.Error()));
        return 1;
    }
    if (Interrupted != 0)
    {
        return InterruptedStatus;
    }
    puts("fbdemo: done");
    return 0;
}
