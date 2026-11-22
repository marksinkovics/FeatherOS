#include <catch2/catch_test_macros.hpp>

#include "fake_device.h"
#include "rig.h"

#include "kernel/boot_options.h"
#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"
#include "kernel/input/pointer.h"
#include "kernel/terminal/terminal.h"
#include "kernel/usb/device.h"
#include "kernel/usb/hid/driver.h"
#include "support/scheduler_stubs.h"
#include "support/terminal_device.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace T = FeatherOS::Usb;
namespace H = FeatherOS::Usb::Hid;
namespace Input = FeatherOS::Input;
using FeatherOS::Usb::Speed;
using FeatherOS::Usb::Status;
using FeatherOS::Usb::Testing::MakeKeyboard;
using FeatherOS::Usb::Testing::MakeMouse;
using FeatherOS::Usb::Testing::MakeWheelMouse;
using FeatherOS::Usb::Testing::Rig;
using FeatherOS::Usb::Testing::ScriptedDevice;

using Scripted = std::shared_ptr<ScriptedDevice>;
using Bytes = std::vector<uint8_t>;

constexpr uint16_t KeyA = 30;
constexpr uint16_t KeyB = 48;
constexpr uint16_t KeyLeftShift = 42;
constexpr uint16_t KeyCapsLock = 58;
constexpr uint16_t KeyNumLock = 69;
constexpr uint8_t SetReport = 9;
constexpr uint8_t SetIdle = 0x0A;
constexpr uint8_t SetProtocol = 0x0B;
constexpr uint8_t GetDescriptor = 6;
constexpr uint8_t ShiftModifier = 0x02;
constexpr uint8_t UsageA = 0x04;
constexpr uint8_t UsageB = 0x05;
constexpr uint8_t UsageCapsLock = 0x39;
constexpr uint8_t UsageNumLock = 0x53;
const Bytes Idle = {0, 0, 0, 0, 0, 0, 0, 0};

struct Key
{
    const Input::Keyboard* keyboard;
    uint16_t code;
    bool pressed;

    bool operator==(const Key& other) const
    {
        return keyboard == other.keyboard && code == other.code && pressed == other.pressed;
    }
};

std::vector<Key> keys;
std::atomic<int> inside = 0;
std::atomic<int> overlaps = 0;

void Record(Input::Keyboard& keyboard, uint16_t code, bool pressed)
{
    if (inside.fetch_add(1) != 0)
    {
        overlaps.fetch_add(1);
    }
    keys.push_back({&keyboard, code, pressed});
    inside.fetch_sub(1);
}

std::vector<uint8_t> leds_seen;

void RememberLeds(uint8_t state)
{
    leds_seen.push_back(state);
}

Bytes Report(uint8_t modifiers, std::initializer_list<uint8_t> usages)
{
    Bytes report = {modifiers, 0, 0, 0, 0, 0, 0, 0};
    size_t slot = 2;
    for (const uint8_t usage : usages)
    {
        report[slot++] = usage;
    }
    return report;
}

namespace User = FeatherOS::User;

struct PointerEvent
{
    uint16_t type;
    uint16_t code;
    int32_t value;

    bool operator==(const PointerEvent& other) const
    {
        return type == other.type && code == other.code && value == other.value;
    }
};

constexpr PointerEvent PointerSync = {User::InputEventSync, User::InputSyncReport, 0};

PointerEvent Rel(uint16_t code, int32_t value)
{
    return {User::InputEventRelative, code, value};
}

PointerEvent Button(uint16_t code, int32_t value)
{
    return {User::InputEventKey, code, value};
}

std::vector<PointerEvent> ReadPointer(Input::Pointer::Reader& reader)
{
    std::vector<PointerEvent> events;
    for (;;)
    {
        User::InputEvent raw[16];
        const uint64_t bytes = Input::Pointer::Read(reader, raw, sizeof(raw), true);
        if (User::IsSyscallError(bytes))
        {
            return events;
        }
        for (uint64_t index = 0; index < bytes / sizeof(raw[0]); ++index)
        {
            events.push_back({raw[index].type, raw[index].code, raw[index].value});
        }
    }
}

using PointerEvents = std::vector<PointerEvent>;

