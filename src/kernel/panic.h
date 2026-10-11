#ifndef FEATHEROS_PANIC_H
#define FEATHEROS_PANIC_H

namespace FeatherOS
{

__attribute__((noreturn)) void Panic(const char* reason);

}

#define FEATHEROS_ASSERT(condition)                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            FeatherOS::Panic("Assertion failed: " #condition);                                     \
        }                                                                                          \
    } while (0)

#endif
