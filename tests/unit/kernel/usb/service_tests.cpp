#include <catch2/catch_test_macros.hpp>

#include "fake_device.h"
#include "rig.h"

#include "kernel/log_ring.h"
#include "kernel/usb/device.h"
#include "kernel/usb/driver.h"
#include "kernel/usb/hub.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{

namespace T = FeatherOS::Usb;
using FeatherOS::Usb::Speed;
using FeatherOS::Usb::Status;
using FeatherOS::Usb::Testing::MakeHub;
using FeatherOS::Usb::Testing::MakeKeyboard;
using FeatherOS::Usb::Testing::MakeMassStorage;
using FeatherOS::Usb::Testing::Rig;
using FeatherOS::Usb::Testing::ScriptedDevice;

using Scripted = std::shared_ptr<ScriptedDevice>;
using Outcome = ScriptedDevice::Outcome;

struct State
{
    T::DeviceTransfer transfer;
    FeatherOS::Memory::Dma::Buffer buffer{};
    std::string path;
};

struct Record
{
    int reports = 0;
    int detaches = 0;
    std::vector<std::string> serviced;
    std::vector<void*> states;
    bool service_result = false;

    void Reset()
    {
        *this = Record();
    }
};

Record bulk;
Record keys;

void OnReport(T::DeviceTransfer& transfer)
{
    Record& record = transfer.length == 512 ? bulk : keys;
    ++record.reports;
    transfer.device->Submit(transfer);
}

Status AttachWith(T::Device& device, const T::InterfaceView& interface, void** state,
                  uint32_t length)
{
    auto* entry = new State;
    entry->path = device.path;
    if (!device.Host().AllocateBuffer(512, &entry->buffer))
    {
        delete entry;
        return Status::ControllerError;
    }
    entry->transfer.endpoint = interface.endpoints[0].address;
    entry->transfer.buffer = &entry->buffer;
    entry->transfer.length = length;
    entry->transfer.callback = OnReport;
    entry->transfer.driver_context = entry;
    if (!device.Submit(entry->transfer))
    {
        device.Host().FreeBuffer(entry->buffer);
        delete entry;
        return Status::NoDevice;
    }
    *state = entry;
    return Status::Success;
}

void Detach(T::Device& device, void* state)
{
    auto* entry = static_cast<State*>(state);
    (entry->transfer.length == 512 ? bulk : keys).detaches++;
    device.Host().FreeBuffer(entry->buffer);
    delete entry;
}

bool Serviced(Record& record, T::Device& device, void* state)
{
    record.serviced.emplace_back(device.path);
    record.states.push_back(state);
    return record.service_result;
}

const T::Driver BulkDriver = {
    "bulk-test",
    [](const T::InterfaceView& interface) { return interface.class_code == 8; },
    [](T::Device& device, const T::InterfaceView& interface, void** state)
    { return AttachWith(device, interface, state, 512); },
    Detach,
    [](T::Device& device, void* state) { return Serviced(bulk, device, state); },
    true};

const T::Driver KeyDriver = {
    "key-test",
    [](const T::InterfaceView& interface) { return interface.class_code == 3; },
    [](T::Device& device, const T::InterfaceView& interface, void** state)
    { return AttachWith(device, interface, state, 8); },
    Detach,
    [](T::Device& device, void* state) { return Serviced(keys, device, state); },
    false};

const T::Driver* const Drivers[] = {&T::HubDriver, &BulkDriver, &KeyDriver};

class ServiceRig
{
public:
    ServiceRig()
    {
        bulk.Reset();
        keys.Reset();
        rig.drivers = Drivers;
        REQUIRE(rig.Create());
    }

    uint32_t Plug(uint32_t port, const Scripted& device)
    {
        rig.fake.Connect(port, Speed::High, true, device);
        rig.Settle();
        return rig.manager->SlotOfPort(port);
    }

    T::Device* Device(uint32_t slot)
    {
        return const_cast<T::Device*>(rig.manager->Find(slot));
    }

    Rig rig;
};

