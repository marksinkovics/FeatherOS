#include <catch2/catch_test_macros.hpp>

#include <limine.h>

#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"
#include "user/vi_editor.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace
{

using FeatherOS::User::Key;
using FeatherOS::User::KeyPress;
using FeatherOS::User::ViEditor;

struct SavedFile
{
    std::string name;
    std::string text;
    int saves = 0;
    bool fail = false;
};

bool Save(void* context, const char* name, const char* text, size_t length)
{
    auto* saved = static_cast<SavedFile*>(context);
    if (saved->fail)
    {
        return false;
    }
    saved->name = name;
    saved->text.assign(text, length);
    ++saved->saves;
    return true;
}

// ESC is the Escape key, '\n' Enter, '\t' Tab, 0x7F Backspace and other bytes below 27 Ctrl+A
// to Ctrl+Z.
ViEditor::Status Type(ViEditor& editor, const std::string& keys)
{
    ViEditor::Status status = ViEditor::Status::Running;
    for (const char character : keys)
    {
        KeyPress press = {Key::Character, character};
        if (character == '\x1b')
        {
            press = {Key::Escape, '\0'};
        }
        else if (character == '\n')
        {
            press = {Key::Enter, '\0'};
        }
        else if (character == '\t')
        {
            press = {Key::Tab, '\0'};
        }
        else if (character == '\x7f')
        {
            press = {Key::Backspace, '\0'};
        }
        else if (character >= 1 && character <= 26)
        {
            press = {Key::Control, static_cast<char>(character - 1 + 'a')};
        }
        status = editor.Press(press);
    }
    return status;
}

void Press(ViEditor& editor, Key key)
{
    editor.Press({key, '\0'});
}

std::string Text(const ViEditor& editor)
{
    size_t length = 0;
    char* text = editor.Text(&length);
    std::string result(text, length);
    free(text);
    return result;
}

void Open(ViEditor& editor, const std::string& text, const char* name = "notes.txt")
{
    REQUIRE(editor.Open(name, text.data(), text.size(), false));
}

void RequireCursor(const ViEditor& editor, size_t line, size_t column)
{
    REQUIRE(editor.CursorLine() == line);
    REQUIRE(editor.CursorColumn() == column);
}

} // namespace

TEST_CASE("vi loads lines and reports the file")
{
    SavedFile saved;
    ViEditor editor(Save, &saved);
    Open(editor, "one\ntwo\n");
    REQUIRE(std::string(editor.Message()) == "\"notes.txt\" 2L, 8B");
    REQUIRE(Text(editor) == "one\ntwo\n");
    REQUIRE_FALSE(editor.Modified());

    ViEditor last_line(Save, &saved);
    Open(last_line, "no newline");
    REQUIRE(Text(last_line) == "no newline\n");
    ViEditor empty(Save, &saved);
    REQUIRE(empty.Open("new.txt", "", 0, true));
    REQUIRE(std::string(empty.Message()) == "\"new.txt\" [New]");
    REQUIRE(Text(empty) == "\n");
}

TEST_CASE("vi inserts text in each way and Escape steps back")
{
    SavedFile saved;
    ViEditor editor(Save, &saved);
    REQUIRE(editor.Open(nullptr, "", 0, true));
    Type(editor, "ihello world");
    REQUIRE(editor.CurrentMode() == ViEditor::Mode::Insert);
    Type(editor, "\x1b");
    REQUIRE(editor.CurrentMode() == ViEditor::Mode::Normal);
    RequireCursor(editor, 0, 10);
    Type(editor, "othird\x1bOsecond\x1b");
    REQUIRE(Text(editor) == "hello world\nsecond\nthird\n");
    RequireCursor(editor, 1, 5);
    Type(editor, "I> \x1b" "A!\x1b" "0ab\x1b");
    REQUIRE(Text(editor) == "hello world\n>b second!\nthird\n");

    // Enter splits a line; Backspace at a line's start joins it to the one above.
    Type(editor, "ggwi\n\x1b");
    REQUIRE(Text(editor) == "hello \nworld\n>b second!\nthird\n");
    RequireCursor(editor, 1, 0);
    Type(editor, "i\x7f\x1b");
    REQUIRE(Text(editor) == "hello world\n>b second!\nthird\n");
    RequireCursor(editor, 0, 5);
    Type(editor, "$a\x7f\x7f\tX\x1b");
    REQUIRE(Text(editor) == "hello wor\tX\n>b second!\nthird\n");
    REQUIRE(editor.Modified());
}

