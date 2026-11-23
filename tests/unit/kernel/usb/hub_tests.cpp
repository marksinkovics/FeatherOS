#include <catch2/catch_test_macros.hpp>

#include "fake_device.h"
#include "rig.h"

#include "kernel/usb/descriptors.h"
#include "kernel/usb/device.h"
#include "kernel/usb/driver.h"
#include "kernel/usb/hub.h"
#include "kernel/usb/hub_state.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{

namespace T = FeatherOS::Usb;
namespace P = FeatherOS::Usb::HubPort;
using FeatherOS::Usb::Location;
using FeatherOS::Usb::Speed;
using FeatherOS::Usb::Status;
using FeatherOS::Usb::Testing::MakeHub;
using FeatherOS::Usb::Testing::MakeKeyboard;
using FeatherOS::Usb::Testing::MakeMouse;
using FeatherOS::Usb::Testing::Rig;
using FeatherOS::Usb::Testing::ScriptedDevice;
using FeatherOS::Usb::Testing::ScriptedHub;

using Scripted = std::shared_ptr<ScriptedDevice>;
using Hub = std::shared_ptr<ScriptedHub>;

constexpr uint16_t Connected = P::StatusBit::Connection;
constexpr uint16_t Enabled = P::StatusBit::Enable;

P::Event Status3(uint16_t status, uint16_t change)
{
    return {P::EventKind::Status, status, change};
}

P::State Fresh(bool usb3 = false)
{
    P::State state;
    state.usb3 = usb3;
    return state;
}

// Walks a state to the point where it waits for its port reset.
P::State InReset(bool usb3 = false)
{
    P::State state = Fresh(usb3);
    P::Step(state, {P::EventKind::PoweredUp});
    P::Step(state, Status3(Connected, P::ChangeBit::Connection));
    P::Step(state, Status3(Connected, 0));
    REQUIRE(state.phase == P::Phase::Reset);
    return state;
}

TEST_CASE("hub T26: power-on, debounce, reset and enumeration in order", "[hub][usb]")
{
    P::State state = Fresh();
    P::Action action = P::Step(state, {P::EventKind::PoweredUp});
    REQUIRE(state.phase == P::Phase::Idle);
    REQUIRE(action.next == P::Next::ReadStatus);

    action = P::Step(state, Status3(Connected, P::ChangeBit::Connection));
    REQUIRE(state.phase == P::Phase::Debounce);
    REQUIRE(action.next == P::Next::Debounce);
    REQUIRE(action.clear == P::ChangeBit::Connection);

    // Stable after the debounce time: the reset starts, and nothing is enumerated yet.
    action = P::Step(state, Status3(Connected, 0));
    REQUIRE(state.phase == P::Phase::Reset);
    REQUIRE(action.next == P::Next::Reset);

    action = P::Step(state, Status3(Connected, 0));
    REQUIRE(action.next == P::Next::WaitReset);
    REQUIRE(state.phase == P::Phase::Reset);

    action = P::Step(state, Status3(Connected | Enabled, P::ChangeBit::Reset));
    REQUIRE(action.next == P::Next::Enumerate);
    REQUIRE((action.clear & P::ChangeBit::Reset) != 0);
    REQUIRE(action.speed == Speed::Full);
    REQUIRE(state.phase == P::Phase::Enumerating);

    action = P::Step(state, {P::EventKind::Enumerated});
    REQUIRE(state.phase == P::Phase::Active);
    REQUIRE(action.next == P::Next::None);
}

TEST_CASE("hub T26: the speed comes from the USB 2 status bits and is SuperSpeed on a USB 3 hub",
          "[hub][usb]")
{
    REQUIRE(P::PortSpeed(false, Connected | Enabled) == Speed::Full);
    REQUIRE(P::PortSpeed(false, Connected | Enabled | P::StatusBit::LowSpeed) == Speed::Low);
    REQUIRE(P::PortSpeed(false, Connected | Enabled | P::StatusBit::HighSpeed) == Speed::High);
    REQUIRE(P::PortSpeed(true, Connected | Enabled) == Speed::Super);
    REQUIRE(P::PortSpeed(true, Connected | Enabled | (1U << P::StatusBit::SuperSpeedShift)) ==
            Speed::SuperPlus);

    for (const bool usb3 : {false, true})
    {
        P::State state = InReset(usb3);
        const uint16_t speed_bits = usb3 ? 0 : P::StatusBit::HighSpeed;
        const P::Action action = P::Step(
            state, Status3(Connected | Enabled | speed_bits, P::ChangeBit::Reset));
        REQUIRE(action.next == P::Next::Enumerate);
        REQUIRE(action.speed == (usb3 ? Speed::Super : Speed::High));
    }
}

TEST_CASE("hub T26: every change bit a hub reports is cleared with its own selector",
          "[hub][usb]")
{
    struct Bit
    {
        uint16_t bit;
        uint8_t usb2;
        uint8_t usb3;
    };
    const Bit bits[] = {
        {P::ChangeBit::Connection, 16, 16},     {P::ChangeBit::Enable, 17, 0},
        {P::ChangeBit::Suspend, 18, 0},         {P::ChangeBit::OverCurrent, 19, 19},
        {P::ChangeBit::Reset, 20, 20},          {P::ChangeBit::BhReset, 0, 29},
        {P::ChangeBit::LinkState, 0, 25},       {P::ChangeBit::ConfigError, 0, 26},
    };
    for (const bool usb3 : {false, true})
    {
        uint16_t known = 0;
        for (const Bit& entry : bits)
        {
            const uint8_t selector = usb3 ? entry.usb3 : entry.usb2;
            REQUIRE(P::ClearSelector(usb3, entry.bit) == selector);
            known |= selector != 0 ? entry.bit : 0;
            if (selector == 0)
            {
                continue;
            }
            // A change in any phase, with or without a device, is cleared by that step.
            for (const P::Phase phase :
                 {P::Phase::PowerOn, P::Phase::Idle, P::Phase::Debounce, P::Phase::Reset,
                  P::Phase::Active, P::Phase::Failed, P::Phase::Off})
            {
                P::State state = Fresh(usb3);
                state.phase = phase;
                const P::Action action = P::Step(state, Status3(Connected | Enabled, entry.bit));
                REQUIRE((action.clear & entry.bit) != 0);
            }
        }
        REQUIRE(P::KnownChanges(usb3) == known);
    }
}