class HidRig
{
public:
    explicit HidRig(bool record = true)
    {
        keys.clear();
        leds_seen.clear();
        inside = 0;
        overlaps = 0;
        FeatherOS::BootOptions::SelectUsbHid(FeatherOS::BootOptions::UsbHidMode::Default);
        Input::SetLockState(false, false);
        Input::SetLedCallback(RememberLeds);
        H::SetKeySink(record ? Record : nullptr);
        rig.drivers = T::BuiltInDrivers();
        REQUIRE(rig.Create());
    }

    ~HidRig()
    {
        for (uint32_t port = 1; port <= 8; ++port)
        {
            rig.fake.Disconnect(port);
        }
        rig.Settle();
        H::SetKeySink(nullptr);
        Input::SetLedCallback(nullptr);
        Input::SetLockState(false, false);
        FeatherOS::BootOptions::SelectUsbHid(FeatherOS::BootOptions::UsbHidMode::Default);
    }

    uint32_t Plug(uint32_t port, const Scripted& device, Speed speed = Speed::High)
    {
        rig.fake.Connect(port, speed, true, device);
        rig.Settle();
        return rig.manager->SlotOfPort(port);
    }

    void Send(const Scripted& device, const Bytes& report)
    {
        rig.fake.DeliverReport(device, 0x81, report);
        rig.controller->ProcessEvents();
    }

    void Tap(const Scripted& device, uint8_t usage, uint8_t modifiers = 0)
    {
        Send(device, Report(modifiers, {usage}));
        Send(device, Idle);
    }

    void Advance(uint32_t milliseconds)
    {
        rig.hardware.Pause(milliseconds);
        H::ServiceRepeat(*rig.controller, rig.hardware.Now());
    }

    size_t Requests(const Scripted& device, uint8_t request) const
    {
        size_t count = 0;
        for (const auto& entry : device->Requests())
        {
            count += entry.request == request && entry.type >= 0x21 ? 1 : 0;
        }
        return count;
    }

    Rig rig;
};

std::string Listing()
{
    char buffer[1024];
    const size_t length = T::FormatDevices(buffer, sizeof(buffer));
    return std::string(buffer, length);
}

size_t Count(const std::vector<Key>& events, uint16_t code, bool pressed)
{
    size_t count = 0;
    for (const Key& key : events)
    {
        count += key.code == code && key.pressed == pressed ? 1 : 0;
    }
    return count;
}

std::string ReadAll(FeatherOS::Terminals::Terminal& terminal)
{
    terminal.SetSettings({0, 0, 0, 0}, 1);
    std::string text;
    char buffer[64];
    for (;;)
    {
        const uint64_t count = terminal.Read(0, buffer, sizeof(buffer));
        REQUIRE_FALSE(FeatherOS::User::IsSyscallError(count));
        if (count == 0)
        {
            return text;
        }
        text.append(buffer, count);
    }
}

TEST_CASE("hid T31: a boot keyboard and mouse bind, use the boot layouts and queue two transfers",
          "[usb][hid]")
{
    HidRig world;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    const Scripted mouse = MakeMouse(Speed::High);
    REQUIRE(world.Plug(1, keyboard) != 0);
    REQUIRE(world.Plug(2, mouse) != 0);
    REQUIRE(Listing() ==
            "1-1 dev 2 high 0627:0001 class 03/01/01 hid-keyboard\n"
            "1-2 dev 3 high 0627:0002 class 03/01/02 hid-mouse\n");

    for (const Scripted& device : {keyboard, mouse})
    {
        REQUIRE(world.Requests(device, SetProtocol) == 1);
        REQUIRE(world.Requests(device, SetIdle) == 1);
    }
    // The keyboard needs no report descriptor, the mouse reads its own.
    REQUIRE(keyboard->Count(0x81, GetDescriptor) == 0);
    REQUIRE(mouse->Count(0x81, GetDescriptor) == 1);
    for (const auto& request : mouse->Requests())
    {
        if (request.request == SetProtocol)
        {
            REQUIRE(request.value == 1);
        }
    }
    for (const auto& request : keyboard->Requests())
    {
        if (request.request == SetProtocol)
        {
            REQUIRE(request.type == 0x21);
            REQUIRE(request.value == 0);
            REQUIRE(request.index == 0);
        }
        if (request.request == SetIdle)
        {
            REQUIRE(request.value == 0);
        }
    }

    // Both queued transfers take a report before either is resubmitted.
    world.rig.fake.DeliverReport(keyboard, 0x81, Report(0, {UsageA}));
    world.rig.fake.DeliverReport(keyboard, 0x81, Idle);
    REQUIRE_FALSE(keyboard->HasReport(0x81));
    world.rig.fake.DeliverReport(keyboard, 0x81, Report(0, {UsageB}));
    REQUIRE(keyboard->HasReport(0x81));
    world.rig.controller->ProcessEvents();
    world.rig.controller->ProcessEvents();
    REQUIRE_FALSE(keyboard->HasReport(0x81));
    REQUIRE(Count(keys, KeyA, true) == 1);
    REQUIRE(Count(keys, KeyA, false) == 1);
    REQUIRE(Count(keys, KeyB, true) == 1);
    REQUIRE(world.rig.fake.Faults() == 0);
}

