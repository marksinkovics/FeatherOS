#include <catch2/catch_test_macros.hpp>

#include "../kernel/usb/fake_device.h"
#include "../kernel/usb/fake_hardware.h"
#include "../kernel/usb/fake_xhci.h"
#include "../kernel/usb/rig.h"

#include "kernel/boot_options.h"
#include "kernel/input/input.h"
#include "kernel/input/pointer.h"
#include "kernel/usb/device.h"
#include "kernel/usb/driver.h"
#include "kernel/usb/hid/driver.h"
#include "kernel/usb/manager.h"
#include "kernel/usb/xhci/controller.h"
#include "kernel/usb/xhci/hardware.h"
#include "kernel/usb/xhci/registers.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{

namespace T = FeatherOS::Usb;
namespace X = FeatherOS::Usb::Xhci;
namespace H = FeatherOS::Usb::Hid;
namespace Input = FeatherOS::Input;
namespace User = FeatherOS::User;
using FeatherOS::Usb::Speed;
using FeatherOS::Usb::Status;
using FeatherOS::Usb::Transfer;
using FeatherOS::Usb::Testing::FakeHardware;
using FeatherOS::Usb::Testing::FakeXhci;
using FeatherOS::Usb::Testing::MakeKeyboard;
using FeatherOS::Usb::Testing::Rig;
using FeatherOS::Usb::Testing::ScriptedDevice;

using Scripted = std::shared_ptr<ScriptedDevice>;
using Outcome = ScriptedDevice::Outcome;

// R4-01

struct ResetRecord
{
    int attaches = 0;
    int detaches = 0;
    int serviced = 0;
};

ResetRecord resets;

const T::Driver ResetDriver = {
    "reset-test",
    [](const T::InterfaceView& interface) { return interface.class_code == 3; },
    [](T::Device&, const T::InterfaceView&, void** state)
    {
        ++resets.attaches;
        *state = new int(0);
        return Status::Success;
    },
    [](T::Device&, void* state)
    {
        ++resets.detaches;
        delete static_cast<int*>(state);
    },
    [](T::Device&, void*)
    {
        ++resets.serviced;
        return resets.serviced == 1;
    },
    false};

const T::Driver* const ResetDrivers[] = {&ResetDriver};

// A drive on a USB 3 root port that is enabled in U0 (as after a normal connect) asks for a
// port reset, the way the storage driver does after a failed BOMS reset. The reset must reach
// the port: ResetPort returns true without writing PR or WPR while the port is enabled in U0.
TEST_CASE("R4-01: re-enumeration of a USB 3 root-port device really resets the port",
          "[R4-01]")
{
    resets = {};
    Rig rig;
    rig.fake.SetUsb3Ports(5);
    rig.drivers = ResetDrivers;
    REQUIRE(rig.Create());
    rig.fake.Connect(5, Speed::Super, true, MakeKeyboard(Speed::Super));
    rig.Settle();
    const uint32_t slot = rig.manager->SlotOfPort(5);
    REQUIRE(slot != 0);
    REQUIRE(resets.attaches == 1);
    REQUIRE(rig.fake.PortResets(5) == 0);

    auto* device = const_cast<T::Device*>(rig.manager->Find(slot));
    REQUIRE(device != nullptr);
    device->PostWork();
    rig.Settle();
    REQUIRE(resets.serviced == 1);
    REQUIRE(resets.detaches == 1);
    // The device was detached and enumerated again, so the port has to have been reset in
    // between: otherwise SET_ADDRESS goes to a device that already has an address.
    REQUIRE(rig.fake.PortResets(5) == 1);
}

// R4-02

struct Listener
{
    X::Controller* controller = nullptr;
    uint32_t slot = 0;
    Transfer transfer;
    FeatherOS::Memory::Dma::Buffer buffer{};
    int resubmits = 0;
    std::vector<Status> statuses;
};

void Listen(Transfer& transfer)
{
    auto* listener = static_cast<Listener*>(transfer.context);
    listener->statuses.push_back(transfer.status);
    if (listener->resubmits > 0)
    {
        --listener->resubmits;
        listener->controller->Submit(listener->slot, transfer);
    }
}

struct SecondFailure
{
    Rig* rig = nullptr;
    int calls = 0;
};

