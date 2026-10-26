#ifndef FEATHEROS_USER_LINE_EDITOR_H
#define FEATHEROS_USER_LINE_EDITOR_H

#include "key_decoder.h"

#include <stddef.h>

namespace FeatherOS::User
{

constexpr size_t MaximumEditedLineLength = 63;
constexpr size_t MaximumHistoryLines = 32;

class LineHistory
{
public:
    // Blank lines and repeats of the newest entry are not kept; the oldest entry goes when full.
    void Add(const char* line);

    size_t Count() const;
    // Age 0 is the newest entry.
    const char* Entry(size_t age) const;

private:
    char m_lines[MaximumHistoryLines][MaximumEditedLineLength + 1] = {};
    size_t m_newest = 0;
    size_t m_count = 0;
};

// Edits one line in a raw terminal, as a shell prompt does: the cursor and Home/End keys move
// within it, Up and Down walk the history, and the Emacs keys most shells have work too (Ctrl+A,
// E, B, F, P, N, D, H, U, K, W, and L to clear the screen). Each key appends what the terminal must
// show to Output, using \b and the VT100 sequences, so a line that wraps redraws correctly when
// the terminal is `columns` wide and wraps as soon as a row fills.
class LineEditor
{
public:
    enum class Status : unsigned char
    {
        Editing,
        Accepted,
        // Ctrl+C: the line is discarded and the caller starts a new one.
        Cancelled,
    };

    void Begin(const char* prompt, size_t columns, LineHistory* history);
    Status Press(const KeyPress& press);
    const char* Line() const;
    size_t Cursor() const;
    const char* Output() const;
    size_t OutputLength() const;
    void ClearOutput();

private:
    const char* m_prompt = "";
    size_t m_prompt_length = 0;
    size_t m_columns = 80;
    LineHistory* m_history = nullptr;
    // 0 while editing a new line; n while showing the history entry of age n - 1.
    size_t m_history_age = 0;
    char m_line[MaximumEditedLineLength + 1] = {};
    // The new line, kept while the history is shown.
    char m_draft[MaximumEditedLineLength + 1] = {};
    size_t m_length = 0;
    size_t m_cursor = 0;
    // Rows between the prompt's first row and the terminal's cursor.
    size_t m_cursor_row = 0;
    // Room for a redraw of the longest prompt and line.
    char m_output[512] = {};
    size_t m_output_length = 0;

    void Append(const char* text);
    void AppendControl(size_t count, char command);
    // At the end of a row the cursor has already wrapped, so the next character starts a row.
    bool AtRowStart(size_t position) const;
    // Draws the prompt and line again, then puts the cursor back where it belongs. Erasing below
    // clears what a longer line left behind.
    void Refresh();

    void Insert(char character);
    void EraseBefore();
    void Erase(size_t start, size_t count);
    void ErasePreviousWord();
    void MoveTo(size_t position);
    void MoveToEnd();
    void Recall(size_t age);
};

} // namespace FeatherOS::User

#endif
