#include "command_line.h"
#include "graphics/display.h"
#include "graphics/font.h"

#include <errno.h>
#include <featheros/framebuffer.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace
{

using FeatherOS::Graphics::Display;
using FeatherOS::Graphics::Event;
using FeatherOS::Graphics::EventKind;
using FeatherOS::Graphics::Font;
using FeatherOS::Graphics::Status;
using FeatherOS::Graphics::Surface;
using FeatherOS::Text::FontStatus;

constexpr uint32_t ScreenWidth = 1024;
constexpr uint32_t ScreenHeight = 768;
constexpr char DefaultFont[] = "sans-24";
constexpr char Usage[] = "usage: fontview [name] | --card | --check PATH\n";
constexpr char Help[] =
    "usage: fontview [name] | --card | --check PATH\n"
    "fontview [name] shows the glyph table of /system/fonts/<name>.ffnt (default sans-24) on the\n"
    "screen of the console it runs on: PgUp and PgDn change the page, Esc or Ctrl+C quits.\n"
    "--card draws the fixed test card of every size, light on dark and dark on light, prints\n"
    "'fontview: card drawn' and waits for a key.\n"
    "--check PATH prints 'fontview: font ok glyphs G line L' or 'fontview: font rejected\n"
    "<reason>' (exit 1), and 'fontview: font unreadable <error>' for a file it cannot read.";

constexpr uint32_t CardDarkBackground = 0x101820;
constexpr uint32_t CardDarkText = 0xE8E8E8;
constexpr uint32_t CardLightBackground = 0xF4F0E8;
constexpr uint32_t CardLightText = 0x182030;
constexpr int32_t CardSplit = 384;
constexpr int32_t CardMargin = 16;
constexpr int32_t CardTop = 8;
constexpr int32_t CardGap = 4;
constexpr char CardSample[] = "Hamburgefonstiv 0123 \xC3\xA9";
constexpr char CardMissing[] = "Missing: \xCD\xB8";
constexpr const char* CardFonts[] = {"mono-16", "mono-20", "mono-24", "mono-32", "sans-16",
                                     "sans-20", "sans-24", "sans-32", "sans-48", "sans-64"};
constexpr uint32_t CardFontCount = sizeof(CardFonts) / sizeof(CardFonts[0]);
constexpr char CardMissingFont[] = "sans-24";

constexpr uint32_t PageBackground = 0x101820;
constexpr uint32_t PageText = 0xE8E8E8;
constexpr uint32_t PageDim = 0x8890A0;
constexpr int32_t PageColumns = 16;
constexpr int32_t PageCellWidth = 64;
constexpr int32_t PageHeaderHeight = 40;
constexpr int32_t PageCellPadding = 8;
constexpr uint32_t MissingGlyph = 0x378;
constexpr int PollMilliseconds = 500;

volatile sig_atomic_t Interrupted = 0;

void OnInterrupt(int)
{
    Interrupted = 1;
}

void InstallHandler()
{
    struct sigaction action = {};
    action.sa_handler = OnInterrupt;
    sigaction(SIGINT, &action, nullptr);
}

void Encode(uint32_t code_point, char* out)
{
    if (code_point < 0x80)
    {
        *out++ = static_cast<char>(code_point);
    }
    else if (code_point < 0x800)
    {
        *out++ = static_cast<char>(0xC0 | (code_point >> 6));
        *out++ = static_cast<char>(0x80 | (code_point & 0x3F));
    }
    else if (code_point < 0x10000)
    {
        *out++ = static_cast<char>(0xE0 | (code_point >> 12));
        *out++ = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        *out++ = static_cast<char>(0x80 | (code_point & 0x3F));
    }
    else
    {
        *out++ = static_cast<char>(0xF0 | (code_point >> 18));
        *out++ = static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        *out++ = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        *out++ = static_cast<char>(0x80 | (code_point & 0x3F));
    }
    *out = '\0';
}

bool OpenDisplay(Display* display)
{
    const Status opened = display->Open();
    if (opened == Status::Ok)
    {
        if (display->Width() == ScreenWidth && display->Height() == ScreenHeight)
        {
            return true;
        }
        puts("fontview: unsupported screen");
        display->Close();
        return false;
    }
    if (opened == Status::OpenFailed)
    {
        printf("fontview: /dev/fb0: %s\n", strerror(display->Error()));
    }
    else if (opened == Status::AcquireFailed)
    {
        printf("fontview: FB_ACQUIRE: %s\n", strerror(display->Error()));
    }
    else
    {
        puts("fontview: cannot open the display");
    }
    return false;
}

bool PresentAll(Display* display)
{
    display->InvalidateAll();
    if (display->Present() == Status::Ok)
    {
        return true;
    }
    printf("fontview: FB_PRESENT: %s\n", strerror(display->Error()));
    return false;
}

enum class Key : uint8_t
{
    None,
    Quit,
    Previous,
    Next,
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
        if (event.code == KEY_ESC)
        {
            return Key::Quit;
        }
        if (event.code == KEY_PAGEUP)
        {
            return Key::Previous;
        }
        if (event.code == KEY_PAGEDOWN)
        {
            return Key::Next;
        }
    }
    return Key::Quit;
}