TEST_CASE("hub T26: a disconnect during the reset ends in Idle without a device", "[hub][usb]")
{
    P::State state = InReset();
    const P::Action action = P::Step(state, Status3(0, P::ChangeBit::Connection));
    REQUIRE(state.phase == P::Phase::Idle);
    REQUIRE(action.next == P::Next::None);
    REQUIRE_FALSE(action.detach);
    REQUIRE((action.clear & P::ChangeBit::Connection) != 0);

    // A reset that finishes with the port disabled is a failure, not a device.
    state = InReset();
    const P::Action failed = P::Step(state, Status3(Connected, P::ChangeBit::Reset));
    REQUIRE(state.phase == P::Phase::Failed);
    REQUIRE(failed.next == P::Next::None);
}

TEST_CASE("hub T26: a reset that never completes fails the port until the next connection",
          "[hub][usb]")
{
    P::State state = InReset();
    P::Step(state, {P::EventKind::ResetTimeout});
    REQUIRE(state.phase == P::Phase::Failed);
    P::Action action = P::Step(state, Status3(Connected, 0));
    REQUIRE(state.phase == P::Phase::Failed);
    REQUIRE(action.next == P::Next::None);
    action = P::Step(state, Status3(Connected, P::ChangeBit::Connection));
    REQUIRE(state.phase == P::Phase::Debounce);
    REQUIRE(action.next == P::Next::Debounce);

    state.phase = P::Phase::Failed;
    P::Step(state, Status3(0, P::ChangeBit::Connection));
    REQUIRE(state.phase == P::Phase::Idle);
}

TEST_CASE("hub T26: a connection that keeps changing is debounced again, then given up",
          "[hub][usb]")
{
    P::State state = Fresh();
    P::Step(state, {P::EventKind::PoweredUp});
    P::Step(state, Status3(Connected, P::ChangeBit::Connection));
    for (uint32_t round = 1; round < P::MaxDebounceRounds; ++round)
    {
        const P::Action action = P::Step(state, Status3(Connected, P::ChangeBit::Connection));
        REQUIRE(action.next == P::Next::Debounce);
        REQUIRE(state.phase == P::Phase::Debounce);
    }
    const P::Action last = P::Step(state, Status3(Connected, P::ChangeBit::Connection));
    REQUIRE(last.next == P::Next::None);
    REQUIRE(state.phase == P::Phase::Failed);

    state = Fresh();
    P::Step(state, {P::EventKind::PoweredUp});
    P::Step(state, Status3(Connected, P::ChangeBit::Connection));
    P::Step(state, Status3(0, P::ChangeBit::Connection));
    REQUIRE(state.phase == P::Phase::Idle);
}

TEST_CASE("hub T26: an unplugged device is detached, a replaced one is debounced again",
          "[hub][usb]")
{
    P::State state = InReset();
    P::Step(state, Status3(Connected | Enabled, P::ChangeBit::Reset));
    P::Step(state, {P::EventKind::Enumerated});
    REQUIRE(state.phase == P::Phase::Active);

    P::Action action = P::Step(state, Status3(Connected | Enabled, P::ChangeBit::Enable));
    REQUIRE_FALSE(action.detach);
    REQUIRE(state.phase == P::Phase::Active);

    action = P::Step(state, Status3(Connected | Enabled, P::ChangeBit::Connection));
    REQUIRE(action.detach);
    REQUIRE(action.next == P::Next::Debounce);
    REQUIRE(state.phase == P::Phase::Debounce);

    state = InReset();
    P::Step(state, Status3(Connected | Enabled, P::ChangeBit::Reset));
    P::Step(state, {P::EventKind::Enumerated});
    action = P::Step(state, Status3(0, P::ChangeBit::Connection));
    REQUIRE(action.detach);
    REQUIRE(action.next == P::Next::None);
    REQUIRE(state.phase == P::Phase::Idle);

    state = InReset();
    P::Step(state, Status3(Connected | Enabled, P::ChangeBit::Reset));
    P::Step(state, {P::EventKind::EnumerationFailed});
    REQUIRE(state.phase == P::Phase::Failed);
}

TEST_CASE("hub T26: a requested reset detaches the child and resets the port again",
          "[hub][usb]")
{
    P::State state = InReset();
    P::Step(state, Status3(Connected | Enabled, P::ChangeBit::Reset));
    P::Step(state, {P::EventKind::Enumerated});
    REQUIRE(state.phase == P::Phase::Active);

    P::Action action = P::Step(state, {P::EventKind::RequestReset});
    REQUIRE(action.detach);
    REQUIRE(action.next == P::Next::Reset);
    REQUIRE(state.phase == P::Phase::Reset);

    action = P::Step(state, Status3(Connected | Enabled, P::ChangeBit::Reset));
    REQUIRE(action.next == P::Next::Enumerate);
    REQUIRE(state.phase == P::Phase::Enumerating);

    // A port that holds no device has nothing to reset.
    for (P::State other : {Fresh(), InReset(), state})
    {
        const P::Phase before = other.phase;
        action = P::Step(other, {P::EventKind::RequestReset});
        REQUIRE_FALSE(action.detach);
        REQUIRE(action.next == P::Next::None);
        REQUIRE(other.phase == before);
    }
}

