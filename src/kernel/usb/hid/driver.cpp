#include "driver.h"

#include "kernel/boot_options.h"
#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"
#include "kernel/input/pointer.h"
#include "kernel/sync/spinlock.h"
#include "kernel/usb/device.h"
#include "kernel/usb/hid/keyboard.h"
#include "kernel/usb/hid/report.h"
#include "user/thread_safety.h"

#include <atomic>
#include <span>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Usb::Hid
{
namespace
{

constexpr uint32_t SlotCount = 2;
constexpr uint32_t QueueSize = 512;
constexpr uint32_t TicksPerSecond = 1000;
constexpr uint8_t HidClass = 3;
constexpr uint8_t BootSubclass = 1;
constexpr uint8_t KeyboardProtocol = 1;
constexpr uint8_t MouseProtocol = 2;
constexpr uint8_t InterfaceToHost = 0x81;
constexpr uint8_t ClassToInterface = 0x21;
constexpr uint8_t GetDescriptorRequest = 6;
constexpr uint8_t SetReportRequest = 9;
constexpr uint8_t SetIdleRequest = 0x0A;
constexpr uint8_t SetProtocolRequest = 0x0B;
constexpr uint8_t OutputReport = 2;
constexpr uint16_t ProtocolBoot = 0;
constexpr uint16_t ProtocolReport = 1;
constexpr uint32_t EndpointPacketMask = 0x7FF;
constexpr uint8_t NoLedState = 0xFF;
constexpr uint32_t MouseButtons = 8;
constexpr size_t MaxMouseEvents = MouseButtons + 5;

struct Port;

struct Slot
{
    DeviceTransfer transfer;
    Memory::Dma::Buffer buffer{};
    Port* port = nullptr;
};

constinit Sync::SpinLock registry_lock;
constinit std::atomic<KeySink> key_sink = Input::SubmitKey;

// The event task of the device's controller runs completions and repeats, but a transfer that
// failed or was cancelled completes on the manager task. State shared between them is under
// lock; keys reach Input one drainer at a time, in order, with no lock held.
struct Port
{
    Port() : repeater(TicksPerSecond) {}

    Device* device = nullptr;
    HostController* host = nullptr;
    uint32_t index = 0;
    uint8_t interface = 0;
    bool keyboard = false;
    bool mouse = false;
    bool has_leds = false;
    uint32_t transfer_length = 0;
    ReportMap map;
    Slot slots[SlotCount];
    Memory::Dma::Buffer control{};
    // Only the manager task reads or writes it.
    uint8_t leds_sent = NoLedState;
    // Only the drainer uses it.
    Input::Keyboard input;
    std::atomic<uint64_t> deadline = NoDeadline;
    uint32_t busy GUARDED_BY(registry_lock) = 0;

    Sync::SpinLock lock;
    KeySet down GUARDED_BY(lock);
    Repeater repeater GUARDED_BY(lock);
    KeyEvent queue[QueueSize] GUARDED_BY(lock) = {};
    uint32_t head GUARDED_BY(lock) = 0;
    uint32_t count GUARDED_BY(lock) = 0;
    bool draining GUARDED_BY(lock) = false;

    // Held while a report is submitted, so two tasks never put one mouse's reports out of order.
    Sync::SpinLock mouse_lock;
    uint32_t buttons GUARDED_BY(mouse_lock) = 0;
};

Port* ports[MaxPorts] GUARDED_BY(registry_lock) = {};

void Enqueue(Port& port, const KeyEvent* events, size_t count) REQUIRES(port.lock)
{
    for (size_t index = 0; index < count && port.count < QueueSize; ++index)
    {
        port.queue[(port.head + port.count) % QueueSize] = events[index];
        ++port.count;
    }
}

void Apply(Port& port, const KeyEvent* events, size_t count, uint64_t now) REQUIRES(port.lock)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (events[index].pressed)
        {
            port.repeater.Press(events[index].code, now);
        }
        else
        {
            port.repeater.Release(events[index].code);
        }
    }
    Enqueue(port, events, count);
    port.deadline.store(port.repeater.Deadline(), std::memory_order_release);
}

void Drain(Port& port)
{
    {
        Sync::SpinLockGuard guard(port.lock);
        if (port.draining)
        {
            return;
        }
        port.draining = true;
    }
    for (;;)
    {
        KeyEvent event;
        {
            Sync::SpinLockGuard guard(port.lock);
            if (port.count == 0)
            {
                port.draining = false;
                return;
            }
            event = port.queue[port.head];
            port.head = (port.head + 1) % QueueSize;
            --port.count;
        }
        key_sink.load(std::memory_order_acquire)(port.input, event.code, event.pressed);
    }
}

