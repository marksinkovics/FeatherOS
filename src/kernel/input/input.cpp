#include "input.h"

#include "kernel/arch/serial.h"
#include "kernel/input/keyboard.h"
#include "kernel/sync/spinlock.h"
#include "kernel/terminal/terminal.h"

#include <atomic>

namespace FeatherOS::Input
{
namespace
{

// One caller reads the serial port at a time and keeps it while delivering a Ctrl+C, so bytes
// typed after the Ctrl+C cannot be passed on before it from another CPU. A timer tick that finds it
// taken, perhaps by the code it interrupted, skips its poll.
constinit Sync::SpinLock poll_lock;
Terminals::Terminal* serial_target = nullptr;
// Changes when another console is shown, perhaps on another CPU than a key's interrupt.
std::atomic<Terminals::Terminal*> keyboard_target = nullptr;
void (*console_switch)(uint32_t console) = nullptr;
bool (*graphics_keys)(const KeyEvent& event) = nullptr;
std::atomic<uint8_t> current_modifiers = 0;
bool previous_was_carriage_return GUARDED_BY(poll_lock) = false;

} // namespace

void SetTerminal(Terminals::Terminal* terminal)
{
    serial_target = terminal;
    SetKeyboardTerminal(terminal);
}

void SetKeyboardTerminal(Terminals::Terminal* terminal)
{
    keyboard_target.store(terminal, std::memory_order_release);
}

void SetConsoleSwitch(void (*switch_console)(uint32_t console))
{
    console_switch = switch_console;
}

void SetGraphicsKeys(bool (*take_key)(const KeyEvent& event))
{
    graphics_keys = take_key;
}

bool IsInterruptKey(const KeyEvent& event)
{
    return event.code == KeyCode::C && event.transition == KeyTransition::Press &&
           (event.modifiers & Modifier::Control) != 0;
}

uint8_t CurrentModifiers()
{
    return current_modifiers.load(std::memory_order_acquire);
}

void SubmitKey(Keyboard& keyboard, uint16_t code, bool pressed)
{
    char bytes[Keyboard::MaximumKeyBytes];
    const KeyTransition transition = keyboard.Track(code, pressed);
    const size_t length = keyboard.Press(code, pressed, bytes);
    const uint8_t modifiers = keyboard.Modifiers();
    current_modifiers.store(modifiers, std::memory_order_release);
    uint32_t console;
    if (keyboard.TakeConsoleSwitch(&console))
    {
        if (console_switch != nullptr)
        {
            console_switch(console);
        }
        return;
    }
    if (transition != KeyTransition::None && graphics_keys != nullptr &&
        graphics_keys({code, transition, modifiers}))
    {
        return;
    }
    if (length > 0)
    {
        SubmitBytes(bytes, length);
    }
}

void SubmitBytes(const char* bytes, size_t length)
{
    Terminals::Terminal* target = keyboard_target.load(std::memory_order_acquire);
    if (target != nullptr)
    {
        target->Receive(bytes, length);
    }
}

// Each byte goes on alone, so a Ctrl+C reaches the terminal as one.
void Poll()
{
    if (!poll_lock.TryLock())
    {
        return;
    }
    char byte;
    while (Serial::ReadCharacter(&byte))
    {
        const bool after_carriage_return = previous_was_carriage_return;
        previous_was_carriage_return = byte == '\r';
        if ((byte != '\n' || !after_carriage_return) && serial_target != nullptr)
        {
            serial_target->Receive(&byte, 1);
        }
    }
    poll_lock.Unlock();
}

} // namespace FeatherOS::Input