TEST_CASE("vi moves by characters, words, lines and counts")
{
    SavedFile saved;
    ViEditor editor(Save, &saved);
    Open(editor, "  first line here\nsecond, with punctuation\n\nlast\n");
    Type(editor, "w");
    RequireCursor(editor, 0, 2);
    Type(editor, "w");
    RequireCursor(editor, 0, 8);
    Type(editor, "w");
    RequireCursor(editor, 0, 13);
    Type(editor, "w");
    RequireCursor(editor, 1, 0);
    Type(editor, "w");
    RequireCursor(editor, 1, 6);
    Type(editor, "e");
    RequireCursor(editor, 1, 11);
    Type(editor, "b");
    RequireCursor(editor, 1, 8);
    // An empty line is a word of its own.
    Type(editor, "2w");
    RequireCursor(editor, 2, 0);
    Type(editor, "w");
    RequireCursor(editor, 3, 0);
    Type(editor, "w");
    RequireCursor(editor, 3, 3);
    Type(editor, "gg^");
    RequireCursor(editor, 0, 2);
    Type(editor, "$");
    RequireCursor(editor, 0, 16);
    // $ keeps the cursor at the end of each line it moves to.
    Type(editor, "j");
    RequireCursor(editor, 1, 23);
    Type(editor, "j");
    RequireCursor(editor, 2, 0);
    Type(editor, "k0");
    RequireCursor(editor, 1, 0);
    Type(editor, "5l");
    RequireCursor(editor, 1, 5);
    // j and k aim for the column they started from, through shorter lines.
    Type(editor, "jj");
    RequireCursor(editor, 3, 3);
    Type(editor, "kk");
    RequireCursor(editor, 1, 5);
    Type(editor, "2G");
    RequireCursor(editor, 1, 0);
    Type(editor, "G");
    RequireCursor(editor, 3, 0);
    Type(editor, "1G3h");
    RequireCursor(editor, 0, 0);
    Type(editor, "\n");
    RequireCursor(editor, 1, 0);
    Type(editor, "-");
    RequireCursor(editor, 0, 2);
    Press(editor, Key::End);
    RequireCursor(editor, 0, 16);
    Press(editor, Key::Home);
    Press(editor, Key::Down);
    Press(editor, Key::Right);
    RequireCursor(editor, 1, 1);
    Type(editor, ":4\n");
    RequireCursor(editor, 3, 0);
    Type(editor, ":1\n:$\n");
    RequireCursor(editor, 3, 0);
}

