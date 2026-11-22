#include <catch2/catch_test_macros.hpp>

#include "fake_device.h"
#include "fake_hardware.h"
#include "fake_xhci.h"
#include "rig.h"

#include "kernel/filesystem/procfs.h"
#include "kernel/sync/mutex.h"
#include "kernel/usb/device.h"
#include "kernel/usb/driver.h"
#include "kernel/usb/xhci/trb.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace T = FeatherOS::Usb;
namespace X = FeatherOS::Usb::Xhci;
using FeatherOS::Usb::Speed;
using FeatherOS::Usb::Status;
using FeatherOS::Usb::Testing::MakeKeyboard;
using FeatherOS::Usb::Testing::MakeMouse;
using FeatherOS::Usb::Testing::Rig;
using FeatherOS::Usb::Testing::ScriptedDevice;

using Scripted = std::shared_ptr<ScriptedDevice>;

struct Event
{
    std::string kind;
    std::string path;
    Status status = Status::Success;
    uint32_t in_flight = 0;
    size_t stops = 0;
    size_t disabled = 0;
};

// The functions of a Driver are plain pointers, so the fake driver's bookkeeping is global.
struct Recording
{
    Rig* rig = nullptr;
    std::vector<Event> events;
    bool resubmit_on_error = false;
    bool refuse_attach = false;

    void Reset(Rig* new_rig)
    {
        rig = new_rig;
        events.clear();
        resubmit_on_error = false;
        refuse_attach = false;
    }

    size_t Count(const std::string& kind, const std::string& path = "") const
    {
        return static_cast<size_t>(std::count_if(events.begin(), events.end(),
                                                 [&](const Event& event)
                                                 {
                                                     return event.kind == kind &&
                                                            (path.empty() || event.path == path);
                                                 }));
    }
};

Recording recording;

struct State
{
    T::DeviceTransfer transfer;
    FeatherOS::Memory::Dma::Buffer buffer{};
    std::string path;
};

size_t StopCommands()
{
    const auto& all = recording.rig->fake.CommandTypes();
    return static_cast<size_t>(std::count(all.begin(), all.end(), X::TrbType::StopEndpoint));
}

void OnReport(T::DeviceTransfer& transfer)
{
    auto* state = static_cast<State*>(transfer.driver_context);
    Event event;
    event.kind = "report";
    event.path = state->path;
    event.status = transfer.status;
    event.in_flight = transfer.device->in_flight.load();
    recording.events.push_back(event);
    if (T::Succeeded(transfer.status) || recording.resubmit_on_error)
    {
        transfer.device->Submit(transfer);
    }
}

bool Matches(const T::InterfaceView& interface)
{
    return interface.class_code == 3;
}

Status Attach(T::Device& device, const T::InterfaceView& interface, void** state)
{
    if (recording.refuse_attach)
    {
        return Status::NoDevice;
    }
    auto* entry = new State;
    entry->path = device.path;
    if (!device.Host().AllocateBuffer(64, &entry->buffer))
    {
        delete entry;
        return Status::ControllerError;
    }
    entry->transfer.endpoint = interface.endpoints[0].address;
    entry->transfer.buffer = &entry->buffer;
    entry->transfer.length = 8;
    entry->transfer.callback = OnReport;
    entry->transfer.driver_context = entry;
    if (!device.Submit(entry->transfer))
    {
        device.Host().FreeBuffer(entry->buffer);
        delete entry;
        return Status::NoDevice;
    }
    *state = entry;
    Event event;
    event.kind = "attach";
    event.path = device.path;
    recording.events.push_back(event);
    return Status::Success;
}

void Detach(T::Device& device, void* state)
{
    auto* entry = static_cast<State*>(state);
    Event event;
    event.kind = "detach";
    event.path = entry->path;
    event.in_flight = device.in_flight.load();
    event.stops = StopCommands();
    event.disabled = recording.rig->fake.DisabledSlots().size();
    recording.events.push_back(event);
    device.Host().FreeBuffer(entry->buffer);
    delete entry;
}

const T::Driver ProbeDriver = {"hid-test", Matches, Attach, Detach};
const T::Driver* const ProbeDrivers[] = {&ProbeDriver};

class DriverRig
{
public:
    DriverRig()
    {
        recording.Reset(&rig);
        rig.drivers = ProbeDrivers;
        REQUIRE(rig.Create());
    }
    ~DriverRig()
    {
        recording.Reset(nullptr);
    }

    uint32_t Plug(uint32_t port, Scripted device, Speed speed = Speed::High)
    {
        rig.fake.Connect(port, speed, true, std::move(device));
        rig.Settle();
        return rig.manager->SlotOfPort(port);
    }

