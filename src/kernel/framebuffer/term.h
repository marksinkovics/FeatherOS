#ifndef TERM_H
#define TERM_H

#include <stdint.h>

#include "kernel/config.h"

extern "C"
{
#include "kernel/framebuffer/font.h"
}

class Framebuffer;

namespace FeatherOS
{

struct TermConfig
{
    static const uint32_t Row = Config::Height / FONT_HEIGHT;
    static const uint32_t Column = Config::Width / FONT_WIDTH;
    static const uint32_t Padding = 1;
    static const uint32_t BufferLen = Row * Column;
};

class Term
{
public:
    void Init(Framebuffer* framebuffer);

    void Print(const char* str);
    void PrintLn(const char* str);
    // Blanks the cell before the cursor and moves back onto it, across a line wrap if needed.
    void EraseCharacter();
    void SetCursorVisible(bool visible);
    // The top row: left text from its start, right text against its end.
    void SetStatus(const char* left, const char* right);
    uint8_t CharacterAt(uint32_t row, uint32_t column) const;

    // Draws cells that changed since the last refresh; Redraw repaints the whole screen.
    void Refresh();
    void Redraw();

private:
    static const uint32_t StatusRow = 0;
    static const uint32_t FirstContentRow = 1;

    Framebuffer* m_framebuffer;
    uint8_t m_buffer[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    // Characters currently rendered in the framebuffer's back buffer.
    uint8_t m_screen[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    uint32_t m_pending_scroll_rows = 0;
    bool m_redraw_all = true;
    uint32_t m_bg_color = FeatherOS::Color::BGColor;
    uint32_t m_fg_color = FeatherOS::Color::FGColor;
    uint32_t m_rows = 0;
    uint32_t m_columns = 0;
    uint32_t m_row_offset = 0;
    uint32_t m_column_offset = 0;

    uint32_t m_row_index = 0;
    uint32_t m_column_index = 0;
    bool m_cursor_visible = false;
    // Where the cursor was last rendered; m_cursor_drawn is false when it is not on screen.
    bool m_cursor_drawn = false;
    uint32_t m_cursor_row = 0;
    uint32_t m_cursor_column = 0;

    void Write(const char* str);
    void Scroll();
    void ApplyPendingScroll();
    void DrawCell(uint32_t row, uint32_t column);
    void MarkDirty(uint32_t row, uint32_t* dirty_top, uint32_t* dirty_bottom) const;
    bool CursorAt(uint32_t row, uint32_t column) const;
};

}; // namespace FeatherOS

#endif