TEST_CASE("vi deletes, yanks and puts characters and lines")
{
    SavedFile saved;
    ViEditor editor(Save, &saved);
    Open(editor, "abcdef\none\ntwo\nthree\n");
    Type(editor, "x");
    REQUIRE(Text(editor) == "bcdef\none\ntwo\nthree\n");
    Type(editor, "2x$p");
    REQUIRE(Text(editor) == "defbc\none\ntwo\nthree\n");
    Type(editor, "0lD");
    REQUIRE(Text(editor) == "d\none\ntwo\nthree\n");
    RequireCursor(editor, 0, 0);
    Type(editor, "$X");
    Type(editor, "P");
    REQUIRE(Text(editor) == "efbcd\none\ntwo\nthree\n");
    Type(editor, "j2dd");
    REQUIRE(Text(editor) == "efbcd\nthree\n");
    Type(editor, "p");
    REQUIRE(Text(editor) == "efbcd\nthree\none\ntwo\n");
    RequireCursor(editor, 2, 0);
    Type(editor, "ggyyGp");
    REQUIRE(Text(editor) == "efbcd\nthree\none\ntwo\nefbcd\n");
    Type(editor, "k2YggP");
    REQUIRE(Text(editor) == "two\nefbcd\nefbcd\nthree\none\ntwo\nefbcd\n");
    Type(editor, "r!");
    Type(editor, "l3rz");
    REQUIRE(Text(editor) == "!wo\nefbcd\nefbcd\nthree\none\ntwo\nefbcd\n");
    Type(editor, "2rz");
    REQUIRE(Text(editor) == "!zz\nefbcd\nefbcd\nthree\none\ntwo\nefbcd\n");
    // A count longer than the line leaves it unchanged.
    Type(editor, "9r-");
    REQUIRE(Text(editor) == "!zz\nefbcd\nefbcd\nthree\none\ntwo\nefbcd\n");
    Type(editor, "3J");
    REQUIRE(Text(editor) == "!zz efbcd efbcd\nthree\none\ntwo\nefbcd\n");
    Type(editor, "G9dd");
    REQUIRE(Text(editor) == "!zz efbcd efbcd\nthree\none\ntwo\n");
    Type(editor, "gg99dd");
    REQUIRE(Text(editor) == "\n");
}

TEST_CASE("vi undoes and redoes changes and tracks unsaved ones")
{
    SavedFile saved;
    ViEditor editor(Save, &saved);
    Open(editor, "base\n");
    Type(editor, "Aone\x1b" "otwo\x1b" "dd");
    REQUIRE(Text(editor) == "baseone\n");
    Type(editor, "u");
    REQUIRE(Text(editor) == "baseone\ntwo\n");
    Type(editor, "u");
    REQUIRE(Text(editor) == "baseone\n");
    Type(editor, "u");
    REQUIRE(Text(editor) == "base\n");
    REQUIRE_FALSE(editor.Modified());
    Type(editor, "u");
    REQUIRE(std::string(editor.Message()) == "Already at oldest change");
    Type(editor, "\x12\x12");
    REQUIRE(Text(editor) == "baseone\ntwo\n");
    REQUIRE(editor.Modified());
    Type(editor, "\x12\x12");
    REQUIRE(Text(editor) == "baseone\n");
    REQUIRE(std::string(editor.Message()) == "Already at newest change");
    // An insert that typed nothing is not a change, and a new change ends redo.
    Type(editor, "i\x1bu");
    REQUIRE(Text(editor) == "baseone\ntwo\n");
    RequireCursor(editor, 1, 2);
    Type(editor, "x\x12");
    REQUIRE(Text(editor) == "baseone\ntw\n");
    REQUIRE(std::string(editor.Message()) == "Already at newest change");
}

