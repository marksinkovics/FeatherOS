#include "probe_wait.h"

#include "kernel/klogger.h"
#include "kernel/sync/spinlock.h"
#include "user/thread_safety.h"

namespace FeatherOS::Storage::ProbeWait
{
namespace
{

struct Entry
{
    const void* key;
    char name[NameCapacity];
};

constinit KLogger logger;
constinit Sync::SpinLock lock;
Entry entries[MaxPending] GUARDED_BY(lock) = {};
uint32_t pending GUARDED_BY(lock) = 0;
// Only its address is used, as the wait channel.
constinit char channel = 0;

bool NonePending(void*)
{
    Sync::SpinLockGuard guard(lock);
    return pending == 0;
}

} // namespace

bool Begin(const void* key, const char* name)
{
    Sync::SpinLockGuard guard(lock);
    for (Entry& entry : entries)
    {
        if (entry.key != nullptr)
        {
            continue;
        }
        entry.key = key;
        size_t length = 0;
        for (; name[length] != '\0' && length + 1 < NameCapacity; ++length)
        {
            entry.name[length] = name[length];
        }
        entry.name[length] = '\0';
        ++pending;
        return true;
    }
    return false;
}

void End(Environment& environment, const void* key)
{
    {
        Sync::SpinLockGuard guard(lock);
        bool found = false;
        for (Entry& entry : entries)
        {
            if (entry.key == key && key != nullptr)
            {
                entry.key = nullptr;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return;
        }
        --pending;
    }
    environment.Wake(&channel);
}

uint32_t Pending()
{
    Sync::SpinLockGuard guard(lock);
    return pending;
}

bool Wait(Environment& environment, uint64_t deadline_ms, char* late, size_t capacity)
{
    if (environment.WaitUntil(&channel, deadline_ms, NonePending, nullptr))
    {
        return true;
    }
    size_t length = 0;
    Sync::SpinLockGuard guard(lock);
    for (const Entry& entry : entries)
    {
        if (entry.key == nullptr)
        {
            continue;
        }
        if (length != 0 && length + 2 < capacity)
        {
            late[length++] = ',';
            late[length++] = ' ';
        }
        for (const char* text = entry.name; *text != '\0' && length + 1 < capacity; ++text)
        {
            late[length++] = *text;
        }
    }
    if (capacity != 0)
    {
        late[length] = '\0';
    }
    return false;
}

bool MayWait(bool timer_running)
{
    if (!timer_running)
    {
        logger.Error("Storage: the timer is not running, so no controller is probed");
    }
    return timer_running;
}

void Reset()
{
    Sync::SpinLockGuard guard(lock);
    for (Entry& entry : entries)
    {
        entry.key = nullptr;
    }
    pending = 0;
}

} // namespace FeatherOS::Storage::ProbeWait