size_t AddEvent(User::InputEvent* events, size_t count, uint16_t type, uint16_t code, int32_t value)
{
    events[count] = {};
    events[count].type = type;
    events[count].code = code;
    events[count].value = value;
    return count + 1;
}

size_t AddRelative(User::InputEvent* events, size_t count, uint16_t code, int32_t value)
{
    return value == 0 ? count : AddEvent(events, count, User::InputEventRelative, code, value);
}

void SubmitMouse(Port& port, const MouseReport& report) REQUIRES(port.mouse_lock)
{
    User::InputEvent events[MaxMouseEvents] = {};
    size_t count = 0;
    const uint32_t changed = (report.buttons ^ port.buttons) & ((1U << MouseButtons) - 1);
    for (uint32_t button = 0; button < MouseButtons; ++button)
    {
        if ((changed >> button & 1U) != 0)
        {
            count = AddEvent(events, count, User::InputEventKey,
                             static_cast<uint16_t>(User::InputButtonLeft + button),
                             static_cast<int32_t>(report.buttons >> button & 1U));
        }
    }
    port.buttons = report.buttons & ((1U << MouseButtons) - 1);
    count = AddRelative(events, count, User::InputRelativeX, report.dx);
    count = AddRelative(events, count, User::InputRelativeY, report.dy);
    count = AddRelative(events, count, User::InputRelativeWheel, report.wheel);
    count = AddRelative(events, count, User::InputRelativeHorizontalWheel, report.pan);
    if (count != 0)
    {
        count = AddEvent(events, count, User::InputEventSync, User::InputSyncReport, 0);
        Input::Pointer::Submit(events, count);
    }
}

void ReleaseButtons(Port& port)
{
    Sync::SpinLockGuard guard(port.mouse_lock);
    if (port.buttons != 0)
    {
        SubmitMouse(port, MouseReport{});
    }
}

void ReleaseInput(Port& port)
{
    KeyEvent events[MaxKeyEvents];
    {
        Sync::SpinLockGuard guard(port.lock);
        const size_t count = ReleaseAll(&port.down, events, MaxKeyEvents);
        port.repeater.Stop();
        Apply(port, events, count, 0);
    }
    Drain(port);
    if (port.mouse)
    {
        ReleaseButtons(port);
    }
}

void HandleReport(Port& port, const uint8_t* data, size_t length)
{
    const std::span<const uint8_t> report(data, length);
    if (port.keyboard)
    {
        KeySet keys;
        if (ExtractKeyboard(port.map, report, &keys))
        {
            KeyEvent events[MaxKeyEvents];
            {
                Sync::SpinLockGuard guard(port.lock);
                const size_t count = Advance(&port.down, keys, events, MaxKeyEvents);
                Apply(port, events, count, port.host->NowMilliseconds());
            }
            Drain(port);
        }
    }
    if (port.mouse)
    {
        MouseReport mouse;
        if (ExtractMouse(port.map, report, &mouse))
        {
            Sync::SpinLockGuard guard(port.mouse_lock);
            SubmitMouse(port, mouse);
        }
    }
}

void OnReport(DeviceTransfer& transfer)
{
    auto* slot = static_cast<Slot*>(transfer.driver_context);
    Port& port = *slot->port;
    const Usb::Status status = transfer.status;
    // Cancelled and NoDevice end a detach, which releases the keys itself.
    if (status == Usb::Status::Cancelled || status == Usb::Status::NoDevice)
    {
        return;
    }
    if (Succeeded(status))
    {
        uint8_t report[MaxReportBytes];
        size_t length = transfer.actual < MaxReportBytes ? transfer.actual : MaxReportBytes;
        for (size_t index = 0; index < length; ++index)
        {
            report[index] = slot->buffer.address[index];
        }
        HandleReport(port, report, length);
    }
    else
    {
        ReleaseInput(port);
    }
    port.device->Submit(transfer);
}

Usb::Status Request(Port& port, const SetupPacket& setup, Memory::Dma::Buffer& buffer)
{
    uint32_t actual = 0;
    return port.host->Control(port.device->slot, setup, buffer, &actual);
}

bool SetProtocol(Port& port, uint16_t protocol)
{
    Memory::Dma::Buffer none{};
    return Request(port, {ClassToInterface, SetProtocolRequest, protocol, port.interface, 0},
                   none) == Usb::Status::Success;
}

void SetIdle(Port& port)
{
    Memory::Dma::Buffer none{};
    Request(port, {ClassToInterface, SetIdleRequest, 0, port.interface, 0}, none);
}

