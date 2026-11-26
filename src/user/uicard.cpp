#include "command_line.h"
#include "graphics/display.h"
#include "ui/card.h"

#include <featheros/framebuffer.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace
{

using FeatherOS::Graphics::Canvas;
using FeatherOS::Graphics::Display;
using FeatherOS::Graphics::Event;
using FeatherOS::Graphics::EventKind;
using FeatherOS::Graphics::Status;
using FeatherOS::UI::AccentId;
using FeatherOS::UI::ThemeId;

constexpr char Usage[] = "usage: uicard [--page N] [--theme id] [--accent id]\n";
constexpr char Help[] =
    "usage: uicard [--page N] [--theme id] [--accent id]\n"
    "Takes the screen of the console it runs on and draws page N (1 to 4, default 1) of the\n"
    "desktop's test card: 1 colours, shapes and type, 2 glyphs and the feather, 3 app tiles and\n"
    "pointers, 4 the wallpaper with the shadows. The theme is one of dusk, airy, ink, paper,\n"
    "ember, terra, hcd, hcl (default dusk); the accent default, blue, teal, green, amber, coral,\n"
    "pink or violet. It prints 'uicard: page N drawn in T ms' and waits: Left and Right change\n"
    "the page, Up and Down the theme, Esc, q or Ctrl+C quits and gives the screen back.";
constexpr int PollMilliseconds = 500;

volatile sig_atomic_t Interrupted = 0;

void OnInterrupt(int)
{
    Interrupted = 1;
}

unsigned long long Milliseconds()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<unsigned long long>(now.tv_sec) * 1000 +
           static_cast<unsigned long long>(now.tv_nsec) / 1000000;
}

struct Options
{
    uint32_t page = 1;
    ThemeId theme = ThemeId::Dusk;
    AccentId accent = AccentId::Default;
};

bool ParseOptions(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; i += 2)
    {
        if (i + 1 >= argc)
        {
            return false;
        }
        const char* key = argv[i];
        const char* value = argv[i + 1];
        if (strcmp(key, "--page") == 0)
        {
            unsigned long long page = 0;
            if (!FeatherOS::User::ParseUnsigned(value, &page) || page < 1 ||
                page > FeatherOS::UI::CardPageCount)
            {
                return false;
            }
            options->page = static_cast<uint32_t>(page);
        }
        else if (strcmp(key, "--theme") == 0)
        {
            if (!FeatherOS::UI::ParseThemeId(value, &options->theme))
            {
                return false;
            }
        }
        else if (strcmp(key, "--accent") == 0)
        {
            if (!FeatherOS::UI::ParseAccentId(value, &options->accent))
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    return true;
}

enum class Key : uint8_t
{
    Quit,
    Previous,
    Next,
    ThemeUp,
    ThemeDown,
    Redraw,
    Failed,
};

Key WaitKey(Display* display)
{
    while (Interrupted == 0)
    {
        Event event = {};
        const Status status = display->WaitEvent(PollMilliseconds, &event);
        if (status == Status::Timeout || status == Status::Interrupted)
        {
            continue;
        }
        if (status != Status::Ok)
        {
            return Key::Failed;
        }
        if (event.kind == EventKind::Shown)
        {
            return Key::Redraw;
        }
        if (event.kind != EventKind::Press)
        {
            continue;
        }
        if (event.code == KEY_C && (event.modifiers & FB_MOD_CONTROL) != 0)
        {
            return Key::Quit;
        }
        switch (event.code)
        {
        case KEY_ESC:
        case KEY_Q:
            return Key::Quit;
        case KEY_LEFT:
            return Key::Previous;
        case KEY_RIGHT:
            return Key::Next;
        case KEY_UP:
            return Key::ThemeUp;
        case KEY_DOWN:
            return Key::ThemeDown;
        default:
            break;
        }
    }
    return Key::Quit;
}

bool OpenDisplay(Display* display)
{
    const Status opened = display->Open();
    if (opened == Status::Ok)
    {
        return true;
    }
    if (opened == Status::OpenFailed)
    {
        printf("uicard: /dev/fb0: %s\n", strerror(display->Error()));
    }
    else if (opened == Status::AcquireFailed)
    {
        printf("uicard: FB_ACQUIRE: %s\n", strerror(display->Error()));
    }
    else
    {
        puts("uicard: cannot open the display");
    }
    return false;
}

bool Draw(Display* display, const Options& options, bool report)
{
    const unsigned long long start = Milliseconds();
    Canvas canvas(*display->Screen());
    FeatherOS::UI::DrawCard(canvas, display->Width(), display->Height(), options.page,
                            options.theme, options.accent);
    const unsigned long long elapsed = Milliseconds() - start;
    display->InvalidateAll();
    if (display->Present() != Status::Ok)
    {
        printf("uicard: FB_PRESENT: %s\n", strerror(display->Error()));
        return false;
    }
    if (report)
    {
        printf("uicard: page %u drawn in %llu ms\n", options.page, elapsed);
    }
    return true;
}

int Run(Options options)
{
    Display display;
    if (!OpenDisplay(&display))
    {
        return 1;
    }
    struct sigaction action = {};
    action.sa_handler = OnInterrupt;
    sigaction(SIGINT, &action, nullptr);
    bool ok = Draw(&display, options, true);
    while (ok)
    {
        const Key key = WaitKey(&display);
        bool report = true;
        if (key == Key::Quit)
        {
            break;
        }
        if (key == Key::Failed)
        {
            ok = false;
            break;
        }
        const uint32_t themes = FeatherOS::UI::ThemeCount;
        if (key == Key::Previous && options.page > 1)
        {
            --options.page;
        }
        else if (key == Key::Next && options.page < FeatherOS::UI::CardPageCount)
        {
            ++options.page;
        }
        else if (key == Key::ThemeUp)
        {
            options.theme =
                static_cast<ThemeId>((static_cast<uint32_t>(options.theme) + 1) % themes);
        }
        else if (key == Key::ThemeDown)
        {
            options.theme =
                static_cast<ThemeId>((static_cast<uint32_t>(options.theme) + themes - 1) % themes);
        }
        else if (key != Key::Redraw)
        {
            continue;
        }
        if (key == Key::Redraw)
        {
            report = false;
        }
        ok = Draw(&display, options, report);
    }
    display.Close();
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    Options options;
    if (!ParseOptions(argc, argv, &options))
    {
        fputs(Usage, stderr);
        return 1;
    }
    return Run(options);
}
