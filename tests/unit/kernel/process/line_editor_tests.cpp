#include <catch2/catch_test_macros.hpp>

#include <limine.h>

#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"
#include "user/line_editor.h"
#include "user/text/utf8.h"

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

void TypeBytes(LineEditor& editor, const std::string& bytes)
{
    for (const char byte : bytes)
    {
        REQUIRE(editor.Press(Character(byte)) == LineEditor::Status::Editing);
    }
}

const std::string Acute = "\xC3\xA9";
const std::string Euro = "\xE2\x82\xAC";
const std::string Grin = "\xF0\x9F\x98\x80";

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
    editor.Press(Control('q'));
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

TEST_CASE("Ctrl+Z finishes the line on screen and keeps it for after the stop")
{
    LineEditor editor;
    editor.Begin("> ", 80, nullptr);
    Type(editor, "ab");
    editor.Press(Special(Key::Left));
    TakeOutput(editor);
    REQUIRE(editor.Press(Control('z')) == LineEditor::Status::Suspended);
    const std::string output = TakeOutput(editor);
    REQUIRE(output.substr(output.size() - 3) == "^Z\n");
    editor.Redraw();
    REQUIRE(TakeOutput(editor).find("> ab") != std::string::npos);
    REQUIRE(editor.Cursor() == 2);
    REQUIRE(editor.Press(Special(Key::Enter)) == LineEditor::Status::Accepted);
    REQUIRE(std::string(editor.Line()) == "ab");
}

TEST_CASE("a redraw starts from the cursor's row, even when the line wraps")
{
    LineEditor editor;
    editor.Begin("> ", 80, nullptr);
    Type(editor, "ls");
    editor.Press(Special(Key::Left));
    TakeOutput(editor);
    editor.Redraw();
    REQUIRE(TakeOutput(editor) == "\r> ls\x1b[J\x1b[4G");

    editor.Begin("> ", 10, nullptr);
    Type(editor, "abcdefghijkl");
    TakeOutput(editor);
    editor.Redraw();
    REQUIRE(TakeOutput(editor) == "\r> abcdefghijkl\x1b[J\x1b[5G");
}

