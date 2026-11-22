#include "term.h"
#include "kernel/framebuffer/framebuffer.h"

namespace FeatherOS
{

namespace
{

// Parsed once from the compiled-in font and then only read, so drawing never allocates.
Text::FontView console_font;
Text::FontStatus console_font_status = Text::FontStatus::Missing;

bool IsControl(char32_t code_point)
{
    return code_point < 0x20 || code_point == 0x7F;
}

bool IsC1Control(char32_t code_point)
{
    return code_point >= 0x80 && code_point <= 0x9F;
}

// Fills out with the code points of text, at most capacity of them, and returns how many.
uint32_t DecodeStatus(const char* text, char32_t* out, uint32_t capacity)
{
    uint32_t count = 0;
    Text::Utf8Decoder decoder;
    const auto keep = [&](char32_t code_point)
    {
        if (!IsC1Control(code_point) && count < capacity)
        {
            out[count++] = code_point;
        }
    };
    char32_t decoded[2];
    for (uint32_t index = 0; text != nullptr && text[index] != '\0'; ++index)
    {
        const size_t found = decoder.Push(static_cast<uint8_t>(text[index]), decoded);
        for (size_t item = 0; item < found; ++item)
        {
            keep(decoded[item]);
        }
    }
    const size_t found = decoder.Finish(decoded);
    for (size_t item = 0; item < found; ++item)
    {
        keep(decoded[item]);
    }
    return count;
}

} // namespace

Text::FontStatus Term::FontStatus()
{
    return console_font_status;
}

void Term::Init(Framebuffer* framebuffer)
{
    if (!console_font.Valid())
    {
        console_font_status =
            Text::FontView::Parse(ConsoleFontData, ConsoleFontSize, &console_font);
    }
    m_framebuffer = framebuffer;
    m_decoder = Text::Utf8Decoder();
    m_rows = framebuffer == nullptr ? 0 : framebuffer->GetHeight() / TermConfig::CellHeight;
    m_columns = framebuffer == nullptr ? 0 : framebuffer->GetWidth() / TermConfig::CellWidth;
    m_pending_scroll_rows = 0;
    m_redraw_all = true;
    m_cursor_visible = false;
    m_cursor_drawn = false;
    if (m_rows > TermConfig::Row)
    {
        m_rows = TermConfig::Row;
    }
    if (m_columns > TermConfig::Column)
    {
        m_columns = TermConfig::Column;
    }
    m_row_offset = m_rows > 2 * TermConfig::Padding ? TermConfig::Padding : 0;
    m_column_offset = m_columns > 2 * TermConfig::Padding ? TermConfig::Padding : 0;
    m_rows -= 2 * m_row_offset;
    m_columns -= 2 * m_column_offset;
    m_row_index = FirstContentRow;
    m_column_index = 0;
    m_attribute = 0;
    m_escape_state = EscapeState::Text;
    m_cursor_enabled = true;
    m_saved_row = FirstContentRow;
    m_saved_column = 0;
    RebuildPalettes();

    for (uint32_t row = 0; row < TermConfig::Row; ++row)
    {
        for (uint32_t column = 0; column < TermConfig::Column; ++column)
        {
            m_buffer[row][column] = BlankCell;
        }
    }
}

void Term::RebuildPalettes()
{
    if (m_framebuffer == nullptr)
    {
        return;
    }
    for (uint32_t coverage = 0; coverage < 16; ++coverage)
    {
        m_palette[coverage] =
            m_framebuffer->packColor(Text::Blend(m_bg_color, m_fg_color, coverage));
        m_reversed_palette[coverage] =
            m_framebuffer->packColor(Text::Blend(m_fg_color, m_bg_color, coverage));
    }
}

void Term::Print(const char* str)
{
    Write(str);
    Refresh();
    m_output_count.fetch_add(1, std::memory_order_release);
}

void Term::PrintLn(const char* str)
{
    Write(str);
    Write("\n");
    Refresh();
    m_output_count.fetch_add(1, std::memory_order_release);
}

void Term::Write(const char* str)
{
    if (str == nullptr || m_rows <= FirstContentRow || m_columns == 0)
    {
        return;
    }

    for (uint32_t index = 0; str[index] != '\0'; ++index)
    {
        char32_t decoded[2];
        const size_t count = m_decoder.Push(static_cast<uint8_t>(str[index]), decoded);
        for (size_t item = 0; item < count; ++item)
        {
            Accept(decoded[item]);
        }
    }
}

void Term::Accept(char32_t code_point)
{
    if (IsC1Control(code_point))
    {
        return;
    }
    if (!HandleEscape(code_point))
    {
        WriteCharacter(code_point);
    }
}

void Term::WriteCharacter(char32_t character)
{
    if (character == '\r')
    {
        m_column_index = 0;
        return;
    }
    if (character == '\b')
    {
        m_column_index = m_column_index > 0 ? m_column_index - 1 : 0;
        return;
    }
    if (character != '\n')
    {
        m_buffer[m_row_index][m_column_index] =
            (character & CodePointMask) | (static_cast<uint32_t>(m_attribute) << AttributeShift);
        ++m_column_index;
    }
    // The cursor wraps as soon as a row fills, so it always rests on a visible cell.
    if (character == '\n' || m_column_index == m_columns)
    {
        m_column_index = 0;
        ++m_row_index;
        if (m_row_index >= m_rows)
        {
            Scroll();
        }
    }
}

// Sequences may arrive split across writes, so the parser keeps its state between calls.
bool Term::HandleEscape(char32_t character)
{
    switch (m_escape_state)
    {
    case EscapeState::Text:
        if (character != '\x1b')
        {
            return false;
        }
        m_escape_state = EscapeState::Escape;
        return true;
    case EscapeState::Escape:
        if (character == '[')
        {
            m_escape_state = EscapeState::Control;
            m_parameter_count = 0;
            m_private_sequence = false;
            for (uint32_t& parameter : m_parameters)
            {
                parameter = 0;
            }
            return true;
        }
        // ESC 7 and ESC 8 also save and restore the cursor; other sequences are ignored.
        if (character == '7' || character == '8')
        {
            ExecuteControl(character == '7' ? 's' : 'u');
        }
        m_escape_state = EscapeState::Text;
        return true;
    case EscapeState::Control:
        break;
    }
    if (character >= '0' && character <= '9')
    {
        m_parameter_count = m_parameter_count == 0 ? 1 : m_parameter_count;
        uint32_t& parameter = m_parameters[m_parameter_count - 1];
        parameter =
            parameter < 10000 ? parameter * 10 + static_cast<uint32_t>(character - '0') : parameter;
        return true;
    }
    if (character == ';')
    {
        m_parameter_count = m_parameter_count == 0 ? 2 : m_parameter_count + 1;
        m_parameter_count =
            m_parameter_count > MaximumParameters ? MaximumParameters : m_parameter_count;
        return true;
    }
    if (character == '?' && m_parameter_count == 0)
    {
        m_private_sequence = true;
        return true;
    }
    // Bytes from @ to ~ end the sequence; others, such as intermediate bytes, are skipped.
    if (character >= '@' && character <= '~')
    {
        ExecuteControl(static_cast<char>(character));
        m_escape_state = EscapeState::Text;
    }
    return true;
}

void Term::MoveCursor(uint32_t row, uint32_t column)
{
    m_row_index = row < FirstContentRow ? FirstContentRow : row >= m_rows ? m_rows - 1 : row;
    m_column_index = column >= m_columns ? m_columns - 1 : column;
}

// Entering saves the main screen's text rows and cursor and starts on a blank screen; leaving
// brings them back. The status bar is never part of either.
void Term::SwitchScreen(bool alternate)
{
    if (alternate == m_alternate_screen)
    {
        return;
    }
    for (uint32_t row = FirstContentRow; row < m_rows; ++row)
    {
        for (uint32_t column = 0; column < m_columns; ++column)
        {
            if (alternate)
            {
                m_main_buffer[row][column] = m_buffer[row][column];
                m_buffer[row][column] = BlankCell;
            }
            else
            {
                m_buffer[row][column] = m_main_buffer[row][column];
            }
        }
    }
    if (alternate)
    {
        m_main_row = m_row_index;
        m_main_column = m_column_index;
    }
    else
    {
        MoveCursor(m_main_row, m_main_column);
    }
    m_alternate_screen = alternate;
}

void Term::ClearCells(uint32_t row, uint32_t first_column, uint32_t end_column)
{
    for (uint32_t column = first_column; column < end_column && column < m_columns; ++column)
    {
        m_buffer[row][column] = BlankCell;
    }
}

void Term::ExecuteControl(char command)
{
    // A missing or zero parameter counts as 1 for movement, and as 0 for erasing modes.
    auto count = [this](uint32_t index)
    { return index < m_parameter_count && m_parameters[index] != 0 ? m_parameters[index] : 1; };
    const uint32_t mode = m_parameter_count > 0 ? m_parameters[0] : 0;
    switch (command)
    {
    case 'A':
        MoveCursor(m_row_index > count(0) ? m_row_index - count(0) : 0, m_column_index);
        break;
    case 'B':
        MoveCursor(m_row_index + count(0), m_column_index);
        break;
    case 'C':
        MoveCursor(m_row_index, m_column_index + count(0));
        break;
    case 'D':
        MoveCursor(m_row_index, m_column_index > count(0) ? m_column_index - count(0) : 0);
        break;
    case 'G':
        MoveCursor(m_row_index, count(0) - 1);
        break;
    case 'd':
        MoveCursor(FirstContentRow + count(0) - 1, m_column_index);
        break;
    case 'H':
    case 'f':
        MoveCursor(FirstContentRow + count(0) - 1, count(1) - 1);
        break;
    case 'J':
        for (uint32_t row = FirstContentRow; row < m_rows; ++row)
        {
            const bool before = row < m_row_index;
            const bool after = row > m_row_index;
            if (mode >= 2 || (mode == 0 && after) || (mode == 1 && before))
            {
                ClearCells(row, 0, m_columns);
            }
        }
        if (mode == 0)
        {
            ClearCells(m_row_index, m_column_index, m_columns);
        }
        else if (mode == 1)
        {
            ClearCells(m_row_index, 0, m_column_index + 1);
        }
        break;
    case 'K':
        ClearCells(m_row_index, mode == 0 ? m_column_index : 0,
                   mode == 1 ? m_column_index + 1 : m_columns);
        break;
    case 'm':
        for (uint32_t index = 0; index < (m_parameter_count == 0 ? 1 : m_parameter_count); ++index)
        {
            const uint32_t value = m_parameters[index];
            m_attribute = value == 0 || value == 27 ? 0 : value == 7 ? Reverse : m_attribute;
        }
        break;
    case 's':
        m_saved_row = m_row_index;
        m_saved_column = m_column_index;
        break;
    case 'u':
        MoveCursor(m_saved_row, m_saved_column);
        break;
    case 'h':
    case 'l':
        if (m_private_sequence && mode == 25)
        {
            m_cursor_enabled = command == 'h';
        }
        else if (m_private_sequence && mode == 1049)
        {
            SwitchScreen(command == 'h');
        }
        break;
    default:
        break;
    }
}

void Term::Scroll()
{
    for (uint32_t row = FirstContentRow + 1; row < m_rows; ++row)
    {
        for (uint32_t column = 0; column < m_columns; ++column)
        {
            m_buffer[row - 1][column] = m_buffer[row][column];
        }
    }
    ClearCells(m_rows - 1, 0, m_columns);
    m_row_index = m_rows - 1;
    ++m_pending_scroll_rows;
}

void Term::EraseCharacter()
{
    if (m_rows <= FirstContentRow || m_columns == 0)
    {
        return;
    }
    if (m_column_index > 0)
    {
        --m_column_index;
    }
    else if (m_row_index > FirstContentRow)
    {
        --m_row_index;
        m_column_index = m_columns - 1;
    }
    else
    {
        return;
    }
    m_buffer[m_row_index][m_column_index] = BlankCell;
    Refresh();
}

void Term::SetCursorVisible(bool visible)
{
    m_cursor_visible = visible;
    Refresh();
}

void Term::ResetModes()
{
    SwitchScreen(false);
    m_attribute = 0;
    m_escape_state = EscapeState::Text;
    m_cursor_enabled = true;
    Refresh();
}

void Term::SetStatus(const char* left, const char* right)
{
    if (m_rows == 0 || m_columns == 0)
    {
        return;
    }
    char32_t left_text[TermConfig::Column];
    char32_t right_text[TermConfig::Column];
    const uint32_t left_length = DecodeStatus(left, left_text, TermConfig::Column);
    const uint32_t right_length = DecodeStatus(right, right_text, TermConfig::Column);
    for (uint32_t column = 0; column < m_columns; ++column)
    {
        m_buffer[StatusRow][column] = BlankCell;
    }
    // One blank cell separates the text from each edge of the bar.
    const uint32_t right_end = m_columns > 1 ? m_columns - 1 : m_columns;
    const uint32_t right_start = right_length < right_end ? right_end - right_length : 0;
    for (uint32_t index = 0; right_start + index < right_end; ++index)
    {
        m_buffer[StatusRow][right_start + index] = right_text[index];
    }
    const uint32_t left_end = right_length == 0 ? right_end : right_start;
    for (uint32_t index = 0; index < left_length && 2 + index < left_end; ++index)
    {
        m_buffer[StatusRow][1 + index] = left_text[index];
    }
    Refresh();
}

char32_t Term::CharacterAt(uint32_t row, uint32_t column) const
{
    return row < m_rows && column < m_columns ? (m_buffer[row][column] & CodePointMask) : 0;
}

bool Term::ReversedAt(uint32_t row, uint32_t column) const
{
    return row < m_rows && column < m_columns &&
           ((m_buffer[row][column] >> AttributeShift) & Reverse) != 0;
}

uint32_t Term::CursorRow() const
{
    return m_row_index;
}

uint32_t Term::CursorColumn() const
{
    return m_column_index;
}

uint64_t Term::OutputCount() const
{
    return m_output_count.load(std::memory_order_acquire);
}

uint32_t Term::ContentRows() const
{
    return m_rows > FirstContentRow ? m_rows - FirstContentRow : 0;
}

uint32_t Term::Columns() const
{
    return m_columns;
}

bool Term::CursorAt(uint32_t row, uint32_t column) const
{
    return m_cursor_visible && m_cursor_enabled && row == m_row_index && column == m_column_index;
}

void Term::MarkDirty(uint32_t row, uint32_t* dirty_top, uint32_t* dirty_bottom) const
{
    const uint32_t top = (row + m_row_offset) * TermConfig::CellHeight;
    if (top < *dirty_top)
    {
        *dirty_top = top;
    }
    if (top + TermConfig::CellHeight > *dirty_bottom)
    {
        *dirty_bottom = top + TermConfig::CellHeight;
    }
}

void Term::Refresh()
{
    if (m_framebuffer == nullptr || !m_shown || m_graphics)
    {
        return;
    }

    if (m_redraw_all)
    {
        m_framebuffer->clear(m_bg_color);
        for (uint32_t row = 0; row < TermConfig::Row; ++row)
        {
            // Bar cells are drawn in inverted colors, so even its spaces must be painted.
            const uint32_t rendered = row == StatusRow ? 0 : BlankCell;
            for (uint32_t column = 0; column < TermConfig::Column; ++column)
            {
                m_screen[row][column] = rendered;
            }
        }
        m_pending_scroll_rows = 0;
        m_cursor_drawn = false;
    }

    // Pixel rows [dirty_top, dirty_bottom) must be copied to the visible framebuffer.
    uint32_t dirty_top = UINT32_MAX;
    uint32_t dirty_bottom = 0;
    if (m_pending_scroll_rows > 0)
    {
        ApplyPendingScroll();
        MarkDirty(FirstContentRow, &dirty_top, &dirty_bottom);
        MarkDirty(m_rows - 1, &dirty_top, &dirty_bottom);
    }

    for (uint32_t row = 0; row < m_rows; ++row)
    {
        for (uint32_t column = 0; column < m_columns; ++column)
        {
            if (m_buffer[row][column] == m_screen[row][column])
            {
                continue;
            }
            DrawCell(row, column);
            m_screen[row][column] = m_buffer[row][column];
            MarkDirty(row, &dirty_top, &dirty_bottom);
        }
    }

    const bool cursor_moved =
        m_cursor_drawn != m_cursor_visible ||
        (m_cursor_drawn && (m_cursor_row != m_row_index || m_cursor_column != m_column_index));
    if (cursor_moved && m_rows > FirstContentRow && m_columns > 0)
    {
        if (m_cursor_drawn)
        {
            DrawCell(m_cursor_row, m_cursor_column);
            MarkDirty(m_cursor_row, &dirty_top, &dirty_bottom);
        }
        m_cursor_drawn = m_cursor_visible;
        m_cursor_row = m_row_index;
        m_cursor_column = m_column_index;
        if (m_cursor_drawn)
        {
            DrawCell(m_cursor_row, m_cursor_column);
            MarkDirty(m_cursor_row, &dirty_top, &dirty_bottom);
        }
    }

    if (m_redraw_all)
    {
        m_framebuffer->draw();
        m_redraw_all = false;
    }
    else if (dirty_top < dirty_bottom)
    {
        m_framebuffer->draw(dirty_top, dirty_bottom - dirty_top);
    }
}

void Term::Redraw()
{
    m_redraw_all = true;
    Refresh();
}

void Term::SetShown(bool shown)
{
    m_shown = shown;
    if (!shown)
    {
        return;
    }
    if (m_graphics)
    {
        Blank();
        return;
    }
    Redraw();
}

void Term::SetGraphics(bool graphics)
{
    if (m_graphics == graphics)
    {
        return;
    }
    m_graphics = graphics;
    if (graphics)
    {
        if (m_shown)
        {
            Blank();
        }
        return;
    }
    m_redraw_all = true;
    if (m_shown)
    {
        Redraw();
    }
}

bool Term::Graphics() const
{
    return m_graphics;
}

void Term::Blank()
{
    if (m_framebuffer == nullptr)
    {
        return;
    }
    m_framebuffer->clear(FeatherOS::Color::Black);
    m_framebuffer->draw();
}

bool Term::Shown() const
{
    return m_shown;
}

void Term::ApplyPendingScroll()
{
    // Shift the rendered pixels and their cell record together so only the new rows differ.
    const uint32_t scroll_rows = m_pending_scroll_rows;
    const uint32_t content_rows = m_rows - FirstContentRow;
    m_pending_scroll_rows = 0;
    m_framebuffer->scrollUp((m_row_offset + FirstContentRow) * TermConfig::CellHeight,
                            content_rows * TermConfig::CellHeight,
                            scroll_rows * TermConfig::CellHeight, m_bg_color);
    for (uint32_t row = FirstContentRow; row < m_rows; ++row)
    {
        const uint32_t source_row = row + scroll_rows;
        for (uint32_t column = 0; column < m_columns; ++column)
        {
            m_screen[row][column] = source_row < m_rows ? m_screen[source_row][column] : BlankCell;
        }
    }
    // The rendered cursor moved up with the pixels, or scrolled off.
    if (m_cursor_drawn && m_cursor_row >= FirstContentRow + scroll_rows)
    {
        m_cursor_row -= scroll_rows;
    }
    else
    {
        m_cursor_drawn = false;
    }
}

void Term::DrawCell(uint32_t row, uint32_t column)
{
    const uint32_t x = (column + m_column_offset) * TermConfig::CellWidth;
    const uint32_t y = (row + m_row_offset) * TermConfig::CellHeight;
    const uint32_t cell = m_buffer[row][column];
    const char32_t code_point = cell & CodePointMask;
    // The cursor shows as the opposite of the cell's own colors.
    const bool reversed = row == StatusRow || ((cell >> AttributeShift) & Reverse) != 0;
    const bool inverted = CursorAt(row, column) ? !reversed : reversed;
    const uint32_t background = inverted ? m_fg_color : m_bg_color;
    const uint32_t foreground = inverted ? m_bg_color : m_fg_color;
    m_framebuffer->drawRectangle(x, y, TermConfig::CellWidth, TermConfig::CellHeight, background);
    if (row == StatusRow && (code_point == StatusGreenDot || code_point == StatusRedDot))
    {
        DrawDot(x, y, code_point == StatusGreenDot ? DotGreen : DotRed);
        return;
    }
    if (IsControl(code_point) || code_point == ' ')
    {
        return;
    }
    if (!console_font.Valid())
    {
        DrawMissingBox(x, y, foreground);
        return;
    }
    m_framebuffer->drawGlyph(console_font, console_font.Find(code_point), x, y,
                             inverted ? m_reversed_palette : m_palette);
}

void Term::DrawMissingBox(uint32_t x, uint32_t y, uint32_t color)
{
    constexpr uint32_t width = TermConfig::CellWidth - 2;
    constexpr uint32_t height = TermConfig::CellHeight - 4;
    m_framebuffer->drawRectangle(x + 1, y + 2, width, 1, color);
    m_framebuffer->drawRectangle(x + 1, y + 1 + height, width, 1, color);
    m_framebuffer->drawRectangle(x + 1, y + 2, 1, height, color);
    m_framebuffer->drawRectangle(x + width, y + 2, 1, height, color);
}

void Term::DrawDot(uint32_t x, uint32_t y, uint32_t color)
{
    const int32_t radius = TermConfig::CellWidth / 2 - 1;
    const int32_t center_x = TermConfig::CellWidth / 2;
    const int32_t center_y = TermConfig::CellHeight / 2;
    for (int32_t dy = -radius; dy <= radius; ++dy)
    {
        for (int32_t dx = -radius; dx <= radius; ++dx)
        {
            // The extra radius rounds off the corners less, so the dot looks round at this size.
            if (dx * dx + dy * dy <= radius * radius + radius)
            {
                m_framebuffer->drawPixel(x + static_cast<uint32_t>(center_x + dx),
                                         y + static_cast<uint32_t>(center_y + dy), color);
            }
        }
    }
}

}; // namespace FeatherOS
