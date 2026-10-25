#ifndef FEATHEROS_USER_LINE_EDITOR_H
#define FEATHEROS_USER_LINE_EDITOR_H

#include "key_decoder.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace FeatherOS::User
{

constexpr size_t MaximumEditedLineLength = 63;
constexpr size_t MaximumHistoryLines = 32;

class LineHistory
{
public:
    // Blank lines and repeats of the newest entry are not kept; the oldest entry goes when full.
    void Add(const char* line)
    {
        if (line[strspn(line, " ")] == '\0' || (m_count > 0 && strcmp(Entry(0), line) == 0))
        {
            return;
        }
        m_newest = (m_newest + 1) % MaximumHistoryLines;
        snprintf(m_lines[m_newest], sizeof(m_lines[m_newest]), "%s", line);
        m_count = m_count < MaximumHistoryLines ? m_count + 1 : m_count;
    }

    size_t Count() const
    {
        return m_count;
    }

    // Age 0 is the newest entry.
    const char* Entry(size_t age) const
    {
        return m_lines[(m_newest + MaximumHistoryLines - age) % MaximumHistoryLines];
    }

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

    void Begin(const char* prompt, size_t columns, LineHistory* history)
    {
        m_prompt = prompt;
        m_prompt_length = strlen(prompt);
        m_columns = columns == 0 ? 80 : columns;
        m_history = history;
        m_history_age = 0;
        m_line[0] = '\0';
        m_length = 0;
        m_cursor = 0;
        m_cursor_row = 0;
        ClearOutput();
        Append(prompt);
        m_cursor_row = m_prompt_length / m_columns;
        if (m_prompt_length > 0 && m_prompt_length % m_columns == 0)
        {
            Append(" \b");
        }
    }

    Status Press(const KeyPress& press)
    {
        if (press.key == Key::Character)
        {
            Insert(press.character);
            return Status::Editing;
        }
        const char control = press.key == Key::Control ? press.character : '\0';
        if (press.key == Key::Enter)
        {
            MoveToEnd();
            Append("\n");
            if (m_history != nullptr)
            {
                m_history->Add(m_line);
            }
            return Status::Accepted;
        }
        if (control == 'c')
        {
            MoveToEnd();
            Append("^C\n");
            return Status::Cancelled;
        }
        if (press.key == Key::Backspace || control == 'h')
        {
            EraseBefore();
        }
        else if (press.key == Key::Delete || control == 'd')
        {
            Erase(m_cursor, m_cursor < m_length ? 1 : 0);
        }
        else if (press.key == Key::Left || control == 'b')
        {
            MoveTo(m_cursor > 0 ? m_cursor - 1 : 0);
        }
        else if (press.key == Key::Right || control == 'f')
        {
            MoveTo(m_cursor < m_length ? m_cursor + 1 : m_length);
        }
        else if (press.key == Key::Home || control == 'a')
        {
            MoveTo(0);
        }
        else if (press.key == Key::End || control == 'e')
        {
            MoveTo(m_length);
        }
        else if (press.key == Key::Up || control == 'p')
        {
            Recall(m_history_age + 1);
        }
        else if (press.key == Key::Down || control == 'n')
        {
            Recall(m_history_age > 0 ? m_history_age - 1 : 0);
        }
        else if (control == 'u')
        {
            Erase(0, m_cursor);
        }
        else if (control == 'k')
        {
            Erase(m_cursor, m_length - m_cursor);
        }
        else if (control == 'w')
        {
            ErasePreviousWord();
        }
        else if (control == 'l')
        {
            Append("\x1b[H\x1b[2J");
            m_cursor_row = 0;
            Refresh();
        }
        return Status::Editing;
    }

    const char* Line() const
    {
        return m_line;
    }

    size_t Cursor() const
    {
        return m_cursor;
    }

    const char* Output() const
    {
        return m_output;
    }

    size_t OutputLength() const
    {
        return m_output_length;
    }

    void ClearOutput()
    {
        m_output_length = 0;
        m_output[0] = '\0';
    }

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

    void Append(const char* text)
    {
        const size_t length = strlen(text);
        if (m_output_length + length < sizeof(m_output))
        {
            memcpy(m_output + m_output_length, text, length + 1);
            m_output_length += length;
        }
    }

    void AppendControl(size_t count, char command)
    {
        char text[16];
        snprintf(text, sizeof(text), "\x1b[%zu%c", count, command);
        Append(text);
    }

    // At the end of a row the cursor has already wrapped, so the next character starts a row.
    bool AtRowStart(size_t position) const
    {
        return (m_prompt_length + position) % m_columns == 0;
    }

    // Draws the prompt and line again, then puts the cursor back where it belongs. Erasing below
    // clears what a longer line left behind.
    void Refresh()
    {
        if (m_cursor_row > 0)
        {
            AppendControl(m_cursor_row, 'A');
        }
        Append("\r");
        Append(m_prompt);
        Append(m_line);
        const size_t end = m_prompt_length + m_length;
        // A terminal that holds the cursor at the last column until the next character, unlike
        // FeatherOS's, also ends up at the start of the next row this way.
        if (end > 0 && end % m_columns == 0)
        {
            Append(" \b");
        }
        Append("\x1b[J");
        const size_t target = m_prompt_length + m_cursor;
        const size_t end_row = end / m_columns;
        const size_t target_row = target / m_columns;
        if (end_row > target_row)
        {
            AppendControl(end_row - target_row, 'A');
        }
        AppendControl(target % m_columns + 1, 'G');
        m_cursor_row = target_row;
    }

    void Insert(char character)
    {
        if (m_length == MaximumEditedLineLength)
        {
            return;
        }
        memmove(m_line + m_cursor + 1, m_line + m_cursor, m_length - m_cursor + 1);
        m_line[m_cursor] = character;
        ++m_length;
        ++m_cursor;
        if (m_cursor != m_length)
        {
            Refresh();
            return;
        }
        const char text[] = {character, '\0'};
        Append(text);
        if (AtRowStart(m_cursor))
        {
            Append(" \b");
            ++m_cursor_row;
        }
    }

    void EraseBefore()
    {
        if (m_cursor == 0)
        {
            return;
        }
        if (m_cursor == m_length && !AtRowStart(m_cursor))
        {
            m_line[--m_length] = '\0';
            --m_cursor;
            Append("\b \b");
            return;
        }
        Erase(m_cursor - 1, 1);
    }

    void Erase(size_t start, size_t count)
    {
        if (count == 0)
        {
            return;
        }
        memmove(m_line + start, m_line + start + count, m_length - start - count + 1);
        m_length -= count;
        m_cursor = start;
        Refresh();
    }

    void ErasePreviousWord()
    {
        size_t start = m_cursor;
        while (start > 0 && m_line[start - 1] == ' ')
        {
            --start;
        }
        while (start > 0 && m_line[start - 1] != ' ')
        {
            --start;
        }
        Erase(start, m_cursor - start);
    }

    void MoveTo(size_t position)
    {
        if (position != m_cursor)
        {
            m_cursor = position;
            Refresh();
        }
    }

    void MoveToEnd()
    {
        MoveTo(m_length);
    }

    void Recall(size_t age)
    {
        if (m_history == nullptr || age == m_history_age || age > m_history->Count())
        {
            return;
        }
        if (m_history_age == 0)
        {
            memcpy(m_draft, m_line, m_length + 1);
        }
        m_history_age = age;
        snprintf(m_line, sizeof(m_line), "%s", age == 0 ? m_draft : m_history->Entry(age - 1));
        m_length = strlen(m_line);
        m_cursor = m_length;
        Refresh();
    }
};

} // namespace FeatherOS::User

#endif