void OnRecovered(void* context, uint32_t, uint8_t, uint32_t, Status)
{
    auto* state = static_cast<SecondFailure*>(context);
    if (state->calls++ == 0)
    {
        // The event task runs now: the transfer the callback resubmitted has failed already,
        // after RecoverEndpoint cleared recovery_needed and before ServiceEndpoints' own clear.
        state->rig->controller->ProcessEvents();
    }
}

TEST_CASE("R4-02: a failure flagged while an endpoint is being recovered is recovered too",
          "[R4-02]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Scripted device = MakeKeyboard(Speed::High);
    rig.fake.Connect(1, Speed::High, true, device);
    rig.Settle();
    const uint32_t slot = rig.manager->SlotOfPort(1);
    REQUIRE(slot != 0);
    {
        const T::Device* info = rig.manager->Find(slot);
        const T::InterfaceView& interface = info->configuration.interfaces[0];
        const std::vector<T::EndpointDescriptor> endpoints(
            interface.endpoints, interface.endpoints + interface.endpoint_count);
        REQUIRE(rig.controller->ConfigureEndpoints(slot, endpoints, nullptr));
        FeatherOS::Memory::Dma::Buffer none{};
        uint32_t actual = 0;
        REQUIRE(rig.controller->Control(slot, {0x00, 9, info->configuration.value, 0, 0}, none,
                                        &actual) == Status::Success);
    }

    SecondFailure second;
    second.rig = &rig;
    rig.controller->SetRecoveryHandler(OnRecovered, &second);

    Listener listener;
    REQUIRE(rig.controller->AllocateBuffer(64, &listener.buffer));
    listener.controller = rig.controller.get();
    listener.slot = slot;
    listener.transfer.endpoint = 0x81;
    listener.transfer.buffer = &listener.buffer;
    listener.transfer.length = 8;
    listener.transfer.context = &listener;
    listener.transfer.done = Listen;
    // One resubmit by the callback, like the HID driver's, and the device stalls both tries.
    listener.resubmits = 1;
    device->FailEndpoint(0x81, Outcome::Stall, 2);

    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    rig.controller->ProcessEvents();
    for (int round = 0; round < 4; ++round)
    {
        rig.Settle();
    }
    REQUIRE(second.calls >= 1);
    // The second stall must be recovered as well: its transfer finishes with Stall too. With
    // the bug the endpoint stays halted, recovery_needed stays set and the mask bit is gone.
    REQUIRE(listener.statuses == std::vector<Status>{Status::Stall, Status::Stall});
    rig.controller->FreeBuffer(listener.buffer);
}

// R4-03

// Passes everything to the fake machine but writes the abort of the command ring as the
// production Write64 does: low dword, then high dword (the fake's own Write64 goes high
// first, which hides the hazard).
class LowFirstHardware final : public X::Hardware
{
public:
    explicit LowFirstHardware(FakeHardware& inner) : inner_(inner) {}

    uint32_t Read32(uint32_t offset) override
    {
        return inner_.Read32(offset);
    }
    void Write32(uint32_t offset, uint32_t value) override
    {
        inner_.Write32(offset, value);
    }
    void Write64(uint32_t offset, uint64_t value) override
    {
        if (offset == FakeXhci::OperationalOffset(X::Op::CommandRing) &&
            (value & X::CommandRingControl::CommandAbort) != 0)
        {
            inner_.Write32(offset, static_cast<uint32_t>(value));
            inner_.Write32(offset + 4, static_cast<uint32_t>(value >> 32));
            return;
        }
        inner_.Write64(offset, value);
    }
    uint32_t ReadConfig32(uint16_t offset) override
    {
        return inner_.ReadConfig32(offset);
    }
    void WriteConfig32(uint16_t offset, uint32_t value) override
    {
        inner_.WriteConfig32(offset, value);
    }
    bool AllocateDma(uint64_t bytes, uint64_t below, FeatherOS::Memory::Dma::Buffer* buffer) override
    {
        return inner_.AllocateDma(bytes, below, buffer);
    }
    void FreeDma(const FeatherOS::Memory::Dma::Buffer& buffer) override
    {
        inner_.FreeDma(buffer);
    }
    uint64_t NowMilliseconds() override
    {
        return inner_.NowMilliseconds();
    }
    void Pause(uint32_t milliseconds) override
    {
        inner_.Pause(milliseconds);
    }
    bool WaitUntil(bool (*done)(const void* context), const void* context,
                   uint64_t deadline_milliseconds) override
    {
        return inner_.WaitUntil(done, context, deadline_milliseconds);
    }
    void WakeWaiters() override
    {
        inner_.WakeWaiters();
    }
    void SignalEvents() override
    {
        inner_.SignalEvents();
    }
    void WaitForEvents(uint64_t deadline_milliseconds) override
    {
        inner_.WaitForEvents(deadline_milliseconds);
    }
    X::InterruptKind SetUpInterrupts(bool allow, void (*handler)(void* context),
                                     void* context) override
    {
        return inner_.SetUpInterrupts(allow, handler, context);
    }

private:
    FakeHardware& inner_;
};

