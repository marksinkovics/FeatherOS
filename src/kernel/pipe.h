#ifndef FEATHEROS_PIPE_H
#define FEATHEROS_PIPE_H

#include <stdint.h>

// A one-way channel between descriptors: bytes written to the write end are read, in order, from
// the read end. Functions return the value a syscall hands back. Buffers are kernel memory.
namespace FeatherOS::Pipes
{

struct Pipe;

constexpr uint64_t Capacity = 4096;
// Writes up to this size never interleave with other writers' bytes, as POSIX's PIPE_BUF.
constexpr uint64_t AtomicSize = 512;

// True once the caller should stop waiting, such as after Ctrl+C.
using InterruptCheck = bool (*)();

// Starts with one reader and one writer; nullptr without memory.
Pipe* Create();
// The pipe is freed once both ends are closed.
void CloseEnd(Pipe* pipe, bool write_end);
// Waits until there is data, then returns what is there, up to length. Returns 0 once the
// buffer is empty and no writer is left, and SyscallErrorInterrupted when interrupted says so.
uint64_t Read(Pipe* pipe, void* buffer, uint64_t length, InterruptCheck interrupted);
// Waits until all of buffer fits, writing at most AtomicSize bytes in one piece. Fails with
// SyscallErrorBrokenPipe when no reader is left.
uint64_t Write(Pipe* pipe, const void* buffer, uint64_t length, InterruptCheck interrupted);
uint64_t Buffered(Pipe* pipe);

} // namespace FeatherOS::Pipes

#endif
