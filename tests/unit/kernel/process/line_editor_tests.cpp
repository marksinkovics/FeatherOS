#include <catch2/catch_test_macros.hpp>

#include <limine.h>

#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"
#include "user/line_editor.h"

#include <array>
#include <string>

namespace
{

using FeatherOS::User::Key;
using FeatherOS::User::KeyPress;
using FeatherOS::User::LineEditor;
using FeatherOS::User::LineHistory;

KeyPress Character(char character)
{
    return {Key::Character, character};
}

KeyPress Control(char letter)
{
    return {Key::Control, letter};
}

KeyPress Special(Key key)
{
    return {key, '\0'};
}

void Type(LineEditor& editor, const std::string& text)
{
    for (const char character : text)
    {
        REQUIRE(editor.Press(Character(character)) == LineEditor::Status::Editing);
    }
}

std::string TakeOutput(LineEditor& editor)
{
    std::string output(editor.Output(), editor.OutputLength());
    editor.ClearOutput();
    return output;
}

} // namespace

TEST_CASE("the line editor inserts, erases and moves within the line")
{
    LineEditor editor;
    editor.Begin("> ", 80, nullptr);
    Type(editor, "ls /dv");
    editor.Press(Special(Key::Left));
    Type(editor, "e");
    REQUIRE(std::string(editor.Line()) == "ls /dev");
    REQUIRE(editor.Cursor() == 6);
    editor.Press(Special(Key::Home));
    editor.Press(Special(Key::Delete));
    REQUIRE(std::string(editor.Line()) == "s /dev");
    REQUIRE(editor.Cursor() == 0);
    editor.Press(Control('f'));
    editor.Press(Control('d'));
    REQUIRE(std::string(editor.Line()) == "s/dev");
    editor.Press(Special(Key::End));
    editor.Press(Special(Key::Right));
    REQUIRE(editor.Cursor() == 5);
    editor.Press(Special(Key::Delete));
    REQUIRE(std::string(editor.Line()) == "s/dev");

    editor.Press(Control('a'));
    Type(editor, "cat ");
    editor.Press(Control('e'));
    Type(editor, " two  words");
    editor.Press(Control('w'));
    REQUIRE(std::string(editor.Line()) == "cat s/dev two  ");
    editor.Press(Control('w'));
    REQUIRE(std::string(editor.Line()) == "cat s/dev ");
    editor.Press(Control('b'));
    editor.Press(Control('b'));
    editor.Press(Control('k'));
    REQUIRE(std::string(editor.Line()) == "cat s/de");
    editor.Press(Control('b'));
    editor.Press(Control('u'));
    REQUIRE(std::string(editor.Line()) == "e");
    REQUIRE(editor.Cursor() == 0);
    editor.Press(Control('h'));
    editor.Press(Special(Key::Left));
    REQUIRE(std::string(editor.Line()) == "e");
    REQUIRE(editor.Press(Special(Key::Enter)) == LineEditor::Status::Accepted);
}

TEST_CASE("the line editor holds at most its maximum and ignores other keys")
{
    LineEditor editor;
    editor.Begin("> ", 80, nullptr);
    Type(editor, std::string(FeatherOS::User::MaximumEditedLineLength + 5, 'x'));
    REQUIRE(std::string(editor.Line()) ==
            std::string(FeatherOS::User::MaximumEditedLineLength, 'x'));
    editor.Press(Special(Key::Tab));
    editor.Press(Special(Key::Escape));
    editor.Press(Special(Key::PageUp));
    editor.Press(Special(Key::Unknown));
    editor.Press(Control('z'));
    REQUIRE(editor.Cursor() == FeatherOS::User::MaximumEditedLineLength);
    REQUIRE(editor.Press(Control('c')) == LineEditor::Status::Cancelled);
}

TEST_CASE("typing at the end of a line echoes only what changed")
{
    LineEditor editor;
    editor.Begin("/home> ", 80, nullptr);
    REQUIRE(TakeOutput(editor) == "/home> ");
    Type(editor, "abx");
    REQUIRE(TakeOutput(editor) == "abx");
    editor.Press(Special(Key::Backspace));
    REQUIRE(TakeOutput(editor) == "\b \b");
    REQUIRE(editor.Press(Special(Key::Enter)) == LineEditor::Status::Accepted);
    REQUIRE(TakeOutput(editor) == "\n");
    REQUIRE(std::string(editor.Line()) == "ab");

    editor.Begin("> ", 80, nullptr);
    TakeOutput(editor);
    Type(editor, "ab");
    editor.Press(Special(Key::Left));
    TakeOutput(editor);
    // Ctrl+C finishes the line on screen first, so ^C follows all of it.
    REQUIRE(editor.Press(Control('c')) == LineEditor::Status::Cancelled);
    const std::string output = TakeOutput(editor);
    REQUIRE(output.size() >= 4);
    REQUIRE(output.substr(output.size() - 3) == "^C\n");
    editor.Press(Control('c'));
    editor.Begin("> ", 80, nullptr);
    REQUIRE(TakeOutput(editor) == "> ");
}

