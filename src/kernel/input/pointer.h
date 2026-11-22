#ifndef FEATHEROS_INPUT_POINTER_H
#define FEATHEROS_INPUT_POINTER_H

#include "kernel/pipe.h"
#include "user/syscall_abi.h"

#include <stddef.h>
#include <stdint.h>

// The events of every pointer device, merged, for /dev/mouse. A ring of RingSize records with a
// global sequence number; each Reader keeps its own cursor into it. Producers submit one whole
// report at a time, so a reader sees reports in order. Functions that return a syscall value use
// User::SyscallError* codes.
namespace FeatherOS::Input::Pointer
{

constexpr uint32_t RingSize = 256;

// A Reader is only used through the functions below, which serialize on the queue's lock.
struct Reader
{
    uint64_t next = 0;
};

// events ends in a User::InputSyncReport and has at most RingSize records; anything else is
// ignored. The time of every record is set here, from the monotonic uptime. Wakes blocked readers
// and Poll callers, so it may sleep-wake but never blocks.
void Submit(const User::InputEvent* events, size_t count);

// A reader at the end of the queue, which sees only what is submitted from now on.
Reader Open();

// Copies whole records, as many as fit in bytes and are queued, and returns their size.
// A reader that fell more than RingSize records behind first gets one User::InputSyncDropped, then
// continues at the oldest report still queued. SyscallErrorInvalidArgument if bytes is below one
// record. With nothing queued, SyscallErrorWouldBlock if nonblocking, otherwise it waits, until
// interrupted says to stop (SyscallErrorInterrupted).
uint64_t Read(Reader& reader, void* buffer, uint64_t bytes, bool nonblocking,
              Pipes::InterruptCheck interrupted = nullptr);

// User::PollReadable while the reader is behind the end of the queue, else 0.
int16_t PollEvents(const Reader& reader);

} // namespace FeatherOS::Input::Pointer

#endif
