#include "term.h"
#include "kernel/framebuffer/framebuffer.h"

namespace FeatherOS
{

void Term::Init(Framebuffer* framebuffer)
{
    m_framebuffer = framebuffer;
    m_row_index = 0;
    m_column_index = 0;
    m_rows = framebuffer == nullptr ? 0 : framebuffer->GetHeight() / FONT_HEIGHT;
    m_columns = framebuffer == nullptr ? 0 : framebuffer->GetWidth() / FONT_WIDTH;
    m_input_length = 0;
    m_input_active = false;
    m_prompt[0] = '\0';
    m_uptime_text[0] = '\0';
    m_pending_scroll_rows = 0;
    m_redraw_all = true;
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
    m_content_rows = m_rows > 1 ? m_rows - 1 : m_rows;

    for (uint32_t row = 0; row < TermConfig::Row; ++row)
    {
        for (uint32_t column = 0; column < TermConfig::Column; ++column)
        {
            // uint32_t index = (row * TermConfig::Row) + column;
            // char c = 'A' + (index % ('Z' - 'A' + 1));
            m_buffer[row][column] = ' ';
        }
    }
}

void Term::Print(const char* str)
{
    Write(str);
    Refresh();
}

void Term::Write(const char* str)
{
    if (str == nullptr)
    {
        return;
    }

    uint32_t character_index = 0;
    char character = str[0];
    while (character != '\0')
    {
        if (m_content_rows == 0 || m_columns == 0)
        {
            return;
        }

        if (character == '\n')
        {
            m_column_index = 0;
            m_row_index++;
            if (m_row_index >= m_content_rows)
            {
                Scroll();
            }
            character = str[++character_index];
            continue;
        }

        m_buffer[m_row_index][m_column_index] = character;
        m_column_index++;

        if (m_column_index == m_columns)
        {
            m_column_index = 0;
            m_row_index++;
            if (m_row_index >= m_content_rows)
            {
                Scroll();
            }
        }

        character = str[++character_index];
    }
}

void Term::Scroll()
{
    for (uint32_t row = 1; row < m_content_rows; ++row)
    {
        for (uint32_t column = 0; column < m_columns; ++column)
        {
            m_buffer[row - 1][column] = m_buffer[row][column];
        }
    }

    for (uint32_t column = 0; column < m_columns; ++column)
    {
        m_buffer[m_content_rows - 1][column] = ' ';
    }
    m_row_index = m_content_rows - 1;
    ++m_pending_scroll_rows;
}

void Term::PrintLn(const char* str)
{
    Write(str);
    Write("\n");
    Refresh();
}

void Term::BeginInput(const char* prompt)
{
    uint32_t length = 0;
    while (prompt != nullptr && prompt[length] != '\0' && length < sizeof(m_prompt) - 1)
    {
        m_prompt[length] = prompt[length];
        ++length;
    }
    m_prompt[length] = '\0';
    m_input_active = true;
    m_input_length = 0;
    m_input_line[0] = '\0';
    DrawInputLine();
    Refresh();
}

void Term::EndInput()
{
    m_input_active = false;
    m_prompt[0] = '\0';
    m_input_length = 0;
    m_input_line[0] = '\0';
    DrawInputLine();
    Refresh();
}

void Term::HandleInputEvent(const Input::Event& event)
{
    if (!m_input_active)
    {
        return;
    }

    if (event.type == Input::EventType::Character)
    {
        uint32_t prompt_length = 0;
        while (m_prompt[prompt_length] != '\0')
        {
            ++prompt_length;
        }
        uint32_t status_length = 0;
        while (status_length < sizeof(m_uptime_text) - 1 && m_uptime_text[status_length] != '\0')
        {
            ++status_length;
        }
        const uint32_t status_width = status_length == 0 ? 0 : status_length + 2;
        const uint32_t input_end = m_columns > status_width ? m_columns - status_width : 0;
        if (m_input_length < sizeof(m_input_line) - 1 && prompt_length + m_input_length < input_end)
        {
            m_input_line[m_input_length++] = event.character;
            m_input_line[m_input_length] = '\0';
        }
    }
    else if (event.type == Input::EventType::Backspace)
    {
        if (m_input_length > 0)
        {
            m_input_line[--m_input_length] = '\0';
        }
    }
    else if (event.type == Input::EventType::Enter)
    {
        Write(m_prompt);
        Write(m_input_line);
        Write("\n");
        m_input_length = 0;
        m_input_line[0] = '\0';
    }

    DrawInputLine();
    Refresh();
}

void Term::SetUptimeText(const char* text)
{
    uint32_t index = 0;
    if (text != nullptr)
    {
        while (text[index] != '\0' && index < sizeof(m_uptime_text) - 1)
        {
            m_uptime_text[index] = text[index];
            ++index;
        }
    }
    m_uptime_text[index] = '\0';
    DrawInputLine();
    Refresh();
}

void Term::DrawInputLine()
{
    if (m_rows == 0 || m_columns == 0)
    {
        return;
    }

    const uint32_t input_row = m_rows - 1;
    for (uint32_t column = 0; column < m_columns; ++column)
    {
        m_buffer[input_row][column] = ' ';
    }

    uint32_t status_length = 0;
    while (status_length < sizeof(m_uptime_text) - 1 && m_uptime_text[status_length] != '\0')
    {
        ++status_length;
    }
    const uint32_t status_width = status_length == 0 ? 0 : status_length + 2;
    const uint32_t status_start = m_columns > status_width ? m_columns - status_width : 0;

    uint32_t column = 0;
    while (m_prompt[column] != '\0' && column < status_start)
    {
        m_buffer[input_row][column] = static_cast<uint8_t>(m_prompt[column]);
        ++column;
    }
    for (uint32_t index = 0; index < m_input_length && column < status_start; ++index)
    {
        m_buffer[input_row][column++] = static_cast<uint8_t>(m_input_line[index]);
    }
    for (uint32_t index = 0; index < status_length && status_start + index + 1 < m_columns; ++index)
    {
        m_buffer[input_row][status_start + index + 1] = static_cast<uint8_t>(m_uptime_text[index]);
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
            for (uint32_t column = 0; column < TermConfig::Column; ++column)
            {
                m_screen[row][column] = ' ';
            }
        }
        m_pending_scroll_rows = 0;
    }

