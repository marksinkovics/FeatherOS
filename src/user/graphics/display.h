#ifndef FEATHEROS_USER_GRAPHICS_DISPLAY_H
#define FEATHEROS_USER_GRAPHICS_DISPLAY_H

#include "dirty_region.h"
#include "surface.h"

#include <stddef.h>
#include <stdint.h>

// The screen surface lives as long as the Display; Swift needs to be told a pointer into it is
// safe.
#ifdef __clang__
#define FEATHEROS_SWIFT_INTERIOR_POINTER __attribute__((swift_attr("import_unsafe")))
#else
#define FEATHEROS_SWIFT_INTERIOR_POINTER
#endif

namespace FeatherOS::Graphics
{

enum class Status : int32_t
{
    Ok = 0,
    Timeout,
    Interrupted,
    NotOpen,
    OpenFailed,
    InfoFailed,
    AcquireFailed,
    PresentFailed,
    ReadFailed,
    OutOfMemory,
};

enum class EventKind : uint8_t
{
    Release = 0,
    Press = 1,
    Repeat = 2,
    Shown = 3,
    Hidden = 4,
    Unknown = 255,
};

struct Event
{
    EventKind kind;
    uint8_t modifiers;
    uint16_t code;
    uint32_t time_ms;
};

// The calls Display makes to the kernel, so that a test can replace them. Poll returns a positive
// number when the descriptor has something to read, 0 at the timeout and -1 with errno set.
struct System
{
    int (*Open)(const char* path);
    int (*Control)(int descriptor, unsigned long request, void* argument);
    long (*Read)(int descriptor, void* buffer, size_t length);
    int (*Poll)(int descriptor, int timeout_ms);
    int (*Close)(int descriptor);
};

const System* DefaultSystem();

class Display
{
public:
    Display();
    explicit Display(const System* system);
    Display(const Display&) = delete;
    Display(Display&& other) noexcept;
    Display& operator=(const Display&) = delete;
    ~Display();

    Status Open();
    void Close();
    bool IsOpen() const;

    int32_t Width() const;
    int32_t Height() const;
    uint32_t Console() const;
    int Descriptor() const;
    int Error() const;
    FEATHEROS_SWIFT_INTERIOR_POINTER Surface* Screen();

    void Invalidate(int32_t x, int32_t y, int32_t width, int32_t height);
    void InvalidateAll();
    Status Present();

    Status WaitEvent(int32_t timeout_ms, Event* event);
    Status DroppedEvents(uint32_t* count);

private:
    static constexpr uint32_t EventBatch = 16;

    Status Fail(Status status);

    const System* m_system;
    int m_descriptor;
    int m_error;
    uint32_t m_console;
    Surface m_screen;
    DirtyRegion m_dirty;
    Event m_events[EventBatch];
    uint32_t m_next_event;
    uint32_t m_event_count;
};

} // namespace FeatherOS::Graphics

#endif