bool LoadReportMap(Port& port, const InterfaceView& interface)
{
    const uint16_t length = HidReportLength(interface);
    if (length == 0 || length > MaxDescriptorBytes)
    {
        return false;
    }
    Memory::Dma::Buffer buffer{};
    if (!port.host->AllocateBuffer(MaxDescriptorBytes, &buffer))
    {
        return false;
    }
    uint32_t actual = 0;
    const SetupPacket setup = {InterfaceToHost, GetDescriptorRequest,
                               static_cast<uint16_t>(DescriptorType::HidReport << 8),
                               port.interface, length};
    const Usb::Status status = port.host->Control(port.device->slot, setup, buffer, &actual);
    bool parsed = false;
    if (Succeeded(status) && actual > 0)
    {
        uint8_t bytes[MaxDescriptorBytes];
        const size_t count = actual < MaxDescriptorBytes ? actual : MaxDescriptorBytes;
        for (size_t index = 0; index < count; ++index)
        {
            bytes[index] = buffer.address[index];
        }
        parsed = ParseReportDescriptor(std::span<const uint8_t>(bytes, count), &port.map) ==
                 Hid::Status::Ok;
    }
    port.host->FreeBuffer(buffer);
    if (!parsed)
    {
        port.map = ReportMap{};
    }
    return parsed;
}

bool Configure(Port& port, const InterfaceView& interface)
{
    const bool boot_capable =
        interface.subclass == BootSubclass &&
        (interface.protocol == KeyboardProtocol || interface.protocol == MouseProtocol);
    const bool mouse_interface = boot_capable && interface.protocol == MouseProtocol;
    const bool by_report = !boot_capable || mouse_interface ||
                           BootOptions::SelectedUsbHid() == BootOptions::UsbHidMode::Report;
    if (by_report && LoadReportMap(port, interface) &&
        (mouse_interface ? port.map.has_mouse : (port.map.has_keyboard || port.map.has_mouse)))
    {
        port.keyboard = port.map.has_keyboard;
        port.mouse = port.map.has_mouse;
        if (boot_capable)
        {
            SetProtocol(port, ProtocolReport);
        }
        SetIdle(port);
        return true;
    }
    if (!boot_capable || !SetProtocol(port, ProtocolBoot))
    {
        return false;
    }
    port.keyboard = interface.protocol == KeyboardProtocol;
    port.mouse = !port.keyboard;
    port.map = port.keyboard ? BootKeyboard() : BootMouse();
    SetIdle(port);
    return true;
}

void SendLedReport(Port& port, uint8_t state)
{
    uint8_t report[MaxReportBytes];
    size_t length = 0;
    port.leds_sent = state;
    if (!BuildLedReport(port.map, state, report, sizeof(report), &length))
    {
        return;
    }
    for (size_t index = 0; index < length; ++index)
    {
        port.control.address[index] = report[index];
    }
    const uint8_t id = port.map.uses_ids ? report[0] : 0;
    Request(port,
            {ClassToInterface, SetReportRequest, static_cast<uint16_t>((OutputReport << 8) | id),
             port.interface, static_cast<uint16_t>(length)},
            port.control);
}

bool Register(Port& port)
{
    Sync::SpinLockGuard guard(registry_lock);
    for (uint32_t index = 0; index < MaxPorts; ++index)
    {
        if (ports[index] == nullptr)
        {
            ports[index] = &port;
            port.index = index;
            return true;
        }
    }
    return false;
}

void Unregister(Port& port)
{
    for (bool first = true;; first = false)
    {
        {
            Sync::SpinLockGuard guard(registry_lock);
            if (first)
            {
                ports[port.index] = nullptr;
            }
            if (port.busy == 0)
            {
                return;
            }
        }
        port.host->Delay(1);
    }
}

void Release(Port* port)
{
    for (Slot& slot : port->slots)
    {
        if (slot.buffer.size != 0)
        {
            port->host->FreeBuffer(slot.buffer);
        }
    }
    if (port->control.size != 0)
    {
        port->host->FreeBuffer(port->control);
    }
    delete port;
}

const EndpointDescriptor* FindEndpoint(const InterfaceView& interface)
{
    for (uint32_t index = 0; index < interface.endpoint_count; ++index)
    {
        const EndpointDescriptor& endpoint = interface.endpoints[index];
        if (endpoint.type == EndpointType::Interrupt && (endpoint.address & 0x80U) != 0)
        {
            return &endpoint;
        }
    }
    return nullptr;
}

