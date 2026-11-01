#ifndef FEATHEROS_USER_WINDOW_SIZE_H
#define FEATHEROS_USER_WINDOW_SIZE_H

#include <stddef.h>

namespace FeatherOS::User
{

struct WindowSize
{
    size_t rows;
    size_t columns;
};

// Catches SIGWINCH from now on, so a read that waits fails with EINTR when the size changes.
void WatchWindowSize();
// True once after one or more SIGWINCH.
bool WindowSizeChanged();
// Standard output's terminal, 24 by 80 when it has no size.
WindowSize ReadWindowSize();

} // namespace FeatherOS::User

#endif
