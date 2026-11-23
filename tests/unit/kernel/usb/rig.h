#ifndef FEATHEROS_TESTS_USB_RIG_H
#define FEATHEROS_TESTS_USB_RIG_H

#include "fake_hardware.h"
#include "fake_xhci.h"

#include "kernel/usb/driver.h"
#include "kernel/usb/manager.h"
#include "kernel/usb/xhci/controller.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace FeatherOS::Usb::Testing
{

inline Pci::Function QemuFunction()
{
    return {{0, 4, 0}, 0x1b36, 0x000d, 0x0c, 0x03, 0x30, 0, 1};
}

// A controller over the fake hardware with its manager; Settle runs the manager and the event
// task by hand, as the two kernel tasks would.
class Rig
{
public:
    explicit Rig(uint32_t ports = 8, bool context64 = false, uint64_t base = 0x1'2000'0000ULL,
                 const Pci::Function& pci = QemuFunction())
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
        controller = std::make_unique<Xhci::Controller>(hardware, function);
        const bool created = controller->Initialize(allow_interrupts);
        HostController& host = wrap_host ? wrap_host(*controller) : *controller;
        manager = std::make_unique<Manager>(host, 1, drivers);
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
    Pci::Function function;
    std::unique_ptr<Xhci::Controller> controller;
    std::unique_ptr<Manager> manager;
    std::span<const Driver* const> drivers;
    // Lets a test put its own controller between the manager and the real one.
    std::function<HostController&(HostController&)> wrap_host;
    uint32_t baseline = 0;
};

} // namespace FeatherOS::Usb::Testing

#endif