TEST_CASE("hid T32: keys typed on a USB keyboard reach the terminal with Shift and Caps Lock",
          "[usb][hid]")
{
    TestScheduler::Reset();
    TestTerminalDevice screen;
    FeatherOS::Terminals::Terminal terminal(screen.Device());
    Input::SetTerminal(&terminal);
    {
        HidRig world(false);
        const Scripted keyboard = MakeKeyboard(Speed::High);
        REQUIRE(world.Plug(1, keyboard) != 0);
        world.Tap(keyboard, UsageA);
        world.Tap(keyboard, UsageA, ShiftModifier);
        world.Tap(keyboard, UsageCapsLock);
        world.Tap(keyboard, UsageB);
        world.Tap(keyboard, UsageCapsLock);
        world.Tap(keyboard, UsageB);
        // Rollover reports change nothing, the held key keeps counting as down.
        world.Send(keyboard, Report(0, {UsageA}));
        world.Send(keyboard, Report(0, {1, 1, 1, 1, 1, 1}));
        world.Send(keyboard, Idle);
        REQUIRE(ReadAll(terminal) == "aABba");
        REQUIRE_FALSE(Input::CapsLock());
    }
    Input::SetTerminal(nullptr);
}

TEST_CASE("hid T37: usbhid=report reads the descriptor and types through the parser",
          "[usb][hid]")
{
    HidRig world;
    FeatherOS::BootOptions::SelectUsbHid(FeatherOS::BootOptions::UsbHidMode::Report);
    const Scripted keyboard = MakeKeyboard(Speed::High);
    REQUIRE(world.Plug(1, keyboard) != 0);
    REQUIRE(keyboard->Count(0x81, GetDescriptor) == 1);
    for (const auto& request : keyboard->Requests())
    {
        if (request.type == 0x81 && request.request == GetDescriptor)
        {
            REQUIRE(request.value == 0x2200);
            REQUIRE(request.length == 63);
        }
        if (request.request == SetProtocol)
        {
            REQUIRE(request.value == 1);
        }
    }
    REQUIRE(world.Requests(keyboard, SetProtocol) == 1);
    world.Tap(keyboard, UsageA, ShiftModifier);
    REQUIRE(Count(keys, KeyLeftShift, true) == 1);
    REQUIRE(Count(keys, KeyA, true) == 1);
    REQUIRE(Count(keys, KeyA, false) == 1);
    REQUIRE(Count(keys, KeyLeftShift, false) == 1);
}

TEST_CASE("hid: a boot mouse with a wheel descriptor reports the wheel by default", "[usb][hid]")
{
    HidRig world;
    const Scripted mouse = MakeWheelMouse(Speed::High, true);
    REQUIRE(world.Plug(1, mouse) != 0);
    REQUIRE(Listing().find("hid-mouse\n") != std::string::npos);
    REQUIRE(world.Requests(mouse, SetProtocol) == 1);
    Input::Pointer::Reader reader = Input::Pointer::Open();
    world.Send(mouse, {0x00, 0, 0, 2, 0});
    REQUIRE(ReadPointer(reader) == PointerEvents{Rel(User::InputRelativeWheel, 2), PointerSync});
}

