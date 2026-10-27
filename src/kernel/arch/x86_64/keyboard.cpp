#include "keyboard.h"

#include "interrupts.h"
#include "kernel/arch/keyboard.h"
#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"

namespace FeatherOS::Arch::X86_64
{
namespace
{

constexpr uint16_t KeyboardDataPort = 0x60;
constexpr uint16_t KeyboardStatusPort = 0x64;
constexpr uint16_t KeyboardCommandPort = 0x64;

Input::Keyboard keyboard;
bool extended_scan_code = false;
// Pause sends E1 1D 45 E1 9D C5, which no key code stands for.
uint32_t pause_bytes_left = 0;

uint8_t ReadPort(uint16_t port)
{
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void WritePort(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

// Set 1 scancodes below 0x59 are the key codes themselves; E0-prefixed keys get their own codes.
uint16_t ExtendedKeyCode(uint8_t scan_code)
{
    switch (scan_code)
    {
    case 0x1C:
        return Input::KeyCode::KeypadEnter;
    case 0x1D:
        return Input::KeyCode::RightControl;
    case 0x47:
        return Input::KeyCode::Home;
    case 0x48:
        return Input::KeyCode::Up;
    case 0x49:
        return Input::KeyCode::PageUp;
    case 0x4B:
        return Input::KeyCode::Left;
    case 0x4D:
        return Input::KeyCode::Right;
    case 0x4F:
        return Input::KeyCode::End;
    case 0x50:
        return Input::KeyCode::Down;
    case 0x51:
        return Input::KeyCode::PageDown;
    case 0x52:
        return Input::KeyCode::Insert;
    case 0x53:
        return Input::KeyCode::Delete;
    default:
        return 0;
    }
}

void OnKeyboardScanCode(uint8_t scan_code)
{
    if (pause_bytes_left > 0)
    {
        --pause_bytes_left;
        return;
    }
    if (scan_code == 0xE1)
    {
        pause_bytes_left = 2;
        return;
    }
    if (scan_code == 0xE0)
    {
        extended_scan_code = true;
        return;
    }
    const bool pressed = (scan_code & 0x80U) == 0;
    const auto base = static_cast<uint8_t>(scan_code & 0x7FU);
    const uint16_t code = extended_scan_code ? ExtendedKeyCode(base) : base < 0x59 ? base : 0;
    extended_scan_code = false;
    char bytes[Input::Keyboard::MaximumKeyBytes];
    const size_t length = code == 0 ? 0 : keyboard.Press(code, pressed, bytes);
    if (length > 0)
    {
        Input::SubmitBytes(bytes, length);
    }
}

bool WaitForControllerWrite()
{
    for (uint32_t attempt = 0; attempt < 100000; ++attempt)
    {
        if ((ReadPort(KeyboardStatusPort) & 0x02U) == 0)
        {
            return true;
        }
    }
    return false;
}

void OnKeyboardInterrupt()
{
    if ((ReadPort(KeyboardStatusPort) & 0x01U) != 0)
    {
        const uint8_t scan_code = ReadPort(KeyboardDataPort);
        OnKeyboardScanCode(scan_code);
    }
}

} // namespace

bool InitializeKeyboard()
{
    if (!WaitForControllerWrite())
    {
        return false;
    }
    RegisterInterruptHandler(1, OnKeyboardInterrupt);
    WritePort(KeyboardCommandPort, 0xAE);
    UnmaskIrq(1);
    return true;
}

} // namespace FeatherOS::Arch::X86_64

namespace FeatherOS::Arch::Keyboard
{

bool Initialize(const void* platform_data, uint64_t hhdm_offset)
{
    (void)platform_data;
    (void)hhdm_offset;
    return X86_64::InitializeKeyboard();
}

} // namespace FeatherOS::Arch::Keyboard