TEST_CASE("R4-03: the command ring still works after a command was aborted",
          "[R4-03]")
{
    Rig rig;
    LowFirstHardware hardware(rig.hardware);
    struct Teardown
    {
        Rig& rig;
        ~Teardown()
        {
            rig.manager.reset();
            rig.controller.reset();
        }
    } teardown{rig};
    rig.hardware.on_idle = [&rig]
    {
        if (rig.controller != nullptr)
        {
            rig.controller->ProcessEvents();
        }
    };
    // The rig's memory starts above 4 GiB, so the command ring sits there and its pointer's
    // high dword is not zero.
    rig.controller = std::make_unique<X::Controller>(hardware, rig.function);
    REQUIRE(rig.controller->Initialize(true));
    rig.manager = std::make_unique<T::Manager>(*rig.controller, 1, T::BuiltInDrivers());

    rig.fake.HoldNextCommand(X::TrbType::AddressDevice);
    rig.fake.Connect(1, Speed::High);
    rig.Settle();
    REQUIRE(rig.fake.AbortCount() >= 1);
    REQUIRE(rig.fake.Addressed().empty());

    // The ring has to carry on after the aborted command: the next device is addressed. The
    // high-dword write of the abort leaves the Command Ring Pointer pointing below 4 GiB.
    rig.fake.Connect(2, Speed::Full);
    rig.Settle();
    REQUIRE(rig.fake.Addressed().size() == 1);
    REQUIRE(rig.fake.Faults() == 0);
}

// R4-04

std::string Listing()
{
    char buffer[1024];
    const size_t length = T::FormatDevices(buffer, sizeof(buffer));
    return std::string(buffer, length);
}

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

TEST_CASE("R4-04: an input report longer than the endpoint packet is parsed as one report",
          "[R4-04]")
{
    FeatherOS::BootOptions::SelectUsbHid(FeatherOS::BootOptions::UsbHidMode::Default);
    Rig rig;
    rig.drivers = T::BuiltInDrivers();
    REQUIRE(rig.Create());

    // Buttons (1 byte), 7 bytes of constant padding, then X and Y: ten bytes, while the
    // interrupt IN endpoint of the scripted device has wMaxPacketSize 8.
    const std::vector<uint8_t> descriptor = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
        0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
        0x95, 0x07, 0x75, 0x08, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
        0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0};
    Scripted mouse = std::make_shared<ScriptedDevice>(
        FeatherOS::Usb::Testing::MakeDeviceDescriptor(0x1234, 0x9999, 64),
        FeatherOS::Usb::Testing::GenericMouseConfiguration(
            static_cast<uint16_t>(descriptor.size())));
    mouse->SetReportDescriptor(0, descriptor);
    rig.fake.Connect(1, Speed::High, true, mouse);
    rig.Settle();
    REQUIRE(rig.manager->SlotOfPort(1) != 0);
    REQUIRE(Listing().find("hid\n") != std::string::npos);

    Input::Pointer::Reader reader = Input::Pointer::Open();
    rig.fake.DeliverReport(mouse, 0x81, {0, 0, 0, 0, 0, 0, 0, 0, 5, 0xFB});
    rig.controller->ProcessEvents();
    const std::vector<PointerEvent> events = ReadPointer(reader);

    const std::vector<PointerEvent> expected = {
        {User::InputEventRelative, User::InputRelativeX, 5},
        {User::InputEventRelative, User::InputRelativeY, -5},
        {User::InputEventSync, User::InputSyncReport, 0}};
    REQUIRE(events == expected);

    for (uint32_t port = 1; port <= 8; ++port)
    {
        rig.fake.Disconnect(port);
    }
    rig.Settle();
}

} // namespace