size_t LogLines(const std::string& needle)
{
    static char text[FeatherOS::KernelLog::Capacity];
    const size_t length = FeatherOS::KernelLog::Snapshot(text, sizeof(text));
    const std::string all(text, length);
    size_t count = 0;
    for (size_t at = all.find(needle); at != std::string::npos; at = all.find(needle, at + 1))
    {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("usb T23: ten stalls on a protocol driver's bulk endpoint do not unbind it",
          "[usb][xhci][service]")
{
    ServiceRig world;
    const Scripted storage = MakeMassStorage(Speed::High);
    storage->FailEndpoint(0x81, Outcome::Stall, 10);
    const uint32_t slot = world.Plug(1, storage);
    REQUIRE(slot != 0);
    REQUIRE(bulk.reports == 10);
    REQUIRE(bulk.detaches == 0);
    REQUIRE(T::DriverOf(*world.Device(slot), 0) == &BulkDriver);
    world.rig.fake.Disconnect(1);
    world.rig.Settle();
    REQUIRE(bulk.detaches == 1);
    REQUIRE(world.rig.hardware.Outstanding() == world.rig.baseline);
}

TEST_CASE("usb T23: a HID driver unbinds after three stalls in a second", "[usb][xhci][service]")
{
    ServiceRig world;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    keyboard->FailEndpoint(0x81, Outcome::Stall, 3);
    const uint32_t slot = world.Plug(1, keyboard);
    REQUIRE(slot != 0);
    REQUIRE(keys.detaches == 1);
    REQUIRE(T::DriverOf(*world.Device(slot), 0) == nullptr);
}

TEST_CASE("usb T23: other errors on a protocol driver's endpoint still count",
          "[usb][xhci][service]")
{
    ServiceRig world;
    const Scripted storage = MakeMassStorage(Speed::High);
    storage->FailEndpoint(0x81, Outcome::TransactionError, 3);
    const uint32_t slot = world.Plug(1, storage);
    REQUIRE(slot != 0);
    REQUIRE(bulk.detaches == 1);
    REQUIRE(T::DriverOf(*world.Device(slot), 0) == nullptr);
}

TEST_CASE("usb T23: stalls on one device's protocol endpoint leave a neighbour's count alone",
          "[usb][xhci][service]")
{
    ServiceRig world;
    const Scripted storage = MakeMassStorage(Speed::High);
    const Scripted keyboard = MakeKeyboard(Speed::High);
    storage->FailEndpoint(0x81, Outcome::Stall, 5);
    keyboard->FailEndpoint(0x81, Outcome::Stall, 3);
    const uint32_t first = world.Plug(1, storage);
    const uint32_t second = world.Plug(2, keyboard);
    REQUIRE(T::DriverOf(*world.Device(first), 0) == &BulkDriver);
    REQUIRE(second != 0);
    REQUIRE(keys.detaches == 1);
    REQUIRE(bulk.detaches == 0);
}

TEST_CASE("usb: a driver's service runs on the manager after PostWork", "[usb][xhci][service]")
{
    ServiceRig world;
    const uint32_t slot = world.Plug(1, MakeMassStorage(Speed::High));
    REQUIRE(slot != 0);
    T::Device* device = world.Device(slot);
    REQUIRE(bulk.serviced.empty());
    device->PostWork();
    REQUIRE(bulk.serviced.empty());
    world.rig.Settle();
    REQUIRE(bulk.serviced == std::vector<std::string>{"1-1"});
    REQUIRE(bulk.states[0] == device->driver_state[0]);
    REQUIRE(keys.serviced.empty());
    world.rig.Settle();
    REQUIRE(bulk.serviced.size() == 1);
}

TEST_CASE("usb: a service request for a port reset is logged and does not detach the device",
          "[usb][xhci][service]")
{
    ServiceRig world;
    const uint32_t slot = world.Plug(1, MakeMassStorage(Speed::High));
    REQUIRE(slot != 0);
    bulk.service_result = true;
    const size_t before = LogLines("USB 1-1: port reset requested");
    world.Device(slot)->PostWork();
    world.rig.Settle();
    REQUIRE(bulk.serviced.size() == 1);
    REQUIRE(LogLines("USB 1-1: port reset requested") == before + 1);
    REQUIRE(world.rig.manager->Find(slot) != nullptr);
    REQUIRE(T::DriverOf(*world.Device(slot), 0) == &BulkDriver);
    REQUIRE(bulk.detaches == 0);
}

TEST_CASE("usb: work posted for a device that was unplugged is not serviced", "[usb][xhci][service]")
{
    ServiceRig world;
    const uint32_t slot = world.Plug(1, MakeMassStorage(Speed::High));
    REQUIRE(slot != 0);
    world.Device(slot)->PostWork();
    world.rig.fake.Disconnect(1);
    // The same device number goes to the next device, which must not see the old request.
    const Scripted replacement = MakeMassStorage(Speed::High);
    world.rig.fake.Connect(2, Speed::High, true, replacement);
    world.rig.Settle();
    REQUIRE(world.rig.manager->SlotOfPort(2) != 0);
    REQUIRE(bulk.detaches == 1);
    REQUIRE(bulk.serviced.empty());
}

TEST_CASE("usb: work for a device whose drivers were unbound is not serviced",
          "[usb][xhci][service]")
{
    ServiceRig world;
    const Scripted storage = MakeMassStorage(Speed::High);
    storage->FailEndpoint(0x81, Outcome::TransactionError, 3);
    const uint32_t slot = world.Plug(1, storage);
    REQUIRE(slot != 0);
    REQUIRE(T::DriverOf(*world.Device(slot), 0) == nullptr);
    world.Device(slot)->PostWork();
    world.rig.Settle();
    REQUIRE(bulk.serviced.empty());
}

TEST_CASE("usb: a hub's work still goes to the hub, not to a driver's service",
          "[usb][xhci][service][hub]")
{
    ServiceRig world;
    const auto hub = MakeHub(Speed::High, 4);
    const Scripted storage = MakeMassStorage(Speed::High);
    hub->Plug(2, storage, Speed::High);
    const uint32_t slot = world.Plug(1, hub);
    REQUIRE(slot != 0);
    REQUIRE(storage->configuration() == 1);
    REQUIRE(bulk.serviced.empty());
    const Scripted late = MakeKeyboard(Speed::High);
    hub->Plug(3, late, Speed::High);
    world.rig.fake.Wake(hub);
    world.rig.Settle();
    REQUIRE(late->configuration() == 1);
    REQUIRE(bulk.serviced.empty());
    REQUIRE(keys.serviced.empty());
}
