#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

constexpr char Help[] =
    "usage: stty [-F device] [size] [rows n] [cols n]\n"
    "Shows the terminal's size and modes, or with size only its rows and columns.\n"
    "rows and cols set the size, which sends SIGWINCH to the terminal's foreground\n"
    "group when it changes. -F acts on another terminal, such as /dev/pts/0.";

namespace
{

void ShowSettings(int descriptor, const winsize& size)
{
    termios settings{};
    if (tcgetattr(descriptor, &settings) != 0)
    {
        printf("rows %u; columns %u;\n", size.ws_row, size.ws_col);
        return;
    }
    printf("rows %u; columns %u;\n%sicanon %secho %sisig %sicrnl\n", size.ws_row, size.ws_col,
           (settings.c_lflag & ICANON) != 0 ? "" : "-", (settings.c_lflag & ECHO) != 0 ? "" : "-",
           (settings.c_lflag & ISIG) != 0 ? "" : "-", (settings.c_iflag & ICRNL) != 0 ? "" : "-");
}

bool ParseDimension(const char* text, unsigned short* value)
{
    unsigned long long parsed = 0;
    if (text == nullptr || !FeatherOS::User::ParseUnsigned(text, &parsed) || parsed == 0 ||
        parsed > 0xffff)
    {
        return false;
    }
    *value = static_cast<unsigned short>(parsed);
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
    int first = 1;
    const char* device = nullptr;
    if (argc > 2 && strcmp(argv[1], "-F") == 0)
    {
        device = argv[2];
        first = 3;
    }
    const int descriptor = device == nullptr ? STDIN_FILENO : open(device, O_RDWR | O_NOCTTY);
    if (descriptor < 0)
    {
        printf("stty: %s: %s\n", device, strerror(errno));
        return 1;
    }
    winsize size{};
    if (ioctl(descriptor, TIOCGWINSZ, &size) != 0)
    {
        printf("stty: %s: %s\n", device == nullptr ? "standard input" : device, strerror(errno));
        return 1;
    }
    bool show_size = false;
    bool resize = false;
    for (int index = first; index < argc; ++index)
    {
        if (strcmp(argv[index], "size") == 0)
        {
            show_size = true;
        }
        else if (strcmp(argv[index], "rows") == 0 && ParseDimension(argv[index + 1], &size.ws_row))
        {
            resize = true;
            ++index;
        }
        else if (strcmp(argv[index], "cols") == 0 && ParseDimension(argv[index + 1], &size.ws_col))
        {
            resize = true;
            ++index;
        }
        else
        {
            puts("usage: stty [-F device] [size] [rows n] [cols n]");
            return 1;
        }
    }
    if (resize && ioctl(descriptor, TIOCSWINSZ, &size) != 0)
    {
        printf("stty: %s\n", strerror(errno));
        return 1;
    }
    if (show_size)
    {
        printf("%u %u\n", size.ws_row, size.ws_col);
    }
    else if (!resize)
    {
        ShowSettings(descriptor, size);
    }
    return 0;
}
