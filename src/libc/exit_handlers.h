#ifndef FEATHEROS_LIBC_EXIT_HANDLERS_H
#define FEATHEROS_LIBC_EXIT_HANDLERS_H

namespace FeatherOS::LibC
{

// Shared by atexit and the C++ runtime's __cxa_atexit, which registers static destructors.
bool AddExitHandler(void (*function)(void*), void* argument);

} // namespace FeatherOS::LibC

#endif