    // Pixel rows [dirty_top, dirty_bottom) must be copied to the visible framebuffer.
    uint32_t dirty_top = UINT32_MAX;
    uint32_t dirty_bottom = 0;
    if (m_pending_scroll_rows > 0)
    {
        ApplyPendingScroll();
        dirty_top = m_row_offset * FONT_HEIGHT;
        dirty_bottom = (m_row_offset + m_content_rows) * FONT_HEIGHT;
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
            const uint32_t top = (row + m_row_offset) * FONT_HEIGHT;
            if (top < dirty_top)
            {
                dirty_top = top;
            }
            if (top + FONT_HEIGHT > dirty_bottom)
            {
                dirty_bottom = top + FONT_HEIGHT;
            }
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
    m_pending_scroll_rows = 0;
    m_framebuffer->scrollUp(m_row_offset * FONT_HEIGHT, m_content_rows * FONT_HEIGHT,
                            scroll_rows * FONT_HEIGHT, m_bg_color);
    for (uint32_t row = 0; row < m_content_rows; ++row)
    {
        const uint32_t source_row = row + scroll_rows;
        for (uint32_t column = 0; column < m_columns; ++column)
        {
            m_screen[row][column] =
                source_row < m_content_rows ? m_screen[source_row][column] : ' ';
        }
    }
}

void Term::DrawCell(uint32_t row, uint32_t column)
{
    const uint32_t x = (column + m_column_offset) * FONT_WIDTH;
    const uint32_t y = (row + m_row_offset) * FONT_HEIGHT;
    uint8_t font_index = 0;
    if (indexForCharacter(static_cast<char>(m_buffer[row][column]), &font_index) == 0)
    {
        m_framebuffer->drawCharacter(font_index, x, y, m_bg_color, m_fg_color);
    }
    else
    {
        m_framebuffer->drawRectangle(x, y, FONT_WIDTH, FONT_HEIGHT, m_bg_color);
    }
}

}; // namespace FeatherOS