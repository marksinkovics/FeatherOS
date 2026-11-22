#ifndef FEATHEROS_LOG_RING_H
#define FEATHEROS_LOG_RING_H

#include "kernel/sync/spinlock.h"
#include "user/thread_safety.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS
{

// Whole lines in a circular buffer; a full ring drops its oldest lines to take a new one.
class LogRing
{
public:
    constexpr LogRing(char* storage, size_t capacity) : storage(storage), capacity(capacity) {}

    void Append(const char* text, size_t length);
    // Copies the newest whole lines that fit with a terminating NUL; returns the text's length.
    size_t Snapshot(char* buffer, size_t buffer_capacity);

private:
    char* const storage;
    const size_t capacity;
    Sync::SpinLock lock;
    uint64_t first GUARDED_BY(lock) = 0;
    uint64_t next GUARDED_BY(lock) = 0;

    void DropOldestLineLocked() REQUIRES(lock);
    char At(uint64_t position) const REQUIRES(lock);
};

namespace KernelLog
{

constexpr size_t Capacity = 64 * 1024;

void Append(const char* text);
size_t Snapshot(char* buffer, size_t buffer_capacity);

} // namespace KernelLog

} // namespace FeatherOS

#endif
