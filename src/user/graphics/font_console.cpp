#include "font.h"

#include "kernel/framebuffer/console_font.h"

namespace FeatherOS::Graphics
{

// A separate object file, so only programs that call Console carry the font's bytes.
const Font* Font::Console()
{
    static Font font;
    static const Text::FontStatus status = font.LoadMemory(ConsoleFontData, ConsoleFontSize);
    return status == Text::FontStatus::Ok ? &font : nullptr;
}

} // namespace FeatherOS::Graphics
