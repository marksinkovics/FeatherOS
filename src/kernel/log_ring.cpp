#include "log_ring.h"

namespace FeatherOS
{

char LogRing::At(uint64_t position) const
{
    return storage[position % capacity];
}

void LogRing::DropOldestLineLocked()
{
    while (first < next && At(first) != '\n')
    {
        ++first;
    }
    if (first < next)
    {
        ++first;
    }
}

void LogRing::Append(const char* text, size_t length)
{
    while (length > 0 && text[length - 1] == '\n')
    {
        --length;
    }
    if (capacity < 2)
    {
        return;
    }
    if (length > capacity - 1)
    {
        length = capacity - 1;
    }
    Sync::SpinLockGuard guard(lock);
    while (next - first + length + 1 > capacity)
    {
        DropOldestLineLocked();
    }
    for (size_t index = 0; index < length; ++index)
    {
        storage[(next + index) % capacity] = text[index];
    }
    storage[(next + length) % capacity] = '\n';
    next += length + 1;
}

size_t LogRing::Snapshot(char* buffer, size_t buffer_capacity)
{
    if (buffer_capacity == 0)
    {
        return 0;
    }
    Sync::SpinLockGuard guard(lock);
    uint64_t start = first;
    while (next - start > buffer_capacity - 1)
    {
        while (start < next && At(start) != '\n')
        {
            ++start;
        }
        if (start < next)
        {
            ++start;
        }
    }
    size_t length = 0;
    for (; start < next; ++start)
    {
        buffer[length++] = At(start);
    }
    buffer[length] = '\0';
    return length;
}

namespace KernelLog
{

namespace
{

char storage[Capacity];
constinit LogRing ring(storage, Capacity);

size_t Length(const char* text)
{
    size_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

} // namespace

void Append(const char* text)
{
    if (text != nullptr)
    {
        ring.Append(text, Length(text));
    }
}

size_t Snapshot(char* buffer, size_t buffer_capacity)
{
    return ring.Snapshot(buffer, buffer_capacity);
}

} // namespace KernelLog

} // namespace FeatherOS
