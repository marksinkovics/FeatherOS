#include <catch2/catch_test_macros.hpp>

#include "fake_device.h"
#include "fake_hardware.h"
#include "fake_xhci.h"

#include "kernel/usb/manager.h"
#include "kernel/usb/xhci/context.h"
#include "kernel/usb/xhci/controller.h"
#include "kernel/usb/xhci/registers.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{

namespace X = FeatherOS::Usb::Xhci;
using FeatherOS::Pci::Function;
using FeatherOS::Usb::EndpointDescriptor;
using FeatherOS::Usb::SetupPacket;
using FeatherOS::Usb::Speed;
using FeatherOS::Usb::Status;
using FeatherOS::Usb::Transfer;
using FeatherOS::Usb::Testing::MakeKeyboard;
using FeatherOS::Usb::Testing::MakeMouse;
using FeatherOS::Usb::Testing::ScriptedDevice;
using FeatherOS::Usb::Testing::FakeHardware;
using FeatherOS::Usb::Testing::FakeXhci;
using FeatherOS::Usb::Testing::PhysicalMemory;

constexpr uint64_t FourGiB = 1ULL << 32;

Function QemuFunction()
{
    return {{0, 4, 0}, 0x1b36, 0x000d, 0x0c, 0x03, 0x30, 0, 1};
}

class Rig
{
public:
    explicit Rig(uint32_t ports = 8, bool context64 = false, uint64_t base = 0x1'2000'0000ULL,
                 const Function& pci = QemuFunction())
        : memory(base), fake(memory, ports, context64), hardware(fake, memory), function(pci)
    {
        hardware.on_idle = [this]
        {
            if (controller != nullptr)
            {
                controller->ProcessEvents();
            }
        };
    }

    bool Create(bool allow_interrupts = true)
    {
        controller = std::make_unique<X::Controller>(hardware, function);
        const bool created = controller->Initialize(allow_interrupts);
        manager = std::make_unique<FeatherOS::Usb::Manager>(*controller);
        baseline = hardware.Outstanding();
        return created;
    }

    void Settle()
    {
        controller->ProcessEvents();
        for (int round = 0; round < 64 && manager->HasWork(); ++round)
        {
            manager->Run();
            controller->ProcessEvents();
        }
    }

    uint32_t Command(uint32_t reg) const
    {
        return fake.UsbCommand() & reg;
    }

    PhysicalMemory memory;
    FakeXhci fake;
    FakeHardware hardware;
    Function function;
    std::unique_ptr<X::Controller> controller;
    std::unique_ptr<FeatherOS::Usb::Manager> manager;
    uint32_t baseline = 0;
};

TEST_CASE("xhci: the controller line names ports, slots, context size and interrupt kind",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    char line[128];
    REQUIRE(rig.controller->Describe(line, sizeof(line)));
    REQUIRE(std::string(line) ==
            "USB: xHCI 1b36:000d at 00:04.0, 8 ports, 64 slots, 32-byte contexts, MSI-X");
    REQUIRE(rig.controller->Interrupts() == X::InterruptKind::MsiX);
    REQUIRE(rig.controller->InterruptRegistered());
    REQUIRE(rig.controller->RootPorts() == 8);
    REQUIRE(rig.controller->SlotCount() == 64);
    char small[16];
    REQUIRE_FALSE(rig.controller->Describe(small, sizeof(small)));

    Rig wide(4, true);
    wide.hardware.available = X::InterruptKind::Msi;
    REQUIRE(wide.Create());
    REQUIRE(wide.controller->Describe(line, sizeof(line)));
    REQUIRE(std::string(line) ==
            "USB: xHCI 1b36:000d at 00:04.0, 4 ports, 64 slots, 64-byte contexts, MSI");

    Rig intx;
    intx.hardware.available = X::InterruptKind::Intx;
    REQUIRE(intx.Create());
    REQUIRE(intx.controller->Describe(line, sizeof(line)));
    REQUIRE(std::string(line).ends_with("INTx"));
}

TEST_CASE("xhci: interrupts are enabled except in polling mode", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    REQUIRE(rig.Command(X::UsbCmd::RunStop) != 0);
    REQUIRE(rig.Command(X::UsbCmd::InterrupterEnable) != 0);
    REQUIRE(rig.Command(X::UsbCmd::HostSystemErrorEnable) != 0);
    REQUIRE(rig.fake.InterruptModeration() == 4000);
    REQUIRE(rig.fake.ErstSize() == 1);
    REQUIRE(rig.fake.ConfiguredSlots() == 64);
    REQUIRE(rig.hardware.InterruptsAllowed());

    Rig polling;
    REQUIRE(polling.Create(false));
    REQUIRE(polling.controller->Interrupts() == X::InterruptKind::Polling);
    REQUIRE_FALSE(polling.controller->InterruptRegistered());
    REQUIRE(polling.Command(X::UsbCmd::RunStop) != 0);
    REQUIRE(polling.Command(X::UsbCmd::InterrupterEnable) == 0);
    REQUIRE_FALSE(polling.hardware.InterruptsAllowed());
    char line[128];
    REQUIRE(polling.controller->Describe(line, sizeof(line)));
    REQUIRE(std::string(line).ends_with("polling"));

    // A machine with no MSI and no INTx route is polled as well.
    Rig none;
    none.hardware.available = X::InterruptKind::Polling;
    REQUIRE(none.Create());
    REQUIRE(none.Command(X::UsbCmd::InterrupterEnable) == 0);
}

TEST_CASE("xhci: the interrupt top half quiets the controller and signals the event task",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    rig.fake.SetStatusBits(X::UsbSts::EventInterrupt);
    REQUIRE((rig.fake.Read32(FakeXhci::OperationalOffset(X::Op::UsbSts)) &
             X::UsbSts::EventInterrupt) != 0);
    rig.hardware.FireInterrupt();
    REQUIRE(rig.hardware.Signals() == 1);
    REQUIRE((rig.fake.Read32(FakeXhci::OperationalOffset(X::Op::UsbSts)) &
             X::UsbSts::EventInterrupt) == 0);
}

TEST_CASE("xhci T13: the firmware gives up ownership after a few polls", "[xhci]")
{
    Rig rig;
    rig.fake.EnableLegacySupport(true, 3);
    REQUIRE(rig.Create());
    REQUIRE(rig.fake.Resets().size() == 1);
    const FakeXhci::ResetRecord& reset = rig.fake.Resets()[0];
    REQUIRE(reset.legacy_present);
    REQUIRE(reset.os_owned);
    REQUIRE_FALSE(reset.bios_owned);
    REQUIRE(reset.smi_enables == 0);
    REQUIRE(reset.smi_status == 0);
    REQUIRE(reset.halted);
    // It did not wait out the whole second.
    REQUIRE(rig.hardware.Now() < 100);
}

