#include "keyboard.h"

#include "kernel/input/keyboard.h"

namespace FeatherOS::Usb::Hid
{
namespace
{

// HID Usage Tables 1.4 section 10 (Keyboard/Keypad page) to the Linux evdev key codes.
constexpr uint8_t Letters[26] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                                 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
// 0x1E-0x27 are 1 to 9 and 0, 0x28-0x38 Enter to '/', 0x39 Caps Lock.
constexpr uint8_t Controls[] = {28, 1,  14, 15, 57, 12, 13, 26, 27,
                                43, 43, 39, 40, 41, 51, 52, 53, 58};
// 0x46-0x53: Print Screen, Scroll Lock, Pause, Insert, Home, Page Up, Delete, End, Page Down,
// Right, Left, Down, Up, Num Lock.
constexpr uint8_t Navigation[] = {99,  70,  119, 110, 102, 104, 111,
                                  107, 109, 106, 105, 108, 103, 69};
// 0x54-0x63: keypad / * - + Enter 1 to 9 0 .
constexpr uint8_t Keypad[] = {98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83};
constexpr uint8_t Modifiers[8] = {29, 42, 56, 125, 97, 54, 100, 126};

bool IsRepeatable(uint16_t code)
{
    namespace KeyCode = Input::KeyCode;
    switch (code)
    {
    case KeyCode::LeftControl:
    case KeyCode::LeftShift:
    case KeyCode::RightShift:
    case KeyCode::LeftAlt:
    case KeyCode::RightControl:
    case KeyCode::RightAlt:
    case KeyCode::LeftMeta:
    case KeyCode::RightMeta:
    case KeyCode::CapsLock:
    case KeyCode::NumLock:
    case KeyCode::ScrollLock:
        return false;
    default:
        return code != 0;
    }
}

bool Contains(const KeySet& set, uint16_t usage)
{
    for (size_t i = 0; i < set.count; ++i)
    {
        if (set.keys[i] == usage)
        {
            return true;
        }
    }
    return false;
}

uint64_t TicksFor(uint32_t milliseconds, uint32_t ticks_per_second)
{
    const uint64_t ticks = (uint64_t{milliseconds} * ticks_per_second + 999) / 1000;
    return ticks == 0 ? 1 : ticks;
}

} // namespace

uint16_t UsageToKeyCode(uint16_t usage)
{
    if (usage >= 0x04 && usage <= 0x1D)
    {
        return Letters[usage - 0x04];
    }
    if (usage >= 0x1E && usage <= 0x26)
    {
        return static_cast<uint16_t>(usage - 0x1E + 2);
    }
    if (usage == 0x27)
    {
        return 11;
    }
    if (usage >= 0x28 && usage <= 0x39)
    {
        return Controls[usage - 0x28];
    }
    if (usage >= 0x3A && usage <= 0x43)
    {
        return static_cast<uint16_t>(usage - 0x3A + 59);
    }
    if (usage == 0x44 || usage == 0x45)
    {
        return static_cast<uint16_t>(usage - 0x44 + 87);
    }
    if (usage >= 0x46 && usage <= 0x53)
    {
        return Navigation[usage - 0x46];
    }
    if (usage >= 0x54 && usage <= 0x63)
    {
        return Keypad[usage - 0x54];
    }
    if (usage == 0x64)
    {
        return 86;
    }
    if (usage >= 0xE0 && usage <= 0xE7)
    {
        return Modifiers[usage - 0xE0];
    }
    return 0;
}

size_t Diff(const KeySet& before, const KeySet& after, KeyEvent* out, size_t capacity)
{
    if (after.rollover)
    {
        return 0;
    }
    size_t length = 0;
    const auto emit = [&](uint16_t usage, bool pressed)
    {
        const uint16_t code = UsageToKeyCode(usage);
        if (code != 0 && length < capacity)
        {
            out[length++] = {code, pressed};
        }
    };
    for (size_t i = 0; i < before.count; ++i)
    {
        if (!Contains(after, before.keys[i]))
        {
            emit(before.keys[i], false);
        }
    }
    for (uint16_t bit = 0; bit < 8; ++bit)
    {
        if ((before.modifiers & ~after.modifiers & (1U << bit)) != 0)
        {
            emit(static_cast<uint16_t>(0xE0 + bit), false);
        }
    }
    for (uint16_t bit = 0; bit < 8; ++bit)
    {
        if ((after.modifiers & ~before.modifiers & (1U << bit)) != 0)
        {
            emit(static_cast<uint16_t>(0xE0 + bit), true);
        }
    }
    for (size_t i = 0; i < after.count; ++i)
    {
        if (!Contains(before, after.keys[i]))
        {
            emit(after.keys[i], true);
        }
    }
    return length;
}

size_t Advance(KeySet* down, const KeySet& report, KeyEvent* out, size_t capacity)
{
    const size_t length = Diff(*down, report, out, capacity);
    if (!report.rollover)
    {
        *down = report;
    }
    return length;
}

size_t ReleaseAll(KeySet* down, KeyEvent* out, size_t capacity)
{
    const size_t length = Diff(*down, KeySet{}, out, capacity);
    *down = KeySet{};
    return length;
}

Repeater::Repeater(uint32_t ticks_per_second)
    : m_delay_ticks(TicksFor(DelayMilliseconds, ticks_per_second)),
      m_rate_ticks(TicksFor(RateMilliseconds, ticks_per_second))
{
}

void Repeater::Press(uint16_t code, uint64_t tick)
{
    if (!IsRepeatable(code))
    {
        return;
    }
    m_code = code;
    m_next = tick + m_delay_ticks;
}

void Repeater::Release(uint16_t code)
{
    if (code == m_code)
    {
        m_code = 0;
    }
}

void Repeater::Stop()
{
    m_code = 0;
}

bool Repeater::Due(uint64_t tick, uint16_t* code)
{
    if (m_code == 0 || tick < m_next)
    {
        return false;
    }
    *code = m_code;
    m_next += m_rate_ticks;
    if (m_next <= tick)
    {
        m_next = tick + m_rate_ticks;
    }
    return true;
}

uint64_t Repeater::Deadline() const
{
    return m_code == 0 ? UINT64_MAX : m_next;
}

} // namespace FeatherOS::Usb::Hid
