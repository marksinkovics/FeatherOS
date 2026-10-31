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
    m_attribute = 0;
    m_escape_state = EscapeState::Text;
    m_cursor_enabled = true;
    m_saved_row = FirstContentRow;
    m_saved_column = 0;

    for (uint32_t row = 0; row < TermConfig::Row; ++row)
    {
        for (uint32_t column = 0; column < TermConfig::Column; ++column)
        {
            m_buffer[row][column] = ' ';
            m_attributes[row][column] = 0;
        }
    }
}

void Term::Print(const char* str)
{
    Write(str);
    Refresh();
    __atomic_add_fetch(&m_output_count, 1, __ATOMIC_RELEASE);
}

void Term::PrintLn(const char* str)
{
    Write(str);
    Write("\n");
    Refresh();
    __atomic_add_fetch(&m_output_count, 1, __ATOMIC_RELEASE);
}

void Term::Write(const char* str)
{
    if (str == nullptr || m_rows <= FirstContentRow || m_columns == 0)
    {
        return;
    }

    for (uint32_t index = 0; str[index] != '\0'; ++index)
    {
        if (!HandleEscape(str[index]))
        {
            WriteCharacter(str[index]);
        }
    }
}

void Term::WriteCharacter(char character)
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
        m_buffer[m_row_index][m_column_index] = character;
        m_attributes[m_row_index][m_column_index] = m_attribute;
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
bool Term::HandleEscape(char character)
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
        ExecuteControl(character);
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
                m_main_attributes[row][column] = m_attributes[row][column];
                m_buffer[row][column] = ' ';
                m_attributes[row][column] = 0;
            }
            else
            {
                m_buffer[row][column] = m_main_buffer[row][column];
                m_attributes[row][column] = m_main_attributes[row][column];
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
        m_buffer[row][column] = ' ';
        m_attributes[row][column] = 0;
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
            m_attributes[row - 1][column] = m_attributes[row][column];
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
    m_buffer[m_row_index][m_column_index] = ' ';
    m_attributes[m_row_index][m_column_index] = 0;
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

bool Term::ReversedAt(uint32_t row, uint32_t column) const
{
    return row < m_rows && column < m_columns && (m_attributes[row][column] & Reverse) != 0;
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
    return __atomic_load_n(&m_output_count, __ATOMIC_ACQUIRE);
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
                m_screen_attributes[row][column] = 0;
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
            if (m_buffer[row][column] == m_screen[row][column] &&
                m_attributes[row][column] == m_screen_attributes[row][column])
            {
                continue;
            }
            DrawCell(row, column);
            m_screen[row][column] = m_buffer[row][column];
            m_screen_attributes[row][column] = m_attributes[row][column];
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
            m_screen_attributes[row][column] =
                source_row < m_rows ? m_screen_attributes[source_row][column] : 0;
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
    // The cursor shows as the opposite of the cell's own colors.
    const bool reversed = row == StatusRow || (m_attributes[row][column] & Reverse) != 0;
    const bool inverted = CursorAt(row, column) ? !reversed : reversed;
    const uint32_t background = inverted ? m_fg_color : m_bg_color;
    const uint32_t foreground = inverted ? m_bg_color : m_fg_color;
    const char character = static_cast<char>(m_buffer[row][column]);
    uint8_t font_index = 0;
    if (indexForCharacter(character, &font_index) == 0)
    {
        m_framebuffer->drawCharacter(font_index, x, y, background, foreground);
        return;
    }
    m_framebuffer->drawRectangle(x, y, FONT_WIDTH, FONT_HEIGHT, background);
    if (row == StatusRow && (character == StatusGreenDot || character == StatusRedDot))
    {
        DrawDot(x, y, character == StatusGreenDot ? DotGreen : DotRed);
    }
}

void Term::DrawDot(uint32_t x, uint32_t y, uint32_t color)
{
    const int32_t radius = FONT_WIDTH / 2 - 1;
    const int32_t center_x = FONT_WIDTH / 2;
    const int32_t center_y = FONT_HEIGHT / 2;
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