TEST_CASE("xhci T13: ownership the firmware never gives up is forced after a second", "[xhci]")
{
    Rig rig;
    rig.fake.EnableLegacySupport(true, -1);
    REQUIRE(rig.Create());
    const FakeXhci::ResetRecord& reset = rig.fake.Resets()[0];
    REQUIRE(reset.os_owned);
    REQUIRE_FALSE(reset.bios_owned);
    REQUIRE(reset.smi_enables == 0);
    REQUIRE(reset.smi_status == 0);
    REQUIRE(rig.hardware.Now() >= 1000);
    REQUIRE(rig.hardware.Now() < 1100);
}

TEST_CASE("xhci T13: SMI enables are cleared and status bits written even without firmware",
          "[xhci]")
{
    Rig rig;
    rig.fake.EnableLegacySupport(false, -1);
    REQUIRE(rig.Create());
    REQUIRE(rig.fake.OsOwned());
    REQUIRE(rig.fake.SmiEnables() == 0);
    REQUIRE(rig.fake.SmiStatus() == 0);
    REQUIRE(rig.fake.Resets()[0].os_owned);
    REQUIRE(rig.hardware.Now() < 100);
}

TEST_CASE("xhci T13: a running controller is stopped before the reset", "[xhci]")
{
    Rig rig;
    rig.fake.StartRunning();
    rig.fake.SetHaltDelay(3, false);
    REQUIRE(rig.Create());
    REQUIRE(rig.fake.Resets().size() == 1);
    REQUIRE(rig.fake.Resets()[0].halted);

    Rig stuck;
    stuck.fake.StartRunning();
    stuck.fake.SetHaltDelay(0, true);
    REQUIRE_FALSE(stuck.Create());
    REQUIRE(stuck.fake.Resets().empty());
    REQUIRE(stuck.hardware.Now() >= 32);
    REQUIRE(stuck.hardware.Now() < 100);
    REQUIRE(stuck.hardware.Outstanding() == 0);
}

TEST_CASE("xhci T13: no register is touched while the controller is not ready", "[xhci]")
{
    Rig rig;
    rig.fake.SetResetDelay(5, false);
    REQUIRE(rig.Create());
    REQUIRE(rig.fake.NotReadyAccesses() == 0);
    REQUIRE(rig.fake.Resets().size() == 1);

    Rig never;
    never.fake.SetResetDelay(0, true);
    REQUIRE_FALSE(never.Create());
    REQUIRE(never.fake.NotReadyAccesses() == 0);
    REQUIRE(never.hardware.Now() >= 1000);
    REQUIRE(never.hardware.Now() < 1100);
    REQUIRE(never.hardware.Outstanding() == 0);
}

TEST_CASE("xhci T13: Intel controllers get ten seconds and their ports switched", "[xhci]")
{
    Function intel = QemuFunction();
    intel.vendor = 0x8086;
    intel.device = 0x1E31;
    Rig never(8, false, 0x1'2000'0000ULL, intel);
    never.hardware.SetConfig(0xDC, 0x1234);
    never.hardware.SetConfig(0xD4, 0x5678);
    never.fake.SetResetDelay(0, true);
    REQUIRE_FALSE(never.Create());
    REQUIRE(never.hardware.Now() >= 10000);
    REQUIRE(never.hardware.Now() < 10100);
    bool usb3 = false;
    bool usb2 = false;
    for (const auto& write : never.hardware.ConfigWrites())
    {
        usb3 = usb3 || (write.offset == 0xD8 && write.value == 0x1234);
        usb2 = usb2 || (write.offset == 0xD0 && write.value == 0x5678);
    }
    REQUIRE(usb3);
    REQUIRE(usb2);

    Rig other;
    REQUIRE(other.Create());
    REQUIRE(other.hardware.ConfigWrites().empty());
}

TEST_CASE("xhci T14: scratchpads", "[xhci]")
{
    for (const uint32_t count : {0U, 1U, 31U, 32U, 100U, 1023U})
    {
        INFO(count);
        Rig rig;
        rig.fake.SetScratchpads(count);
        REQUIRE(rig.Create());
        const uint64_t table = rig.fake.DcbaaPointer();
        REQUIRE(table != 0);
        REQUIRE((table & 63) == 0);
        const uint64_t array = rig.fake.DcbaaEntry(0);
        if (count == 0)
        {
            REQUIRE(array == 0);
            continue;
        }
        REQUIRE(array != 0);
        REQUIRE((array & 63) == 0);
        std::set<uint64_t> pages;
        for (uint32_t index = 0; index < count; ++index)
        {
            volatile const uint8_t* entry = rig.memory.Translate(array + index * 8ULL, 8);
            REQUIRE(entry != nullptr);
            uint64_t page = 0;
            for (uint32_t byte = 0; byte < 8; ++byte)
            {
                page |= static_cast<uint64_t>(entry[byte]) << (byte * 8);
            }
            REQUIRE((page & 4095) == 0);
            volatile const uint8_t* contents = rig.memory.Translate(page, 4096);
            REQUIRE(contents != nullptr);
            for (uint32_t offset = 0; offset < 4096; ++offset)
            {
                REQUIRE(contents[offset] == 0);
            }
            pages.insert(page);
        }
        REQUIRE(pages.size() == count);
        for (uint32_t slot = 1; slot <= 64; ++slot)
        {
            REQUIRE(rig.fake.DcbaaEntry(slot) == 0);
        }
    }
}

