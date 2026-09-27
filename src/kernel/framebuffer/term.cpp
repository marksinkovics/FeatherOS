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
    if (m_rows > TermConfig::Row)
    {
        m_rows = TermConfig::Row;
    }
    if (m_columns > TermConfig::Column)
    {
        m_columns = TermConfig::Column;
    }

    for (uint32_t row = 0; row < TermConfig::Row; ++row)
    {
        for (uint32_t column = 0; column < TermConfig::Column; ++column)
        {
            // uint32_t index = (row * TermConfig::Row) + column;
            //char c = 'A' + (index % ('Z' - 'A' + 1));
            m_buffer[row][column] = ' ';
        }
    }
}

void Term::Print(const char* str)
{
    if (str == nullptr)
    {
        return;
    }

    uint32_t character_index = 0;
    char character = str[0];
    while (character != '\0')
    {
        if (m_rows == 0 || m_columns == 0)
        {
            return;
        }

        if (character == '\n')
        {
            m_column_index = 0;
            m_row_index++;
            if (m_row_index >= m_rows)
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
            if (m_row_index >= m_rows)
            {
                Scroll();
            }
        }

        character = str[++character_index];
    }

    Refresh();
}

void Term::Scroll()
{
    for (uint32_t row = 1; row < m_rows; ++row)
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
}

void Term::PrintLn(const char* str)
{
    Print(str);
    Print("\n");
}

void Term::Refresh()
{
    if (m_framebuffer == nullptr)
    {
        return;
    }

    m_framebuffer->clear(Color::BGColor);

    for (uint32_t row = 0; row < m_rows; ++row)
    {
        for (uint32_t column = 0; column < m_columns; ++column)
        {
            char character = (char)m_buffer[row][column];
            uint8_t font_index = 0;
            if (indexForCharacter(character, &font_index) == 0)
            {
                uint32_t x = (column * FONT_WIDTH);
                uint32_t y = row * FONT_HEIGHT;
                m_framebuffer->drawCharacter(font_index, x, y, m_bg_color, m_fg_color);
            }
        }
    }

    m_framebuffer->draw();
}

};