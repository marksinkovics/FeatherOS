#ifndef TERM_H
#define TERM_H

#include <stdint.h>

#include "kernel/config.h"
#include "kernel/input/input.h"

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
    // Shows prompt and edits input on the bottom row until EndInput.
    void BeginInput(const char* prompt);
    void EndInput();
    void HandleInputEvent(const Input::Event& event);
    void SetUptimeText(const char* text);

    // Draws cells that changed since the last refresh; Redraw repaints the whole screen.
    void Refresh();
    void Redraw();

private:
    Framebuffer* m_framebuffer;
    uint8_t m_buffer[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    // Characters currently rendered in the framebuffer's back buffer.
    uint8_t m_screen[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    uint32_t m_pending_scroll_rows = 0;
    bool m_redraw_all = true;
    char m_input_line[64];
    char m_prompt[32];
    char m_uptime_text[32];
    uint32_t m_input_length = 0;
    uint32_t m_bg_color = FeatherOS::Color::BGColor;
    uint32_t m_fg_color = FeatherOS::Color::FGColor;
    uint32_t m_rows = 0;
    uint32_t m_columns = 0;
    uint32_t m_content_rows = 0;
    uint32_t m_row_offset = 0;
    uint32_t m_column_offset = 0;

    uint32_t m_row_index = 0;
    uint32_t m_column_index = 0;
    bool m_input_active = false;

    void Write(const char* str);
    void Scroll();
    void ApplyPendingScroll();
    void DrawCell(uint32_t row, uint32_t column);
    void DrawInputLine();
};

}; // namespace FeatherOS

#endif