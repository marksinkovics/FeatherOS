#ifndef FEATHEROS_USER_VI_EDITOR_H
#define FEATHEROS_USER_VI_EDITOR_H

#include "key_decoder.h"

#include <stddef.h>

namespace FeatherOS::User
{

// A line of text on the heap, always followed by a terminator.
struct ViLine
{
    char* text;
    size_t length;
    size_t capacity;
};

// The lines of a file; there is always at least one, which may be empty.
class ViBuffer
{
public:
    ViBuffer() = default;
    ViBuffer(const ViBuffer&) = delete;
    ViBuffer& operator=(const ViBuffer&) = delete;
    ~ViBuffer();

    // A final newline ends the last line rather than starting an empty one.
    bool Load(const char* text, size_t length);
    void Swap(ViBuffer& other);
    size_t Count() const;
    const ViLine& Line(size_t index) const;
    ViLine* MutableLine(size_t index);
    bool InsertLines(size_t at, size_t count);

    // Removing every line leaves one empty line.
    void RemoveLines(size_t at, size_t count);

    // The lines joined with '\n', after the last one too when final_newline; the caller frees it.
    char* Serialize(bool final_newline, size_t* length) const;

private:
    ViLine* m_lines = nullptr;
    size_t m_count = 0;
    size_t m_capacity = 0;

    void Clear();
};

// A small vi: normal, insert, ':' command and '/' search modes. Press takes one key; Render
// appends what the terminal must show, redrawing only the rows that changed. Saving goes through
// the function given to the constructor, so the editor itself never touches files.
class ViEditor
{
public:
    using SaveFunction = bool (*)(void* context, const char* name, const char* text, size_t length);
    enum class Mode : unsigned char
    {
        Normal,
        Insert,
        Command,
        Search,
    };

    enum class Status : unsigned char
    {
        Running,
        Quit,
    };

    static constexpr size_t MaximumUndoSteps = 100;
    static constexpr size_t MaximumUndoBytes = 512UL * 1024;
    static constexpr size_t TabWidth = 8;

    ViEditor(SaveFunction save, void* context);
    ViEditor(const ViEditor&) = delete;
    ViEditor& operator=(const ViEditor&) = delete;
    ~ViEditor();

    // name is null for a buffer that has no file yet; new_file says the file did not exist.
    bool Open(const char* name, const char* text, size_t length, bool new_file);
    void Resize(size_t rows, size_t columns);
    Status Press(const KeyPress& press);
    void Render();
    const char* Output() const;
    size_t OutputLength() const;
    void ClearOutput();
    Mode CurrentMode() const;
    size_t CursorLine() const;
    size_t CursorColumn() const;
    bool Modified() const;
    const char* Message() const;
    const char* Name() const;

    // The text as :w would write it; the caller frees it.
    char* Text(size_t* length) const;

private:
    struct Snapshot
    {
        char* text;
        size_t length;
        size_t line;
        size_t column;
        unsigned long version;
    };

    struct Register
    {
        char* text;
        size_t length;
        // Whole lines, joined with '\n', rather than characters within a line.
        bool linewise;
    };

    SaveFunction m_save;
    void* m_save_context;
    ViBuffer m_buffer;
    char m_name[64] = {};
    Mode m_mode = Mode::Normal;
    size_t m_line = 0;
    size_t m_column = 0;
    // The column j and k aim for; SIZE_MAX after $ keeps them at the end of each line.
    size_t m_desired_column = 0;
    size_t m_count = 0;
    char m_pending = '\0';
    char m_message[96] = {};
    ViLine m_command = {nullptr, 0, 0};
    char m_pattern[64] = {};
    bool m_search_forward = true;
    Register m_register = {nullptr, 0, false};
    // Each change gets a new version, and undo brings back the version it had.
    unsigned long m_version = 0;
    unsigned long m_next_version = 0;
    unsigned long m_saved_version = 0;
    unsigned long m_insert_start_version = 0;
    Snapshot m_undo[MaximumUndoSteps] = {};
    size_t m_undo_count = 0;
    Snapshot m_redo[MaximumUndoSteps] = {};
    size_t m_redo_count = 0;
    size_t m_rows = 24;
    size_t m_columns = 80;
    size_t m_top = 0;
    size_t m_left = 0;
    // What each screen row shows, to redraw only rows that changed.
    char* m_frame = nullptr;
    bool m_frame_valid = false;
    ViLine m_row = {nullptr, 0, 0};
    ViLine m_output = {nullptr, 0, 0};

    void SetName(const char* name);
    __attribute__((format(printf, 2, 3))) void SetMessage(const char* format, ...);
    void Append(const char* text);
    __attribute__((format(printf, 2, 3))) void AppendFormat(const char* format, ...);
    const ViLine& CurrentLine() const;
    size_t LastColumn() const;
    static size_t DisplayColumn(const ViLine& line, size_t column);

    // Puts the cursor back on a character: past the end only in insert mode.
    void ClampColumn();
    void GoToLine(size_t line, size_t column);
    void FirstNonBlank();
    void MoveVertically(size_t line);

    // Undo keeps whole-buffer snapshots, which is simple and cheap for the small files written
    // here; the oldest are dropped past the step and byte limits.
    static void ClearSnapshots(Snapshot* snapshots, size_t* count);
    bool TakeSnapshot(Snapshot* snapshot) const;
    void PushSnapshot(Snapshot* stack, size_t* count, const Snapshot& snapshot);

    // Called before a change; false when there is no memory to undo it, so the change is skipped.
    bool BeginChange();
    void EndChange();
    void Restore(Snapshot* from, size_t* from_count, Snapshot* to, size_t* to_count,
                 const char* empty_message);
    void SetRegister(const char* text, size_t length, bool linewise);
    bool YankLines(size_t first, size_t count);
    size_t LinesFrom(size_t count) const;

    // Word motions treat the end of each line as a blank, and stop at empty lines, as vi does.
    int ClassAt(size_t line, size_t column) const;
    bool Next(size_t* line, size_t* column) const;
    bool Previous(size_t* line, size_t* column) const;
    bool EmptyLine(size_t line) const;
    void WordForward();
    void WordBackward();
    void WordEnd();
    void Search(bool forward);
    bool Write(const char* name);
    Status Execute(const char* command);
    void EnterCommandLine(Mode mode, char prompt);
    Status PressCommandLine(const KeyPress& press);
    void EnterInsert(size_t column);
    void OpenLine(bool below);
    void FinishInsert();
    void JoinWithNext(size_t line, bool add_space);
    void PressInsert(const KeyPress& press);

    // Cursor and page keys, shared by both modes; false for other keys.
    bool Move(const KeyPress& press, size_t count, bool counted);

    // Normal-mode motions written as characters; false for other keys.
    bool MoveByCharacter(char character, size_t count, bool counted);
    void DeleteCharacters(size_t first, size_t count);
    void DeleteLines(size_t count);
    void Put(bool after);
    Status PressPending(char pending, const KeyPress& press, size_t count, bool counted);
    Status PressNormal(const KeyPress& press);
    void BuildTextRow(size_t line_index);

    // The bottom row: the command being typed, or a message or the mode on the left and the
    // cursor's line and column on the right. It stays a column short of the edge, so writing it
    // never wraps and scrolls the screen.
    void BuildStatusRow();
};

} // namespace FeatherOS::User

#endif
