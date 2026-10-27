#include "command_line.h"
#include "raw_terminal.h"
#include "vi_editor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

constexpr char Help[] =
    "usage: vi [file]\n"
    "A small vi. Normal mode: h j k l or the arrows, w b e, 0 ^ $, gg G, Ctrl+F and\n"
    "Ctrl+B to page, x X D dd J r, yy p P, u and Ctrl+R to undo and redo, n N to search\n"
    "again; a number first repeats, as in 3dd. i a I A o O start inserting, and\n"
    "Escape returns. :w [file], :q, :q!, :wq, :x and :N run commands; /text and ?text\n"
    "search forward and back.";

namespace
{

using FeatherOS::User::ViEditor;

bool SaveFile(void*, const char* name, const char* text, size_t length)
{
    FILE* file = fopen(name, "w");
    if (file == nullptr)
    {
        return false;
    }
    const bool written = fwrite(text, 1, length, file) == length;
    return fclose(file) == 0 && written;
}

// Reads the whole file into a heap buffer the caller frees; *missing is set when there is none.
char* LoadFile(const char* name, size_t* length, bool* missing)
{
    *length = 0;
    *missing = false;
    FILE* file = fopen(name, "r");
    if (file == nullptr)
    {
        *missing = errno == ENOENT;
        return *missing ? static_cast<char*>(malloc(1)) : nullptr;
    }
    size_t capacity = 4096;
    auto* text = static_cast<char*>(malloc(capacity));
    while (text != nullptr)
    {
        *length += fread(text + *length, 1, capacity - *length, file);
        if (*length < capacity)
        {
            break;
        }
        capacity *= 2;
        auto* larger = static_cast<char*>(realloc(text, capacity));
        if (larger == nullptr)
        {
            free(text);
        }
        text = larger;
    }
    const bool failed = ferror(file) != 0;
    fclose(file);
    if (failed)
    {
        free(text);
        return nullptr;
    }
    return text;
}

void Flush(ViEditor& editor)
{
    write(STDOUT_FILENO, editor.Output(), editor.OutputLength());
    editor.ClearOutput();
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv) || argc > 2)
    {
        puts(Help);
        return argc > 2 ? 1 : 0;
    }
    const char* name = argc == 2 ? argv[1] : nullptr;
    size_t length = 0;
    bool missing = true;
    char* text =
        name == nullptr ? static_cast<char*>(malloc(1)) : LoadFile(name, &length, &missing);
    if (text == nullptr)
    {
        printf("vi: %s: %s\n", name, strerror(errno));
        return 1;
    }
    auto* editor = new ViEditor(SaveFile, nullptr);
    const bool opened = editor->Open(name, text, length, missing);
    free(text);
    FeatherOS::User::RawTerminal terminal;
    if (!opened || !terminal.Enter())
    {
        puts(opened ? "vi: standard input is not the console" : "vi: out of memory");
        delete editor;
        return 1;
    }
    winsize window = {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) != 0)
    {
        window.ws_row = 24;
        window.ws_col = 80;
    }
    editor->Resize(window.ws_row, window.ws_col);
    // The alternate screen keeps the shell's screen to come back to, as vim does.
    fputs("\x1b[?1049h", stdout);
    fflush(stdout);
    int status = 0;
    for (;;)
    {
        editor->Render();
        Flush(*editor);
        FeatherOS::User::KeyPress press;
        if (!terminal.ReadKey(&press))
        {
            status = 1;
            break;
        }
        if (editor->Press(press) == ViEditor::Status::Quit)
        {
            break;
        }
    }
    terminal.Leave();
    fputs("\x1b[?1049l", stdout);
    delete editor;
    return status;
}
