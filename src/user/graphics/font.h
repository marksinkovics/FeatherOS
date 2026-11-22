#ifndef FEATHEROS_USER_GRAPHICS_FONT_H
#define FEATHEROS_USER_GRAPHICS_FONT_H

#include "../text/font_data.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __clang__
#define FEATHEROS_SWIFT_INTERIOR_POINTER __attribute__((swift_attr("import_unsafe")))
#else
#define FEATHEROS_SWIFT_INTERIOR_POINTER
#endif

namespace FeatherOS::Graphics
{

class Font
{
public:
    Font();
    Font(const Font&) = delete;
    Font(Font&& other) noexcept;
    Font& operator=(const Font&) = delete;
    Font& operator=(Font&& other) noexcept;
    ~Font();

    // An unreadable file gives Short with errno set; a file over 4 MiB gives Size.
    Text::FontStatus Load(const char* path);
    Text::FontStatus LoadNamed(const char* name);
    // Borrows the bytes, which must outlive the font.
    Text::FontStatus LoadMemory(const uint8_t* bytes, size_t size);
    static const Font* Console();

    bool Valid() const;
    int32_t LineHeight() const;
    int32_t Ascent() const;
    int32_t Descent() const;
    bool Monospace() const;
    int32_t CellWidth() const;
    // Zero for C0 controls and DEL, which draw nothing; a code point the font lacks has the width
    // of its missing box.
    int32_t Advance(uint32_t code_point) const;
    bool HasGlyph(uint32_t code_point) const;
    int32_t MeasureText(const char* utf8) const;
    FEATHEROS_SWIFT_INTERIOR_POINTER const char* Name() const;
    FEATHEROS_SWIFT_INTERIOR_POINTER const char* Notice() const;

    const Text::FontView& View() const;

private:
    void Reset();

    uint8_t* m_owned;
    Text::FontView m_view;
};

// The font `name` loaded into `storage`, else the console font, so a program that cannot read its
// fonts still draws; the result is `storage` only when the load worked.
const Font* LoadFontOrConsole(const char* name, Font* storage);

} // namespace FeatherOS::Graphics

#endif