TEST_CASE("history recalls earlier lines and keeps the line being typed")
{
    LineHistory history;
    history.Add("first");
    history.Add("   ");
    history.Add("second");
    history.Add("second");
    REQUIRE(history.Count() == 2);

    LineEditor editor;
    editor.Begin("> ", 80, &history);
    Type(editor, "draft");
    editor.Press(Special(Key::Up));
    REQUIRE(std::string(editor.Line()) == "second");
    REQUIRE(editor.Cursor() == 6);
    editor.Press(Control('p'));
    REQUIRE(std::string(editor.Line()) == "first");
    editor.Press(Special(Key::Up));
    REQUIRE(std::string(editor.Line()) == "first");
    editor.Press(Control('n'));
    REQUIRE(std::string(editor.Line()) == "second");
    editor.Press(Special(Key::Down));
    REQUIRE(std::string(editor.Line()) == "draft");
    editor.Press(Special(Key::Down));
    REQUIRE(std::string(editor.Line()) == "draft");

    // Accepting an edited recall adds it as a new entry; the old one stays as it was.
    editor.Press(Special(Key::Up));
    Type(editor, "!");
    REQUIRE(editor.Press(Special(Key::Enter)) == LineEditor::Status::Accepted);
    REQUIRE(history.Count() == 3);
    REQUIRE(std::string(history.Entry(0)) == "second!");
    REQUIRE(std::string(history.Entry(1)) == "second");

    LineHistory full;
    for (int index = 0; index < 40; ++index)
    {
        full.Add(std::to_string(index).c_str());
    }
    REQUIRE(full.Count() == FeatherOS::User::MaximumHistoryLines);
    REQUIRE(std::string(full.Entry(0)) == "39");
    REQUIRE(std::string(full.Entry(FeatherOS::User::MaximumHistoryLines - 1)) == "8");
}

TEST_CASE("line edits redraw correctly on a terminal where the line wraps")
{
    constexpr uint64_t width = 320;
    constexpr uint64_t height = 112;
    std::array<uint8_t, width * height * 4> pixels{};
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
    REQUIRE(columns == 38);
    REQUIRE(terminal.ContentRows() == 4);

    const auto row_text = [&](uint32_t row)
    {
        std::string text;
        for (uint32_t column = 0; column < columns; ++column)
        {
            text += static_cast<char>(terminal.CharacterAt(row, column));
        }
        return text;
    };
    const auto screen = [&](uint32_t first_row, size_t length)
    {
        std::string text;
        for (uint32_t row = first_row; row <= terminal.ContentRows(); ++row)
        {
            text += row_text(row);
        }
        return text.substr(0, length);
    };
    LineHistory history;
    LineEditor editor;
    const auto show = [&]()
    {
        terminal.Print(std::string(editor.Output(), editor.OutputLength()).c_str());
        editor.ClearOutput();
    };
    const std::string prompt = "/home/work> ";
    const auto expect_cursor = [&](uint32_t first_row)
    {
        const size_t position = prompt.size() + editor.Cursor();
        REQUIRE(terminal.CursorRow() == first_row + position / columns);
        REQUIRE(terminal.CursorColumn() == position % columns);
    };

    editor.Begin(prompt.c_str(), columns, &history);
    show();
    // 26 characters fill the first row exactly, so the cursor starts the second row.
    const std::string first = std::string(26, 'a');
    Type(editor, first);
    show();
    expect_cursor(1);
    REQUIRE(row_text(1) == prompt + first);
    Type(editor, "bcd");
    show();
    expect_cursor(1);

    editor.Press(Special(Key::Home));
    show();
    expect_cursor(1);
    Type(editor, "XY");
    show();
    expect_cursor(1);
    const std::string line = "XY" + first + "bcd";
    REQUIRE(std::string(editor.Line()) == line);
    REQUIRE(screen(1, prompt.size() + line.size() + 2) == prompt + line + "  ");

    // Erasing back across the wrap removes the second row's text.
    editor.Press(Special(Key::End));
    for (int index = 0; index < 7; ++index)
    {
        editor.Press(Special(Key::Backspace));
        show();
        expect_cursor(1);
    }
    REQUIRE(screen(1, columns + 4) == prompt + "XY" + std::string(22, 'a') + "      ");
    editor.Press(Control('u'));
    show();
    expect_cursor(1);
    REQUIRE(screen(1, columns * 2) == prompt + std::string(columns * 2 - prompt.size(), ' '));

    // A long recalled line that starts on the last row scrolls the screen up by one.
    history.Add(line.c_str());
    REQUIRE(editor.Press(Special(Key::Enter)) == LineEditor::Status::Accepted);
    show();
    terminal.Print("\n\n");
    editor.Begin(prompt.c_str(), columns, &history);
    show();
    expect_cursor(4);
    editor.Press(Special(Key::Up));
    show();
    expect_cursor(3);
    REQUIRE(screen(3, prompt.size() + line.size()) == prompt + line);
    editor.Press(Special(Key::Home));
    editor.Press(Special(Key::Right));
    show();
    expect_cursor(3);
    editor.Press(Control('l'));
    show();
    expect_cursor(1);
    REQUIRE(screen(1, prompt.size() + line.size()) == prompt + line);
}