TEST_CASE("hub T26: over-current powers the port off and detaches what it held", "[hub][usb]")
{
    P::State state = Fresh();
    P::Step(state, {P::EventKind::PoweredUp});
    P::Action action = P::Step(
        state, Status3(Connected | P::StatusBit::OverCurrent,
                       P::ChangeBit::OverCurrent | P::ChangeBit::Connection));
    REQUIRE(action.power_off);
    REQUIRE_FALSE(action.detach);
    REQUIRE(action.next == P::Next::None);
    REQUIRE(state.phase == P::Phase::Off);
    REQUIRE((action.clear & P::ChangeBit::OverCurrent) != 0);
    REQUIRE((action.clear & P::ChangeBit::Connection) != 0);

    action = P::Step(state, Status3(Connected, P::ChangeBit::Connection));
    REQUIRE_FALSE(action.power_off);
    REQUIRE(action.next == P::Next::None);

    state = InReset();
    P::Step(state, Status3(Connected | Enabled, P::ChangeBit::Reset));
    P::Step(state, {P::EventKind::Enumerated});
    action = P::Step(state, Status3(Connected | P::StatusBit::OverCurrent,
                                    P::ChangeBit::OverCurrent));
    REQUIRE(action.power_off);
    REQUIRE(action.detach);
    REQUIRE(state.phase == P::Phase::Off);
}

TEST_CASE("hub T27: a child's route, depth and TT follow the hub it hangs from", "[hub][usb]")
{
    const auto site = [](Speed speed, uint8_t root, uint8_t depth, uint32_t route, uint32_t slot,
                         bool multi)
    {
        P::HubSite entry = {};
        entry.location.speed = speed;
        entry.location.root_port = root;
        entry.location.depth = depth;
        entry.location.route = route;
        entry.slot = slot;
        entry.multi_tt = multi;
        return entry;
    };
    Location child;

    // A high-speed hub on root port 3: a full-speed device behind its port 2 uses its TT.
    REQUIRE(P::ChildLocation(site(Speed::High, 3, 0, 0, 5, false), 2, Speed::Full, &child));
    REQUIRE(child.route == 0x2);
    REQUIRE(child.depth == 1);
    REQUIRE(child.root_port == 3);
    REQUIRE(child.speed == Speed::Full);
    REQUIRE(child.tt_hub_slot == 5);
    REQUIRE(child.tt_port == 1);
    REQUIRE_FALSE(child.tt_multi);

    // A multi-TT hub names the port, a single-TT hub port 1.
    REQUIRE(P::ChildLocation(site(Speed::High, 3, 0, 0, 5, true), 7, Speed::Low, &child));
    REQUIRE(child.tt_hub_slot == 5);
    REQUIRE(child.tt_port == 7);
    REQUIRE(child.tt_multi);

    // High-speed devices behind a high-speed hub need no TT.
    REQUIRE(P::ChildLocation(site(Speed::High, 3, 0, 0, 5, true), 7, Speed::High, &child));
    REQUIRE(child.tt_hub_slot == 0);
    REQUIRE(child.tt_port == 0);
    REQUIRE_FALSE(child.tt_multi);

    // A full-speed hub below that hub: its child keeps the high-speed hub as its TT, and the TT
    // port is the high-speed hub's port the path leaves by.
    Location full_hub;
    REQUIRE(P::ChildLocation(site(Speed::High, 3, 0, 0, 5, true), 4, Speed::Full, &full_hub));
    P::HubSite below = {full_hub, 6, false};
    REQUIRE(P::ChildLocation(below, 3, Speed::Low, &child));
    REQUIRE(child.route == 0x34);
    REQUIRE(child.depth == 2);
    REQUIRE(child.tt_hub_slot == 5);
    REQUIRE(child.tt_port == 4);
    REQUIRE(child.tt_multi);

    // A full-speed hub on a root port has no TT above it.
    REQUIRE(P::ChildLocation(site(Speed::Full, 1, 0, 0, 2, false), 1, Speed::Low, &child));
    REQUIRE(child.tt_hub_slot == 0);
    REQUIRE(child.tt_port == 0);

    // SuperSpeed hubs give SuperSpeed devices and no TT.
    REQUIRE(P::ChildLocation(site(Speed::Super, 2, 0, 0, 4, false), 1, Speed::Super, &child));
    REQUIRE(child.tt_hub_slot == 0);
}

TEST_CASE("hub T27: the route string holds five tiers and fifteen ports", "[hub][usb]")
{
    Location child;
    P::HubSite hub = {};
    hub.location.speed = Speed::High;
    hub.location.root_port = 2;
    uint32_t route = 0;
    for (uint8_t depth = 0; depth < P::MaxDepth; ++depth)
    {
        hub.location.depth = depth;
        hub.location.route = route;
        REQUIRE(P::ChildLocation(hub, static_cast<uint8_t>(depth + 1), Speed::High, &child));
        REQUIRE(child.depth == depth + 1);
        route |= static_cast<uint32_t>(depth + 1) << (4 * depth);
        REQUIRE(child.route == route);
    }
    REQUIRE(route == 0x54321);
    hub.location.depth = P::MaxDepth;
    hub.location.route = route;
    REQUIRE_FALSE(P::ChildLocation(hub, 1, Speed::High, &child));

    hub.location.depth = 0;
    hub.location.route = 0;
    REQUIRE(P::ChildLocation(hub, 15, Speed::High, &child));
    REQUIRE(child.route == 0xF);
    REQUIRE_FALSE(P::ChildLocation(hub, 16, Speed::High, &child));
    REQUIRE_FALSE(P::ChildLocation(hub, 0, Speed::High, &child));
}

