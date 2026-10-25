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

// A text console below a one-row status bar. Print understands a VT100 subset, so programs can
// draw full screens: CSI n A/B/C/D move the cursor, CSI row;column H places it (row 1 is the
// first row below the bar), CSI n J and CSI n K erase the screen and the line, CSI 7 m and
// CSI 27 m (or 0 m) turn reverse video on and off, CSI s and CSI u save and restore the cursor,
// CSI ?25 l and h hide and show it, CSI ?1049 h and l switch to an alternate screen and back,
// and \r and \b move it within the line.
class Term
{
public:
    void Init(Framebuffer* framebuffer);

    void Print(const char* str);
    void PrintLn(const char* str);
    // Blanks the cell before the cursor and moves back onto it, across a line wrap if needed.
    void EraseCharacter();
    void SetCursorVisible(bool visible);
    // Back to the main screen and normal video with the cursor enabled, as after a full-screen
    // program that did not restore them.
    void ResetModes();
    // The top row: left text from its start, right text against its end.
    void SetStatus(const char* left, const char* right);
    uint8_t CharacterAt(uint32_t row, uint32_t column) const;
    bool ReversedAt(uint32_t row, uint32_t column) const;
    uint32_t CursorRow() const;
    uint32_t CursorColumn() const;
    // The text area below the status bar.
    uint32_t ContentRows() const;
    uint32_t Columns() const;

    // Draws cells that changed since the last refresh; Redraw repaints the whole screen.
    void Refresh();
    void Redraw();

private:
    static const uint32_t StatusRow = 0;
    static const uint32_t FirstContentRow = 1;
    static const uint32_t MaximumParameters = 4;
    static const uint8_t Reverse = 1;

    enum class EscapeState : uint8_t
    {
        Text,
        // After ESC.
        Escape,
        // After ESC [, collecting parameters.
        Control,
    };

    Framebuffer* m_framebuffer;
    uint8_t m_buffer[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    uint8_t m_attributes[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    // Characters and attributes currently rendered in the framebuffer's back buffer.
    uint8_t m_screen[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    uint8_t m_screen_attributes[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    // Applied to the characters written next.
    uint8_t m_attribute = 0;
    EscapeState m_escape_state = EscapeState::Text;
    uint32_t m_parameters[MaximumParameters] = {};
    uint32_t m_parameter_count = 0;
    bool m_private_sequence = false;
    // Programs hide the cursor with CSI ?25 l; it shows only while also visible for input.
    bool m_cursor_enabled = true;
    // The main screen's text rows and cursor, kept while a full-screen program draws on the
    // alternate screen, so leaving it shows the shell as it was.
    bool m_alternate_screen = false;
    uint8_t m_main_buffer[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    uint8_t m_main_attributes[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    uint32_t m_main_row = 0;
    uint32_t m_main_column = 0;
    uint32_t m_saved_row = FirstContentRow;
    uint32_t m_saved_column = 0;
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
    void WriteCharacter(char character);
    // Returns true while character belongs to an escape sequence.
    bool HandleEscape(char character);
    void ExecuteControl(char command);
    void MoveCursor(uint32_t row, uint32_t column);
    void ClearCells(uint32_t row, uint32_t first_column, uint32_t end_column);
    void SwitchScreen(bool alternate);
    void Scroll();
    void ApplyPendingScroll();
    void DrawCell(uint32_t row, uint32_t column);
    void MarkDirty(uint32_t row, uint32_t* dirty_top, uint32_t* dirty_bottom) const;
    bool CursorAt(uint32_t row, uint32_t column) const;
};

}; // namespace FeatherOS

#endif