#ifndef FEATHEROS_USB_HID_KEYBOARD_H
#define FEATHEROS_USB_HID_KEYBOARD_H

#include "kernel/usb/hid/report.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Usb::Hid
{

struct KeyEvent
{
    uint16_t code;
    bool pressed;
};

// A key set holds at most MaxKeys keys and eight modifiers, so a Diff needs room for a release
// and a press of each; with less, events at the end are lost.
constexpr size_t MaxKeyEvents = 2 * (MaxKeys + 8);

// The evdev key code of a keyboard-page usage, 0 for the ones without a key.
uint16_t UsageToKeyCode(uint16_t usage);

// Releases first (keys, then modifiers), then presses (modifiers, then keys), so Shift is down
// when the key pressed with it arrives. A rollover report changes nothing.
size_t Diff(const KeySet& before, const KeySet& after, KeyEvent* out, size_t capacity);
// Diff, then remember the report as what is down, unless it is a rollover.
size_t Advance(KeySet* down, const KeySet& report, KeyEvent* out, size_t capacity);
size_t ReleaseAll(KeySet* down, KeyEvent* out, size_t capacity);

class Repeater
{
public:
    static constexpr uint32_t DelayMilliseconds = 500;
    static constexpr uint32_t RateMilliseconds = 33;

    explicit Repeater(uint32_t ticks_per_second);

    // Modifiers and lock keys are ignored; any other key replaces the one repeating.
    void Press(uint16_t code, uint64_t tick);
    void Release(uint16_t code);
    void Stop();
    bool Due(uint64_t tick, uint16_t* code);
    uint64_t Deadline() const;

private:
    uint64_t m_delay_ticks;
    uint64_t m_rate_ticks;
    uint16_t m_code = 0;
    uint64_t m_next = 0;
};

} // namespace FeatherOS::Usb::Hid

#endif