TEST_CASE("hub: the hub descriptor parser refuses short and wrong descriptors", "[hub][usb]")
{
    const std::vector<uint8_t> usb2 = {9, 0x29, 4, 0x60, 0, 7, 0, 0, 0xFF};
    T::HubDescriptor hub{};
    REQUIRE(T::ParseHub(usb2, false, &hub) == T::ParseStatus::Ok);
    REQUIRE(hub.ports == 4);
    REQUIRE(hub.think_time == 3);
    REQUIRE(hub.power_on_to_good == 7);
    REQUIRE(T::ParseHub(usb2, true, &hub) == T::ParseStatus::Malformed);
    REQUIRE(T::ParseHub(std::vector<uint8_t>(usb2.begin(), usb2.begin() + 6), false, &hub) ==
            T::ParseStatus::Truncated);
    std::vector<uint8_t> longer = usb2;
    longer[0] = 12;
    REQUIRE(T::ParseHub(longer, false, &hub) == T::ParseStatus::Truncated);
    std::vector<uint8_t> none = usb2;
    none[2] = 0;
    REQUIRE(T::ParseHub(none, false, &hub) == T::ParseStatus::Malformed);
    std::vector<uint8_t> tiny = usb2;
    tiny[0] = 3;
    REQUIRE(T::ParseHub(tiny, false, &hub) == T::ParseStatus::Truncated);
    const std::vector<uint8_t> usb3 = {12, 0x2A, 8, 0x20, 0, 3, 0, 0, 0, 0, 0, 0};
    REQUIRE(T::ParseHub(usb3, true, &hub) == T::ParseStatus::Ok);
    REQUIRE(hub.ports == 8);
    REQUIRE(hub.think_time == 1);
}

struct Event
{
    std::string kind;
    std::string path;
    size_t disabled = 0;
};

std::vector<Event> events;
const T::Driver* probe_drivers[2];

bool ProbeMatches(const T::InterfaceView& interface)
{
    return interface.class_code == 3;
}

Status ProbeAttach(T::Device& device, const T::InterfaceView&, void** state)
{
    events.push_back({"attach", device.path, 0});
    *state = &device;
    return Status::Success;
}

void ProbeDetach(T::Device& device, void*)
{
    events.push_back({"detach", device.path, 0});
}

const T::Driver ProbeDriver = {"probe", ProbeMatches, ProbeAttach, ProbeDetach};

struct SlotContext
{
    uint32_t route;
    uint32_t speed;
    bool multi_tt;
    bool hub;
    uint32_t root_port;
    uint32_t ports;
    uint32_t tt_slot;
    uint32_t tt_port;
    uint32_t think_time;
};

SlotContext Decode(const uint32_t* words)
{
    return {words[0] & 0xFFFFFU,       (words[0] >> 20) & 0xFU,    ((words[0] >> 25) & 1U) != 0,
            ((words[0] >> 26) & 1U) != 0, (words[1] >> 16) & 0xFFU, words[1] >> 24,
            words[2] & 0xFFU,          (words[2] >> 8) & 0xFFU,    (words[2] >> 16) & 3U};
}

class World
{
public:
    explicit World(uint32_t usb3_first = 0)
    {
        events.clear();
        rig.fake.SetUsb3Ports(usb3_first);
        probe_drivers[0] = &T::HubDriver;
        probe_drivers[1] = &ProbeDriver;
        rig.drivers = probe_drivers;
        REQUIRE(rig.Create());
        for (uint32_t port = 1; port <= 8; ++port)
        {
            REQUIRE(rig.fake.SlotOfPort(port) == 0);
        }
    }
    ~World()
    {
        events.clear();
    }

    void Plug(uint32_t port, Scripted device, Speed speed)
    {
        rig.fake.Connect(port, speed, true, std::move(device));
        rig.Settle();
    }

    void PlugBehind(const Hub& hub, uint8_t port, Scripted child, Speed speed)
    {
        hub->Plug(port, std::move(child), speed);
        rig.fake.Wake(hub);
        rig.Settle();
    }

    void UnplugBehind(const Hub& hub, uint8_t port)
    {
        hub->Unplug(port);
        rig.fake.Wake(hub);
        rig.Settle();
    }

    T::Device* At(const std::string& path)
    {
        for (uint32_t slot = 1; slot <= T::MaxDeviceSlots; ++slot)
        {
            const T::Device* device = rig.manager->Find(slot);
            if (device != nullptr && path == device->path)
            {
                return const_cast<T::Device*>(device);
            }
        }
        return nullptr;
    }

    uint32_t SlotOf(const std::string& path)
    {
        const T::Device* device = At(path);
        return device != nullptr ? device->slot : 0;
    }

    size_t Count(const std::string& kind, const std::string& path = "") const
    {
        return static_cast<size_t>(std::count_if(
            events.begin(), events.end(), [&](const Event& event)
            { return event.kind == kind && (path.empty() || event.path == path); }));
    }

    SlotContext Addressed(uint32_t slot) const
    {
        for (const auto& record : rig.fake.Addressed())
        {
            if (record.slot == slot)
            {
                return Decode(record.slot_words);
            }
        }
        FAIL("slot was never addressed");
        return {};
    }