TEST_CASE("hid: a mouse with a refused descriptor falls back to boot and still moves",
          "[usb][hid]")
{
    HidRig world;
    const Scripted mouse = MakeMouse(Speed::High);
    mouse->SetReportDescriptor(0, {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01});
    REQUIRE(world.Plug(1, mouse) != 0);
    REQUIRE(Listing() == "1-1 dev 2 high 0627:0002 class 03/01/02 hid-mouse\n");
    REQUIRE(world.Requests(mouse, SetProtocol) == 1);
    for (const auto& request : mouse->Requests())
    {
        if (request.request == SetProtocol)
        {
            REQUIRE(request.value == 0);
        }
    }
    Input::Pointer::Reader reader = Input::Pointer::Open();
    world.Send(mouse, {0x01, 5, 0xFB});
    REQUIRE(ReadPointer(reader) == PointerEvents{Button(0x110, 1), Rel(User::InputRelativeX, 5),
                                                 Rel(User::InputRelativeY, -5), PointerSync});
}

TEST_CASE("hid: a refused report descriptor falls back to the boot protocol", "[usb][hid]")
{
    HidRig world;
    FeatherOS::BootOptions::SelectUsbHid(FeatherOS::BootOptions::UsbHidMode::Report);
    const Scripted keyboard = MakeKeyboard(Speed::High);
    // Unbalanced: the collection never ends.
    keyboard->SetReportDescriptor(0, {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01});
    REQUIRE(world.Plug(1, keyboard) != 0);
    REQUIRE(Listing() == "1-1 dev 2 high 0627:0001 class 03/01/01 hid-keyboard\n");
    REQUIRE(world.Requests(keyboard, SetProtocol) == 1);
    for (const auto& request : keyboard->Requests())
    {
        if (request.request == SetProtocol)
        {
            REQUIRE(request.value == 0);
        }
    }
    world.Tap(keyboard, UsageB);
    REQUIRE(Count(keys, KeyB, true) == 1);
}

TEST_CASE("hid: an interface that is not a boot interface binds by its descriptor",
          "[usb][hid]")
{
    HidRig world;
    const Scripted keyboard = std::make_shared<ScriptedDevice>(
        FeatherOS::Usb::Testing::MakeDeviceDescriptor(0x1234, 0x5678, 64),
        FeatherOS::Usb::Testing::GenericKeyboardConfiguration(63));
    keyboard->SetReportDescriptor(0, FeatherOS::Usb::Testing::BootKeyboardDescriptor());
    REQUIRE(world.Plug(1, keyboard) != 0);
    REQUIRE(Listing() == "1-1 dev 2 high 1234:5678 class 03/00/00 hid\n");
    REQUIRE(world.Requests(keyboard, SetProtocol) == 0);
    REQUIRE(world.Requests(keyboard, SetIdle) == 1);
    world.Tap(keyboard, UsageA);
    REQUIRE(Count(keys, KeyA, true) == 1);

    // Without a usable descriptor nothing is left to drive it.
    const Scripted other = std::make_shared<ScriptedDevice>(
        FeatherOS::Usb::Testing::MakeDeviceDescriptor(0x1234, 0x5679, 64),
        FeatherOS::Usb::Testing::GenericKeyboardConfiguration(63));
    REQUIRE(world.Plug(2, other) != 0);
    REQUIRE(Listing().find("1-2 dev 3 high 1234:5679 class 03/00/00 unbound\n") !=
            std::string::npos);
}

TEST_CASE("hid T35: a hot-plugged keyboard types, and the next one still works after it is gone",
          "[usb][hid]")
{
    HidRig world;
    for (int round = 0; round < 3; ++round)
    {
        keys.clear();
        const Scripted keyboard = MakeKeyboard(Speed::High);
        REQUIRE(world.Plug(4, keyboard) != 0);
        world.Tap(keyboard, UsageA);
        REQUIRE(Count(keys, KeyA, true) == 1);
        world.rig.fake.Disconnect(4);
        world.rig.Settle();
        REQUIRE(Listing().empty());
    }
    REQUIRE(world.rig.hardware.Outstanding() == world.rig.baseline);
}

