// Host renderer for the desktop's artwork: the same code the programs run, writing files the
// checks in tools/gui/ compare against the design. Subcommands:
//
//   guirender pointer --name N --size S --accent RRGGBB [--frame F] --system DIR --out FILE
//   guirender card --size WxH --page N --theme ID --accent ID --system DIR --out FILE
//
// DIR is a folder laid out like /system (gui/outlines.fvo and fonts/ inside).
//
// A pointer is written as a PAM image (RGB_ALPHA) holding premultiplied colour. A card is the
// binary PPM that QEMU's screendump writes for the same screen.
#include "user/graphics/canvas.h"
#include "user/graphics/surface.h"
#include "user/ui/art.h"
#include "user/ui/card.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Graphics = FeatherOS::Graphics;
namespace UI = FeatherOS::UI;

namespace
{

struct PointerName
{
    const char* name;
    UI::PointerId id;
};

// The names of docs/design/gui/cursors/cursors.json.
constexpr PointerName PointerNames[] = {
    {"default", UI::PointerId::Arrow},
    {"pointer", UI::PointerId::Link},
    {"text", UI::PointerId::Text},
    {"progress", UI::PointerId::Working},
    {"wait", UI::PointerId::Busy},
    {"grab", UI::PointerId::Move},
    {"grabbing", UI::PointerId::Moving},
    {"ns", UI::PointerId::ResizeVertical},
    {"ew", UI::PointerId::ResizeHorizontal},
    {"nwse", UI::PointerId::ResizeNwse},
    {"nesw", UI::PointerId::ResizeNesw},
    {"no", UI::PointerId::NotAllowed},
    {"cross", UI::PointerId::Precise},
};

struct Options
{
    std::string name;
    std::string out;
    std::string system;
    std::string theme = "dusk";
    // RRGGBB for a pointer, an accent id for a card.
    std::string accent_text;
    uint32_t size = 0;
    uint32_t height = 0;
    uint32_t frame = 0;
    uint32_t page = 1;
};

[[noreturn]] void Fail(const char* message)
{
    std::fprintf(stderr, "guirender: %s\n", message);
    std::exit(1);
}

bool ParseNumber(const char* text, int base, uint32_t& value)
{
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, base);
    if (end == text || *end != '\0' || parsed > 0xFFFFFFFFUL)
    {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

// "S" or "WxH".
bool ParseSize(const std::string& text, Options& options)
{
    const std::size_t cross = text.find('x');
    if (cross == std::string::npos)
    {
        return ParseNumber(text.c_str(), 10, options.size);
    }
    return ParseNumber(text.substr(0, cross).c_str(), 10, options.size) &&
           ParseNumber(text.substr(cross + 1).c_str(), 10, options.height);
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 2; i < argc; i += 2)
    {
        if (i + 1 >= argc)
        {
            Fail("an option without a value");
        }
        const char* key = argv[i];
        const char* value = argv[i + 1];
        bool ok = true;
        if (std::strcmp(key, "--name") == 0)
        {
            options.name = value;
        }
        else if (std::strcmp(key, "--out") == 0)
        {
            options.out = value;
        }
        else if (std::strcmp(key, "--system") == 0)
        {
            options.system = value;
        }
        else if (std::strcmp(key, "--size") == 0)
        {
            ok = ParseSize(value, options);
        }
        else if (std::strcmp(key, "--accent") == 0)
        {
            options.accent_text = value;
        }
        else if (std::strcmp(key, "--theme") == 0)
        {
            options.theme = value;
        }
        else if (std::strcmp(key, "--page") == 0)
        {
            ok = ParseNumber(value, 10, options.page);
        }
        else if (std::strcmp(key, "--frame") == 0)
        {
            ok = ParseNumber(value, 10, options.frame);
        }
        else
        {
            Fail("unknown option");
        }
        if (!ok)
        {
            Fail("a bad option value");
        }
    }
    return options;
}

void LoadSystemArt(const std::string& system)
{
    const std::string path = system + "/gui/outlines.fvo";
    if (UI::LoadArt(path.c_str()) != Graphics::OutlineStatus::Ok)
    {
        Fail("cannot load the outlines file");
    }
}

bool WritePam(const std::string& path, const Graphics::Surface& surface)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        return false;
    }
    std::fprintf(file, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                 surface.Width(), surface.Height());
    for (int32_t y = 0; y < surface.Height(); ++y)
    {
        for (int32_t x = 0; x < surface.Width(); ++x)
        {
            const uint32_t pixel = surface.PixelAt(x, y);
            const unsigned char bytes[4] = {
                static_cast<unsigned char>(pixel >> 16), static_cast<unsigned char>(pixel >> 8),
                static_cast<unsigned char>(pixel), static_cast<unsigned char>(pixel >> 24)};
            std::fwrite(bytes, 1, 4, file);
        }
    }
    return std::fclose(file) == 0;
}