    std::string Listing() const
    {
        char buffer[2048];
        const size_t length = T::FormatDevices(buffer, sizeof(buffer));
        return std::string(buffer, length);
    }

    Rig rig;
};

TEST_CASE("hub: a high-speed hub is configured and its devices are enumerated behind it",
          "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::High, 4, true, 2);
    hub->Plug(1, MakeKeyboard(Speed::High), Speed::High);
    hub->Plug(2, MakeMouse(Speed::High), Speed::High);
    world.Plug(1, hub, Speed::High);

    REQUIRE(world.At("1-1") != nullptr);
    REQUIRE(world.At("1-1.1") != nullptr);
    REQUIRE(world.At("1-1.2") != nullptr);
    REQUIRE(world.At("1-1.3") == nullptr);
    REQUIRE(world.Count("attach") == 2);
    REQUIRE(hub->configuration() == 1);
    // Multi-TT: alternate setting 1, and MTT in the slot context.
    REQUIRE(hub->Alternates() == std::vector<uint16_t>{1});
    REQUIRE(hub->Depths().empty());
    const uint32_t hub_slot = world.SlotOf("1-1");
    const auto& updates = rig.fake.SlotUpdates();
    const auto found = std::find_if(updates.begin(), updates.end(), [&](const auto& update)
                                    { return update.slot == hub_slot && update.add_flags == 1; });
    REQUIRE(found != updates.end());
    const SlotContext context = Decode(found->slot_words);
    REQUIRE(context.hub);
    REQUIRE(context.multi_tt);
    REQUIRE(context.ports == 4);
    REQUIRE(context.think_time == 2);
    REQUIRE(context.root_port == 1);
    // The status change endpoint went in with the first Configure Endpoint, the hub fields with
    // a second one that adds no endpoint.
    REQUIRE(std::any_of(rig.fake.Configured().begin(), rig.fake.Configured().end(),
                        [&](const auto& endpoint)
                        { return endpoint.slot == hub_slot && endpoint.dci == 3; }));
    for (uint8_t port = 1; port <= 4; ++port)
    {
        REQUIRE(hub->Powered(port));
    }

    // High-speed children of a high-speed hub: route nibbles, no TT.
    const SlotContext keyboard = world.Addressed(world.SlotOf("1-1.1"));
    REQUIRE(keyboard.route == 0x1);
    REQUIRE(keyboard.root_port == 1);
    REQUIRE(keyboard.tt_slot == 0);
    const SlotContext mouse = world.Addressed(world.SlotOf("1-1.2"));
    REQUIRE(mouse.route == 0x2);
    REQUIRE(world.Listing() ==
            "1-1 dev 2 high 05e3:0608 class 09/00/01 hub\n"
            "1-1.1 dev 3 high 0627:0001 class 03/01/01 probe\n"
            "1-1.2 dev 4 high 0627:0002 class 03/01/02 probe\n");
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub I29: a child is enumerated only after its reset completed and every change bit was "
          "cleared",
          "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::High, 4);
    hub->Plug(2, MakeKeyboard(Speed::High), Speed::High);
    struct Seen
    {
        uint8_t request;
        uint16_t value;
        uint16_t index;
        size_t addressed;
        uint32_t resets;
    };
    std::vector<Seen> seen;
    hub->on_request = [&](const ScriptedDevice::Request& request)
    {
        seen.push_back({request.request, request.value, request.index,
                        rig.fake.Addressed().size(), hub->Resets(2)});
    };
    world.Plug(1, hub, Speed::High);
    REQUIRE(world.At("1-1.2") != nullptr);

    const auto reset = std::find_if(seen.begin(), seen.end(), [](const Seen& entry)
                                    { return entry.request == 3 && entry.value == 4; });
    const auto cleared = std::find_if(seen.begin(), seen.end(), [](const Seen& entry)
                                      { return entry.request == 1 && entry.value == 20; });
    REQUIRE(reset != seen.end());
    REQUIRE(cleared != seen.end());
    // Before the reset only the hub itself was addressed; the child came after C_PORT_RESET was
    // cleared and the port showed no change bit.
    REQUIRE(reset->addressed == 1);
    REQUIRE(cleared->addressed == 1);
    REQUIRE(cleared > reset);
    REQUIRE(rig.fake.Addressed().size() == 2);
    for (uint8_t port = 1; port <= 4; ++port)
    {
        REQUIRE(hub->Change(port) == 0);
    }
    // The connection change was cleared before the debounce, and no reset went to a port with
    // nothing on it.
    REQUIRE(hub->Resets(1) == 0);
    REQUIRE(hub->Resets(2) == 1);
    const auto& clears = hub->PortClears();
    REQUIRE(std::count(clears.begin(), clears.end(), std::make_pair<uint8_t, uint8_t>(2, 16)) >=
            1);
    REQUIRE(std::count(clears.begin(), clears.end(), std::make_pair<uint8_t, uint8_t>(2, 20)) ==
            1);
}