    T::Device* Device(uint32_t slot)
    {
        return const_cast<T::Device*>(rig.manager->Find(slot));
    }

    Rig rig;
};

std::string Listing()
{
    char buffer[1024];
    const size_t length = T::FormatDevices(buffer, sizeof(buffer));
    return std::string(buffer, length);
}

TEST_CASE("usb T28: root-port devices bind, detach in the design's order and give their slots back",
          "[usb][xhci]")
{
    DriverRig world;
    Rig& rig = world.rig;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    const Scripted mouse = MakeMouse(Speed::High);
    const uint32_t first = world.Plug(1, keyboard);
    const uint32_t second = world.Plug(2, mouse);
    REQUIRE(first != 0);
    REQUIRE(second != 0);
    REQUIRE(recording.Count("attach") == 2);
    REQUIRE(keyboard->configuration() == 1);
    REQUIRE(mouse->configuration() == 1);
    REQUIRE(rig.fake.Configured().size() == 2);
    const T::Device* device = rig.manager->Find(first);
    REQUIRE(device != nullptr);
    REQUIRE(device->number == 2);
    REQUIRE(std::string(device->path) == "1-1");
    REQUIRE(Listing() ==
            "1-1 dev 2 high 0627:0001 class 03/01/01 hid-test\n"
            "1-2 dev 3 high 0627:0002 class 03/01/02 hid-test\n");

    recording.events.clear();
    rig.fake.Disconnect(1);
    rig.Settle();
    REQUIRE(recording.events.size() == 2);
    // The pending transfer completes cancelled while the driver is still bound, then the driver
    // detaches with nothing in flight and before its slot is disabled.
    REQUIRE(recording.events[0].kind == "report");
    REQUIRE(recording.events[0].status == Status::Cancelled);
    REQUIRE(recording.events[1].kind == "detach");
    REQUIRE(recording.events[1].path == "1-1");
    REQUIRE(recording.events[1].in_flight == 0);
    REQUIRE(recording.events[1].stops >= 2);
    REQUIRE(recording.events[1].disabled == 0);
    REQUIRE(rig.fake.DisabledSlots() == std::vector<uint32_t>{first});
    REQUIRE(rig.manager->Find(first) == nullptr);
    REQUIRE(Listing() == "1-2 dev 3 high 0627:0002 class 03/01/02 hid-test\n");

    // A new device takes the lowest free number, not the next one.
    recording.events.clear();
    const Scripted replacement = MakeKeyboard(Speed::High);
    const uint32_t third = world.Plug(4, replacement);
    REQUIRE(third != 0);
    REQUIRE(world.Device(third)->number == 2);
    REQUIRE(Listing() ==
            "1-4 dev 2 high 0627:0001 class 03/01/01 hid-test\n"
            "1-2 dev 3 high 0627:0002 class 03/01/02 hid-test\n");
    REQUIRE(recording.Count("attach", "1-4") == 1);

    // Another unplug leaves the neighbours alone, then the last device goes.
    recording.events.clear();
    rig.fake.Disconnect(2);
    rig.Settle();
    REQUIRE(recording.Count("detach", "1-2") == 1);
    REQUIRE(recording.Count("report", "1-4") == 0);
    rig.fake.Disconnect(4);
    rig.Settle();
    REQUIRE(recording.Count("detach") == 2);
    REQUIRE(Listing().empty());
    REQUIRE(rig.fake.DisabledSlots().size() == 3);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("usb I20: a device's children are detached before it", "[usb][xhci]")
{
    DriverRig world;
    Rig& rig = world.rig;
    const uint32_t parent_slot = world.Plug(1, MakeKeyboard(Speed::High));
    const uint32_t child_slot = world.Plug(2, MakeMouse(Speed::High));
    REQUIRE(parent_slot != 0);
    REQUIRE(child_slot != 0);
    // Hubs arrive in step 9; until then a root-port device stands in for the hub.
    world.Device(child_slot)->parent = world.Device(parent_slot);
    recording.events.clear();
    rig.fake.Disconnect(1);
    rig.Settle();
    std::vector<std::string> order;
    for (const Event& event : recording.events)
    {
        if (event.kind == "detach")
        {
            order.push_back(event.path);
        }
    }
    REQUIRE(order == (std::vector<std::string>{"1-2", "1-1"}));
    REQUIRE(rig.fake.DisabledSlots() == (std::vector<uint32_t>{child_slot, parent_slot}));
    REQUIRE(Listing().empty());
    rig.fake.Disconnect(2);
    rig.Settle();
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("usb I20: detach waits until the device's callbacks have finished",
          "[usb][xhci][concurrency]")
{
    DriverRig world;
    Rig& rig = world.rig;
    const uint32_t slot = world.Plug(1, MakeKeyboard(Speed::High));
    REQUIRE(slot != 0);
    T::Device* device = world.Device(slot);
    // A callback still running on the event task counts as one in flight.
    device->in_flight.fetch_add(1);
    std::thread finishing(
        [device]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            device->in_flight.fetch_sub(1);
        });
    rig.hardware.pause_microseconds = 200;
    rig.fake.Disconnect(1);
    rig.Settle();
    rig.hardware.pause_microseconds = 0;
    finishing.join();
    REQUIRE(recording.Count("detach") == 1);
    REQUIRE(recording.events.back().in_flight == 0);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("usb: no callback runs after detach, and a stale generation is dropped",
          "[usb][xhci]")
{
    DriverRig world;
    Rig& rig = world.rig;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    const uint32_t slot = world.Plug(1, keyboard);
    REQUIRE(slot != 0);
    T::Device* device = world.Device(slot);
    REQUIRE(device != nullptr);

    // Bumping the generation stands for a completion that outlived its detach.
    rig.fake.DeliverReport(keyboard, 0x81, {0, 0, 4, 0, 0, 0, 0, 0});
    rig.controller->ProcessEvents();
    REQUIRE(recording.Count("report") == 1);
    device->generation.fetch_add(1);
    rig.fake.DeliverReport(keyboard, 0x81, {0, 0, 5, 0, 0, 0, 0, 0});
    rig.controller->ProcessEvents();
    REQUIRE(recording.Count("report") == 1);
    REQUIRE(device->in_flight.load() == 0);

    rig.fake.Disconnect(1);
    rig.Settle();
    const size_t seen = recording.events.size();
    rig.fake.DeliverReport(keyboard, 0x81, {0, 0, 6, 0, 0, 0, 0, 0});
    rig.controller->ProcessEvents();
    rig.Settle();
    REQUIRE(recording.events.size() == seen);
    REQUIRE(recording.events.back().kind == "detach");
}

TEST_CASE("usb: a device unplugged while its configuration is selected is detached cleanly",
          "[usb][xhci]")
{
    DriverRig world;
    Rig& rig = world.rig;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    keyboard->on_request = [&](const ScriptedDevice::Request& request)
    {
        if (request.request == 9)
        {
            rig.fake.Disconnect(1);
        }
    };
    rig.fake.Connect(1, Speed::High, true, keyboard);
    rig.Settle();
    REQUIRE(recording.Count("attach") == recording.Count("detach"));
    REQUIRE(rig.manager->SlotOfPort(1) == 0);
    REQUIRE(rig.manager->Find(1) == nullptr);
    REQUIRE(rig.fake.DisabledSlots() == std::vector<uint32_t>{1});
    REQUIRE(Listing().empty());
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
    REQUIRE_FALSE(rig.manager->HasWork());
}

TEST_CASE("usb: a device that refuses its configuration is listed unbound with no driver",
          "[usb][xhci]")
{
    DriverRig world;
    Rig& rig = world.rig;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    keyboard->FailControl(ScriptedDevice::Outcome::Stall, 1, 9);
    const uint32_t slot = world.Plug(1, keyboard);
    REQUIRE(slot != 0);
    REQUIRE(recording.Count("attach") == 0);
    REQUIRE(Listing() == "1-1 dev 2 high 0627:0001 class 03/01/01 unbound\n");
    rig.fake.Disconnect(1);
    rig.Settle();
    REQUIRE(recording.Count("detach") == 0);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("usb: a device without a matching driver is listed as unbound and stays unconfigured",
          "[usb][xhci]")
{
    DriverRig world;
    Rig& rig = world.rig;
    const std::vector<uint8_t> storage = FeatherOS::Usb::Testing::MassStorageConfiguration();
    const Scripted device = std::make_shared<ScriptedDevice>(
        FeatherOS::Usb::Testing::MakeDeviceDescriptor(0x1234, 0x5678, 64, 0, 0), storage);
    const uint32_t slot = world.Plug(3, device);
    REQUIRE(slot != 0);
    REQUIRE(recording.Count("attach") == 0);
    REQUIRE(device->configuration() == 0);
    REQUIRE(rig.fake.Configured().empty());
    REQUIRE(Listing() == "1-3 dev 2 high 1234:5678 class 08/06/50 unbound\n");
    rig.fake.Disconnect(3);
    rig.Settle();
    REQUIRE(Listing().empty());
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("usb: a driver that refuses an interface leaves the device unbound", "[usb][xhci]")
{
    DriverRig world;
    Rig& rig = world.rig;
    recording.refuse_attach = true;
    const uint32_t slot = world.Plug(1, MakeKeyboard(Speed::High));
    REQUIRE(slot != 0);
    REQUIRE(Listing() == "1-1 dev 2 high 0627:0001 class 03/01/01 unbound\n");
    rig.fake.Disconnect(1);
    rig.Settle();
    REQUIRE(recording.Count("detach") == 0);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("usb: three recoveries in a second unbind the drivers but keep the device",
          "[usb][xhci]")
{
    DriverRig world;
    Rig& rig = world.rig;
    recording.resubmit_on_error = true;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    keyboard->FailEndpoint(0x81, ScriptedDevice::Outcome::Stall, 3);
    const uint32_t slot = world.Plug(1, keyboard);
    REQUIRE(slot != 0);
    rig.Settle();
    // Three stalls, then the transfer queued after them is cancelled by the unbinding.
    REQUIRE(recording.Count("report") == 4);
    REQUIRE(recording.events[recording.events.size() - 2].status == Status::Cancelled);
    REQUIRE(recording.Count("detach") == 1);
    REQUIRE(recording.events.back().kind == "detach");
    REQUIRE(recording.events.back().in_flight == 0);
    REQUIRE(Listing() == "1-1 dev 2 high 0627:0001 class 03/01/01 unbound\n");
    REQUIRE(rig.manager->Find(slot) != nullptr);
    rig.fake.Disconnect(1);
    rig.Settle();
    REQUIRE(recording.Count("detach") == 1);
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("usb: /proc/usb has the devices' lines and is listed in /proc", "[usb][xhci]")
{
    DriverRig world;
    REQUIRE(world.Plug(1, MakeKeyboard(Speed::High)) != 0);
    REQUIRE(world.Plug(2, MakeMouse(Speed::Full), Speed::Full) != 0);
    FeatherOS::Procfs::SetSource({nullptr, nullptr, nullptr, nullptr, nullptr, T::FormatDevices});
    char text[FeatherOS::Procfs::MaximumFileSize];
    size_t length = 0;
    REQUIRE(FeatherOS::Procfs::Generate("usb", text, &length));
    REQUIRE(std::string(text, length) ==
            "1-1 dev 2 high 0627:0001 class 03/01/01 hid-test\n"
            "1-2 dev 3 full 0627:0002 class 03/01/02 hid-test\n");
    FeatherOS::Filesystem::DirectoryEntry entry;
    std::vector<std::string> names;
    for (uint32_t index = 0; FeatherOS::Procfs::ReadDirectory("", index, &entry); ++index)
    {
        names.emplace_back(entry.name);
    }
    REQUIRE(std::find(names.begin(), names.end(), "usb") != names.end());
    bool directory = true;
    uint64_t number = 0;
    REQUIRE(FeatherOS::Procfs::Find("usb", &number, &directory));
    REQUIRE_FALSE(directory);
    FeatherOS::Procfs::SetSource({});
}

TEST_CASE("usb: /proc/usb gives whole lines only when the buffer is short", "[usb][xhci]")
{
    DriverRig world;
    REQUIRE(world.Plug(1, MakeKeyboard(Speed::High)) != 0);
    REQUIRE(world.Plug(2, MakeMouse(Speed::High)) != 0);
    char text[64];
    const size_t length = T::FormatDevices(text, sizeof(text));
    REQUIRE(std::string(text, length) == "1-1 dev 2 high 0627:0001 class 03/01/01 hid-test\n");
    REQUIRE(T::FormatDevices(text, 0) == 0);
}

TEST_CASE("usb I21: completions never wait for the topology lock", "[usb][xhci][concurrency]")
{
    DriverRig world;
    Rig& rig = world.rig;
    const Scripted keyboard = MakeKeyboard(Speed::High);
    REQUIRE(world.Plug(1, keyboard) != 0);
    REQUIRE(recording.Count("attach") == 1);
    bool finished = false;
    std::future<void> delivered;
    {
        // The manager holds topology_lock while it attaches and detaches; the event task has to
        // keep delivering reports meanwhile.
        FeatherOS::Sync::MutexGuard held(FeatherOS::Usb::topology_lock);
        delivered = std::async(std::launch::async,
                               [&]
                               {
                                   for (uint8_t key = 4; key < 8; ++key)
                                   {
                                       rig.fake.DeliverReport(keyboard, 0x81,
                                                              {0, 0, key, 0, 0, 0, 0, 0});
                                       rig.controller->ProcessEvents();
                                   }
                               });
        finished = delivered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    }
    delivered.get();
    REQUIRE(finished);
    REQUIRE(recording.Count("report") == 4);
}

} // namespace