TEST_CASE("xhci: a controller without 4 KiB pages or reachable memory is refused", "[xhci]")
{
    Rig pages;
    pages.fake.SetPageSize(2);
    REQUIRE_FALSE(pages.Create());
    REQUIRE(pages.hardware.Outstanding() == 0);

    Rig narrow;
    narrow.fake.SetAc64(false);
    REQUIRE_FALSE(narrow.Create());
    REQUIRE(narrow.hardware.Outstanding() == 0);

    Rig low(8, false, 0x1000'0000ULL);
    low.fake.SetAc64(false);
    REQUIRE(low.Create());
    for (const auto& buffer : low.hardware.Live())
    {
        REQUIRE(buffer.physical + buffer.size <= FourGiB);
    }
}

TEST_CASE("xhci T17: running out of memory during bring-up leaks nothing", "[xhci]")
{
    bool created = false;
    for (uint32_t allowed = 0; allowed < 10 && !created; ++allowed)
    {
        INFO(allowed);
        Rig rig;
        rig.fake.SetScratchpads(4);
        rig.hardware.allocations_before_failure = allowed;
        created = rig.Create();
        if (!created)
        {
            REQUIRE(rig.hardware.Outstanding() == 0);
        }
    }
    REQUIRE(created);
}

TEST_CASE("xhci: the controller frees its memory when it goes away", "[xhci]")
{
    Rig rig;
    rig.fake.SetScratchpads(8);
    REQUIRE(rig.Create());
    rig.fake.Connect(1, Speed::High);
    rig.Settle();
    REQUIRE(rig.hardware.Outstanding() == rig.baseline + 3);
    rig.controller.reset();
    REQUIRE(rig.hardware.Outstanding() == 0);
}

struct EnumeratedPort
{
    uint32_t port;
    Speed speed;
    uint32_t packet;
};

void CheckEnumeration(bool context64)
{
    Rig rig(8, context64);
    rig.fake.SetUsb3Ports(5);
    REQUIRE(rig.Create());
    const EnumeratedPort ports[] = {{1, Speed::High, 64},
                                    {2, Speed::Full, 8},
                                    {3, Speed::Low, 8},
                                    {5, Speed::Super, 512}};
    for (const EnumeratedPort& port : ports)
    {
        rig.fake.Connect(port.port, port.speed);
    }
    rig.Settle();
    REQUIRE(rig.fake.Addressed().size() == 4);
    REQUIRE(rig.fake.Faults() == 0);
    std::set<uint32_t> slots;
    for (size_t index = 0; index < 4; ++index)
    {
        const EnumeratedPort& port = ports[index];
        INFO(port.port);
        const auto& record = rig.fake.Addressed()[index];
        REQUIRE(record.root_port == port.port);
        REQUIRE(record.speed_id == X::SpeedId(port.speed));
        REQUIRE(record.ep0_max_packet == port.packet);
        REQUIRE(record.entries == 1);
        REQUIRE(record.route == 0);
        REQUIRE(record.add_flags == 3);
        REQUIRE((record.ep0_ring & 1) == 1);
        slots.insert(record.slot);

        X::SlotInfo info;
        REQUIRE(rig.controller->GetSlot(record.slot, &info));
        REQUIRE(info.port == port.port);
        REQUIRE(info.speed == port.speed);
        REQUIRE((rig.fake.PortStatus(port.port) & X::PortSc::ChangeMask) == 0);
        REQUIRE((rig.fake.PortStatus(port.port) & X::PortSc::Enabled) != 0);
        REQUIRE(rig.fake.PortEnableClears(port.port) == 0);
    }
    REQUIRE(slots.size() == 4);
    // A USB 2 port is reset once; a SuperSpeed port that trained into U0 is not reset at all.
    REQUIRE(rig.fake.PortResets(1) == 1);
    REQUIRE(rig.fake.PortResets(5) == 0);
    REQUIRE_FALSE(rig.manager->HasWork());
}

TEST_CASE("xhci: high, full, low and SuperSpeed root ports are addressed (32-byte contexts)",
          "[xhci]")
{
    CheckEnumeration(false);
}

TEST_CASE("xhci T10: high, full, low and SuperSpeed root ports are addressed (64-byte contexts)",
          "[xhci]")
{
    CheckEnumeration(true);
}

TEST_CASE("xhci: a SuperSpeed port that is not in U0 gets a warm reset", "[xhci]")
{
    Rig rig;
    rig.fake.SetUsb3Ports(5);
    REQUIRE(rig.Create());
    rig.fake.Connect(6, Speed::Super, false);
    rig.Settle();
    REQUIRE(rig.fake.Addressed().size() == 1);
    REQUIRE(rig.fake.Addressed()[0].root_port == 6);
    REQUIRE(rig.fake.PortResets(6) == 1);
}

TEST_CASE("xhci: unplugging a device disables its slot and frees its memory", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    rig.fake.Connect(2, Speed::High);
    rig.Settle();
    REQUIRE(rig.fake.Addressed().size() == 1);
    const uint32_t slot = rig.fake.Addressed()[0].slot;
    REQUIRE(rig.hardware.Outstanding() == rig.baseline + 3);
    REQUIRE(rig.fake.DcbaaEntry(slot) != 0);

    rig.fake.Disconnect(2);
    rig.Settle();
    X::SlotInfo info;
    REQUIRE_FALSE(rig.controller->GetSlot(slot, &info));
    REQUIRE(rig.fake.DisabledSlots() == std::vector<uint32_t>{slot});
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
    REQUIRE(rig.fake.DcbaaEntry(slot) == 0);
    REQUIRE((rig.fake.PortStatus(2) & X::PortSc::ChangeMask) == 0);
}

TEST_CASE("xhci: a device that goes away during the debounce is not enumerated", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    rig.fake.Connect(1, Speed::Full);
    bool removed = false;
    rig.hardware.on_idle = [&]
    {
        if (!removed)
        {
            removed = true;
            rig.fake.Disconnect(1);
        }
        rig.controller->ProcessEvents();
    };
    rig.Settle();
    REQUIRE(rig.fake.Addressed().empty());
    REQUIRE(rig.fake.CommandTypes().empty());
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("xhci: no free device slot leaves the port alone", "[xhci]")
{
    Rig rig;
    rig.fake.SetSlotLimit(1);
    REQUIRE(rig.Create());
    rig.fake.Connect(1, Speed::High);
    rig.fake.Connect(2, Speed::High);
    rig.Settle();
    REQUIRE(rig.fake.Addressed().size() == 1);
    X::SlotInfo info;
    REQUIRE(rig.controller->GetSlot(1, &info));
    // The failed port is not retried by the events its own reset caused.
    REQUIRE_FALSE(rig.manager->HasWork());
    REQUIRE(rig.hardware.Outstanding() == rig.baseline + 3);
}

TEST_CASE("xhci: powered ports when the controller has port power control", "[xhci]")
{
    Rig rig;
    rig.fake.SetPortPowerControl(true);
    REQUIRE(rig.Create());
    for (uint32_t port = 1; port <= 8; ++port)
    {
        REQUIRE((rig.fake.PortStatus(port) & X::PortSc::Power) != 0);
    }
}

TEST_CASE("xhci: events drive the command ring through many laps", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    for (int round = 0; round < 100; ++round)
    {
        rig.fake.Connect(1, round % 2 == 0 ? Speed::High : Speed::Full);
        rig.Settle();
        REQUIRE(rig.fake.Addressed().size() == static_cast<size_t>(round) + 1);
        rig.fake.Disconnect(1);
        rig.Settle();
    }
    REQUIRE(rig.fake.DisabledSlots().size() == 100);
    REQUIRE(rig.fake.Faults() == 0);
    REQUIRE(rig.fake.DroppedEvents() == 0);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("xhci: polling mode enumerates from the tick", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create(false));
    rig.fake.Connect(1, Speed::High);
    rig.Settle();
    REQUIRE(rig.fake.Addressed().size() == 1);
}

TEST_CASE("xhci T9: events that name no pending command are ignored", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    rig.fake.HoldNextCommand(X::TrbType::EnableSlot);
    rig.fake.Connect(1, Speed::High);
    bool injected = false;
    bool released = false;
    rig.hardware.on_idle = [&]
    {
        if (injected && !released)
        {
            // The waiter has looked at the command once more by now.
            released = true;
            rig.fake.ReleaseHeldCommand();
        }
        if (!injected && !rig.fake.CommandTypes().empty())
        {
            injected = true;
            const uint64_t pending = rig.fake.LastHeldTrb();
            // An address nobody ever submitted, one past the pending TRB that no command used, and
            // the pending TRB's address with another completion code than it will get.
            rig.fake.PostInjectedEvent(X::MakeTrb(X::TrbType::CommandCompletionEvent,
                                                  3U << X::TrbControl::SlotShift, 0x1234560,
                                                  X::CompletionCode::StallError
                                                      << X::CompletionShift));
            rig.fake.PostInjectedEvent(X::MakeTrb(X::TrbType::CommandCompletionEvent,
                                                  3U << X::TrbControl::SlotShift,
                                                  pending + X::TrbSize,
                                                  X::CompletionCode::TrbError
                                                      << X::CompletionShift));
        }
        rig.controller->ProcessEvents();
    };
    rig.Settle();
    REQUIRE(injected);
    REQUIRE(released);
    REQUIRE(rig.fake.Addressed().size() == 1);
    REQUIRE(rig.fake.Addressed()[0].slot == 1);
    X::SlotInfo info;
    REQUIRE(rig.controller->GetSlot(1, &info));
    REQUIRE_FALSE(rig.controller->GetSlot(3, &info));
    REQUIRE(rig.fake.AbortCount() == 0);
}

TEST_CASE("xhci T17: a command that never completes is aborted and its slot disabled",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    rig.fake.HoldNextCommand(X::TrbType::AddressDevice);
    rig.fake.Connect(1, Speed::High);
    rig.Settle();
    REQUIRE(rig.fake.AbortCount() == 1);
    REQUIRE(rig.hardware.Now() >= 5000);
    REQUIRE(rig.hardware.Now() < 6000);
    REQUIRE(rig.fake.Addressed().empty());
    REQUIRE(rig.fake.DisabledSlots() == std::vector<uint32_t>{1});
    X::SlotInfo info;
    REQUIRE_FALSE(rig.controller->GetSlot(1, &info));
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
    REQUIRE(rig.fake.DcbaaEntry(1) == 0);
    REQUIRE(rig.fake.Faults() == 0);

    // The ring carries on past the aborted command, so the next device works, and a late event
    // for the aborted one changes nothing.
    rig.fake.PostInjectedEvent(X::MakeTrb(X::TrbType::CommandCompletionEvent,
                                          1U << X::TrbControl::SlotShift, rig.fake.LastHeldTrb(),
                                          X::CompletionCode::Success << X::CompletionShift));
    rig.fake.Connect(2, Speed::Full);
    rig.Settle();
    REQUIRE(rig.fake.Addressed().size() == 1);
    REQUIRE(rig.fake.Addressed()[0].root_port == 2);
    REQUIRE(rig.controller->GetSlot(1, &info));
    REQUIRE(info.port == 2);
    REQUIRE(rig.fake.AbortCount() == 1);
}

TEST_CASE("xhci T17: a command that fails frees what the device took", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    rig.hardware.allocations_before_failure = 1;
    rig.fake.Connect(1, Speed::High);
    rig.Settle();
    REQUIRE(rig.fake.Addressed().empty());
    REQUIRE(rig.fake.DisabledSlots() == std::vector<uint32_t>{1});
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("xhci: a host system error restarts the controller once, then disables it", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    rig.fake.Connect(1, Speed::High);
    rig.Settle();
    REQUIRE(rig.fake.Addressed().size() == 1);
    REQUIRE(rig.fake.Resets().size() == 1);

    rig.fake.SetStatusBits(X::UsbSts::HostSystemError);
    rig.controller->ProcessEvents();
    REQUIRE(rig.manager->HasWork());
    rig.Settle();
    REQUIRE(rig.fake.Resets().size() == 2);
    REQUIRE(rig.controller->Restarts() == 1);
    REQUIRE_FALSE(rig.controller->Disabled());
    REQUIRE(rig.fake.Addressed().size() == 2);
    X::SlotInfo info;
    REQUIRE(rig.controller->GetSlot(1, &info));
    REQUIRE(info.port == 1);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline + 3);

    rig.fake.SetStatusBits(X::UsbSts::HostSystemError);
    rig.controller->ProcessEvents();
    rig.Settle();
    REQUIRE(rig.controller->Disabled());
    REQUIRE(rig.fake.Resets().size() == 2);
    REQUIRE_FALSE(rig.manager->HasWork());
    REQUIRE(rig.Command(X::UsbCmd::RunStop) == 0);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
    rig.fake.Connect(3, Speed::High);
    rig.Settle();
    REQUIRE(rig.fake.Addressed().size() == 2);
}

TEST_CASE("xhci: a command that was pending during an error fails at once", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    rig.fake.HoldNextCommand(X::TrbType::EnableSlot);
    rig.fake.Connect(1, Speed::High);
    bool raised = false;
    rig.hardware.on_idle = [&]
    {
        if (!raised && !rig.fake.CommandTypes().empty())
        {
            raised = true;
            rig.fake.SetStatusBits(X::UsbSts::HostControllerError);
        }
        rig.controller->ProcessEvents();
    };
    rig.controller->ProcessEvents();
    rig.manager->Run();
    REQUIRE(raised);
    REQUIRE(rig.hardware.Now() < 1000);
    REQUIRE(rig.fake.AbortCount() == 0);
    REQUIRE(rig.manager->HasWork());
}

using Device = std::shared_ptr<ScriptedDevice>;
using Outcome = ScriptedDevice::Outcome;
namespace T = FeatherOS::Usb;

uint32_t Enumerate(Rig& rig, uint32_t port, Speed speed, Device device = nullptr)
{
    rig.fake.Connect(port, speed, true, std::move(device));
    rig.Settle();
    return rig.manager->SlotOfPort(port);
}

bool ConfigureFirstInterface(Rig& rig, uint32_t slot)
{
    const T::DeviceInfo* info = rig.manager->Find(slot);
    if (info == nullptr)
    {
        return false;
    }
    const T::InterfaceView& interface = info->configuration.interfaces[0];
    const std::vector<EndpointDescriptor> endpoints(interface.endpoints,
                                                    interface.endpoints + interface.endpoint_count);
    if (!rig.controller->ConfigureEndpoints(slot, endpoints, nullptr))
    {
        return false;
    }
    FeatherOS::Memory::Dma::Buffer none{};
    uint32_t actual = 0;
    return rig.controller->Control(slot, {0x00, 9, info->configuration.value, 0, 0}, none,
                                   &actual) == Status::Success;
}

struct Listener
{
    X::Controller* controller = nullptr;
    uint32_t slot = 0;
    Transfer transfer;
    FeatherOS::Memory::Dma::Buffer buffer{};
    int resubmits = 0;
    std::vector<Status> statuses;
    std::vector<std::vector<uint8_t>> reports;
};

void Listen(Transfer& transfer)
{
    auto* listener = static_cast<Listener*>(transfer.context);
    listener->statuses.push_back(transfer.status);
    if (T::Succeeded(transfer.status))
    {
        listener->reports.emplace_back(const_cast<const uint8_t*>(listener->buffer.address),
                                       const_cast<const uint8_t*>(listener->buffer.address) +
                                           transfer.actual);
    }
    if (listener->resubmits > 0)
    {
        --listener->resubmits;
        listener->controller->Submit(listener->slot, transfer);
    }
}

void Prepare(Rig& rig, Listener& listener, uint32_t slot, uint8_t endpoint, uint32_t length,
             bool callback = true)
{
    REQUIRE(rig.controller->AllocateBuffer(64, &listener.buffer));
    listener.controller = rig.controller.get();
    listener.slot = slot;
    listener.transfer.endpoint = endpoint;
    listener.transfer.buffer = &listener.buffer;
    listener.transfer.length = length;
    listener.transfer.context = &listener;
    listener.transfer.done = callback ? Listen : nullptr;
}

std::vector<uint32_t> CommandsSince(const Rig& rig, size_t from)
{
    const auto& all = rig.fake.CommandTypes();
    return std::vector<uint32_t>(all.begin() + static_cast<long>(from), all.end());
}

struct SpeedCase
{
    uint32_t port;
    Speed speed;
    uint8_t max_packet0;
    const char* product;
    bool evaluates;
};

TEST_CASE("xhci T15: descriptors are read at every speed, Evaluate Context only on a mismatch",
          "[xhci]")
{
    const SpeedCase cases[] = {{1, Speed::High, 64, "QEMU USB Keyboard", false},
                               {2, Speed::Full, 8, "QEMU USB Keyboard", false},
                               {3, Speed::Full, 64, "QEMU USB Keyboard", true},
                               {4, Speed::Low, 8, "QEMU USB Keyboard", false},
                               {5, Speed::Super, 9, "QEMU USB Keyboard", false}};
    for (const bool context64 : {false, true})
    {
        Rig rig(8, context64);
        rig.fake.SetUsb3Ports(5);
        REQUIRE(rig.Create());
        for (const SpeedCase& entry : cases)
        {
            INFO(entry.port);
            const uint32_t evaluated = static_cast<uint32_t>(rig.fake.Evaluated().size());
            const uint32_t slot =
                Enumerate(rig, entry.port, entry.speed, MakeKeyboard(entry.speed, entry.max_packet0));
            REQUIRE(slot != 0);
            const T::DeviceInfo* info = rig.manager->Find(slot);
            REQUIRE(info != nullptr);
            REQUIRE(info->location.speed == entry.speed);
            REQUIRE(info->location.root_port == entry.port);
            REQUIRE(info->device.vendor == 0x0627);
            REQUIRE(info->device.product == 0x0001);
            REQUIRE(std::string(info->product) == entry.product);
            REQUIRE(info->configuration.interface_count == 1);
            REQUIRE(info->configuration.interfaces[0].endpoints[0].address == 0x81);
            REQUIRE(rig.fake.Evaluated().size() == evaluated + (entry.evaluates ? 1 : 0));
            if (entry.evaluates)
            {
                REQUIRE(rig.fake.Evaluated().back().first == slot);
                REQUIRE(rig.fake.Evaluated().back().second == entry.max_packet0);
            }
        }
        REQUIRE(rig.fake.Faults() == 0);
        REQUIRE_FALSE(rig.manager->HasWork());
    }
}

TEST_CASE("xhci T15: a full-speed device's packet size is fixed before any longer transfer",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::Full, 64);
    size_t evaluated_at_second = ~size_t{0};
    int seen = 0;
    device->on_request = [&](const ScriptedDevice::Request& request)
    {
        if (++seen == 2)
        {
            evaluated_at_second = rig.fake.Evaluated().size();
            REQUIRE(request.length > 8);
        }
    };
    REQUIRE(Enumerate(rig, 1, Speed::Full, device) != 0);
    REQUIRE(device->Requests()[0].length == 8);
    REQUIRE(evaluated_at_second == 1);
}

TEST_CASE("xhci T15: enumeration of a port in each state frees what it took", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    // A configuration that claims more than 4 KiB is refused with the slot disabled.
    std::vector<uint8_t> oversized = {9, 2, 0x88, 0x13, 1, 1, 0, 0x80, 50};
    Device big = std::make_shared<ScriptedDevice>(
        FeatherOS::Usb::Testing::MakeDeviceDescriptor(1, 2, 64), oversized);
    REQUIRE(Enumerate(rig, 1, Speed::High, big) == 0);
    REQUIRE(rig.fake.DisabledSlots().size() == 1);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);

    // A device that stalls its device descriptor never gets a slot of its own.
    Device broken = MakeKeyboard(Speed::High);
    broken->FailControl(Outcome::Stall, 100, 6);
    REQUIRE(Enumerate(rig, 2, Speed::High, broken) == 0);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
    REQUIRE_FALSE(rig.manager->HasWork());
}