TEST_CASE("hub: a reset that never completes leaves the port empty and does not stall the manager",
          "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::High, 2);
    hub->HoldReset(1, true);
    hub->Plug(1, MakeKeyboard(Speed::High), Speed::High);
    hub->Plug(2, MakeMouse(Speed::High), Speed::High);
    const uint64_t before = rig.hardware.Now();
    world.Plug(1, hub, Speed::High);
    REQUIRE(world.At("1-1.1") == nullptr);
    REQUIRE(world.At("1-1.2") != nullptr);
    REQUIRE(hub->Resets(1) == 1);
    REQUIRE(rig.hardware.Now() - before >= P::ResetTimeoutMilliseconds);

    // Plugging it again after the fault clears works.
    hub->HoldReset(1, false);
    world.UnplugBehind(hub, 1);
    world.PlugBehind(hub, 1, MakeKeyboard(Speed::High), Speed::High);
    REQUIRE(world.At("1-1.1") != nullptr);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub: a port that enables without a reset change is not enumerated", "[hub][usb][xhci]")
{
    World world;
    const Hub hub = MakeHub(Speed::High, 2);
    hub->SkipResetChange(1, true);
    hub->Plug(1, MakeKeyboard(Speed::High), Speed::High);
    world.Plug(1, hub, Speed::High);
    REQUIRE(world.At("1-1.1") == nullptr);
    REQUIRE(world.Count("attach") == 0);
}

TEST_CASE("hub: a device unplugged during its port reset is never enumerated", "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::High, 2);
    hub->Plug(1, MakeKeyboard(Speed::High), Speed::High);
    hub->on_request = [&](const ScriptedDevice::Request& request)
    {
        if (request.request == 3 && request.value == 4)
        {
            hub->Unplug(1);
        }
    };
    world.Plug(1, hub, Speed::High);
    REQUIRE(world.At("1-1.1") == nullptr);
    REQUIRE(rig.fake.Addressed().size() == 1);
    REQUIRE(hub->Change(1) == 0);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub: a device unplugged during enumeration gives its slot back", "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::High, 2);
    const Scripted keyboard = MakeKeyboard(Speed::High);
    hub->Plug(1, keyboard, Speed::High);
    keyboard->on_request = [&](const ScriptedDevice::Request& request)
    {
        if (request.request == 6 && (request.value >> 8) == 2)
        {
            hub->Unplug(1);
        }
    };
    world.Plug(1, hub, Speed::High);
    REQUIRE(world.At("1-1.1") == nullptr);
    REQUIRE(world.At("1-1") != nullptr);
    REQUIRE(rig.fake.DisabledSlots().size() == 1);
    REQUIRE(hub->Change(1) == 0);
    REQUIRE(hub->Child(1) == nullptr);
    // Only the hub's slot is in use now.
    rig.fake.Disconnect(1);
    rig.Settle();
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
}

TEST_CASE("hub T35: a device plugged into a hub port later is enumerated, and removed again",
          "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::Full, 4);
    world.Plug(5, hub, Speed::Full);
    REQUIRE(world.At("1-5") != nullptr);
    const std::string start = world.Listing();
    REQUIRE(start == "1-5 dev 2 full 05e3:0608 class 09/00/01 hub\n");
    for (int round = 0; round < 3; ++round)
    {
        world.PlugBehind(hub, 3, MakeKeyboard(Speed::Full), Speed::Full);
        REQUIRE(world.At("1-5.3") != nullptr);
        REQUIRE(world.Count("attach", "1-5.3") == static_cast<size_t>(round + 1));
        REQUIRE(world.Listing() ==
                start + "1-5.3 dev 3 full 0627:0001 class 03/01/01 probe\n");
        world.UnplugBehind(hub, 3);
        REQUIRE(world.At("1-5.3") == nullptr);
        REQUIRE(world.Count("detach", "1-5.3") == static_cast<size_t>(round + 1));
        REQUIRE(world.Listing() == start);
    }
    rig.fake.Disconnect(5);
    rig.Settle();
    REQUIRE(world.Listing().empty());
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub T28: a hub unplugged with devices behind it detaches them first and gives back "
          "every slot and buffer",
          "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::High, 4);
    hub->Plug(1, MakeKeyboard(Speed::High), Speed::High);
    hub->Plug(4, MakeMouse(Speed::High), Speed::High);
    world.Plug(2, hub, Speed::High);
    const uint32_t hub_slot = world.SlotOf("1-2");
    const uint32_t first = world.SlotOf("1-2.1");
    const uint32_t second = world.SlotOf("1-2.4");
    REQUIRE(hub_slot != 0);
    REQUIRE(first != 0);
    REQUIRE(second != 0);

    events.clear();
    rig.fake.Disconnect(2);
    rig.Settle();
    // The drivers of the children detached while their slots were still enabled, and the slots
    // were disabled children first.
    REQUIRE(world.Count("detach") == 2);
    for (const Event& event : events)
    {
        REQUIRE(event.kind == "detach");
    }
    REQUIRE(rig.fake.DisabledSlots() == (std::vector<uint32_t>{first, second, hub_slot}));
    REQUIRE(world.Listing().empty());
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);
    REQUIRE(rig.fake.Faults() == 0);

    // Everything can be plugged again, and numbers and slots start over.
    world.Plug(2, hub, Speed::High);
    REQUIRE(world.At("1-2.1") != nullptr);
    REQUIRE(world.At("1-2.4") != nullptr);
}

