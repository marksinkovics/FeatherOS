#ifndef FEATHEROS_USB_HUB_STATE_H
#define FEATHEROS_USB_HUB_STATE_H

#include "kernel/usb/host_controller.h"

#include <stdint.h>

namespace FeatherOS::Usb::HubPort
{

constexpr uint32_t MaxPorts = 15;
constexpr uint32_t MaxDepth = 5;
constexpr uint32_t DebounceMilliseconds = 100;
constexpr uint32_t ResetTimeoutMilliseconds = 500;
constexpr uint32_t MaxDebounceRounds = 15;

namespace Feature
{
constexpr uint8_t PortEnable = 1;
constexpr uint8_t PortReset = 4;
constexpr uint8_t PortPower = 8;
constexpr uint8_t ClearConnection = 16;
constexpr uint8_t ClearOverCurrent = 19;
constexpr uint8_t ClearReset = 20;
constexpr uint8_t ClearLinkState = 25;
constexpr uint8_t ClearConfigError = 26;
constexpr uint8_t ClearBhReset = 29;
} // namespace Feature

namespace StatusBit
{
constexpr uint16_t Connection = 0x0001;
constexpr uint16_t Enable = 0x0002;
constexpr uint16_t OverCurrent = 0x0008;
constexpr uint16_t Reset = 0x0010;
constexpr uint16_t LowSpeed = 0x0200;
constexpr uint16_t HighSpeed = 0x0400;
constexpr uint16_t SuperSpeedShift = 10;
} // namespace StatusBit

namespace ChangeBit
{
constexpr uint16_t Connection = 0x0001;
constexpr uint16_t Enable = 0x0002;
constexpr uint16_t Suspend = 0x0004;
constexpr uint16_t OverCurrent = 0x0008;
constexpr uint16_t Reset = 0x0010;
constexpr uint16_t BhReset = 0x0020;
constexpr uint16_t LinkState = 0x0040;
constexpr uint16_t ConfigError = 0x0080;
} // namespace ChangeBit

enum class Phase : uint8_t
{
    PowerOn,
    Idle,
    Debounce,
    Reset,
    Enumerating,
    Active,
    Failed,
    Off,
};

struct State
{
    Phase phase = Phase::PowerOn;
    bool usb3 = false;
    uint8_t rounds = 0;
};

enum class EventKind : uint8_t
{
    PoweredUp,
    Status,
    ResetTimeout,
    Enumerated,
    EnumerationFailed,
};

// status and change are the two words GET_STATUS(port) returned.
struct Event
{
    EventKind kind;
    uint16_t status = 0;
    uint16_t change = 0;
};

enum class Next : uint8_t
{
    None,
    ReadStatus,
    Debounce,
    Reset,
    WaitReset,
    Enumerate,
};

// The caller does the parts in this order: clear the change bits, detach the child, power the
// port off, then `next`.
struct Action
{
    uint16_t clear = 0;
    bool detach = false;
    bool power_off = false;
    Next next = Next::None;
    Speed speed = Speed::Full;
};

Action Step(State& state, const Event& event);

// The feature selector that clears one change bit (a ChangeBit value) on a hub of this kind;
// 0 for a bit the hub does not define.
uint8_t ClearSelector(bool usb3, uint16_t change_bit);
uint16_t KnownChanges(bool usb3);
Speed PortSpeed(bool usb3, uint16_t status);

struct HubSite
{
    Location location;
    uint32_t slot;
    bool multi_tt;
};

// False when the child would be deeper than MaxDepth or the port is not 1 to MaxPorts.
bool ChildLocation(const HubSite& hub, uint8_t port, Speed speed, Location* child);

} // namespace FeatherOS::Usb::HubPort

#endif
