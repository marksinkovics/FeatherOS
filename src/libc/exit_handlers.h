#ifndef FEATHEROS_LIBC_EXIT_HANDLERS_H
#define FEATHEROS_LIBC_EXIT_HANDLERS_H

namespace FeatherOS::LibC
{

// Shared by atexit and the C++ runtime's __cxa_atexit, which registers static destructors.
bool AddExitHandler(void (*function)(void*), void* argument);
// stdio registers its flush here, so programs that never use stdio do not link it.
void SetExitFlush(void (*flush)());
// For __cxa_thread_atexit: the calling thread's own handlers, which its end (and, for the thread
// that calls exit, exit itself) runs newest first.
bool AddThreadExitHandler(void (*function)(void*), void* argument);
void RunThreadExitHandlers();

} // namespace FeatherOS::LibC

#endif