TEST_CASE("hub I20: a hub in the middle of a chain is detached after everything below it",
          "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub top = MakeHub(Speed::High, 2);
    const Hub middle = MakeHub(Speed::High, 2);
    top->Plug(2, middle, Speed::High);
    middle->Plug(1, MakeKeyboard(Speed::High), Speed::High);
    middle->Plug(2, MakeMouse(Speed::High), Speed::High);
    world.Plug(1, top, Speed::High);
    REQUIRE(world.At("1-1.2.1") != nullptr);
    REQUIRE(world.At("1-1.2.2") != nullptr);
    const uint32_t top_slot = world.SlotOf("1-1");
    const uint32_t middle_slot = world.SlotOf("1-1.2");
    const uint32_t keyboard = world.SlotOf("1-1.2.1");
    const uint32_t mouse = world.SlotOf("1-1.2.2");

    rig.fake.Disconnect(1);
    rig.Settle();
    REQUIRE(rig.fake.DisabledSlots() ==
            (std::vector<uint32_t>{keyboard, mouse, middle_slot, top_slot}));
    REQUIRE(rig.hardware.Outstanding() == rig.baseline);

    // Unplugging only the middle hub leaves the top hub and takes the keyboard with it.
    world.Plug(1, top, Speed::High);
    const uint32_t middle_again = world.SlotOf("1-1.2");
    const uint32_t keyboard_again = world.SlotOf("1-1.2.1");
    const uint32_t mouse_again = world.SlotOf("1-1.2.2");
    const size_t disabled_before = rig.fake.DisabledSlots().size();
    world.UnplugBehind(top, 2);
    const auto& disabled = rig.fake.DisabledSlots();
    REQUIRE(disabled.size() == disabled_before + 3);
    REQUIRE(std::vector<uint32_t>(disabled.end() - 3, disabled.end()) ==
            (std::vector<uint32_t>{keyboard_again, mouse_again, middle_again}));
    REQUIRE(world.At("1-1") != nullptr);
    REQUIRE(world.At("1-1.2") == nullptr);
}