TEST_CASE("hid T23: a held key repeats after 500 ms every 33 ms until it is released",
          "[usb][hid]")
{
    HidRig world;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    REQUIRE(world.Plug(1, keyboard) != 0);
    REQUIRE(H::NextRepeat(*world.rig.controller) == H::NoDeadline);
    const uint64_t start = world.rig.hardware.Now();
    world.Send(keyboard, Report(0, {UsageA}));
    REQUIRE(H::NextRepeat(*world.rig.controller) == start + 500);
    REQUIRE(Count(keys, KeyA, true) == 1);

    world.Advance(499);
    REQUIRE(Count(keys, KeyA, true) == 1);
    world.Advance(1);
    REQUIRE(Count(keys, KeyA, true) == 2);
    REQUIRE(H::NextRepeat(*world.rig.controller) == start + 533);
    world.Advance(33);
    REQUIRE(Count(keys, KeyA, true) == 3);

    // Shift does not stop it, a second key replaces it, and releasing the old one changes nothing.
    world.Send(keyboard, Report(ShiftModifier, {UsageA}));
    world.Advance(0);
    world.Send(keyboard, Report(ShiftModifier, {UsageA, UsageB}));
    const size_t a_before = Count(keys, KeyA, true);
    world.Advance(500);
    REQUIRE(Count(keys, KeyA, true) == a_before);
    REQUIRE(Count(keys, KeyB, true) == 2);
    world.Send(keyboard, Report(ShiftModifier, {UsageB}));
    world.Advance(33);
    REQUIRE(Count(keys, KeyB, true) == 3);
    world.Send(keyboard, Idle);
    REQUIRE(H::NextRepeat(*world.rig.controller) == H::NoDeadline);
    world.Advance(1000);
    REQUIRE(Count(keys, KeyB, true) == 3);
    REQUIRE(Count(keys, KeyB, false) == 1);
}

TEST_CASE("hid I23: unplugging a keyboard releases every key that is down", "[usb][hid]")
{
    HidRig world;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    const Scripted other = MakeKeyboard(Speed::High);
    REQUIRE(world.Plug(1, keyboard) != 0);
    REQUIRE(world.Plug(2, other) != 0);
    world.Send(keyboard, Report(ShiftModifier, {UsageA, UsageB}));
    world.Send(other, Report(0, {UsageB}));
    const Input::Keyboard* first = keys.front().keyboard;
    REQUIRE(Count(keys, KeyLeftShift, true) == 1);
    keys.clear();

    world.rig.fake.Disconnect(1);
    world.rig.Settle();
    // Keys before modifiers, and nothing for the keyboard that stays.
    REQUIRE(keys.size() == 3);
    for (const Key& key : keys)
    {
        REQUIRE(key.keyboard == first);
        REQUIRE_FALSE(key.pressed);
    }
    REQUIRE(keys[2].code == KeyLeftShift);

    // The other keyboard's key keeps repeating until it is released.
    REQUIRE(H::NextRepeat(*world.rig.controller) != H::NoDeadline);
    world.Send(other, Idle);
    REQUIRE(keys.size() == 4);
    REQUIRE(keys[3].keyboard != first);
    REQUIRE(keys[3].code == KeyB);
    REQUIRE_FALSE(keys[3].pressed);
    REQUIRE(H::NextRepeat(*world.rig.controller) == H::NoDeadline);
    world.rig.fake.Disconnect(2);
    world.rig.Settle();
    REQUIRE(world.rig.hardware.Outstanding() == world.rig.baseline);
}

TEST_CASE("hid I23: an endpoint error releases the keys and typing continues", "[usb][hid]")
{
    HidRig world;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    REQUIRE(world.Plug(1, keyboard) != 0);
    world.Send(keyboard, Report(ShiftModifier, {UsageA}));
    keys.clear();

    // An empty report only kicks the ring; the first queued transfer stalls before it.
    keyboard->FailEndpoint(0x81, ScriptedDevice::Outcome::Stall, 1);
    world.rig.fake.DeliverReport(keyboard, 0x81, {});
    world.rig.controller->ProcessEvents();
    REQUIRE(keys.empty());
    world.rig.Settle();
    // No report followed, so only the error itself can have released them.
    REQUIRE(keys.size() == 2);
    REQUIRE(keys[0] == Key{keys[0].keyboard, KeyA, false});
    REQUIRE(keys[1] == Key{keys[0].keyboard, KeyLeftShift, false});

    world.Send(keyboard, Report(0, {UsageB}));
    world.Send(keyboard, Idle);
    REQUIRE(Count(keys, KeyB, true) == 1);
    REQUIRE(Count(keys, KeyB, false) == 1);
    REQUIRE(Listing() == "1-1 dev 2 high 0627:0001 class 03/01/01 hid-keyboard\n");
}

