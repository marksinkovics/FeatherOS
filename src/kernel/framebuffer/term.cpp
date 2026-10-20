#include "term.h"
#include "kernel/framebuffer/framebuffer.h"

namespace FeatherOS
{

void Term::Init(Framebuffer* framebuffer)
{
    m_framebuffer = framebuffer;
    m_rows = framebuffer == nullptr ? 0 : framebuffer->GetHeight() / FONT_HEIGHT;
    m_columns = framebuffer == nullptr ? 0 : framebuffer->GetWidth() / FONT_WIDTH;
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

    for (uint32_t row = 0; row < TermConfig::Row; ++row)
    {
        for (uint32_t column = 0; column < TermConfig::Column; ++column)
        {
            m_buffer[row][column] = ' ';
        }
    }
}

void Term::Print(const char* str)
{
    Write(str);
    Refresh();
}

void Term::PrintLn(const char* str)
{
    Write(str);
    Write("\n");
    Refresh();
}

void Term::Write(const char* str)
{
    if (str == nullptr || m_rows <= FirstContentRow || m_columns == 0)
    {
        return;
    }

    for (uint32_t index = 0; str[index] != '\0'; ++index)
    {
        const char character = str[index];
        if (character != '\n')
        {
            m_buffer[m_row_index][m_column_index] = character;
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

    for (uint32_t column = 0; column < m_columns; ++column)
    {
        m_buffer[m_rows - 1][column] = ' ';
    }
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
    m_buffer[m_row_index][m_column_index] = ' ';
    Refresh();
}

void Term::SetCursorVisible(bool visible)
{
    m_cursor_visible = visible;
    Refresh();
}

void Term::SetStatus(const char* left, const char* right)
{
    if (m_rows == 0 || m_columns == 0)
    {
        return;
    }
    for (uint32_t column = 0; column < m_columns; ++column)
    {
        m_buffer[StatusRow][column] = ' ';
    }
    // One blank cell separates the text from each edge of the bar.
    uint32_t right_length = 0;
    while (right != nullptr && right[right_length] != '\0')
    {
        ++right_length;
    }
    const uint32_t right_end = m_columns > 1 ? m_columns - 1 : m_columns;
    const uint32_t right_start = right_length < right_end ? right_end - right_length : 0;
    for (uint32_t index = 0; right_start + index < right_end; ++index)
    {
        m_buffer[StatusRow][right_start + index] = static_cast<uint8_t>(right[index]);
    }
    const uint32_t left_end = right_length == 0 ? right_end : right_start;
    for (uint32_t index = 0; left != nullptr && left[index] != '\0' && 2 + index < left_end;
         ++index)
    {
        m_buffer[StatusRow][1 + index] = static_cast<uint8_t>(left[index]);
    }
    Refresh();
}

uint8_t Term::CharacterAt(uint32_t row, uint32_t column) const
{
    return row < m_rows && column < m_columns ? m_buffer[row][column] : 0;
}

bool Term::CursorAt(uint32_t row, uint32_t column) const
{
    return m_cursor_visible && row == m_row_index && column == m_column_index;
}

void Term::MarkDirty(uint32_t row, uint32_t* dirty_top, uint32_t* dirty_bottom) const
{
    const uint32_t top = (row + m_row_offset) * FONT_HEIGHT;
    if (top < *dirty_top)
    {
        *dirty_top = top;
    }
    if (top + FONT_HEIGHT > *dirty_bottom)
    {
        *dirty_bottom = top + FONT_HEIGHT;
    }
}

void Term::Refresh()
{
    if (m_framebuffer == nullptr)
    {
        return;
    }

    if (m_redraw_all)
    {
        m_framebuffer->clear(m_bg_color);
        for (uint32_t row = 0; row < TermConfig::Row; ++row)
        {
            // Bar cells are drawn in inverted colors, so even its spaces must be painted.
            const uint8_t rendered = row == StatusRow ? 0 : ' ';
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

void Term::ApplyPendingScroll()
{
    // Shift the rendered pixels and their cell record together so only the new rows differ.
    const uint32_t scroll_rows = m_pending_scroll_rows;
    const uint32_t content_rows = m_rows - FirstContentRow;
    m_pending_scroll_rows = 0;
    m_framebuffer->scrollUp((m_row_offset + FirstContentRow) * FONT_HEIGHT,
                            content_rows * FONT_HEIGHT, scroll_rows * FONT_HEIGHT, m_bg_color);
    for (uint32_t row = FirstContentRow; row < m_rows; ++row)
    {
        const uint32_t source_row = row + scroll_rows;
        for (uint32_t column = 0; column < m_columns; ++column)
        {
            m_screen[row][column] = source_row < m_rows ? m_screen[source_row][column] : ' ';
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
    const uint32_t x = (column + m_column_offset) * FONT_WIDTH;
    const uint32_t y = (row + m_row_offset) * FONT_HEIGHT;
    const bool inverted = row == StatusRow || CursorAt(row, column);
    const uint32_t background = inverted ? m_fg_color : m_bg_color;
    const uint32_t foreground = inverted ? m_bg_color : m_fg_color;
    uint8_t font_index = 0;
    if (indexForCharacter(static_cast<char>(m_buffer[row][column]), &font_index) == 0)
    {
        m_framebuffer->drawCharacter(font_index, x, y, background, foreground);
    }
    else
    {
        m_framebuffer->drawRectangle(x, y, FONT_WIDTH, FONT_HEIGHT, background);
    }
}

}; // namespace FeatherOS
