#include <catch2/catch_test_macros.hpp>

#include "fake_hardware.h"
#include "fake_xhci.h"

#include "kernel/usb/xhci/context.h"
#include "kernel/usb/xhci/controller.h"
#include "kernel/usb/xhci/registers.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>

namespace
{

namespace X = FeatherOS::Usb::Xhci;
using FeatherOS::Pci::Function;
using FeatherOS::Usb::Speed;
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
        baseline = hardware.Outstanding();
        return created;
    }

    void Settle()
    {
        controller->ProcessEvents();
        for (int round = 0; round < 64 && controller->HasManagerWork(); ++round)
        {
            controller->RunManagerWork();
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
    REQUIRE_FALSE(rig.controller->HasManagerWork());
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
    REQUIRE_FALSE(rig.controller->HasManagerWork());
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
    REQUIRE(rig.controller->HasManagerWork());
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
    REQUIRE_FALSE(rig.controller->HasManagerWork());
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
    rig.controller->RunManagerWork();
    REQUIRE(raised);
    REQUIRE(rig.hardware.Now() < 1000);
    REQUIRE(rig.fake.AbortCount() == 0);
    REQUIRE(rig.controller->HasManagerWork());
}

} // namespace