TEST_CASE("vi writes and quits only when the changes are safe")
{
    SavedFile saved;
    ViEditor editor(Save, &saved);
    Open(editor, "keep\n");
    REQUIRE(Type(editor, ":q\n") == ViEditor::Status::Quit);

    ViEditor changed(Save, &saved);
    Open(changed, "keep\n");
    Type(changed, "x");
    REQUIRE(Type(changed, ":q\n") == ViEditor::Status::Running);
    REQUIRE(std::string(changed.Message()) == "No write since last change (add ! to override)");
    Type(changed, ":w\n");
    REQUIRE(saved.name == "notes.txt");
    REQUIRE(saved.text == "eep\n");
    REQUIRE(std::string(changed.Message()) == "\"notes.txt\" 1L, 4B written");
    REQUIRE_FALSE(changed.Modified());
    REQUIRE(Type(changed, ":x\n") == ViEditor::Status::Quit);
    REQUIRE(saved.saves == 1);

    // Writing elsewhere keeps the buffer's own file unsaved.
    ViEditor copy(Save, &saved);
    Open(copy, "a\n");
    Type(copy, "x:w other.txt\n");
    REQUIRE(saved.name == "other.txt");
    REQUIRE(copy.Modified());
    REQUIRE(std::string(copy.Name()) == "notes.txt");
    REQUIRE(Type(copy, ":q!\n") == ViEditor::Status::Quit);

    ViEditor unnamed(Save, &saved);
    REQUIRE(unnamed.Open(nullptr, "", 0, true));
    Type(unnamed, "ihi\x1b:w\n");
    REQUIRE(std::string(unnamed.Message()) == "No file name");
    saved.fail = true;
    Type(unnamed, ":wq new.txt\n");
    REQUIRE(std::string(unnamed.Message()) == "\"new.txt\" could not be written");
    saved.fail = false;
    REQUIRE(Type(unnamed, ":wq new.txt\n") == ViEditor::Status::Quit);
    REQUIRE(saved.text == "hi\n");
    REQUIRE(std::string(unnamed.Name()) == "new.txt");

    ViEditor zz(Save, &saved);
    Open(zz, "z\n");
    Type(zz, "x");
    REQUIRE(Type(zz, "ZZ") == ViEditor::Status::Quit);
    REQUIRE(saved.text == "\n");
    ViEditor zq(Save, &saved);
    Open(zq, "z\n");
    Type(zq, "x");
    REQUIRE(Type(zq, "ZQ") == ViEditor::Status::Quit);

    Type(editor, ":nonsense\n");
    REQUIRE(std::string(editor.Message()) == "Not an editor command: nonsense");
    // Backspace past the colon leaves the command line, and Escape abandons it.
    Type(editor, ":\x7f");
    REQUIRE(editor.CurrentMode() == ViEditor::Mode::Normal);
    REQUIRE(Type(editor, ":q\x1b") == ViEditor::Status::Running);
    REQUIRE(editor.CurrentMode() == ViEditor::Mode::Normal);
}

TEST_CASE("vi searches forward and back, wrapping around")
{
    SavedFile saved;
    ViEditor editor(Save, &saved);
    Open(editor, "alpha beta\ngamma alpha\nbeta\n");
    Type(editor, "/alpha\n");
    RequireCursor(editor, 1, 6);
    REQUIRE(std::string(editor.Message()) == "/alpha");
    Type(editor, "n");
    RequireCursor(editor, 0, 0);
    REQUIRE(std::string(editor.Message()) == "search hit BOTTOM, continuing at TOP");
    Type(editor, "N");
    RequireCursor(editor, 1, 6);
    REQUIRE(std::string(editor.Message()) == "search hit TOP, continuing at BOTTOM");
    Type(editor, "?beta\n");
    RequireCursor(editor, 0, 6);
    Type(editor, "n");
    RequireCursor(editor, 2, 0);
    Type(editor, "/\n");
    RequireCursor(editor, 0, 6);
    Type(editor, "/zeta\n");
    REQUIRE(std::string(editor.Message()) == "Pattern not found: zeta");
    RequireCursor(editor, 0, 6);
}

