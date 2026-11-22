#include "hub_state.h"

namespace FeatherOS::Usb::HubPort
{
namespace
{

constexpr uint16_t Usb2Changes = ChangeBit::Connection | ChangeBit::Enable | ChangeBit::Suspend |
                                 ChangeBit::OverCurrent | ChangeBit::Reset;
constexpr uint16_t Usb3Changes = ChangeBit::Connection | ChangeBit::OverCurrent | ChangeBit::Reset |
                                 ChangeBit::BhReset | ChangeBit::LinkState | ChangeBit::ConfigError;
constexpr uint32_t RouteNibbleBits = 4;

void Debouncing(State& state, Action& action)
{
    state.phase = Phase::Debounce;
    state.rounds = 0;
    action.next = Next::Debounce;
}

Action OnStatus(State& state, const Event& event)
{
    Action action;
    const bool connected = (event.status & StatusBit::Connection) != 0;
    const bool enabled = (event.status & StatusBit::Enable) != 0;
    const bool connection_changed = (event.change & ChangeBit::Connection) != 0;
    action.clear = event.change & KnownChanges(state.usb3);
    if (state.phase == Phase::PowerOn || state.phase == Phase::Off ||
        state.phase == Phase::Enumerating)
    {
        return action;
    }
    if ((event.status & StatusBit::OverCurrent) != 0)
    {
        action.detach = state.phase == Phase::Active;
        action.power_off = true;
        state.phase = Phase::Off;
        return action;
    }
    switch (state.phase)
    {
    case Phase::Idle:
        if (connected)
        {
            Debouncing(state, action);
        }
        break;
    case Phase::Debounce:
        if (!connected)
        {
            state.phase = Phase::Idle;
        }
        else if (connection_changed)
        {
            ++state.rounds;
            if (state.rounds >= MaxDebounceRounds)
            {
                state.phase = Phase::Failed;
            }
            else
            {
                action.next = Next::Debounce;
            }
        }
        else
        {
            state.phase = Phase::Reset;
            action.next = Next::Reset;
        }
        break;
    case Phase::Reset:
        if (!connected)
        {
            state.phase = Phase::Idle;
        }
        else if (connection_changed)
        {
            Debouncing(state, action);
        }
        else if ((event.change & (ChangeBit::Reset | ChangeBit::BhReset)) != 0)
        {
            if (enabled)
            {
                state.phase = Phase::Enumerating;
                action.next = Next::Enumerate;
                action.speed = PortSpeed(state.usb3, event.status);
            }
            else
            {
                state.phase = Phase::Failed;
            }
        }
        else
        {
            action.next = Next::WaitReset;
        }
        break;
    case Phase::Active:
        if (!connected || connection_changed)
        {
            action.detach = true;
            if (connected)
            {
                Debouncing(state, action);
            }
            else
            {
                state.phase = Phase::Idle;
            }
        }
        else if (!enabled)
        {
            action.detach = true;
            state.phase = Phase::Failed;
        }
        break;
    case Phase::Failed:
        if (!connected || connection_changed)
        {
            if (connected)
            {
                Debouncing(state, action);
            }
            else
            {
                state.phase = Phase::Idle;
            }
        }
        break;
    case Phase::PowerOn:
    case Phase::Enumerating:
    case Phase::Off:
        break;
    }
    return action;
}

} // namespace

Action Step(State& state, const Event& event)
{
    Action action;
    switch (event.kind)
    {
    case EventKind::PoweredUp:
        if (state.phase == Phase::PowerOn)
        {
            state.phase = Phase::Idle;
            action.next = Next::ReadStatus;
        }
        return action;
    case EventKind::ResetTimeout:
        if (state.phase == Phase::Reset)
        {
            state.phase = Phase::Failed;
        }
        return action;
    case EventKind::Enumerated:
        if (state.phase == Phase::Enumerating)
        {
            state.phase = Phase::Active;
        }
        return action;
    case EventKind::EnumerationFailed:
        if (state.phase == Phase::Enumerating)
        {
            state.phase = Phase::Failed;
        }
        return action;
    case EventKind::Status:
        break;
    }
    return OnStatus(state, event);
}

uint8_t ClearSelector(bool usb3, uint16_t change_bit)
{
    switch (change_bit)
    {
    case ChangeBit::Connection:
        return Feature::ClearConnection;
    case ChangeBit::Enable:
        return usb3 ? 0 : 17;
    case ChangeBit::Suspend:
        return usb3 ? 0 : 18;
    case ChangeBit::OverCurrent:
        return Feature::ClearOverCurrent;
    case ChangeBit::Reset:
        return Feature::ClearReset;
    case ChangeBit::BhReset:
        return usb3 ? Feature::ClearBhReset : 0;
    case ChangeBit::LinkState:
        return usb3 ? Feature::ClearLinkState : 0;
    case ChangeBit::ConfigError:
        return usb3 ? Feature::ClearConfigError : 0;
    default:
        return 0;
    }
}

uint16_t KnownChanges(bool usb3)
{
    return usb3 ? Usb3Changes : Usb2Changes;
}

Speed PortSpeed(bool usb3, uint16_t status)
{
    if (usb3)
    {
        return ((status >> StatusBit::SuperSpeedShift) & 7U) == 0 ? Speed::Super : Speed::SuperPlus;
    }
    if ((status & StatusBit::LowSpeed) != 0)
    {
        return Speed::Low;
    }
    return (status & StatusBit::HighSpeed) != 0 ? Speed::High : Speed::Full;
}

bool ChildLocation(const HubSite& hub, uint8_t port, Speed speed, Location* child)
{
    const uint32_t depth = static_cast<uint32_t>(hub.location.depth) + 1;
    if (port == 0 || port > MaxPorts || depth > MaxDepth)
    {
        return false;
    }
    *child = {};
    child->speed = speed;
    child->root_port = hub.location.root_port;
    child->depth = static_cast<uint8_t>(depth);
    child->route =
        hub.location.route | (static_cast<uint32_t>(port) << (RouteNibbleBits * (depth - 1)));
    if (speed == Speed::Low || speed == Speed::Full)
    {
        if (hub.location.speed == Speed::High)
        {
            child->tt_hub_slot = hub.slot;
            child->tt_port = hub.multi_tt ? port : 1;
            child->tt_multi = hub.multi_tt;
        }
        else
        {
            child->tt_hub_slot = hub.location.tt_hub_slot;
            child->tt_port = hub.location.tt_port;
            child->tt_multi = hub.location.tt_multi;
        }
    }
    return true;
}

} // namespace FeatherOS::Usb::HubPort
