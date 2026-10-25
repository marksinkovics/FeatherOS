#include "command_line.h"
#include "key_decoder.h"

#include <stdio.h>
#include <termios.h>
#include <unistd.h>

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
    termios original;
    if (tcgetattr(STDIN_FILENO, &original) != 0)
    {
        perror("keys");
        return 1;
    }
    termios raw = original;
    cfmakeraw(&raw);
    // A tenth of a second tells a lone Escape key from the start of a sequence.
    termios waiting = raw;
    waiting.c_cc[VMIN] = 0;
    waiting.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
    {
        perror("keys");
        return 1;
    }
    puts("keys: press keys; q quits");
    fflush(stdout);
    FeatherOS::User::KeyDecoder decoder;
    bool running = true;
    while (running)
    {
        char bytes[16];
        const ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count < 0)
        {
            perror("keys");
            break;
        }
        KeyPress presses[FeatherOS::User::KeyDecoder::MaximumPresses];
        if (count == 0)
        {
            const size_t found = decoder.Flush(presses);
            for (size_t index = 0; index < found && running; ++index)
            {
                running = Show(presses[index]);
            }
        }
        for (ssize_t byte = 0; byte < count && running; ++byte)
        {
            const size_t found = decoder.Feed(bytes[byte], presses);
            for (size_t index = 0; index < found && running; ++index)
            {
                running = Show(presses[index]);
            }
        }
        fflush(stdout);
        tcsetattr(STDIN_FILENO, TCSANOW, decoder.Pending() ? &waiting : &raw);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    puts("keys: done");
    return 0;
}