TEST_CASE("hub T27: a full-speed device behind a high-speed hub names the hub as its TT",
          "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::High, 4, false);
    hub->Plug(3, MakeKeyboard(Speed::Full), Speed::Full);
    world.Plug(6, hub, Speed::High);
    const uint32_t hub_slot = world.SlotOf("1-6");
    const SlotContext keyboard = world.Addressed(world.SlotOf("1-6.3"));
    REQUIRE(keyboard.route == 0x3);
    REQUIRE(keyboard.root_port == 6);
    REQUIRE(keyboard.speed == 1);
    REQUIRE(keyboard.tt_slot == hub_slot);
    REQUIRE(keyboard.tt_port == 1);
    REQUIRE_FALSE(keyboard.multi_tt);
    const SlotContext hub_context = world.Addressed(hub_slot);
    REQUIRE(hub_context.tt_slot == 0);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub T27: a low-speed device behind a full-speed hub behind a multi-TT hub",
          "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub fast = MakeHub(Speed::High, 4, true);
    const Hub slow = MakeHub(Speed::Full, 4);
    slow->Plug(3, MakeKeyboard(Speed::Low), Speed::Low);
    fast->Plug(2, slow, Speed::Full);
    world.Plug(1, fast, Speed::High);
    const uint32_t fast_slot = world.SlotOf("1-1");
    const uint32_t slow_slot = world.SlotOf("1-1.2");
    const uint32_t keyboard_slot = world.SlotOf("1-1.2.3");
    REQUIRE(slow_slot != 0);
    REQUIRE(keyboard_slot != 0);

    const SlotContext slow_context = world.Addressed(slow_slot);
    REQUIRE(slow_context.route == 0x2);
    REQUIRE(slow_context.tt_slot == fast_slot);
    REQUIRE(slow_context.tt_port == 2);
    REQUIRE(slow_context.multi_tt);
    REQUIRE(slow_context.hub == false);

    const SlotContext keyboard = world.Addressed(keyboard_slot);
    REQUIRE(keyboard.route == 0x32);
    REQUIRE(keyboard.speed == 2);
    REQUIRE(keyboard.root_port == 1);
    REQUIRE(keyboard.tt_slot == fast_slot);
    REQUIRE(keyboard.tt_port == 2);
    REQUIRE(keyboard.multi_tt);
    REQUIRE(world.At("1-1.2.3") != nullptr);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub T27: a full-speed hub on a root port gives its devices no TT", "[hub][usb][xhci]")
{
    World world;
    const Hub hub = MakeHub(Speed::Full, 4);
    hub->Plug(2, MakeKeyboard(Speed::Low), Speed::Low);
    hub->Plug(4, MakeMouse(Speed::Full), Speed::Full);
    world.Plug(3, hub, Speed::Full);
    for (const char* path : {"1-3.2", "1-3.4"})
    {
        const SlotContext context = world.Addressed(world.SlotOf(path));
        REQUIRE(context.tt_slot == 0);
        REQUIRE(context.tt_port == 0);
        REQUIRE_FALSE(context.multi_tt);
    }
    REQUIRE(world.Addressed(world.SlotOf("1-3.2")).speed == 2);
    REQUIRE(world.Addressed(world.SlotOf("1-3.4")).speed == 1);
}

TEST_CASE("hub T27: SuperSpeed hubs get SET_HUB_DEPTH with their depth and give SuperSpeed devices",
          "[hub][usb][xhci]")
{
    // The fake controller's ports from 5 on are USB 3.
    World world(5);
    Rig& rig = world.rig;
    const Hub top = MakeHub(Speed::Super, 4);
    const Hub below = MakeHub(Speed::Super, 3);
    const Hub third = MakeHub(Speed::Super, 2);
    third->Plug(2, MakeKeyboard(Speed::Super), Speed::Super);
    below->Plug(1, third, Speed::Super);
    top->Plug(4, below, Speed::Super);
    world.Plug(5, top, Speed::Super);
    REQUIRE(world.At("1-5.4.1.2") != nullptr);
    REQUIRE(top->Depths() == std::vector<uint16_t>{0});
    REQUIRE(below->Depths() == std::vector<uint16_t>{1});
    REQUIRE(third->Depths() == std::vector<uint16_t>{2});
    REQUIRE(top->Alternates().empty());
    const SlotContext keyboard = world.Addressed(world.SlotOf("1-5.4.1.2"));
    REQUIRE(keyboard.route == 0x214);
    REQUIRE(keyboard.speed == 4);
    REQUIRE(keyboard.tt_slot == 0);
    // A SuperSpeed hub's connection and reset changes are cleared with USB 3 selectors only.
    for (const auto& clear : top->PortClears())
    {
        REQUIRE((clear.second == 16 || clear.second == 20 || clear.second == 19 ||
                 clear.second == 25 || clear.second == 26 || clear.second == 29));
    }
    for (uint8_t port = 1; port <= 4; ++port)
    {
        REQUIRE(top->Change(port) == 0);
    }
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub T27: a sixth tier of hubs is refused and its port disabled", "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    std::vector<Hub> hubs;
    for (int level = 0; level < 6; ++level)
    {
        hubs.push_back(MakeHub(Speed::High, 2));
    }
    const Scripted keyboard = MakeKeyboard(Speed::High);
    hubs[5]->Plug(1, keyboard, Speed::High);
    for (int level = 4; level >= 0; --level)
    {
        hubs[static_cast<size_t>(level)]->Plug(1, hubs[static_cast<size_t>(level) + 1],
                                               Speed::High);
    }
    world.Plug(1, hubs[0], Speed::High);
    REQUIRE(world.At("1-1.1.1.1.1") != nullptr);
    // The hub at depth 5 is the sixth tier: it enumerated, its child was refused.
    REQUIRE(world.At("1-1.1.1.1.1.1") != nullptr);
    REQUIRE(world.At("1-1.1.1.1.1.1.1") == nullptr);
    REQUIRE(world.Count("attach") == 0);
    const auto& clears = hubs[5]->PortClears();
    REQUIRE(std::count(clears.begin(), clears.end(), std::make_pair<uint8_t, uint8_t>(1, 1)) ==
            1);
    REQUIRE(hubs[5]->Resets(1) == 1);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub: a hub with more than fifteen ports powers and watches the first fifteen",
          "[hub][usb][xhci]")
{
    World world;
    const Hub hub = MakeHub(Speed::High, 20);
    hub->Plug(15, MakeKeyboard(Speed::High), Speed::High);
    hub->Plug(16, MakeMouse(Speed::High), Speed::High);
    world.Plug(1, hub, Speed::High);
    REQUIRE(world.At("1-1.15") != nullptr);
    REQUIRE(world.At("1-1.16") == nullptr);
    REQUIRE(hub->Powered(15));
    REQUIRE_FALSE(hub->Powered(16));
    REQUIRE(hub->Resets(16) == 0);
}

TEST_CASE("hub: over-current on a port powers it off and detaches its device", "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::High, 3);
    hub->Plug(1, MakeKeyboard(Speed::High), Speed::High);
    hub->Plug(2, MakeMouse(Speed::High), Speed::High);
    world.Plug(1, hub, Speed::High);
    REQUIRE(world.At("1-1.1") != nullptr);
    REQUIRE(world.At("1-1.2") != nullptr);

    hub->OverCurrent(1);
    rig.fake.Wake(hub);
    rig.Settle();
    REQUIRE(world.At("1-1.1") == nullptr);
    REQUIRE(world.At("1-1.2") != nullptr);
    REQUIRE_FALSE(hub->Powered(1));
    REQUIRE(hub->Powered(2));
    REQUIRE(hub->Change(1) == 0);
    REQUIRE(world.Count("detach", "1-1.1") == 1);

    // An over-current on an empty port also switches it off, and the hub keeps working.
    hub->OverCurrent(3);
    rig.fake.Wake(hub);
    rig.Settle();
    REQUIRE_FALSE(hub->Powered(3));
    world.UnplugBehind(hub, 2);
    REQUIRE(world.At("1-1.2") == nullptr);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub: a connector that flaps is debounced and its device stays", "[hub][usb][xhci]")
{
    World world;
    Rig& rig = world.rig;
    const Hub hub = MakeHub(Speed::High, 2);
    hub->Plug(1, MakeKeyboard(Speed::High), Speed::High);
    hub->on_request = [&, flaps = 0](const ScriptedDevice::Request& request) mutable
    {
        // A connection change shows up while the port is being debounced the first two times.
        if (request.request == 0 && request.type == 0xA3 && flaps < 2 && hub->Resets(1) == 0)
        {
            ++flaps;
            hub->Flap(1);
        }
    };
    world.Plug(1, hub, Speed::High);
    REQUIRE(world.At("1-1.1") != nullptr);
    REQUIRE(hub->Resets(1) == 1);
    REQUIRE(hub->Change(1) == 0);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("hub: a device behind a hub works as one on a root port", "[hub][usb][xhci]")
{
    World world;
    const Hub hub = MakeHub(Speed::High, 4);
    const Scripted keyboard = MakeKeyboard(Speed::High);
    hub->Plug(1, keyboard, Speed::High);
    world.Plug(1, hub, Speed::High);
    REQUIRE(world.Count("attach", "1-1.1") == 1);
    REQUIRE(keyboard->configuration() == 1);
    // The device's own control requests and its interrupt endpoint use its own slot.
    REQUIRE(keyboard->Count(0x80, 6) >= 4);
    const T::Device* device = world.At("1-1.1");
    REQUIRE(device->parent == world.At("1-1"));
    REQUIRE(device->parent_port == 1);
    REQUIRE(device->location.depth == 1);
}

} // namespace
