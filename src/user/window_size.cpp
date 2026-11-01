#include "window_size.h"

#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace FeatherOS::User
{
namespace
{

volatile sig_atomic_t window_size_changed = 0;

void NoteWindowSizeChange(int)
{
    window_size_changed = 1;
}

} // namespace

void WatchWindowSize()
{
    struct sigaction action = {};
    action.sa_handler = NoteWindowSizeChange;
    sigaction(SIGWINCH, &action, nullptr);
}

bool WindowSizeChanged()
{
    if (window_size_changed == 0)
    {
        return false;
    }
    window_size_changed = 0;
    return true;
}

WindowSize ReadWindowSize()
{
    winsize window = {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) != 0 || window.ws_row == 0 || window.ws_col == 0)
    {
        return {24, 80};
    }
    return {window.ws_row, window.ws_col};
}

} // namespace FeatherOS::User
