#include "exit_handlers.h"
#include "lock.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>

namespace
{

// A key is in use while its sequence is odd. Each thread's value remembers the sequence it was
// set under, so a value set before pthread_key_delete reads as null once the slot is reused.
struct KeySlot
{
    unsigned sequence;
    void (*destructor)(void*);
};

struct KeyValue
{
    void* value;
    unsigned sequence;
};

constinit FeatherOS::LibC::Lock key_lock;
KeySlot key_slots[PTHREAD_KEYS_MAX] GUARDED_BY(key_lock);
thread_local KeyValue key_values[PTHREAD_KEYS_MAX];

bool KeyInUse(unsigned sequence)
{
    return (sequence & 1) != 0;
}

void RunKeyDestructors()
{
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; ++round)
    {
        bool called = false;
        for (unsigned key = 0; key < PTHREAD_KEYS_MAX; ++key)
        {
            KeyValue& entry = key_values[key];
            void (*destructor)(void*) = nullptr;
            {
                const FeatherOS::LibC::Guard<FeatherOS::LibC::Lock> guard(key_lock);
                if (entry.sequence == key_slots[key].sequence && KeyInUse(entry.sequence))
                {
                    destructor = key_slots[key].destructor;
                }
            }
            void* value = entry.value;
            if (destructor == nullptr || value == nullptr)
            {
                continue;
            }
            entry.value = nullptr;
            destructor(value);
            called = true;
        }
        if (!called)
        {
            return;
        }
    }
}

} // namespace

int pthread_key_create(pthread_key_t* key, void (*destructor)(void* value))
{
    const FeatherOS::LibC::Guard<FeatherOS::LibC::Lock> guard(key_lock);
    for (unsigned index = 0; index < PTHREAD_KEYS_MAX; ++index)
    {
        KeySlot& slot = key_slots[index];
        if (!KeyInUse(slot.sequence))
        {
            slot.destructor = destructor;
            __atomic_store_n(&slot.sequence, slot.sequence + 1, __ATOMIC_RELEASE);
            FeatherOS::LibC::SetKeyDestructors(RunKeyDestructors);
            *key = index;
            return 0;
        }
    }
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    const FeatherOS::LibC::Guard<FeatherOS::LibC::Lock> guard(key_lock);
    if (key >= PTHREAD_KEYS_MAX || !KeyInUse(key_slots[key].sequence))
    {
        return EINVAL;
    }
    __atomic_store_n(&key_slots[key].sequence, key_slots[key].sequence + 1, __ATOMIC_RELEASE);
    key_slots[key].destructor = nullptr;
    return 0;
}

void* pthread_getspecific(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX)
    {
        return nullptr;
    }
    const KeyValue& entry = key_values[key];
    const unsigned sequence = __atomic_load_n(&key_slots[key].sequence, __ATOMIC_ACQUIRE);
    return entry.sequence == sequence && KeyInUse(sequence) ? entry.value : nullptr;
}

int pthread_setspecific(pthread_key_t key, const void* value)
{
    if (key >= PTHREAD_KEYS_MAX)
    {
        return EINVAL;
    }
    const unsigned sequence = __atomic_load_n(&key_slots[key].sequence, __ATOMIC_ACQUIRE);
    if (!KeyInUse(sequence))
    {
        return EINVAL;
    }
    key_values[key] = {const_cast<void*>(value), sequence};
    return 0;
}