TEST_CASE("hid I21 and I25: lock changes only post work, and every keyboard gets the global state",
          "[usb][hid]")
{
    HidRig world(false);
    const Scripted first = MakeKeyboard(Speed::High);
    const Scripted second = MakeKeyboard(Speed::High);
    REQUIRE(world.Plug(1, first) != 0);
    REQUIRE(world.Plug(2, second) != 0);
    // The attach sends the lock state as it is.
    REQUIRE(first->DataOf(SetReport) == std::vector<Bytes>{{0}});
    REQUIRE(second->DataOf(SetReport) == std::vector<Bytes>{{0}});

    const auto leds = [](const Scripted& device)
    {
        std::vector<uint8_t> state;
        for (const Bytes& data : device->DataOf(SetReport))
        {
            state.push_back(data.at(0));
        }
        return state;
    };

    world.Tap(first, UsageCapsLock);
    REQUIRE(Input::CapsLock());
    REQUIRE(leds_seen == std::vector<uint8_t>{Input::LedCapsLock});
    // The event path issued no control transfer.
    REQUIRE(leds(first) == std::vector<uint8_t>{0});
    REQUIRE(leds(second) == std::vector<uint8_t>{0});

    H::SendLeds();
    REQUIRE(leds(first) == (std::vector<uint8_t>{0, 2}));
    REQUIRE(leds(second) == (std::vector<uint8_t>{0, 2}));
    H::SendLeds();
    REQUIRE(leds(first) == (std::vector<uint8_t>{0, 2}));

    // A key held down toggles once; the second keyboard's Num Lock joins the global state.
    world.Send(second, Report(0, {UsageNumLock}));
    world.Send(second, Report(0, {UsageNumLock}));
    world.Send(second, Idle);
    REQUIRE(Input::NumLock());
    H::SendLeds();
    REQUIRE(leds(first) == (std::vector<uint8_t>{0, 2, 3}));
    REQUIRE(leds(second) == (std::vector<uint8_t>{0, 2, 3}));

    world.Tap(first, UsageCapsLock);
    world.Tap(second, UsageNumLock);
    H::SendLeds();
    REQUIRE(leds(first) == (std::vector<uint8_t>{0, 2, 3, 0}));
    REQUIRE(leds(second) == (std::vector<uint8_t>{0, 2, 3, 0}));

    // A keyboard plugged in later starts with the current state.
    Input::SetLockState(true, false);
    const Scripted third = MakeKeyboard(Speed::High);
    REQUIRE(world.Plug(3, third) != 0);
    REQUIRE(leds(third) == std::vector<uint8_t>{2});
    H::SendLeds();
    REQUIRE(leds(first).back() == 2);
    REQUIRE(leds(third).size() == 1);
}

TEST_CASE("hid T33: boot mouse reports become button changes, moves and one sync", "[usb][hid]")
{
    HidRig world;
    const Scripted mouse = MakeMouse(Speed::High);
    REQUIRE(world.Plug(1, mouse) != 0);
    Input::Pointer::Reader reader = Input::Pointer::Open();

    world.Send(mouse, {0x01, 5, 0xFB});
    REQUIRE(ReadPointer(reader) == PointerEvents{Button(0x110, 1), Rel(User::InputRelativeX, 5),
                                                 Rel(User::InputRelativeY, -5), PointerSync});
    // A held button is not repeated, and a report that changes nothing is not submitted.
    world.Send(mouse, {0x01, 0, 0});
    REQUIRE(ReadPointer(reader).empty());
    world.Send(mouse, {0x03, 0x80, 0x7F});
    REQUIRE(ReadPointer(reader) == PointerEvents{Button(0x111, 1), Rel(User::InputRelativeX, -128),
                                                 Rel(User::InputRelativeY, 127), PointerSync});
    world.Send(mouse, {0x00, 0, 0});
    REQUIRE(ReadPointer(reader) ==
            PointerEvents{Button(0x110, 0), Button(0x111, 0), PointerSync});
    world.Send(mouse, {0x04, 0, 0});
    REQUIRE(ReadPointer(reader) == PointerEvents{Button(0x112, 1), PointerSync});
    world.Send(mouse, {0x00, 0, 0});
    REQUIRE(ReadPointer(reader) == PointerEvents{Button(0x112, 0), PointerSync});
}