TEST_CASE("vi draws the file, the status line and the cursor on a terminal")
{
    constexpr uint64_t width = 400;
    constexpr uint64_t height = 144;
    std::vector<uint8_t> pixels(width * height * 4);
    limine_framebuffer descriptor{};
    descriptor.address = pixels.data();
    descriptor.width = width;
    descriptor.height = height;
    descriptor.pitch = width * 4;
    descriptor.bpp = 32;
    descriptor.memory_model = LIMINE_FRAMEBUFFER_RGB;
    descriptor.red_mask_size = 8;
    descriptor.red_mask_shift = 16;
    descriptor.green_mask_size = 8;
    descriptor.green_mask_shift = 8;
    descriptor.blue_mask_size = 8;
    Framebuffer framebuffer;
    REQUIRE(framebuffer.init(&descriptor));
    FeatherOS::Term terminal;
    terminal.Init(&framebuffer);
    const uint32_t columns = terminal.Columns();
    const uint32_t rows = terminal.ContentRows();
    REQUIRE(columns == 48);
    REQUIRE(rows == 6);
    const auto row_text = [&](uint32_t row)
    {
        std::string text;
        for (uint32_t column = 0; column < columns; ++column)
        {
            text += static_cast<char>(terminal.CharacterAt(row + 1, column));
        }
        const size_t end = text.find_last_not_of(' ');
        return end == std::string::npos ? std::string() : text.substr(0, end + 1);
    };

    SavedFile saved;
    ViEditor editor(Save, &saved);
    const std::string wide(48, 'w');
    Open(editor, "first\n\tindented\n" + wide + "\nlast\n");
    editor.Resize(rows, columns);
    const auto show = [&]()
    {
        editor.Render();
        terminal.Print(std::string(editor.Output(), editor.OutputLength()).c_str());
        editor.ClearOutput();
    };
    show();
    REQUIRE(row_text(0) == "first");
    REQUIRE(row_text(1) == "        indented");
    // A row as wide as the screen does not clear the row after it.
    REQUIRE(row_text(2) == wide);
    REQUIRE(row_text(3) == "last");
    REQUIRE(row_text(4) == "~");
    REQUIRE(row_text(5) == "\"notes.txt\" 4L, 70B" + std::string(10, ' ') + "1,1");
    REQUIRE(terminal.CursorRow() == 1);
    REQUIRE(terminal.CursorColumn() == 0);

    // The cursor on a tab's line sits past the tab's spaces.
    Type(editor, "jw");
    show();
    REQUIRE(terminal.CursorRow() == 2);
    REQUIRE(terminal.CursorColumn() == 8);
    REQUIRE(row_text(5).substr(29) == "2,2");

    Type(editor, "ox\x1b");
    show();
    REQUIRE(row_text(2) == "x");
    REQUIRE(row_text(3) == wide);
    REQUIRE(row_text(5).substr(0, 12) == "            ");
    Type(editor, "i");
    show();
    REQUIRE(row_text(5).substr(0, 12) == "-- INSERT --");
    Type(editor, "\x1b:wq");
    show();
    REQUIRE(row_text(5) == ":wq");
    REQUIRE(terminal.CursorRow() == 6);
    REQUIRE(terminal.CursorColumn() == 3);
    Type(editor, "\x1b");

    // Moving past the screen scrolls by lines, and a long line scrolls sideways.
    Type(editor, "G");
    show();
    Type(editor, "o" + std::string(60, 'v') + "\x1b");
    show();
    REQUIRE(row_text(4) == std::string(columns, 'v'));
    REQUIRE(terminal.CursorRow() == 5);
    REQUIRE(terminal.CursorColumn() == columns - 1);
    // Every row scrolls sideways together, as in vim without wrapping.
    REQUIRE(row_text(0) == "nted");
    Type(editor, "0");
    show();
    REQUIRE(row_text(0) == "        indented");
    REQUIRE(row_text(4) == std::string(columns, 'v'));
    REQUIRE(terminal.CursorColumn() == 0);
    Type(editor, "gg");
    show();
    REQUIRE(row_text(0) == "first");
    REQUIRE(terminal.CursorRow() == 1);
}

TEST_CASE("vi runs the shell test's editing session")
{
    SavedFile saved;
    ViEditor editor(Save, &saved);
    REQUIRE(editor.Open("/tmp/vi.txt", "", 0, true));
    REQUIRE(Type(editor, "ihello world\x1bothird line\x1b"
                         "0Osecond line\x1bgg$xuGyypddggJu/third\nrT:wq\n") ==
            ViEditor::Status::Quit);
    REQUIRE(saved.text == "hello world\nsecond line\nThird line\n");
    REQUIRE(std::string(editor.Message()) == "\"/tmp/vi.txt\" 3L, 35B written");
}