Usb::Status Attach(Device& device, const InterfaceView& interface, void** state)
{
    const EndpointDescriptor* endpoint = FindEndpoint(interface);
    if (endpoint == nullptr)
    {
        return Usb::Status::Stall;
    }
    auto* port = new Port;
    port->device = &device;
    port->host = &device.Host();
    port->interface = interface.number;
    uint32_t packet = endpoint->max_packet_size & EndpointPacketMask;
    packet = packet == 0 ? 1 : packet;
    port->transfer_length = packet < MaxReportBytes ? packet : MaxReportBytes;
    bool ready =
        Configure(*port, interface) && port->host->AllocateBuffer(MaxReportBytes, &port->control);
    for (Slot& slot : port->slots)
    {
        ready = ready && port->host->AllocateBuffer(MaxReportBytes, &slot.buffer);
    }
    if (!ready || !Register(*port))
    {
        Release(port);
        return Usb::Status::Stall;
    }
    uint8_t probe[MaxReportBytes];
    size_t probe_length = 0;
    port->has_leds =
        port->keyboard && BuildLedReport(port->map, 0, probe, sizeof(probe), &probe_length);
    bool first = true;
    for (Slot& slot : port->slots)
    {
        slot.port = port;
        slot.transfer.endpoint = endpoint->address;
        slot.transfer.buffer = &slot.buffer;
        slot.transfer.length = port->transfer_length;
        slot.transfer.callback = OnReport;
        slot.transfer.driver_context = &slot;
        if (!device.Submit(slot.transfer) && first)
        {
            Unregister(*port);
            Release(port);
            return Usb::Status::Stall;
        }
        first = false;
    }
    if (port->has_leds)
    {
        SendLedReport(*port, Input::LedState());
    }
    *state = port;
    return Usb::Status::Success;
}

void Detach(Device& device, void* state)
{
    (void)device;
    auto* port = static_cast<Port*>(state);
    Unregister(*port);
    ReleaseInput(*port);
    Release(port);
}

bool HasInterruptIn(const InterfaceView& interface)
{
    return FindEndpoint(interface) != nullptr;
}

bool MatchesKeyboard(const InterfaceView& interface)
{
    return interface.class_code == HidClass && interface.subclass == BootSubclass &&
           interface.protocol == KeyboardProtocol && HasInterruptIn(interface);
}

bool MatchesMouse(const InterfaceView& interface)
{
    return interface.class_code == HidClass && interface.subclass == BootSubclass &&
           interface.protocol == MouseProtocol && HasInterruptIn(interface);
}

bool MatchesGeneric(const InterfaceView& interface)
{
    return interface.class_code == HidClass && HasInterruptIn(interface) &&
           !MatchesKeyboard(interface) && !MatchesMouse(interface);
}

} // namespace

const Driver KeyboardDriver = {"hid-keyboard", MatchesKeyboard, Attach, Detach};
const Driver MouseDriver = {"hid-mouse", MatchesMouse, Attach, Detach};
const Driver GenericDriver = {"hid", MatchesGeneric, Attach, Detach};

void SetKeySink(KeySink sink)
{
    key_sink.store(sink != nullptr ? sink : Input::SubmitKey, std::memory_order_release);
}

void ServiceRepeat(HostController& host, uint64_t now_milliseconds)
{
    Port* due[MaxPorts];
    size_t count = 0;
    {
        Sync::SpinLockGuard guard(registry_lock);
        for (Port* port : ports)
        {
            if (port != nullptr && port->host == &host &&
                port->deadline.load(std::memory_order_acquire) <= now_milliseconds)
            {
                ++port->busy;
                due[count++] = port;
            }
        }
    }
    for (size_t index = 0; index < count; ++index)
    {
        Port& port = *due[index];
        {
            Sync::SpinLockGuard guard(port.lock);
            uint16_t code;
            if (port.repeater.Due(now_milliseconds, &code))
            {
                const KeyEvent event = {code, true};
                Enqueue(port, &event, 1);
            }
            port.deadline.store(port.repeater.Deadline(), std::memory_order_release);
        }
        Drain(port);
        Sync::SpinLockGuard guard(registry_lock);
        --port.busy;
    }
}

uint64_t NextRepeat(HostController& host)
{
    uint64_t next = NoDeadline;
    Sync::SpinLockGuard guard(registry_lock);
    for (const Port* port : ports)
    {
        if (port != nullptr && port->host == &host)
        {
            const uint64_t deadline = port->deadline.load(std::memory_order_acquire);
            next = deadline < next ? deadline : next;
        }
    }
    return next;
}

void SendLeds()
{
    const uint8_t state = Input::LedState();
    Port* stale[MaxPorts];
    size_t count = 0;
    {
        Sync::SpinLockGuard guard(registry_lock);
        for (Port* port : ports)
        {
            if (port != nullptr && port->has_leds && port->leds_sent != state)
            {
                stale[count++] = port;
            }
        }
    }
    for (size_t index = 0; index < count; ++index)
    {
        SendLedReport(*stale[index], state);
    }
}

} // namespace FeatherOS::Usb::Hid