TEST_CASE("xhci: a device unplugged during enumeration fails without recovery commands",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    device->on_request = [&](const ScriptedDevice::Request& request)
    {
        if (request.value >> 8 == 2 && request.length == 9)
        {
            rig.fake.Disconnect(1);
        }
    };
    rig.fake.Connect(1, Speed::High, true, device);
    rig.Settle();
    REQUIRE(rig.manager->SlotOfPort(1) == 0);
    REQUIRE(rig.manager->Find(1) == nullptr);
    REQUIRE(rig.fake.DisabledSlots() == std::vector<uint32_t>{1});
    for (const uint32_t type : rig.fake.CommandTypes())
    {
        REQUIRE(type != X::TrbType::ResetEndpoint);
        REQUIRE(type != X::TrbType::StopEndpoint);
    }
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
    REQUIRE_FALSE(rig.manager->HasWork());
}

TEST_CASE("xhci: a control transfer reports a short data stage", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    const uint32_t slot = Enumerate(rig, 1, Speed::High);
    REQUIRE(slot != 0);
    FeatherOS::Memory::Dma::Buffer buffer{};
    REQUIRE(rig.controller->AllocateBuffer(256, &buffer));
    uint32_t actual = 0;
    REQUIRE(rig.controller->Control(slot, {0x80, 6, 0x0100, 0, 64}, buffer, &actual) ==
            Status::Success);
    REQUIRE(actual == 18);
    REQUIRE(buffer.address[0] == 18);
    REQUIRE(buffer.address[1] == 1);
    // A request the device has nothing for returns no data and still succeeds.
    REQUIRE(rig.controller->Control(slot, {0x00, 9, 1, 0, 0}, buffer, &actual) == Status::Success);
    REQUIRE(actual == 0);
    rig.controller->FreeBuffer(buffer);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("xhci T15: interrupt endpoints are configured and deliver reports at three speeds",
          "[xhci]")
{
    struct Entry
    {
        Speed speed;
        uint32_t port;
        uint8_t interval_exponent;
    };
    // bInterval 10: 2^9 microframes at high speed, floor(log2(80)) = 6 at full and low speed.
    for (const Entry entry : {Entry{Speed::High, 1, 9}, Entry{Speed::Full, 2, 6},
                              Entry{Speed::Low, 3, 6}})
    {
        INFO(static_cast<int>(entry.speed));
        Rig rig;
        REQUIRE(rig.Create());
        Device device = MakeKeyboard(entry.speed);
        const uint32_t slot = Enumerate(rig, entry.port, entry.speed, device);
        REQUIRE(slot != 0);
        REQUIRE(ConfigureFirstInterface(rig, slot));
        REQUIRE(device->configuration() == 1);
        REQUIRE(rig.fake.Configured().size() == 1);
        const auto& configured = rig.fake.Configured()[0];
        REQUIRE(configured.slot == slot);
        REQUIRE(configured.dci == 3);
        REQUIRE(configured.type == 7);
        REQUIRE(configured.max_packet == 8);
        REQUIRE(configured.interval == entry.interval_exponent);
        REQUIRE(configured.entries == 3);
        REQUIRE((configured.ring & 1) == 1);

        Listener listener;
        Prepare(rig, listener, slot, 0x81, 8);
        listener.resubmits = 1;
        REQUIRE(rig.controller->Submit(slot, listener.transfer));
        rig.fake.DeliverReport(device, 0x81, {0, 0, 4, 0, 0, 0, 0, 0});
        rig.controller->ProcessEvents();
        REQUIRE(listener.reports.size() == 1);
        REQUIRE(listener.reports[0] == std::vector<uint8_t>{0, 0, 4, 0, 0, 0, 0, 0});
        // The callback queued the transfer again, and the next report reaches it.
        rig.fake.DeliverReport(device, 0x81, {2, 0, 5, 0, 0, 0, 0, 0});
        rig.controller->ProcessEvents();
        REQUIRE(listener.reports.size() == 2);
        REQUIRE(listener.reports[1][0] == 2);
        REQUIRE(listener.statuses == std::vector<Status>{Status::Success, Status::Success});
        REQUIRE(rig.fake.Faults() == 0);
        rig.controller->FreeBuffer(listener.buffer);
    }
}

TEST_CASE("xhci T15: a mouse report shorter than the buffer is a short packet", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeMouse(Speed::Low);
    const uint32_t slot = Enumerate(rig, 1, Speed::Low, device);
    REQUIRE(slot != 0);
    REQUIRE(std::string(rig.manager->Find(slot)->product) == "QEMU USB Mouse");
    REQUIRE(ConfigureFirstInterface(rig, slot));
    Listener listener;
    Prepare(rig, listener, slot, 0x81, 8);
    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    rig.fake.DeliverReport(device, 0x81, {1, 2, 3, 0});
    rig.controller->ProcessEvents();
    REQUIRE(listener.statuses == std::vector<Status>{Status::ShortPacket});
    REQUIRE(listener.reports.size() == 1);
    REQUIRE(listener.reports[0] == std::vector<uint8_t>{1, 2, 3, 0});
    rig.controller->FreeBuffer(listener.buffer);
}

TEST_CASE("xhci T15: six hundred reports go through the ring across several laps", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(ConfigureFirstInterface(rig, slot));
    Listener listener;
    Prepare(rig, listener, slot, 0x81, 8);
    listener.resubmits = 1000;
    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    for (uint32_t index = 0; index < 600; ++index)
    {
        rig.fake.DeliverReport(device, 0x81,
                               {static_cast<uint8_t>(index), static_cast<uint8_t>(index >> 8), 0, 0,
                                0, 0, 0, 0});
        rig.controller->ProcessEvents();
    }
    REQUIRE(listener.reports.size() == 600);
    for (uint32_t index = 0; index < 600; ++index)
    {
        REQUIRE(listener.reports[index][0] == static_cast<uint8_t>(index));
        REQUIRE(listener.reports[index][1] == static_cast<uint8_t>(index >> 8));
    }
    REQUIRE(rig.fake.Faults() == 0);
    REQUIRE(rig.fake.DroppedEvents() == 0);
    rig.controller->FreeBuffer(listener.buffer);
}

TEST_CASE("xhci T16: a stall on EP0 resets the endpoint, fails the transfer and the next works",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(slot != 0);
    FeatherOS::Memory::Dma::Buffer buffer{};
    REQUIRE(rig.controller->AllocateBuffer(64, &buffer));
    uint32_t actual = 0;
    device->FailControl(Outcome::Stall, 1);
    const size_t before = rig.fake.CommandTypes().size();
    const size_t sets = rig.fake.DequeueSets().size();
    REQUIRE(rig.controller->Control(slot, {0x80, 6, 0x0100, 0, 18}, buffer, &actual) ==
            Status::Stall);
    REQUIRE(CommandsSince(rig, before) ==
            std::vector<uint32_t>{X::TrbType::ResetEndpoint, X::TrbType::SetTrDequeue});
    REQUIRE(rig.fake.DequeueSets().size() == sets + 1);
    // EP0 has no halt feature to clear.
    REQUIRE(device->Count(0x02, 1) == 0);
    FakeXhci::State state;
    // Stopped until the next transfer rings the doorbell.
    REQUIRE(rig.fake.EndpointState(slot, 1, &state));
    REQUIRE(state == FakeXhci::State::Stopped);
    REQUIRE(rig.controller->Control(slot, {0x80, 6, 0x0100, 0, 18}, buffer, &actual) ==
            Status::Success);
    REQUIRE(actual == 18);
    rig.controller->FreeBuffer(buffer);
}

TEST_CASE("xhci T16: a stall on an interrupt endpoint is recovered before it is used again",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(ConfigureFirstInterface(rig, slot));
    Listener listener;
    Prepare(rig, listener, slot, 0x81, 8);
    size_t commands_at_clear = 0;
    device->on_request = [&](const ScriptedDevice::Request& request)
    {
        if (request.request == 1)
        {
            commands_at_clear = rig.fake.CommandTypes().size();
        }
    };
    device->FailEndpoint(0x81, Outcome::Stall, 1);
    const size_t before = rig.fake.CommandTypes().size();
    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    rig.controller->ProcessEvents();
    // The error is the manager's to handle; the transfer is still pending.
    REQUIRE(listener.statuses.empty());
    REQUIRE(rig.manager->HasWork());
    FakeXhci::State state;
    REQUIRE(rig.fake.EndpointState(slot, 3, &state));
    REQUIRE(state == FakeXhci::State::Halted);
    // A halted endpoint takes no new transfer until it is reset.
    Transfer other;
    other.endpoint = 0x81;
    other.buffer = &listener.buffer;
    other.length = 8;
    REQUIRE_FALSE(rig.controller->Submit(slot, other));
    REQUIRE(other.status == Status::Stall);

    rig.Settle();
    REQUIRE(listener.statuses == std::vector<Status>{Status::Stall});
    REQUIRE(CommandsSince(rig, before) ==
            std::vector<uint32_t>{X::TrbType::ResetEndpoint, X::TrbType::SetTrDequeue});
    REQUIRE(commands_at_clear == before + 2);
    REQUIRE(device->Count(0x02, 1) == 1);
    const auto& clear = device->Requests().back();
    REQUIRE(clear.index == 0x81);
    REQUIRE(clear.value == 0);
    REQUIRE(rig.fake.DequeueSets().back().pointer ==
            ((rig.fake.Configured()[0].ring & ~uint64_t{0xF}) + X::TrbSize + 1));
    REQUIRE(rig.fake.EndpointState(slot, 3, &state));
    REQUIRE(state == FakeXhci::State::Stopped);

    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    REQUIRE(rig.fake.EndpointState(slot, 3, &state));
    REQUIRE(state == FakeXhci::State::Running);
    rig.fake.DeliverReport(device, 0x81, {0, 0, 9, 0, 0, 0, 0, 0});
    rig.controller->ProcessEvents();
    REQUIRE(listener.statuses == std::vector<Status>{Status::Stall, Status::Success});
    REQUIRE(listener.reports.back()[2] == 9);
    rig.controller->FreeBuffer(listener.buffer);
}

TEST_CASE("xhci T16: a transfer queued behind a stalled one still runs after recovery", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(ConfigureFirstInterface(rig, slot));
    Listener first;
    Listener second;
    Prepare(rig, first, slot, 0x81, 8);
    Prepare(rig, second, slot, 0x81, 8);
    device->FailEndpoint(0x81, Outcome::Stall, 1);
    REQUIRE(rig.controller->Submit(slot, first.transfer));
    REQUIRE(rig.controller->Submit(slot, second.transfer));
    rig.controller->ProcessEvents();
    rig.Settle();
    REQUIRE(first.statuses == std::vector<Status>{Status::Stall});
    REQUIRE(second.statuses.empty());
    rig.fake.DeliverReport(device, 0x81, {0, 0, 7, 0, 0, 0, 0, 0});
    rig.controller->ProcessEvents();
    REQUIRE(second.statuses == std::vector<Status>{Status::Success});
    REQUIRE(second.reports[0][2] == 7);
    rig.controller->FreeBuffer(first.buffer);
    rig.controller->FreeBuffer(second.buffer);
}

TEST_CASE("xhci T16: transaction errors are retried three times, then handled as a stall",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(ConfigureFirstInterface(rig, slot));
    Listener listener;
    Prepare(rig, listener, slot, 0x81, 8);

    // Two errors, then the report arrives: no stall handling, no halt cleared.
    device->FailEndpoint(0x81, Outcome::TransactionError, 2);
    size_t before = rig.fake.CommandTypes().size();
    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    rig.controller->ProcessEvents();
    rig.Settle();
    REQUIRE(listener.statuses.empty());
    REQUIRE(CommandsSince(rig, before) ==
            (std::vector<uint32_t>{X::TrbType::ResetEndpoint, X::TrbType::SetTrDequeue,
                                   X::TrbType::ResetEndpoint, X::TrbType::SetTrDequeue}));
    rig.fake.DeliverReport(device, 0x81, {0, 0, 6, 0, 0, 0, 0, 0});
    rig.controller->ProcessEvents();
    REQUIRE(listener.statuses == std::vector<Status>{Status::Success});
    REQUIRE(device->Count(0x02, 1) == 0);

    // Errors without end: the original try and three retries, then the stall sequence.
    device->FailEndpoint(0x81, Outcome::TransactionError, 100);
    before = rig.fake.CommandTypes().size();
    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    rig.controller->ProcessEvents();
    for (int round = 0; round < 8 && listener.statuses.size() < 2; ++round)
    {
        rig.Settle();
    }
    REQUIRE(listener.statuses.back() == Status::TransactionError);
    std::vector<uint32_t> expected;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        expected.push_back(X::TrbType::ResetEndpoint);
        expected.push_back(X::TrbType::SetTrDequeue);
    }
    REQUIRE(CommandsSince(rig, before) == expected);
    REQUIRE(device->Count(0x02, 1) == 1);
    rig.controller->FreeBuffer(listener.buffer);
}