int Check(const char* path)
{
    Font font;
    errno = 0;
    const FontStatus status = font.Load(path);
    if (status == FontStatus::Ok)
    {
        printf("fontview: font ok glyphs %u line %d\n",
               static_cast<unsigned>(font.View().GlyphCount()), font.LineHeight());
        return 0;
    }
    if (status == FontStatus::Short && errno != 0)
    {
        printf("fontview: font unreadable %s\n", strerror(errno));
        return 1;
    }
    printf("fontview: font rejected %s\n", FeatherOS::Text::FontStatusName(status));
    return 1;
}

bool LoadFont(Font* font, const char* name)
{
    const FontStatus status = font->LoadNamed(name);
    if (status == FontStatus::Ok)
    {
        return true;
    }
    printf("fontview: %s: %s\n", name, FeatherOS::Text::FontStatusName(status));
    return false;
}

void DrawCardLines(Surface* screen, const Font* fonts, uint32_t background, uint32_t text,
                   int32_t top)
{
    screen->FillRectangle(0, top, ScreenWidth, CardSplit, background);
    int32_t y = top + CardTop;
    for (uint32_t index = 0; index < CardFontCount; ++index)
    {
        screen->DrawText(&fonts[index], CardMargin, y, CardSample, text);
        y += fonts[index].LineHeight() + CardGap;
    }
    screen->DrawText(&fonts[CardFontCount], CardMargin, y, CardMissing, text);
}

int Card()
{
    Font fonts[CardFontCount + 1];
    for (uint32_t index = 0; index < CardFontCount; ++index)
    {
        if (!LoadFont(&fonts[index], CardFonts[index]))
        {
            return 1;
        }
    }
    if (!LoadFont(&fonts[CardFontCount], CardMissingFont))
    {
        return 1;
    }
    Display display;
    if (!OpenDisplay(&display))
    {
        return 1;
    }
    InstallHandler();
    Surface* screen = display.Screen();
    DrawCardLines(screen, fonts, CardDarkBackground, CardDarkText, 0);
    DrawCardLines(screen, fonts, CardLightBackground, CardLightText, CardSplit);
    bool ok = PresentAll(&display);
    if (ok)
    {
        puts("fontview: card drawn");
        Key key = WaitKey(&display);
        while (key == Key::Redraw)
        {
            ok = PresentAll(&display);
            key = ok ? WaitKey(&display) : Key::Quit;
        }
        ok = ok && key != Key::Failed;
    }
    display.Close();
    return ok ? 0 : 1;
}

void DrawPage(Surface* screen, const Font* font, const Font* console, const char* name,
              uint32_t page, uint32_t pages, uint32_t per_page)
{
    screen->Fill(PageBackground);
    const uint32_t count = font->View().GlyphCount();
    const uint32_t first = page * per_page;
    uint32_t last = first + per_page;
    last = last < count ? last : count;
    char header[96];
    snprintf(header, sizeof(header), "%s  page %u of %u  glyphs %u-%u of %u  PgUp PgDn Esc", name,
             page + 1, pages, first + 1, last, count);
    screen->DrawText(console, 16, 8, header, PageText);
    const int32_t cell_height = font->LineHeight() + 2 * PageCellPadding;
    for (uint32_t index = first; index < last; ++index)
    {
        const uint32_t slot = index - first;
        const int32_t x = static_cast<int32_t>(slot % PageColumns) * PageCellWidth;
        const int32_t y = PageHeaderHeight + static_cast<int32_t>(slot / PageColumns) * cell_height;
        const uint32_t code_point =
            index == 0 ? MissingGlyph : font->View().GlyphAt(index).code_point;
        char text[5];
        Encode(code_point, text);
        const int32_t advance = font->Advance(code_point);
        screen->DrawText(font, x + (PageCellWidth - advance) / 2, y + PageCellPadding, text,
                         index == 0 ? PageDim : PageText);
    }
}

int Pages(const char* name)
{
    Font font;
    if (!LoadFont(&font, name))
    {
        return 1;
    }
    Font console_storage;
    const Font* console = FeatherOS::Graphics::LoadFontOrConsole("mono-16", &console_storage);
    Display display;
    if (!OpenDisplay(&display))
    {
        return 1;
    }
    InstallHandler();
    const int32_t cell_height = font.LineHeight() + 2 * PageCellPadding;
    int32_t rows = (static_cast<int32_t>(ScreenHeight) - PageHeaderHeight) / cell_height;
    rows = rows > 0 ? rows : 1;
    const uint32_t per_page = static_cast<uint32_t>(rows) * PageColumns;
    const uint32_t count = font.View().GlyphCount();
    const uint32_t pages = (count + per_page - 1) / per_page;
    uint32_t page = 0;
    bool ok = true;
    bool running = true;
    while (running && ok)
    {
        DrawPage(display.Screen(), &font, console, name, page, pages, per_page);
        ok = PresentAll(&display);
        const Key key = ok ? WaitKey(&display) : Key::Quit;
        if (key == Key::Quit)
        {
            running = false;
        }
        else if (key == Key::Failed)
        {
            ok = false;
        }
        else if (key == Key::Previous && page > 0)
        {
            --page;
        }
        else if (key == Key::Next && page + 1 < pages)
        {
            ++page;
        }
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
    if (argc == 3 && strcmp(argv[1], "--check") == 0)
    {
        return Check(argv[2]);
    }
    if (argc == 2 && strcmp(argv[1], "--card") == 0)
    {
        return Card();
    }
    if (argc == 1)
    {
        return Pages(DefaultFont);
    }
    if (argc == 2 && argv[1][0] != '-')
    {
        return Pages(argv[1]);
    }
    fputs(Usage, stderr);
    return 1;
}
