#ifndef FEATHEROS_FUTEX_H
#define FEATHEROS_FUTEX_H

#include <stdint.h>

// Waiting on a word of user memory, which locks and condition variables in the C library are
// built on. A waiter is keyed by its process (owner) and the word's address, so only threads of
// the same process meet. Checking the word and starting to wait happen under one lock that every
// wake also takes, so a wake that follows a change of the word is never missed.
namespace FeatherOS::Futex
{

using ReadWord = bool (*)(const void* owner, uint64_t address, uint32_t* value);
// Like CallerInterrupted: no locks, safe with the scheduler's lock held.
using Interrupted = bool (*)();

// Returns 0 once woken. SyscallErrorWouldBlock when the word no longer holds expected,
// SyscallErrorTimedOut at deadline_tick (0 waits forever), SyscallErrorInterrupted for a
// signal, and SyscallErrorBadAddress when the word cannot be read.
uint64_t Wait(const void* owner, uint64_t address, uint32_t expected, uint64_t deadline_tick,
              ReadWord read, Interrupted interrupted);
// Wakes up to count of the word's waiters, oldest first, and returns how many it woke.
uint64_t Wake(const void* owner, uint64_t address, uint64_t count);

} // namespace FeatherOS::Futex

#endif
