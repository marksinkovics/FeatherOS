#include "font.h"

#include "../text/utf8.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace FeatherOS::Graphics
{

namespace
{

constexpr const char* FontDirectory = "/system/fonts/";
constexpr const char* FontExtension = ".ffnt";
constexpr size_t MaximumNameLength = 64;

bool IsSkipped(char32_t code_point)
{
    return code_point < 0x20 || code_point == 0x7F;
}

int32_t Saturate(int64_t value)
{
    return value > INT32_MAX ? INT32_MAX : static_cast<int32_t>(value);
}

} // namespace

Font::Font() : m_owned(nullptr) {}

Font::Font(Font&& other) noexcept : m_owned(other.m_owned), m_view(other.m_view)
{
    other.m_owned = nullptr;
    other.m_view = Text::FontView();
}

Font& Font::operator=(Font&& other) noexcept
{
    if (this != &other)
    {
        free(m_owned);
        m_owned = other.m_owned;
        m_view = other.m_view;
        other.m_owned = nullptr;
        other.m_view = Text::FontView();
    }
    return *this;
}

Font::~Font()
{
    free(m_owned);
}

void Font::Reset()
{
    free(m_owned);
    m_owned = nullptr;
    m_view = Text::FontView();
}

Text::FontStatus Font::Load(const char* path)
{
    Reset();
    if (path == nullptr)
    {
        return Text::FontStatus::Short;
    }
    const int descriptor = open(path, O_RDONLY);
    if (descriptor < 0)
    {
        return Text::FontStatus::Short;
    }
    struct stat information;
    if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_size < 0)
    {
        close(descriptor);
        return Text::FontStatus::Short;
    }
    if (static_cast<uint64_t>(information.st_size) > Text::FontView::MaximumFileSize)
    {
        close(descriptor);
        return Text::FontStatus::Size;
    }
    // One byte more than the file had, so a file that grew since fstat reaches Parse with a size
    // that disagrees with its header.
    const size_t capacity = static_cast<size_t>(information.st_size) + 1;
    auto* buffer = static_cast<uint8_t*>(malloc(capacity));
    size_t size = 0;
    bool failed = buffer == nullptr;
    while (!failed && size < capacity)
    {
        const ssize_t count = read(descriptor, buffer + size, capacity - size);
        if (count < 0)
        {
            failed = true;
        }
        else if (count == 0)
        {
            break;
        }
        else
        {
            size += static_cast<size_t>(count);
        }
    }
    close(descriptor);
    if (failed)
    {
        free(buffer);
        return Text::FontStatus::Short;
    }
    const Text::FontStatus status = Text::FontView::Parse(buffer, size, &m_view);
    if (status != Text::FontStatus::Ok)
    {
        free(buffer);
        m_view = Text::FontView();
        return status;
    }
    m_owned = buffer;
    return status;
}

Text::FontStatus Font::LoadNamed(const char* name)
{
    if (name == nullptr || strlen(name) > MaximumNameLength || strchr(name, '/') != nullptr)
    {
        Reset();
        return Text::FontStatus::Short;
    }
    char path[128];
    strcpy(path, FontDirectory);
    strcat(path, name);
    strcat(path, FontExtension);
    return Load(path);
}

Text::FontStatus Font::LoadMemory(const uint8_t* bytes, size_t size)
{
    Reset();
    const Text::FontStatus status = Text::FontView::Parse(bytes, size, &m_view);
    if (status != Text::FontStatus::Ok)
    {
        m_view = Text::FontView();
    }
    return status;
}

bool Font::Valid() const
{
    return m_view.Valid();
}

int32_t Font::LineHeight() const
{
    return m_view.LineHeight();
}

int32_t Font::Ascent() const
{
    return m_view.Ascent();
}

int32_t Font::Descent() const
{
    return m_view.Descent();
}

bool Font::Monospace() const
{
    return m_view.Monospace();
}

int32_t Font::CellWidth() const
{
    return m_view.CellWidth();
}

int32_t Font::Advance(uint32_t code_point) const
{
    if (!m_view.Valid() || IsSkipped(code_point))
    {
        return 0;
    }
    return m_view.Find(code_point).advance;
}

bool Font::HasGlyph(uint32_t code_point) const
{
    return m_view.Valid() && m_view.Contains(code_point);
}

int32_t Font::MeasureText(const char* utf8) const
{
    if (!m_view.Valid() || utf8 == nullptr)
    {
        return 0;
    }
    const size_t length = strlen(utf8);
    int64_t width = 0;
    for (size_t offset = 0; offset < length;)
    {
        size_t used = 0;
        const char32_t code_point = Text::DecodeNext(utf8 + offset, length - offset, &used);
        offset += used;
        width += Advance(code_point);
    }
    return Saturate(width);
}

const char* Font::Name() const
{
    return m_view.Valid() ? m_view.Name() : "";
}

const char* Font::Notice() const
{
    return m_view.Valid() ? m_view.Notice() : "";
}

const Text::FontView& Font::View() const
{
    return m_view;
}

} // namespace FeatherOS::Graphics