TEST_CASE("a resize redraws the line from the prompt's row for the new width")
{
    LineEditor editor;
    editor.Begin("> ", 10, nullptr);
    Type(editor, "abcdefghijkl");
    TakeOutput(editor);
    editor.Resize(20);
    REQUIRE(TakeOutput(editor) == "\x1b[1A\r> abcdefghijkl\x1b[J\x1b[15G");

    editor.Resize(5);
    REQUIRE(TakeOutput(editor) == "\r> abcdefghijkl\x1b[J\x1b[5G");
    Type(editor, "m");
    REQUIRE(TakeOutput(editor) == "m \b");
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

TEST_CASE("the cursor moves and erases by character over two to four byte characters")
{
    LineEditor editor;
    editor.Begin("> ", 80, nullptr);
    const std::string line = "a" + Acute + "b" + Euro + "c" + Grin;
    TypeBytes(editor, line);
    REQUIRE(std::string(editor.Line()) == line);
    REQUIRE(editor.Cursor() == 12);

    // a 0, e-acute 1, b 3, euro 4, c 7, grin 8.
    const size_t lefts[] = {8, 7, 4, 3, 1, 0, 0};
    for (const size_t expected : lefts)
    {
        editor.Press(Special(Key::Left));
        REQUIRE(editor.Cursor() == expected);
    }
    const size_t rights[] = {1, 3, 4, 7, 8, 12, 12};
    for (const size_t expected : rights)
    {
        editor.Press(Special(Key::Right));
        REQUIRE(editor.Cursor() == expected);
    }
    editor.Press(Control('b'));
    editor.Press(Control('b'));
    REQUIRE(editor.Cursor() == 7);
    editor.Press(Control('f'));
    REQUIRE(editor.Cursor() == 8);

    TakeOutput(editor);
    editor.Press(Special(Key::Left));
    // Four cells and the prompt come before the cursor, so it sits in column 7.
    REQUIRE(TakeOutput(editor) == "\r> " + line + "\x1b[J\x1b[7G");

    editor.Press(Special(Key::End));
    TakeOutput(editor);
    editor.Press(Special(Key::Backspace));
    REQUIRE(TakeOutput(editor) == "\b \b");
    REQUIRE(std::string(editor.Line()) == "a" + Acute + "b" + Euro + "c");
    editor.Press(Special(Key::Backspace));
    editor.Press(Special(Key::Backspace));
    REQUIRE(std::string(editor.Line()) == "a" + Acute + "b");
    TakeOutput(editor);

    editor.Press(Special(Key::Home));
    editor.Press(Special(Key::Right));
    editor.Press(Special(Key::Delete));
    REQUIRE(std::string(editor.Line()) == "ab");
    REQUIRE(editor.Cursor() == 1);
    editor.Press(Special(Key::End));
    TypeBytes(editor, Acute);
    editor.Press(Special(Key::Left));
    editor.Press(Special(Key::Backspace));
    REQUIRE(std::string(editor.Line()) == "a" + Acute);
    REQUIRE(editor.Cursor() == 1);
    editor.Press(Special(Key::Delete));
    REQUIRE(std::string(editor.Line()) == "a");
    editor.Press(Special(Key::Delete));
    REQUIRE(std::string(editor.Line()) == "a");
    REQUIRE(editor.Cursor() == 1);
}

TEST_CASE("typing, Ctrl+U, Ctrl+K and Ctrl+W work on whole characters")
{
    LineEditor editor;
    editor.Begin("> ", 80, nullptr);
    TypeBytes(editor, "ab");
    editor.Press(Special(Key::Left));
    TakeOutput(editor);
    TypeBytes(editor, Euro);
    REQUIRE(std::string(editor.Line()) == "a" + Euro + "b");
    REQUIRE(editor.Cursor() == 4);
    REQUIRE(TakeOutput(editor) == "\r> a" + Euro + "b\x1b[J\x1b[5G");

    editor.Press(Control('k'));
    REQUIRE(std::string(editor.Line()) == "a" + Euro);
    editor.Press(Control('u'));
    REQUIRE(std::string(editor.Line()).empty());

    TypeBytes(editor, "go " + Acute + Acute + " " + Grin + "x");
    editor.Press(Control('w'));
    REQUIRE(std::string(editor.Line()) == "go " + Acute + Acute + " ");
    editor.Press(Control('w'));
    REQUIRE(std::string(editor.Line()) == "go ");
    editor.Press(Special(Key::Left));
    editor.Press(Control('d'));
    REQUIRE(std::string(editor.Line()) == "go");
}

TEST_CASE("a character split across reads is neither shown nor edited in pieces")
{
    LineEditor editor;
    editor.Begin("> ", 80, nullptr);
    TakeOutput(editor);
    editor.Press(Character('\xE2'));
    editor.Press(Character('\x82'));
    REQUIRE(TakeOutput(editor).empty());
    REQUIRE(std::string(editor.Line()).empty());
    REQUIRE(editor.Cursor() == 0);
    editor.Press(Character('\xAC'));
    REQUIRE(TakeOutput(editor) == Euro);
    REQUIRE(std::string(editor.Line()) == Euro);
    REQUIRE(editor.Cursor() == 3);

    // Another key ends an incomplete sequence as one invalid cell, which is drawn as the line.
    editor.Press(Character('\xF0'));
    editor.Press(Character('\x9F'));
    TakeOutput(editor);
    editor.Press(Special(Key::Left));
    REQUIRE(std::string(editor.Line()) == Euro + "\xF0\x9F");
    REQUIRE(editor.Cursor() == 3);
    editor.Press(Special(Key::Right));
    REQUIRE(editor.Cursor() == 5);

    LineHistory history;
    editor.Begin("> ", 80, &history);
    editor.Press(Character('\xC3'));
    REQUIRE(editor.Press(Special(Key::Enter)) == LineEditor::Status::Accepted);
    REQUIRE(std::string(editor.Line()) == "\xC3");
    REQUIRE(std::string(history.Entry(0)) == "\xC3");

    editor.Begin("> ", 80, nullptr);
    editor.Press(Character('\xC3'));
    editor.Begin("> ", 80, nullptr);
    editor.Press(Character('\xA9'));
    REQUIRE(std::string(editor.Line()) == "\xA9");
}

TEST_CASE("invalid bytes are one cell each, and bytes that would join are redrawn")
{
    LineEditor editor;
    editor.Begin("> ", 80, nullptr);
    TypeBytes(editor, "\x80\x80");
    REQUIRE(editor.Cursor() == 2);
    TakeOutput(editor);
    editor.Press(Special(Key::Left));
    REQUIRE(editor.Cursor() == 1);
    editor.Press(Special(Key::Left));
    REQUIRE(editor.Cursor() == 0);
    const std::string output = TakeOutput(editor);
    REQUIRE(output.find("\x1b[4G") != std::string::npos);
    REQUIRE(output.substr(output.size() - 4) == "\x1b[3G");

    editor.Begin("> ", 80, nullptr);
    TypeBytes(editor, "\xE2\x82x");
    editor.Press(Special(Key::Backspace));
    TakeOutput(editor);
    TypeBytes(editor, "\xAC");
    REQUIRE(std::string(editor.Line()) == Euro);
    REQUIRE(editor.Cursor() == 3);
    REQUIRE(TakeOutput(editor) == "\r> " + Euro + "\x1b[J\x1b[4G");
    editor.Press(Special(Key::Left));
    REQUIRE(editor.Cursor() == 0);

    // Deleting the byte between a truncated sequence and a continuation byte joins them, so a
    // cursor that was between them now sits at the start of the character.
    editor.Begin("> ", 80, nullptr);
    TypeBytes(editor, "\xE2\x82x\xAC");
    REQUIRE(editor.Cursor() == 4);
    editor.Press(Special(Key::Left));
    editor.Press(Special(Key::Left));
    REQUIRE(editor.Cursor() == 2);
    editor.Press(Special(Key::Delete));
    REQUIRE(std::string(editor.Line()) == Euro);
    REQUIRE(editor.Cursor() == 0);
}

TEST_CASE("history recalls lines with multi-byte characters and puts the cursor after them")
{
    LineHistory history;
    LineEditor editor;
    editor.Begin("> ", 80, &history);
    TypeBytes(editor, "echo " + Acute + Grin);
    editor.Press(Special(Key::Enter));
    editor.Begin("> ", 80, &history);
    TypeBytes(editor, Euro);
    TakeOutput(editor);
    editor.Press(Special(Key::Up));
    REQUIRE(std::string(editor.Line()) == "echo " + Acute + Grin);
    REQUIRE(editor.Cursor() == 11);
    REQUIRE(TakeOutput(editor) == "\r> echo " + Acute + Grin + "\x1b[J\x1b[10G");
    editor.Press(Special(Key::Down));
    REQUIRE(std::string(editor.Line()) == Euro);

    const std::string prompt = Acute + "> ";
    editor.Begin(prompt.c_str(), 80, &history);
    TakeOutput(editor);
    TypeBytes(editor, "ab");
    TakeOutput(editor);
    editor.Press(Special(Key::Home));
    REQUIRE(TakeOutput(editor) == "\r" + prompt + "ab\x1b[J\x1b[4G");
}

TEST_CASE("line edits with multi-byte characters redraw correctly on a terminal where the line wraps")
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

    const auto row_text = [&](uint32_t row)
    {
        std::u32string text;
        for (uint32_t column = 0; column < columns; ++column)
        {
            text += terminal.CharacterAt(row, column);
        }
        return text;
    };
    LineEditor editor;
    const auto show = [&]()
    {
        terminal.Print(std::string(editor.Output(), editor.OutputLength()).c_str());
        editor.ClearOutput();
    };
    const std::string prompt = "/home/work> ";
    const auto expect_cursor = [&](uint32_t first_row)
    {
        const size_t position = prompt.size() + FeatherOS::Text::CellCount(editor.Line(), editor.Cursor());
        REQUIRE(terminal.CursorRow() == first_row + position / columns);
        REQUIRE(terminal.CursorColumn() == position % columns);
    };

    editor.Begin(prompt.c_str(), columns, nullptr);
    show();
    std::string line;
    for (int index = 0; index < 26; ++index)
    {
        line += Acute;
    }
    TypeBytes(editor, line);
    show();
    expect_cursor(1);
    REQUIRE(row_text(1) == U"/home/work> " + std::u32string(26, U'\u00E9'));
    TypeBytes(editor, "b" + Euro + Grin);
    show();
    expect_cursor(1);
    REQUIRE(row_text(2).substr(0, 4) == std::u32string{U'b', U'\u20AC', U'\U0001F600', U' '});

    editor.Press(Special(Key::Home));
    editor.Press(Special(Key::Right));
    editor.Press(Special(Key::Right));
    show();
    expect_cursor(1);
    TypeBytes(editor, Euro);
    show();
    expect_cursor(1);
    REQUIRE(row_text(1).substr(12, 4) == std::u32string{U'\u00E9', U'\u00E9', U'\u20AC', U'\u00E9'});

    editor.Press(Special(Key::End));
    for (int index = 0; index < 4; ++index)
    {
        editor.Press(Special(Key::Backspace));
        show();
        expect_cursor(1);
    }
    REQUIRE(row_text(1).substr(12, 4) == std::u32string{U'\u00E9', U'\u00E9', U'\u20AC', U'\u00E9'});
    REQUIRE(row_text(2) == std::u32string(columns, U' '));
    editor.Press(Control('u'));
    show();
    expect_cursor(1);
    REQUIRE(row_text(1) == U"/home/work> " + std::u32string(columns - 12, U' '));
}