TEST_CASE("xhci T16: control transfers retry transaction errors and stop at babble", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    FeatherOS::Memory::Dma::Buffer buffer{};
    REQUIRE(rig.controller->AllocateBuffer(64, &buffer));
    uint32_t actual = 0;

    device->FailControl(Outcome::TransactionError, 3);
    REQUIRE(rig.controller->Control(slot, {0x80, 6, 0x0100, 0, 18}, buffer, &actual) ==
            Status::Success);
    REQUIRE(actual == 18);

    device->FailControl(Outcome::TransactionError, 4);
    const size_t before = rig.fake.CommandTypes().size();
    REQUIRE(rig.controller->Control(slot, {0x80, 6, 0x0100, 0, 18}, buffer, &actual) ==
            Status::TransactionError);
    REQUIRE(CommandsSince(rig, before).size() == 8);

    device->FailControl(Outcome::Babble, 1);
    REQUIRE(rig.controller->Control(slot, {0x80, 6, 0x0100, 0, 18}, buffer, &actual) ==
            Status::Babble);
    REQUIRE(rig.controller->Control(slot, {0x80, 6, 0x0100, 0, 18}, buffer, &actual) ==
            Status::Success);
    rig.controller->FreeBuffer(buffer);
}

TEST_CASE("xhci T16: recoveries are counted for the driver that unbinds after three in a second",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(ConfigureFirstInterface(rig, slot));
    std::vector<uint32_t> seen;
    rig.controller->SetRecoveryHandler(
        [](void* context, uint32_t, uint8_t endpoint, uint32_t recent)
        {
            REQUIRE(endpoint == 0x81);
            static_cast<std::vector<uint32_t>*>(context)->push_back(recent);
        },
        &seen);
    Listener listener;
    Prepare(rig, listener, slot, 0x81, 8);
    device->FailEndpoint(0x81, Outcome::Stall, 3);
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        REQUIRE(rig.controller->Submit(slot, listener.transfer));
        rig.controller->ProcessEvents();
        rig.Settle();
    }
    REQUIRE(seen == (std::vector<uint32_t>{1, 2, 3}));
    REQUIRE(rig.controller->RecentRecoveries(slot, 0x81) == 3);
    rig.hardware.Pause(1500);
    REQUIRE(rig.controller->RecentRecoveries(slot, 0x81) == 0);
    rig.controller->FreeBuffer(listener.buffer);
}

