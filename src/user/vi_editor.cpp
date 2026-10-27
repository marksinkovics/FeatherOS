#include "vi_editor.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace FeatherOS::User
{
namespace
{

bool Reserve(ViLine* line, size_t length)
{
    if (length + 1 <= line->capacity)
    {
        return true;
    }
    size_t capacity = line->capacity < 16 ? 16 : line->capacity;
    while (capacity < length + 1)
    {
        capacity *= 2;
    }
    auto* text = static_cast<char*>(realloc(line->text, capacity));
    if (text == nullptr)
    {
        return false;
    }
    line->text = text;
    line->capacity = capacity;
    return true;
}

bool Insert(ViLine* line, size_t at, const char* text, size_t length)
{
    if (!Reserve(line, line->length + length))
    {
        return false;
    }
    memmove(line->text + at + length, line->text + at, line->length - at + 1);
    memcpy(line->text + at, text, length);
    line->length += length;
    return true;
}

void Erase(ViLine* line, size_t at, size_t count)
{
    memmove(line->text + at, line->text + at + count, line->length - at - count + 1);
    line->length -= count;
}

bool Assign(ViLine* line, const char* text, size_t length)
{
    if (!Reserve(line, length))
    {
        return false;
    }
    memcpy(line->text, text, length);
    line->text[length] = '\0';
    line->length = length;
    return true;
}

bool IsBlank(char character)
{
    return character == ' ' || character == '\t';
}

} // namespace

ViBuffer::~ViBuffer()
{
    Clear();
    free(m_lines);
}

bool ViBuffer::Load(const char* text, size_t length)
{
    Clear();
    size_t start = 0;
    for (size_t index = 0; index <= length; ++index)
    {
        const bool end = index == length;
        if (!end && text[index] != '\n')
        {
            continue;
        }
        if (end && start == length && m_count > 0)
        {
            break;
        }
        if (!InsertLines(m_count, 1) || !Assign(&m_lines[m_count - 1], text + start, index - start))
        {
            return false;
        }
        start = index + 1;
    }
    return true;
}

void ViBuffer::Swap(ViBuffer& other)
{
    ViLine* lines = m_lines;
    const size_t count = m_count;
    const size_t capacity = m_capacity;
    m_lines = other.m_lines;
    m_count = other.m_count;
    m_capacity = other.m_capacity;
    other.m_lines = lines;
    other.m_count = count;
    other.m_capacity = capacity;
}

size_t ViBuffer::Count() const
{
    return m_count;
}

const ViLine& ViBuffer::Line(size_t index) const
{
    return m_lines[index];
}

ViLine* ViBuffer::MutableLine(size_t index)
{
    return &m_lines[index];
}

bool ViBuffer::InsertLines(size_t at, size_t count)
{
    if (m_count + count > m_capacity)
    {
        size_t capacity = m_capacity < 16 ? 16 : m_capacity;
        while (capacity < m_count + count)
        {
            capacity *= 2;
        }
        auto* lines = static_cast<ViLine*>(realloc(m_lines, capacity * sizeof(ViLine)));
        if (lines == nullptr)
        {
            return false;
        }
        m_lines = lines;
        m_capacity = capacity;
    }
    memmove(m_lines + at + count, m_lines + at, (m_count - at) * sizeof(ViLine));
    for (size_t index = at; index < at + count; ++index)
    {
        m_lines[index] = {nullptr, 0, 0};
        if (!Reserve(&m_lines[index], 0))
        {
            // Undo the insertion so the buffer stays consistent.
            for (size_t free_index = at; free_index < index; ++free_index)
            {
                free(m_lines[free_index].text);
            }
            memmove(m_lines + at, m_lines + at + count, (m_count - at) * sizeof(ViLine));
            return false;
        }
        m_lines[index].text[0] = '\0';
    }
    m_count += count;
    return true;
}

void ViBuffer::RemoveLines(size_t at, size_t count)
{
    for (size_t index = at; index < at + count; ++index)
    {
        free(m_lines[index].text);
    }
    memmove(m_lines + at, m_lines + at + count, (m_count - at - count) * sizeof(ViLine));
    m_count -= count;
    if (m_count == 0)
    {
        InsertLines(0, 1);
    }
}

char* ViBuffer::Serialize(bool final_newline, size_t* length) const
{
    size_t total = 0;
    for (size_t index = 0; index < m_count; ++index)
    {
        total += m_lines[index].length + 1;
    }
    auto* text = static_cast<char*>(malloc(total + 1));
    if (text == nullptr)
    {
        return nullptr;
    }
    size_t used = 0;
    for (size_t index = 0; index < m_count; ++index)
    {
        memcpy(text + used, m_lines[index].text, m_lines[index].length);
        used += m_lines[index].length;
        if (final_newline || index + 1 < m_count)
        {
            text[used++] = '\n';
        }
    }
    text[used] = '\0';
    *length = used;
    return text;
}

void ViBuffer::Clear()
{
    for (size_t index = 0; index < m_count; ++index)
    {
        free(m_lines[index].text);
    }
    m_count = 0;
}

ViEditor::ViEditor(SaveFunction save, void* context) : m_save(save), m_save_context(context) {}

ViEditor::~ViEditor()
{
    ClearSnapshots(m_undo, &m_undo_count);
    ClearSnapshots(m_redo, &m_redo_count);
    free(m_register.text);
    free(m_command.text);
    free(m_frame);
    free(m_row.text);
    free(m_output.text);
}

bool ViEditor::Open(const char* name, const char* text, size_t length, bool new_file)
{
    if (!m_buffer.Load(text, length))
    {
        return false;
    }
    SetName(name);
    if (name == nullptr)
    {
        m_message[0] = '\0';
    }
    else if (new_file)
    {
        SetMessage("\"%s\" [New]", m_name);
    }
    else
    {
        SetMessage("\"%s\" %zuL, %zuB", m_name, length == 0 ? 0 : m_buffer.Count(), length);
    }
    return true;
}

void ViEditor::Resize(size_t rows, size_t columns)
{
    m_rows = rows < 2 ? 2 : rows;
    m_columns = columns < 20 ? 20 : columns;
    free(m_frame);
    m_frame = static_cast<char*>(calloc(m_rows, m_columns + 1));
    m_frame_valid = false;
}

ViEditor::Status ViEditor::Press(const KeyPress& press)
{
    switch (m_mode)
    {
    case Mode::Insert:
        PressInsert(press);
        return Status::Running;
    case Mode::Command:
    case Mode::Search:
        return PressCommandLine(press);
    case Mode::Normal:
        break;
    }
    return PressNormal(press);
}

void ViEditor::Render()
{
    if (m_frame == nullptr || !Reserve(&m_row, m_columns))
    {
        return;
    }
    const size_t text_rows = m_rows - 1;
    if (m_line < m_top)
    {
        m_top = m_line;
    }
    if (m_line >= m_top + text_rows)
    {
        m_top = m_line - text_rows + 1;
    }
    const size_t cursor_column = DisplayColumn(m_buffer.Line(m_line), m_column);
    if (cursor_column < m_left)
    {
        m_left = cursor_column;
    }
    if (cursor_column >= m_left + m_columns)
    {
        m_left = cursor_column - m_columns + 1;
    }
    Append("\x1b[?25l");
    for (size_t row = 0; row < m_rows; ++row)
    {
        if (row < text_rows)
        {
            BuildTextRow(m_top + row);
        }
        else
        {
            BuildStatusRow();
        }
        char* previous = m_frame + row * (m_columns + 1);
        if (m_frame_valid && strcmp(previous, m_row.text) == 0)
        {
            continue;
        }
        memcpy(previous, m_row.text, m_row.length + 1);
        AppendFormat("\x1b[%zu;1H", row + 1);
        Append(m_row.text);
        // After a full row the cursor has wrapped, so erasing would clear the next row.
        if (m_row.length < m_columns)
        {
            Append("\x1b[K");
        }
    }
    m_frame_valid = true;
    if (m_mode == Mode::Command || m_mode == Mode::Search)
    {
        const size_t column = m_command.length;
        AppendFormat("\x1b[%zu;%zuH", m_rows, (column < m_columns ? column : m_columns - 1) + 1);
    }
    else
    {
        AppendFormat("\x1b[%zu;%zuH", m_line - m_top + 1, cursor_column - m_left + 1);
    }
    Append("\x1b[?25h");
}

const char* ViEditor::Output() const
{
    return m_output.text == nullptr ? "" : m_output.text;
}

size_t ViEditor::OutputLength() const
{
    return m_output.length;
}

void ViEditor::ClearOutput()
{
    m_output.length = 0;
    if (m_output.text != nullptr)
    {
        m_output.text[0] = '\0';
    }
}

ViEditor::Mode ViEditor::CurrentMode() const
{
    return m_mode;
}

size_t ViEditor::CursorLine() const
{
    return m_line;
}

size_t ViEditor::CursorColumn() const
{
    return m_column;
}

bool ViEditor::Modified() const
{
    return m_version != m_saved_version;
}

const char* ViEditor::Message() const
{
    return m_message;
}

const char* ViEditor::Name() const
{
    return m_name;
}

char* ViEditor::Text(size_t* length) const
{
    return m_buffer.Serialize(true, length);
}

void ViEditor::SetName(const char* name)
{
    snprintf(m_name, sizeof(m_name), "%s", name == nullptr ? "" : name);
}

void ViEditor::SetMessage(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(m_message, sizeof(m_message), format, arguments);
    va_end(arguments);
}

void ViEditor::Append(const char* text)
{
    Insert(&m_output, m_output.length, text, strlen(text));
}

void ViEditor::AppendFormat(const char* format, ...)
{
    char text[32];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    Append(text);
}

const ViLine& ViEditor::CurrentLine() const
{
    return m_buffer.Line(m_line);
}

size_t ViEditor::LastColumn() const
{
    return CurrentLine().length == 0 ? 0 : CurrentLine().length - 1;
}

size_t ViEditor::DisplayColumn(const ViLine& line, size_t column)
{
    size_t display = 0;
    for (size_t index = 0; index < column && index < line.length; ++index)
    {
        display = line.text[index] == '\t' ? (display / TabWidth + 1) * TabWidth : display + 1;
    }
    return display;
}

void ViEditor::ClampColumn()
{
    const size_t limit = m_mode == Mode::Insert ? CurrentLine().length : LastColumn();
    m_column = m_column > limit ? limit : m_column;
}

void ViEditor::GoToLine(size_t line, size_t column)
{
    m_line = line >= m_buffer.Count() ? m_buffer.Count() - 1 : line;
    m_column = column;
    ClampColumn();
}

void ViEditor::FirstNonBlank()
{
    const ViLine& line = CurrentLine();
    size_t column = 0;
    while (column < line.length && IsBlank(line.text[column]))
    {
        ++column;
    }
    m_column = column;
    ClampColumn();
    m_desired_column = m_column;
}

void ViEditor::MoveVertically(size_t line)
{
    m_line = line >= m_buffer.Count() ? m_buffer.Count() - 1 : line;
    m_column = m_desired_column;
    ClampColumn();
}

void ViEditor::ClearSnapshots(Snapshot* snapshots, size_t* count)
{
    for (size_t index = 0; index < *count; ++index)
    {
        free(snapshots[index].text);
    }
    *count = 0;
}

bool ViEditor::TakeSnapshot(Snapshot* snapshot) const
{
    snapshot->text = m_buffer.Serialize(false, &snapshot->length);
    snapshot->line = m_line;
    snapshot->column = m_column;
    snapshot->version = m_version;
    return snapshot->text != nullptr;
}

void ViEditor::PushSnapshot(Snapshot* stack, size_t* count, const Snapshot& snapshot)
{
    size_t bytes = snapshot.length;
    for (size_t index = 0; index < *count; ++index)
    {
        bytes += stack[index].length;
    }
    while (*count > 0 && (*count == MaximumUndoSteps || bytes > MaximumUndoBytes))
    {
        bytes -= stack[0].length;
        free(stack[0].text);
        memmove(stack, stack + 1, (*count - 1) * sizeof(Snapshot));
        --*count;
    }
    stack[(*count)++] = snapshot;
}

bool ViEditor::BeginChange()
{
    Snapshot snapshot;
    if (!TakeSnapshot(&snapshot))
    {
        SetMessage("Out of memory");
        return false;
    }
    PushSnapshot(m_undo, &m_undo_count, snapshot);
    ClearSnapshots(m_redo, &m_redo_count);
    return true;
}

void ViEditor::EndChange()
{
    m_version = ++m_next_version;
}

void ViEditor::Restore(Snapshot* from, size_t* from_count, Snapshot* to, size_t* to_count,
                       const char* empty_message)
{
    if (*from_count == 0)
    {
        SetMessage("%s", empty_message);
        return;
    }
    Snapshot current;
    if (!TakeSnapshot(&current))
    {
        SetMessage("Out of memory");
        return;
    }
    Snapshot& target = from[*from_count - 1];
    ViBuffer restored;
    if (!restored.Load(target.text, target.length))
    {
        free(current.text);
        SetMessage("Out of memory");
        return;
    }
    m_buffer.Swap(restored);
    PushSnapshot(to, to_count, current);
    m_version = target.version;
    GoToLine(target.line, target.column);
    m_desired_column = m_column;
    free(target.text);
    --*from_count;
}

void ViEditor::SetRegister(const char* text, size_t length, bool linewise)
{
    auto* copy = static_cast<char*>(malloc(length + 1));
    if (copy == nullptr)
    {
        return;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    free(m_register.text);
    m_register = {copy, length, linewise};
}

bool ViEditor::YankLines(size_t first, size_t count)
{
    size_t length = 0;
    for (size_t index = first; index < first + count; ++index)
    {
        length += m_buffer.Line(index).length + 1;
    }
    auto* text = static_cast<char*>(malloc(length));
    if (text == nullptr)
    {
        return false;
    }
    size_t used = 0;
    for (size_t index = first; index < first + count; ++index)
    {
        memcpy(text + used, m_buffer.Line(index).text, m_buffer.Line(index).length);
        used += m_buffer.Line(index).length;
        text[used++] = '\n';
    }
    free(m_register.text);
    m_register = {text, used - 1, true};
    return true;
}

size_t ViEditor::LinesFrom(size_t count) const
{
    const size_t available = m_buffer.Count() - m_line;
    return count < available ? count : available;
}

int ViEditor::ClassAt(size_t line, size_t column) const
{
    const ViLine& text = m_buffer.Line(line);
    if (column >= text.length)
    {
        return 0;
    }
    const auto character = static_cast<unsigned char>(text.text[column]);
    if (IsBlank(static_cast<char>(character)))
    {
        return 0;
    }
    const bool word = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '_';
    return word ? 1 : 2;
}

bool ViEditor::Next(size_t* line, size_t* column) const
{
    if (*column < m_buffer.Line(*line).length)
    {
        ++*column;
        return true;
    }
    if (*line + 1 < m_buffer.Count())
    {
        ++*line;
        *column = 0;
        return true;
    }
    return false;
}

bool ViEditor::Previous(size_t* line, size_t* column) const
{
    if (*column > 0)
    {
        --*column;
        return true;
    }
    if (*line > 0)
    {
        --*line;
        *column = m_buffer.Line(*line).length;
        return true;
    }
    return false;
}

bool ViEditor::EmptyLine(size_t line) const
{
    return m_buffer.Line(line).length == 0;
}

void ViEditor::WordForward()
{
    size_t line = m_line;
    size_t column = m_column;
    const int start = ClassAt(line, column);
    while (start != 0 && ClassAt(line, column) == start && Next(&line, &column))
    {
    }
    while (ClassAt(line, column) == 0 && !(column == 0 && EmptyLine(line) && line != m_line))
    {
        if (!Next(&line, &column))
        {
            break;
        }
    }
    m_line = line;
    m_column = column;
}

void ViEditor::WordBackward()
{
    size_t line = m_line;
    size_t column = m_column;
    if (!Previous(&line, &column))
    {
        return;
    }
    while (ClassAt(line, column) == 0 && !(column == 0 && EmptyLine(line)))
    {
        if (!Previous(&line, &column))
        {
            break;
        }
    }
    const int start = ClassAt(line, column);
    while (start != 0 && column > 0 && ClassAt(line, column - 1) == start)
    {
        --column;
    }
    m_line = line;
    m_column = column;
}

void ViEditor::WordEnd()
{
    size_t line = m_line;
    size_t column = m_column;
    if (!Next(&line, &column))
    {
        return;
    }
    while (ClassAt(line, column) == 0)
    {
        if (!Next(&line, &column))
        {
            break;
        }
    }
    const int start = ClassAt(line, column);
    while (start != 0 && ClassAt(line, column + 1) == start)
    {
        ++column;
    }
    m_line = line;
    m_column = column;
}

void ViEditor::Search(bool forward)
{
    if (m_pattern[0] == '\0')
    {
        SetMessage("No previous search pattern");
        return;
    }
    const size_t lines = m_buffer.Count();
    for (size_t step = 0; step <= lines; ++step)
    {
        const size_t line = forward ? (m_line + step) % lines : (m_line + lines - step) % lines;
        const ViLine& text = m_buffer.Line(line);
        size_t found = SIZE_MAX;
        if (forward)
        {
            const size_t from = step == 0 ? m_column + 1 : 0;
            const char* match = from <= text.length ? strstr(text.text + from, m_pattern) : nullptr;
            found = match == nullptr ? SIZE_MAX : static_cast<size_t>(match - text.text);
        }
        else
        {
            for (const char* match = strstr(text.text, m_pattern); match != nullptr;
                 match = strstr(match + 1, m_pattern))
            {
                const auto column = static_cast<size_t>(match - text.text);
                if (step != 0 || column < m_column)
                {
                    found = column;
                }
            }
        }
        if (found == SIZE_MAX)
        {
            continue;
        }
        const bool wrapped = forward ? line < m_line || (line == m_line && found <= m_column)
                                     : line > m_line || (line == m_line && found >= m_column);
        if (wrapped)
        {
            SetMessage("%s", forward ? "search hit BOTTOM, continuing at TOP"
                                     : "search hit TOP, continuing at BOTTOM");
        }
        else
        {
            SetMessage("%c%s", forward ? '/' : '?', m_pattern);
        }
        m_line = line;
        m_column = found;
        m_desired_column = m_column;
        return;
    }
    SetMessage("Pattern not found: %s", m_pattern);
}

bool ViEditor::Write(const char* name)
{
    const char* target = name[0] != '\0' ? name : m_name;
    if (target[0] == '\0')
    {
        SetMessage("No file name");
        return false;
    }
    size_t length = 0;
    char* text = m_buffer.Serialize(true, &length);
    if (text == nullptr)
    {
        SetMessage("Out of memory");
        return false;
    }
    const bool saved = m_save(m_save_context, target, text, length);
    free(text);
    if (!saved)
    {
        SetMessage("\"%s\" could not be written", target);
        return false;
    }
    if (m_name[0] == '\0')
    {
        SetName(target);
    }
    if (strcmp(target, m_name) == 0)
    {
        m_saved_version = m_version;
    }
    SetMessage("\"%s\" %zuL, %zuB written", target, m_buffer.Count(), length);
    return true;
}

ViEditor::Status ViEditor::Execute(const char* command)
{
    while (*command == ' ' || *command == ':')
    {
        ++command;
    }
    if (*command == '\0')
    {
        return Status::Running;
    }
    if (strcmp(command, "$") == 0)
    {
        GoToLine(m_buffer.Count() - 1, 0);
        FirstNonBlank();
        return Status::Running;
    }
    if (strspn(command, "0123456789") == strlen(command))
    {
        const unsigned long line = strtoul(command, nullptr, 10);
        GoToLine(line == 0 ? 0 : line - 1, 0);
        FirstNonBlank();
        return Status::Running;
    }
    char name[16] = {};
    size_t name_length = strcspn(command, " ");
    if (name_length >= sizeof(name))
    {
        name_length = sizeof(name) - 1;
    }
    memcpy(name, command, name_length);
    const char* argument = command + strcspn(command, " ");
    while (*argument == ' ')
    {
        ++argument;
    }
    if (strcmp(name, "w") == 0)
    {
        Write(argument);
    }
    else if (strcmp(name, "q") == 0)
    {
        if (!Modified())
        {
            return Status::Quit;
        }
        SetMessage("No write since last change (add ! to override)");
    }
    else if (strcmp(name, "wq") == 0 || (strcmp(name, "x") == 0 && Modified()))
    {
        return Write(argument) ? Status::Quit : Status::Running;
    }
    else if (strcmp(name, "q!") == 0 || strcmp(name, "x") == 0)
    {
        return Status::Quit;
    }
    else
    {
        SetMessage("Not an editor command: %s", command);
    }
    return Status::Running;
}

void ViEditor::EnterCommandLine(Mode mode, char prompt)
{
    m_mode = mode;
    Assign(&m_command, &prompt, 1);
    m_message[0] = '\0';
}

ViEditor::Status ViEditor::PressCommandLine(const KeyPress& press)
{
    if (press.key == Key::Character)
    {
        Insert(&m_command, m_command.length, &press.character, 1);
        return Status::Running;
    }
    if (press.key == Key::Backspace)
    {
        Erase(&m_command, m_command.length - 1, 1);
        if (m_command.length == 0)
        {
            m_mode = Mode::Normal;
        }
        return Status::Running;
    }
    const bool cancel =
        press.key == Key::Escape || (press.key == Key::Control && press.character == 'c');
    if (cancel)
    {
        m_mode = Mode::Normal;
        return Status::Running;
    }
    if (press.key != Key::Enter)
    {
        return Status::Running;
    }
    m_mode = Mode::Normal;
    if (m_command.text[0] == ':')
    {
        return Execute(m_command.text + 1);
    }
    m_search_forward = m_command.text[0] == '/';
    if (m_command.length > 1)
    {
        snprintf(m_pattern, sizeof(m_pattern), "%s", m_command.text + 1);
    }
    Search(m_search_forward);
    return Status::Running;
}

void ViEditor::EnterInsert(size_t column)
{
    if (!BeginChange())
    {
        return;
    }
    m_insert_start_version = m_version;
    m_mode = Mode::Insert;
    m_column = column;
    ClampColumn();
    m_message[0] = '\0';
}

void ViEditor::OpenLine(bool below)
{
    if (!BeginChange())
    {
        return;
    }
    m_insert_start_version = m_version;
    const size_t at = below ? m_line + 1 : m_line;
    if (!m_buffer.InsertLines(at, 1))
    {
        SetMessage("Out of memory");
        return;
    }
    EndChange();
    m_line = at;
    m_column = 0;
    m_mode = Mode::Insert;
    m_message[0] = '\0';
}

void ViEditor::FinishInsert()
{
    m_mode = Mode::Normal;
    if (m_version == m_insert_start_version && m_undo_count > 0)
    {
        free(m_undo[m_undo_count - 1].text);
        --m_undo_count;
    }
    m_column = m_column > 0 ? m_column - 1 : 0;
    ClampColumn();
    m_desired_column = m_column;
}

void ViEditor::JoinWithNext(size_t line, bool add_space)
{
    ViLine* first = m_buffer.MutableLine(line);
    const ViLine& second = m_buffer.Line(line + 1);
    size_t skip = 0;
    while (add_space && skip < second.length && IsBlank(second.text[skip]))
    {
        ++skip;
    }
    const bool space =
        add_space && first->length > 0 && skip < second.length && second.text[skip] != ')';
    const size_t join_column = first->length;
    if ((space && !Insert(first, first->length, " ", 1)) ||
        !Insert(first, first->length, second.text + skip, second.length - skip))
    {
        SetMessage("Out of memory");
        return;
    }
    m_buffer.RemoveLines(line + 1, 1);
    m_line = line;
    m_column = join_column;
}

void ViEditor::PressInsert(const KeyPress& press)
{
    ViLine* line = m_buffer.MutableLine(m_line);
    const char control = press.key == Key::Control ? press.character : '\0';
    if (press.key == Key::Character || press.key == Key::Tab)
    {
        const char character = press.key == Key::Tab ? '\t' : press.character;
        if (Insert(line, m_column, &character, 1))
        {
            ++m_column;
            EndChange();
        }
    }
    else if (press.key == Key::Enter)
    {
        if (m_buffer.InsertLines(m_line + 1, 1))
        {
            line = m_buffer.MutableLine(m_line);
            Assign(m_buffer.MutableLine(m_line + 1), line->text + m_column,
                   line->length - m_column);
            line->length = m_column;
            line->text[m_column] = '\0';
            ++m_line;
            m_column = 0;
            EndChange();
        }
    }
    else if (press.key == Key::Backspace || control == 'h')
    {
        if (m_column > 0)
        {
            Erase(line, --m_column, 1);
            EndChange();
        }
        else if (m_line > 0)
        {
            JoinWithNext(m_line - 1, false);
            EndChange();
        }
    }
    else if (press.key == Key::Delete)
    {
        if (m_column < line->length)
        {
            Erase(line, m_column, 1);
            EndChange();
        }
        else if (m_line + 1 < m_buffer.Count())
        {
            JoinWithNext(m_line, false);
            EndChange();
        }
    }
    else if (press.key == Key::Escape || control == 'c')
    {
        FinishInsert();
        return;
    }
    else if (press.key != Key::Control)
    {
        Move(press, 1, false);
        return;
    }
    m_desired_column = m_column;
}

bool ViEditor::Move(const KeyPress& press, size_t count, bool counted)
{
    const char control = press.key == Key::Control ? press.character : '\0';
    const size_t page = m_rows > 3 ? m_rows - 3 : 1;
    switch (press.key)
    {
    case Key::Left:
        m_column = m_column > count ? m_column - count : 0;
        break;
    case Key::Right:
        m_column += count;
        ClampColumn();
        break;
    case Key::Up:
        MoveVertically(m_line > count ? m_line - count : 0);
        return true;
    case Key::Down:
        MoveVertically(m_line + count);
        return true;
    case Key::Home:
        m_column = 0;
        break;
    case Key::End:
        m_column = CurrentLine().length;
        ClampColumn();
        m_desired_column = SIZE_MAX;
        return true;
    case Key::PageUp:
        MoveVertically(m_line > page * count ? m_line - page * count : 0);
        return true;
    case Key::PageDown:
        MoveVertically(m_line + page * count);
        return true;
    default:
        if (control == 'f' || control == 'b')
        {
            return Move({control == 'f' ? Key::PageDown : Key::PageUp, '\0'}, count, counted);
        }
        if (control == 'd' || control == 'u')
        {
            const size_t half = (m_rows - 1) / 2;
            MoveVertically(control == 'd' ? m_line + half : (m_line > half ? m_line - half : 0));
            return true;
        }
        return false;
    }
    m_desired_column = m_column;
    return true;
}

bool ViEditor::MoveByCharacter(char character, size_t count, bool counted)
{
    switch (character)
    {
    case 'h':
        return Move({Key::Left, '\0'}, count, counted);
    case 'l':
    case ' ':
        return Move({Key::Right, '\0'}, count, counted);
    case 'k':
        return Move({Key::Up, '\0'}, count, counted);
    case 'j':
        return Move({Key::Down, '\0'}, count, counted);
    case '0':
        return Move({Key::Home, '\0'}, count, counted);
    case '$':
        MoveVertically(m_line + count - 1);
        return Move({Key::End, '\0'}, count, counted);
    case '^':
        FirstNonBlank();
        return true;
    case '+':
        MoveVertically(m_line + count);
        FirstNonBlank();
        return true;
    case '-':
        MoveVertically(m_line > count ? m_line - count : 0);
        FirstNonBlank();
        return true;
    case 'G':
        GoToLine(counted ? count - 1 : m_buffer.Count() - 1, 0);
        FirstNonBlank();
        return true;
    case 'w':
    case 'b':
    case 'e':
        for (size_t step = 0; step < count; ++step)
        {
            character == 'w' ? WordForward() : character == 'b' ? WordBackward() : WordEnd();
        }
        ClampColumn();
        m_desired_column = m_column;
        return true;
    case 'n':
    case 'N':
        for (size_t step = 0; step < count; ++step)
        {
            Search(m_search_forward == (character == 'n'));
        }
        return true;
    default:
        return false;
    }
}

void ViEditor::DeleteCharacters(size_t first, size_t count)
{
    const ViLine& line = CurrentLine();
    if (first >= line.length || count == 0 || !BeginChange())
    {
        return;
    }
    count = first + count > line.length ? line.length - first : count;
    SetRegister(line.text + first, count, false);
    Erase(m_buffer.MutableLine(m_line), first, count);
    EndChange();
    m_column = first;
    ClampColumn();
    m_desired_column = m_column;
}

void ViEditor::DeleteLines(size_t count)
{
    count = LinesFrom(count);
    if (!BeginChange() || !YankLines(m_line, count))
    {
        return;
    }
    m_buffer.RemoveLines(m_line, count);
    EndChange();
    GoToLine(m_line, 0);
    FirstNonBlank();
    if (count > 2)
    {
        SetMessage("%zu fewer lines", count);
    }
}

void ViEditor::Put(bool after)
{
    if (m_register.text == nullptr || !BeginChange())
    {
        return;
    }
    if (m_register.linewise)
    {
        const size_t at = after ? m_line + 1 : m_line;
        size_t lines = 1;
        for (size_t index = 0; index < m_register.length; ++index)
        {
            lines += m_register.text[index] == '\n' ? 1 : 0;
        }
        if (!m_buffer.InsertLines(at, lines))
        {
            SetMessage("Out of memory");
            return;
        }
        const char* start = m_register.text;
        for (size_t index = 0; index < lines; ++index)
        {
            const char* end = strchr(start, '\n');
            const size_t length = end == nullptr ? strlen(start) : static_cast<size_t>(end - start);
            Assign(m_buffer.MutableLine(at + index), start, length);
            start = end == nullptr ? start + length : end + 1;
        }
        EndChange();
        m_line = at;
        FirstNonBlank();
        return;
    }
    const size_t at = after && CurrentLine().length > 0 ? m_column + 1 : m_column;
    if (!Insert(m_buffer.MutableLine(m_line), at, m_register.text, m_register.length))
    {
        SetMessage("Out of memory");
        return;
    }
    EndChange();
    m_column = at + m_register.length - 1;
    ClampColumn();
    m_desired_column = m_column;
}

ViEditor::Status ViEditor::PressPending(char pending, const KeyPress& press, size_t count,
                                        bool counted)
{
    const char character = press.key == Key::Character ? press.character : '\0';
    if (pending == 'r')
    {
        const ViLine& line = CurrentLine();
        if (character != '\0' && m_column + count <= line.length && BeginChange())
        {
            memset(m_buffer.MutableLine(m_line)->text + m_column, character, count);
            m_column += count - 1;
            EndChange();
        }
    }
    else if (pending == 'g' && character == 'g')
    {
        GoToLine(counted ? count - 1 : 0, 0);
        FirstNonBlank();
    }
    else if (pending == 'd' && character == 'd')
    {
        DeleteLines(count);
    }
    else if (pending == 'y' && character == 'y')
    {
        const size_t lines = LinesFrom(count);
        YankLines(m_line, lines);
        if (lines > 2)
        {
            SetMessage("%zu lines yanked", lines);
        }
    }
    else if (pending == 'Z' && character == 'Z')
    {
        return Execute("x");
    }
    else if (pending == 'Z' && character == 'Q')
    {
        return Status::Quit;
    }
    return Status::Running;
}

ViEditor::Status ViEditor::PressNormal(const KeyPress& press)
{
    const char character = press.key == Key::Character ? press.character : '\0';
    const char control = press.key == Key::Control ? press.character : '\0';
    if (m_pending != 'r' && character >= '0' && character <= '9' &&
        (character != '0' || m_count > 0))
    {
        m_count = m_count < 10000 ? m_count * 10 + static_cast<size_t>(character - '0') : m_count;
        return Status::Running;
    }
    const bool counted = m_count > 0;
    const size_t count = counted ? m_count : 1;
    if (m_pending != '\0')
    {
        const char pending = m_pending;
        m_pending = '\0';
        m_count = 0;
        return PressPending(pending, press, count, counted);
    }
    if (character == 'g' || character == 'd' || character == 'y' || character == 'r' ||
        character == 'Z')
    {
        m_pending = character;
        return Status::Running;
    }
    m_count = 0;
    if (press.key == Key::Escape || control == 'c')
    {
        return Status::Running;
    }
    if (press.key == Key::Enter)
    {
        MoveByCharacter('+', count, counted);
        return Status::Running;
    }
    if (press.key == Key::Backspace || control == 'h')
    {
        Move({Key::Left, '\0'}, count, counted);
        return Status::Running;
    }
    if (control == 'p' || control == 'n')
    {
        Move({control == 'p' ? Key::Up : Key::Down, '\0'}, count, counted);
        return Status::Running;
    }
    if (Move(press, count, counted) || MoveByCharacter(character, count, counted))
    {
        return Status::Running;
    }
    if (press.key == Key::Delete)
    {
        DeleteCharacters(m_column, count);
        return Status::Running;
    }
    if (control == 'r')
    {
        Restore(m_redo, &m_redo_count, m_undo, &m_undo_count, "Already at newest change");
        return Status::Running;
    }
    if (control == 'l')
    {
        m_frame_valid = false;
        Append("\x1b[H\x1b[2J");
        return Status::Running;
    }
    if (control == 'g')
    {
        SetMessage("\"%s\"%s %zu lines --%zu%%--", m_name[0] != '\0' ? m_name : "[No Name]",
                   Modified() ? " [Modified]" : "", m_buffer.Count(),
                   (m_line + 1) * 100 / m_buffer.Count());
        return Status::Running;
    }
    switch (character)
    {
    case 'i':
        EnterInsert(m_column);
        break;
    case 'a':
        EnterInsert(CurrentLine().length == 0 ? 0 : m_column + 1);
        break;
    case 'I':
        FirstNonBlank();
        EnterInsert(m_column);
        break;
    case 'A':
        EnterInsert(CurrentLine().length);
        break;
    case 'o':
    case 'O':
        OpenLine(character == 'o');
        break;
    case 'x':
        DeleteCharacters(m_column, count);
        break;
    case 'X':
        if (m_column > 0)
        {
            const size_t first = m_column > count ? m_column - count : 0;
            DeleteCharacters(first, m_column - first);
        }
        break;
    case 'D':
        DeleteCharacters(m_column, CurrentLine().length);
        break;
    case 'J':
        if (m_line + 1 < m_buffer.Count() && BeginChange())
        {
            const size_t joins = LinesFrom(count < 2 ? 2 : count) - 1;
            for (size_t step = 0; step < joins; ++step)
            {
                JoinWithNext(m_line, true);
            }
            EndChange();
            ClampColumn();
            m_desired_column = m_column;
        }
        break;
    case 'Y':
        YankLines(m_line, LinesFrom(count));
        break;
    case 'p':
    case 'P':
        Put(character == 'p');
        break;
    case 'u':
        Restore(m_undo, &m_undo_count, m_redo, &m_redo_count, "Already at oldest change");
        break;
    case ':':
        EnterCommandLine(Mode::Command, ':');
        break;
    case '/':
    case '?':
        EnterCommandLine(Mode::Search, character);
        break;
    default:
        break;
    }
    return Status::Running;
}

void ViEditor::BuildTextRow(size_t line_index)
{
    m_row.length = 0;
    m_row.text[0] = '\0';
    if (line_index >= m_buffer.Count())
    {
        Insert(&m_row, 0, "~", 1);
        return;
    }
    const ViLine& line = m_buffer.Line(line_index);
    size_t display = 0;
    for (size_t index = 0; index < line.length && m_row.length < m_columns; ++index)
    {
        const char character = line.text[index];
        const size_t width = character == '\t' ? (display / TabWidth + 1) * TabWidth - display : 1;
        for (size_t cell = 0; cell < width && m_row.length < m_columns; ++cell, ++display)
        {
            if (display < m_left)
            {
                continue;
            }
            const auto value = static_cast<unsigned char>(character);
            const char shown = character == '\t'               ? ' '
                               : value < 0x20 || value >= 0x7F ? '?'
                                                               : character;
            m_row.text[m_row.length++] = shown;
        }
    }
    m_row.text[m_row.length] = '\0';
}

void ViEditor::BuildStatusRow()
{
    const size_t width = m_columns - 1;
    m_row.length = 0;
    if (m_mode == Mode::Command || m_mode == Mode::Search)
    {
        const size_t length = m_command.length < width ? m_command.length : width;
        memcpy(m_row.text, m_command.text, length);
        m_row.length = length;
        m_row.text[length] = '\0';
        return;
    }
    const char* left = m_message[0] != '\0'     ? m_message
                       : m_mode == Mode::Insert ? "-- INSERT --"
                                                : "";
    char ruler[32];
    snprintf(ruler, sizeof(ruler), "%zu,%zu", m_line + 1, m_column + 1);
    const size_t ruler_column = width > 18 ? width - 18 : 0;
    size_t length = strlen(left);
    length = length < width ? length : width;
    memcpy(m_row.text, left, length);
    m_row.length = length;
    if (ruler_column >= length + 1)
    {
        memset(m_row.text + m_row.length, ' ', ruler_column - m_row.length);
        m_row.length = ruler_column;
        const size_t ruler_length = strlen(ruler);
        memcpy(m_row.text + m_row.length, ruler, ruler_length);
        m_row.length += ruler_length;
    }
    m_row.text[m_row.length] = '\0';
}

} // namespace FeatherOS::User