TEST_CASE("hid T33: a report-protocol mouse gives wheel and pan events", "[usb][hid]")
{
    HidRig world;
    const Scripted mouse = MakeWheelMouse(Speed::High);
    REQUIRE(world.Plug(1, mouse) != 0);
    REQUIRE(Listing().find("hid\n") != std::string::npos);
    Input::Pointer::Reader reader = Input::Pointer::Open();

    world.Send(mouse, {0x04, 0xFE, 0x03, 0xFF, 0x01});
    REQUIRE(ReadPointer(reader) ==
            PointerEvents{Button(0x112, 1), Rel(User::InputRelativeX, -2),
                          Rel(User::InputRelativeY, 3), Rel(User::InputRelativeWheel, -1),
                          Rel(User::InputRelativeHorizontalWheel, 1), PointerSync});
    world.Send(mouse, {0x00, 0, 0, 2, 0});
    REQUIRE(ReadPointer(reader) ==
            PointerEvents{Button(0x112, 0), Rel(User::InputRelativeWheel, 2), PointerSync});
}

TEST_CASE("hid T33: detach releases the held buttons, and mice keep their own state",
          "[usb][hid]")
{
    HidRig world;
    const Scripted first = MakeMouse(Speed::High);
    const Scripted second = MakeMouse(Speed::High);
    REQUIRE(world.Plug(1, first) != 0);
    REQUIRE(world.Plug(2, second) != 0);
    Input::Pointer::Reader reader = Input::Pointer::Open();

    world.Send(first, {0x05, 0, 0});
    REQUIRE(ReadPointer(reader) ==
            PointerEvents{Button(0x110, 1), Button(0x112, 1), PointerSync});
    // The second mouse sees no button of its own go up, so the first one's stay down.
    world.Send(second, {0x00, 1, 0});
    REQUIRE(ReadPointer(reader) == PointerEvents{Rel(User::InputRelativeX, 1), PointerSync});

    world.rig.fake.Disconnect(1);
    world.rig.Settle();
    REQUIRE(ReadPointer(reader) ==
            PointerEvents{Button(0x110, 0), Button(0x112, 0), PointerSync});
    world.rig.fake.Disconnect(2);
    world.rig.Settle();
    REQUIRE(ReadPointer(reader).empty());
}

TEST_CASE("hid: the sink is never entered twice at once while the event task and repeats race",
          "[usb][hid][concurrency]")
{
    HidRig world;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    REQUIRE(world.Plug(1, keyboard) != 0);
    std::atomic<bool> done = false;
    std::thread repeats(
        [&]
        {
            uint64_t now = 1'000'000;
            while (!done.load())
            {
                H::ServiceRepeat(*world.rig.controller, now);
                now += 40;
            }
        });
    constexpr int Rounds = 300;
    for (int round = 0; round < Rounds; ++round)
    {
        world.Send(keyboard, Report(0, {UsageA}));
        world.Send(keyboard, Idle);
    }
    done = true;
    repeats.join();
    H::ServiceRepeat(*world.rig.controller, 5'000'000);

    REQUIRE(overlaps.load() == 0);
    REQUIRE(Count(keys, KeyA, false) == Rounds);
    // A repeat only ever falls between its key's press and release.
    bool down = false;
    for (const Key& key : keys)
    {
        if (key.pressed)
        {
            down = true;
        }
        else
        {
            REQUIRE(down);
            down = false;
        }
    }
    REQUIRE_FALSE(down);
}

} // namespace
