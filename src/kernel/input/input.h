#ifndef FEATHEROS_INPUT_H
#define FEATHEROS_INPUT_H

#include "kernel/input/keyboard.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Terminals
{
class Terminal;
} // namespace FeatherOS::Terminals

// The consoles' input: bytes from the serial port go to the first console's terminal, and keys to
// the terminal of the console that is shown, as a terminal sends them (see Keyboard). A serial
// "\r\n" counts as one '\r'.
namespace FeatherOS::Input
{

// Both the serial and the keyboard terminal.
void SetTerminal(Terminals::Terminal* terminal);
void SetKeyboardTerminal(Terminals::Terminal* terminal);
// Called for Alt+F1 to Alt+F4 with console 0 to 3, perhaps in interrupt context.
void SetConsoleSwitch(void (*switch_console)(uint32_t console));
struct KeyEvent
{
    uint16_t code;
    KeyTransition transition;
    uint8_t modifiers;
};
// Called for every key but a console switch, perhaps in interrupt context; true if it took the
// key, so its bytes do not go to the terminal.
void SetGraphicsKeys(bool (*take_key)(const KeyEvent& event));
// A Press of C with Control.
bool IsInterruptKey(const KeyEvent& event);
// The modifiers of the keyboard that last submitted a key, for a console switch it caused.
uint8_t CurrentModifiers();
// A key event from a keyboard driver.
void SubmitKey(Keyboard& keyboard, uint16_t code, bool pressed);
// A key's bytes, passed on together.
void SubmitBytes(const char* bytes, size_t length);
// Caps Lock and Num Lock belong to every keyboard at once: a lock key on any of them toggles
// them for all. Each change calls the callback with the new LedState, perhaps in interrupt
// context.
constexpr uint8_t LedNumLock = 1;
constexpr uint8_t LedCapsLock = 2;
bool CapsLock();
bool NumLock();
void ToggleCapsLock();
void ToggleNumLock();
void SetLockState(bool caps_lock, bool num_lock);
// The lock LEDs in the bit layout of a boot keyboard's LED report.
uint8_t LedState();
void SetLedCallback(void (*callback)(uint8_t led_state));
// Reads what the serial port received.
void Poll();

} // namespace FeatherOS::Input

#endif
