#include "command_line.h"
#include "key_decoder.h"
#include "raw_terminal.h"

#include <stdio.h>

constexpr char Help[] = "usage: keys\n"
                        "Switches the console to raw input and prints the name of each key\n"
                        "pressed, including Ctrl+C, until q.";

namespace
{

using FeatherOS::User::Key;
using FeatherOS::User::KeyPress;

const char* KeyName(Key key)
{
    switch (key)
    {
    case Key::Enter:
        return "enter";
    case Key::Tab:
        return "tab";
    case Key::BackTab:
        return "shift-tab";
    case Key::Backspace:
        return "backspace";
    case Key::Escape:
        return "escape";
    case Key::Up:
        return "up";
    case Key::Down:
        return "down";
    case Key::Left:
        return "left";
    case Key::Right:
        return "right";
    case Key::Home:
        return "home";
    case Key::End:
        return "end";
    case Key::Insert:
        return "insert";
    case Key::Delete:
        return "delete";
    case Key::PageUp:
        return "page-up";
    case Key::PageDown:
        return "page-down";
    default:
        return "unknown";
    }
}

// Returns false once q is pressed.
bool Show(const KeyPress& press)
{
    if (press.key == Key::Character)
    {
        if (press.character == 'q')
        {
            return false;
        }
        if (press.character == ' ')
        {
            puts("keys: space");
        }
        else
        {
            printf("keys: %c\n", press.character);
        }
    }
    else if (press.key == Key::Control)
    {
        printf("keys: ctrl-%c\n", press.character);
    }
    else
    {
        printf("keys: %s\n", KeyName(press.key));
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    FeatherOS::User::RawTerminal terminal;
    if (!terminal.Enter())
    {
        perror("keys");
        return 1;
    }
    puts("keys: press keys; q quits");
    fflush(stdout);
    KeyPress press;
    bool running = true;
    while (running)
    {
        if (!terminal.ReadKey(&press))
        {
            perror("keys");
            break;
        }
        running = Show(press);
        fflush(stdout);
    }
    terminal.Leave();
    puts("keys: done");
    return 0;
}