int RenderPointer(const Options& options)
{
    uint32_t accent = 0xF0A840;
    if (!options.accent_text.empty() &&
        (!ParseNumber(options.accent_text.c_str(), 16, accent) || accent > 0xFFFFFF))
    {
        Fail("--accent is not RRGGBB");
    }
    const PointerName* found = nullptr;
    for (const PointerName& entry : PointerNames)
    {
        if (options.name == entry.name)
        {
            found = &entry;
        }
    }
    if (found == nullptr)
    {
        Fail("--name is not a pointer");
    }
    if (options.size == 0 || options.size > 256 || options.out.empty() ||
        options.system.empty())
    {
        Fail("--size (1 to 256), --system and --out are required");
    }
    LoadSystemArt(options.system);
    const int32_t size = static_cast<int32_t>(options.size);
    Graphics::Surface surface(size, size, Graphics::PixelFormat::PremultipliedArgb);
    surface.Fill(0);
    Graphics::Canvas canvas(surface);
    const UI::PointerPoint hotspot = UI::PointerHotspot(found->id, size);
    UI::DrawPointer(canvas, found->id, hotspot.x, hotspot.y, size, 0xFF000000U | accent,
                    options.frame);
    if (!WritePam(options.out, surface))
    {
        Fail("cannot write --out");
    }
    return 0;
}

bool WritePpm(const std::string& path, const Graphics::Surface& surface)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", surface.Width(), surface.Height());
    std::vector<unsigned char> row(static_cast<std::size_t>(surface.Width()) * 3);
    for (int32_t y = 0; y < surface.Height(); ++y)
    {
        for (int32_t x = 0; x < surface.Width(); ++x)
        {
            const uint32_t pixel = surface.PixelAt(x, y);
            unsigned char* out = &row[static_cast<std::size_t>(x) * 3];
            out[0] = static_cast<unsigned char>(pixel >> 16);
            out[1] = static_cast<unsigned char>(pixel >> 8);
            out[2] = static_cast<unsigned char>(pixel);
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    return std::fclose(file) == 0;
}

int RenderCard(const Options& options)
{
    UI::ThemeId theme = UI::ThemeId::Dusk;
    UI::AccentId accent = UI::AccentId::Default;
    if (!UI::ParseThemeId(options.theme.c_str(), &theme))
    {
        Fail("--theme is not a theme id");
    }
    if (!options.accent_text.empty() && !UI::ParseAccentId(options.accent_text.c_str(), &accent))
    {
        Fail("--accent is not an accent id");
    }
    if (options.size == 0 || options.height == 0 || options.size > 4096 || options.height > 4096 ||
        options.page < 1 || options.page > UI::CardPageCount || options.out.empty() ||
        options.system.empty())
    {
        Fail("--size WxH, --page (1 to 4), --system and --out are required");
    }
    LoadSystemArt(options.system);
    const std::string fonts = options.system + "/fonts/";
    UI::SetCardFontDirectory(fonts.c_str());
    const int32_t width = static_cast<int32_t>(options.size);
    const int32_t height = static_cast<int32_t>(options.height);
    Graphics::Surface surface(width, height);
    Graphics::Canvas canvas(surface);
    UI::DrawCard(canvas, width, height, options.page, theme, accent);
    if (!WritePpm(options.out, surface))
    {
        Fail("cannot write --out");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        Fail("usage: guirender pointer|card --size S --system DIR --out FILE ...");
    }
    const Options options = ParseOptions(argc, argv);
    if (std::strcmp(argv[1], "pointer") == 0)
    {
        return RenderPointer(options);
    }
    if (std::strcmp(argv[1], "card") == 0)
    {
        return RenderCard(options);
    }
    Fail("unknown subcommand");
}
