#ifndef TERM_H
#define TERM_H

#include <atomic>
#include <stdint.h>

#include "kernel/config.h"
#include "kernel/framebuffer/console_font.h"
#include "user/text/font_data.h"
#include "user/text/utf8.h"

class Framebuffer;

namespace FeatherOS
{

struct TermConfig
{
    static const uint32_t CellWidth = ConsoleFontCellWidth;
    static const uint32_t CellHeight = ConsoleFontCellHeight;
    // The grid's static maximum, 2560x1600 pixels; a larger screen shows the console in its
    // top-left corner.
    static const uint32_t Row = 100;
    static const uint32_t Column = 320;
    static const uint32_t Padding = 1;
    static const uint32_t BufferLen = Row * Column;
};

// A text console below a one-row status bar. Print takes UTF-8, one code point per cell, drawn
// in the compiled-in console font; bytes that are not valid UTF-8 show as U+FFFD and C1 controls
// are dropped. Print understands a VT100 subset, so programs can
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
    // Whether the compiled-in console font parsed, as Init found it.
    static Text::FontStatus FontStatus();
    // In status text, a cell that shows a green or red dot instead of a character.
    static const char StatusGreenDot = '\x01';
    static const char StatusRedDot = '\x02';

    // The top row: left text from its start, right text against its end.
    void SetStatus(const char* left, const char* right);
    char32_t CharacterAt(uint32_t row, uint32_t column) const;
    bool ReversedAt(uint32_t row, uint32_t column) const;
    uint32_t CursorRow() const;
    uint32_t CursorColumn() const;
    // Grows with every Print and PrintLn, so a reader can tell that output arrived meanwhile.
    uint64_t OutputCount() const;
    // The text area below the status bar.
    uint32_t ContentRows() const;
    uint32_t Columns() const;

    // Draws cells that changed since the last refresh; Redraw repaints the whole screen.
    void Refresh();
    void Redraw();
    // Several terminals may share a framebuffer, of which only the shown one draws. A hidden one
    // keeps its text, and is repainted whole once shown again.
    void SetShown(bool shown);
    bool Shown() const;
    // In graphics mode a program owns the screen: the terminal still takes text into its cells
    // but draws nothing, neither text nor status bar, and the screen is blank until a present,
    // or keeps what it showed with keep_screen. Leaving it repaints the whole console once it is
    // shown.
    void SetGraphics(bool graphics, bool keep_screen = false);
    bool Graphics() const;

private:
    static const uint32_t StatusRow = 0;
    static const uint32_t FirstContentRow = 1;
    static const uint32_t MaximumParameters = 4;
    static const uint8_t Reverse = 1;
    // A cell holds the code point in bits 0 to 20 and the attributes from bit 24.
    static const uint32_t CodePointMask = 0x1FFFFF;
    static const uint32_t AttributeShift = 24;
    static const uint32_t BlankCell = ' ';
    // Darker than pure green and red, so both stand out on the white bar.
    static const uint32_t DotGreen = 0x00A000;
    static const uint32_t DotRed = 0xD00000;

    enum class EscapeState : uint8_t
    {
        Text,
        // After ESC.
        Escape,
        // After ESC [, collecting parameters.
        Control,
    };

    Framebuffer* m_framebuffer;
    bool m_shown = true;
    bool m_graphics = false;
    uint32_t m_buffer[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    // The cells currently rendered in the framebuffer's back buffer.
    uint32_t m_screen[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    // Applied to the characters written next.
    uint8_t m_attribute = 0;
    EscapeState m_escape_state = EscapeState::Text;
    Text::Utf8Decoder m_decoder;
    uint32_t m_parameters[MaximumParameters] = {};
    uint32_t m_parameter_count = 0;
    bool m_private_sequence = false;
    // Programs hide the cursor with CSI ?25 l; it shows only while also visible for input.
    bool m_cursor_enabled = true;
    // The main screen's text rows and cursor, kept while a full-screen program draws on the
    // alternate screen, so leaving it shows the shell as it was.
    bool m_alternate_screen = false;
    uint32_t m_main_buffer[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    uint32_t m_main_row = 0;
    uint32_t m_main_column = 0;
    uint32_t m_saved_row = FirstContentRow;
    uint32_t m_saved_column = 0;
    uint32_t m_pending_scroll_rows = 0;
    bool m_redraw_all = true;
    uint32_t m_bg_color = FeatherOS::Color::BGColor;
    uint32_t m_fg_color = FeatherOS::Color::FGColor;
    // Packed pixels for coverage 0 to 15, for normal and reversed cells.
    uint32_t m_palette[16] = {};
    uint32_t m_reversed_palette[16] = {};
    uint32_t m_rows = 0;
    uint32_t m_columns = 0;
    std::atomic<uint64_t> m_output_count = 0;
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
    void Accept(char32_t code_point);
    void WriteCharacter(char32_t character);
    // Returns true while character belongs to an escape sequence.
    bool HandleEscape(char32_t character);
    void RebuildPalettes();
    void DrawMissingBox(uint32_t x, uint32_t y, uint32_t color);
    void ExecuteControl(char command);
    void MoveCursor(uint32_t row, uint32_t column);
    void ClearCells(uint32_t row, uint32_t first_column, uint32_t end_column);
    void SwitchScreen(bool alternate);
    void Blank();
    void Scroll();
    void ApplyPendingScroll();
    void DrawCell(uint32_t row, uint32_t column);
    void DrawDot(uint32_t x, uint32_t y, uint32_t color);
    void MarkDirty(uint32_t row, uint32_t* dirty_top, uint32_t* dirty_bottom) const;
    bool CursorAt(uint32_t row, uint32_t column) const;
};

}; // namespace FeatherOS

#endif