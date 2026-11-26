#include "display.h"

#include "libc/include/featheros/framebuffer.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace FeatherOS::Graphics
{

namespace
{

int SystemOpen(const char* path)
{
    return open(path, O_RDWR | O_NONBLOCK);
}

int SystemControl(int descriptor, unsigned long request, void* argument)
{
    return ioctl(descriptor, request, argument);
}

long SystemRead(int descriptor, void* buffer, size_t length)
{
    return static_cast<long>(read(descriptor, buffer, length));
}

int SystemPoll(int descriptor, int timeout_ms)
{
    pollfd events = {descriptor, POLLIN, 0};
    return poll(&events, 1, timeout_ms);
}

int SystemClose(int descriptor)
{
    return close(descriptor);
}

const System RealSystem = {SystemOpen, SystemControl, SystemRead, SystemPoll, SystemClose};

EventKind DecodeKind(uint8_t kind)
{
    switch (kind)
    {
    case FB_EVENT_RELEASE:
        return EventKind::Release;
    case FB_EVENT_PRESS:
        return EventKind::Press;
    case FB_EVENT_REPEAT:
        return EventKind::Repeat;
    case FB_EVENT_SHOWN:
        return EventKind::Shown;
    case FB_EVENT_HIDDEN:
        return EventKind::Hidden;
    default:
        return EventKind::Unknown;
    }
}

} // namespace

const System* DefaultSystem()
{
    return &RealSystem;
}

Display::Display() : Display(&RealSystem) {}

Display::Display(const System* system)
    : m_system(system), m_descriptor(-1), m_error(0), m_console(0), m_screen(0, 0), m_dirty(0, 0),
      m_events{}, m_next_event(0), m_event_count(0)
{
}

// Swift imports a type only when it can be moved; the moved-from Display is closed.
Display::Display(Display&& other) noexcept
    : m_system(other.m_system), m_descriptor(other.m_descriptor), m_error(other.m_error),
      m_console(other.m_console), m_screen(static_cast<Surface&&>(other.m_screen)),
      m_dirty(other.m_dirty), m_events{}, m_next_event(other.m_next_event),
      m_event_count(other.m_event_count)
{
    for (uint32_t index = 0; index < EventBatch; ++index)
    {
        m_events[index] = other.m_events[index];
    }
    other.m_descriptor = -1;
    other.m_next_event = 0;
    other.m_event_count = 0;
}

Display::~Display()
{
    Close();
}

Status Display::Fail(Status status)
{
    m_error = errno;
    return status;
}

Status Display::Open()
{
    return Open(0);
}

Status Display::Open(uint32_t acquire_flags)
{
    if (IsOpen())
    {
        return Status::Ok;
    }
    const int descriptor = m_system->Open("/dev/fb0");
    if (descriptor < 0)
    {
        return Fail(Status::OpenFailed);
    }
    fb_info info = {};
    if (m_system->Control(descriptor, FB_GET_INFO, &info) != 0)
    {
        const Status status = Fail(Status::InfoFailed);
        m_system->Close(descriptor);
        return status;
    }
    Surface screen(static_cast<int32_t>(info.width), static_cast<int32_t>(info.height));
    if (!screen.Valid())
    {
        m_system->Close(descriptor);
        m_error = ENOMEM;
        return Status::OutOfMemory;
    }
    if (m_system->Control(descriptor, FB_ACQUIRE,
                          reinterpret_cast<void*>(static_cast<uintptr_t>(acquire_flags))) != 0)
    {
        const Status status = Fail(Status::AcquireFailed);
        m_system->Close(descriptor);
        return status;
    }
    m_descriptor = descriptor;
    m_console = info.console;
    m_screen = static_cast<Surface&&>(screen);
    m_dirty = DirtyRegion(m_screen.Width(), m_screen.Height());
    m_next_event = 0;
    m_event_count = 0;
    return Status::Ok;
}

void Display::Close()
{
    if (m_descriptor < 0)
    {
        return;
    }
    m_system->Control(m_descriptor, FB_RELEASE, nullptr);
    m_system->Close(m_descriptor);
    m_descriptor = -1;
    m_dirty.Clear();
}

bool Display::IsOpen() const
{
    return m_descriptor >= 0;
}

int32_t Display::Width() const
{
    return m_screen.Width();
}

int32_t Display::Height() const
{
    return m_screen.Height();
}

uint32_t Display::Console() const
{
    return m_console;
}

int Display::Descriptor() const
{
    return m_descriptor;
}

int Display::Error() const
{
    return m_error;
}

Surface* Display::Screen()
{
    return &m_screen;
}

void Display::Invalidate(int32_t x, int32_t y, int32_t width, int32_t height)
{
    m_dirty.Add(x, y, width, height);
}

void Display::InvalidateAll()
{
    m_dirty.AddAll();
}

Status Display::Present()
{
    if (!IsOpen())
    {
        return Status::NotOpen;
    }
    const auto stride = static_cast<uint32_t>(m_screen.Width());
    for (uint32_t index = 0; index < m_dirty.Count(); ++index)
    {
        const Rectangle area = m_dirty.At(index);
        fb_present request = {};
        request.pixels =
            m_screen.Pixels() + static_cast<size_t>(area.y) * stride + static_cast<size_t>(area.x);
        request.stride = stride;
        request.x = area.x;
        request.y = area.y;
        request.width = static_cast<uint32_t>(area.width);
        request.height = static_cast<uint32_t>(area.height);
        if (m_system->Control(m_descriptor, FB_PRESENT, &request) != 0)
        {
            return Fail(Status::PresentFailed);
        }
    }
    m_dirty.Clear();
    return Status::Ok;
}

Status Display::WaitEvent(int32_t timeout_ms, Event* event)
{
    if (!IsOpen())
    {
        return Status::NotOpen;
    }
    if (m_next_event == m_event_count)
    {
        const int ready = m_system->Poll(m_descriptor, timeout_ms);
        if (ready < 0)
        {
            return errno == EINTR ? Status::Interrupted : Fail(Status::ReadFailed);
        }
        if (ready == 0)
        {
            return Status::Timeout;
        }
        fb_event raw[EventBatch];
        const long length = m_system->Read(m_descriptor, raw, sizeof(raw));
        if (length < 0)
        {
            if (errno == EAGAIN)
            {
                return Status::Timeout;
            }
            return errno == EINTR ? Status::Interrupted : Fail(Status::ReadFailed);
        }
        if (length == 0 || static_cast<size_t>(length) % sizeof(fb_event) != 0)
        {
            m_error = EIO;
            return Status::ReadFailed;
        }
        m_next_event = 0;
        m_event_count = static_cast<uint32_t>(static_cast<size_t>(length) / sizeof(fb_event));
        for (uint32_t index = 0; index < m_event_count; ++index)
        {
            m_events[index] = {DecodeKind(raw[index].kind), raw[index].modifiers, raw[index].code,
                               raw[index].time_ms};
        }
    }
    if (event != nullptr)
    {
        *event = m_events[m_next_event];
    }
    ++m_next_event;
    return Status::Ok;
}

Status Display::DroppedEvents(uint32_t* count)
{
    if (!IsOpen())
    {
        return Status::NotOpen;
    }
    fb_info info = {};
    if (m_system->Control(m_descriptor, FB_GET_INFO, &info) != 0)
    {
        return Fail(Status::InfoFailed);
    }
    *count = info.dropped_events;
    return Status::Ok;
}

} // namespace FeatherOS::Graphics
