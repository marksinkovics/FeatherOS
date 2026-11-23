#ifndef FEATHEROS_STORAGE_ENVIRONMENT_H
#define FEATHEROS_STORAGE_ENVIRONMENT_H

#include <stdint.h>

namespace FeatherOS::Storage
{

class Environment
{
public:
    virtual uint64_t NowMilliseconds() = 0;
    // Blocks the calling task until ready(context) holds, Wake(channel) is called or the
    // deadline passes; true when ready(context) held at return.
    virtual bool WaitUntil(const void* channel, uint64_t deadline_ms, bool (*ready)(void*),
                           void* context) = 0;
    virtual void Wake(const void* channel) = 0;
    virtual void Sleep(uint32_t milliseconds) = 0;

protected:
    Environment() = default;
    ~Environment() = default;
    Environment(const Environment&) = default;
    Environment& operator=(const Environment&) = default;
};

} // namespace FeatherOS::Storage

#endif