TEST_CASE("xhci T16: a control transfer that never completes times out and the endpoint recovers",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    FeatherOS::Memory::Dma::Buffer buffer{};
    REQUIRE(rig.controller->AllocateBuffer(64, &buffer));
    uint32_t actual = 0;
    device->FailControl(Outcome::Hang, 1);
    const uint64_t start = rig.hardware.Now();
    const size_t before = rig.fake.CommandTypes().size();
    REQUIRE(rig.controller->Control(slot, {0x80, 6, 0x0100, 0, 18}, buffer, &actual) ==
            Status::Timeout);
    REQUIRE(rig.hardware.Now() - start >= 5000);
    REQUIRE(rig.hardware.Now() - start < 6000);
    REQUIRE(CommandsSince(rig, before) ==
            std::vector<uint32_t>{X::TrbType::StopEndpoint, X::TrbType::SetTrDequeue});
    REQUIRE(rig.controller->Control(slot, {0x80, 6, 0x0100, 0, 18}, buffer, &actual) ==
            Status::Success);
    REQUIRE(actual == 18);
    rig.controller->FreeBuffer(buffer);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("xhci: stopping a device's endpoints cancels what is pending and refuses new work",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(ConfigureFirstInterface(rig, slot));
    Listener listener;
    Prepare(rig, listener, slot, 0x81, 8);
    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    rig.controller->ProcessEvents();
    REQUIRE(listener.statuses.empty());
    const size_t before = rig.fake.CommandTypes().size();
    rig.controller->StopEndpoints(slot);
    rig.controller->ProcessEvents();
    REQUIRE(listener.statuses == std::vector<Status>{Status::Cancelled});
    const auto commands = CommandsSince(rig, before);
    REQUIRE(std::count(commands.begin(), commands.end(), X::TrbType::StopEndpoint) == 2);
    REQUIRE_FALSE(rig.controller->Submit(slot, listener.transfer));
    REQUIRE(listener.transfer.status == Status::Cancelled);
    // A late report reaches nobody.
    rig.fake.DeliverReport(device, 0x81, {1, 1, 1, 1, 1, 1, 1, 1});
    rig.controller->ProcessEvents();
    REQUIRE(listener.statuses.size() == 1);
    rig.controller->FreeBuffer(listener.buffer);
}

TEST_CASE("xhci: destroying a configured device frees every ring and cancels its transfers",
          "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(ConfigureFirstInterface(rig, slot));
    REQUIRE(rig.hardware.Outstanding() == rig.baseline + 4);
    Listener listener;
    Prepare(rig, listener, slot, 0x81, 8);
    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    const uint32_t outstanding = rig.hardware.Outstanding();
    rig.fake.Disconnect(1);
    rig.Settle();
    REQUIRE(listener.statuses == std::vector<Status>{Status::NoDevice});
    REQUIRE(rig.hardware.Outstanding() == outstanding - 4);
    REQUIRE_FALSE(rig.controller->Submit(slot, listener.transfer));
    REQUIRE(listener.transfer.status == Status::NoDevice);
    rig.controller->FreeBuffer(listener.buffer);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("xhci: configuring endpoints refuses duplicates and leaks nothing on failure", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    const uint32_t slot = Enumerate(rig, 1, Speed::High);
    REQUIRE(slot != 0);
    const EndpointDescriptor interrupt = {0x81, T::EndpointType::Interrupt, 8, 10, 0, 0, 0};
    const EndpointDescriptor duplicate[] = {interrupt, interrupt};
    REQUIRE_FALSE(rig.controller->ConfigureEndpoints(slot, duplicate, nullptr));
    const EndpointDescriptor control = {0, T::EndpointType::Control, 8, 0, 0, 0, 0};
    REQUIRE_FALSE(rig.controller->ConfigureEndpoints(slot, {&control, 1}, nullptr));
    REQUIRE(rig.hardware.Outstanding() == rig.baseline + 3);
    rig.hardware.allocations_before_failure = 0;
    REQUIRE_FALSE(rig.controller->ConfigureEndpoints(slot, {&interrupt, 1}, nullptr));
    rig.hardware.allocations_before_failure = ~0U;
    REQUIRE(rig.hardware.Outstanding() == rig.baseline + 3);
    REQUIRE(rig.controller->ConfigureEndpoints(slot, {&interrupt, 1}, nullptr));
    REQUIRE_FALSE(rig.controller->ConfigureEndpoints(slot, {&interrupt, 1}, nullptr));
    REQUIRE(rig.hardware.Outstanding() == rig.baseline + 4);
}

TEST_CASE("xhci T9: transfer events that name no pending transfer are ignored", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(ConfigureFirstInterface(rig, slot));
    Listener listener;
    Prepare(rig, listener, slot, 0x81, 8);
    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    const uint64_t ring = rig.fake.Configured()[0].ring & ~uint64_t{0xF};
    for (const uint64_t pointer : {uint64_t{0x1234560}, ring + 5 * X::TrbSize, ring - X::TrbSize})
    {
        for (const uint32_t code : {X::CompletionCode::Success, X::CompletionCode::StallError})
        {
            rig.fake.PostInjectedEvent(X::MakeTrb(
                X::TrbType::TransferEvent, (3U << X::TrbControl::EndpointShift) | (slot << X::TrbControl::SlotShift),
                pointer, code << X::CompletionShift));
        }
    }
    // The right pointer on the wrong endpoint or slot is no match either.
    rig.fake.PostInjectedEvent(X::MakeTrb(
        X::TrbType::TransferEvent, (5U << X::TrbControl::EndpointShift) | (slot << X::TrbControl::SlotShift),
        ring, X::CompletionCode::Success << X::CompletionShift));
    rig.fake.PostInjectedEvent(X::MakeTrb(
        X::TrbType::TransferEvent, (3U << X::TrbControl::EndpointShift) | (9U << X::TrbControl::SlotShift),
        ring, X::CompletionCode::Success << X::CompletionShift));
    rig.controller->ProcessEvents();
    REQUIRE(listener.statuses.empty());
    REQUIRE_FALSE(rig.manager->HasWork());
    rig.fake.DeliverReport(device, 0x81, {0, 0, 3, 0, 0, 0, 0, 0});
    rig.controller->ProcessEvents();
    REQUIRE(listener.statuses == std::vector<Status>{Status::Success});
    rig.controller->FreeBuffer(listener.buffer);
}

TEST_CASE("xhci: a host controller error fails a transfer that waits on it", "[xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device device = MakeKeyboard(Speed::High);
    const uint32_t slot = Enumerate(rig, 1, Speed::High, device);
    REQUIRE(ConfigureFirstInterface(rig, slot));
    Listener listener;
    Prepare(rig, listener, slot, 0x81, 8);
    REQUIRE(rig.controller->Submit(slot, listener.transfer));
    rig.fake.SetStatusBits(X::UsbSts::HostSystemError);
    rig.controller->ProcessEvents();
    REQUIRE(rig.manager->HasWork());
    rig.Settle();
    REQUIRE(listener.statuses == std::vector<Status>{Status::NoDevice});
    REQUIRE(rig.controller->Restarts() == 1);
    // The device is enumerated again after the restart.
    REQUIRE(rig.manager->SlotOfPort(1) != 0);
}

} // namespace
