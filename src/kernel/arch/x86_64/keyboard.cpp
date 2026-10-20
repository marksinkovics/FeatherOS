#include "keyboard.h"

#include "interrupts.h"
#include "kernel/arch/keyboard.h"
#include "kernel/input/input.h"

namespace FeatherOS::Arch::X86_64
{
namespace
{

constexpr uint16_t KeyboardDataPort = 0x60;
constexpr uint16_t KeyboardStatusPort = 0x64;
constexpr uint16_t KeyboardCommandPort = 0x64;

bool shift_pressed = false;
bool control_pressed = false;
bool caps_lock_enabled = false;
bool extended_scan_code = false;

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

char TranslateScanCode(uint8_t scan_code)
{
    static constexpr char number_row[] = "1234567890";
    static constexpr char shifted_number_row[] = "!@#$%^&*()";
    char character = '\0';

    if (scan_code >= 0x02 && scan_code <= 0x0B)
    {
        const uint8_t index = scan_code == 0x0B ? 9 : scan_code - 0x02;
        character = shift_pressed ? shifted_number_row[index] : number_row[index];
    }
    else if (scan_code >= 0x10 && scan_code <= 0x19)
    {
        character = "qwertyuiop"[scan_code - 0x10];
    }
    else if (scan_code >= 0x1E && scan_code <= 0x26)
    {
        character = "asdfghjkl"[scan_code - 0x1E];
    }
    else if (scan_code >= 0x2C && scan_code <= 0x32)
    {
        character = "zxcvbnm"[scan_code - 0x2C];
    }
    else
    {
        switch (scan_code)
        {
        case 0x0C:
            character = shift_pressed ? '_' : '-';
            break;
        case 0x0D:
            character = shift_pressed ? '+' : '=';
            break;
        case 0x1A:
            character = shift_pressed ? '{' : '[';
            break;
        case 0x1B:
            character = shift_pressed ? '}' : ']';
            break;
        case 0x27:
            character = shift_pressed ? ':' : ';';
            break;
        case 0x28:
            character = shift_pressed ? '"' : '\'';
            break;
        case 0x29:
            character = shift_pressed ? '~' : '`';
            break;
        case 0x2B:
            character = shift_pressed ? '|' : '\\';
            break;
        case 0x33:
            character = shift_pressed ? '<' : ',';
            break;
        case 0x34:
            character = shift_pressed ? '>' : '.';
            break;
        case 0x35:
            character = shift_pressed ? '?' : '/';
            break;
        case 0x39:
            character = ' ';
            break;
        default:
            break;
        }
    }

    if (character >= 'a' && character <= 'z' && (shift_pressed != caps_lock_enabled))
    {
        character = static_cast<char>(character - 'a' + 'A');
    }
    return character;
}

void OnKeyboardScanCode(uint8_t scan_code)
{
    if (scan_code == 0xE0)
    {
        extended_scan_code = true;
        return;
    }
    const bool released = (scan_code & 0x80U) != 0;
    if (extended_scan_code)
    {
        extended_scan_code = false;
        // Right Ctrl is E0 1D; other extended keys are ignored.
        if ((scan_code & 0x7FU) == 0x1D)
        {
            control_pressed = !released;
        }
        return;
    }

    scan_code &= 0x7FU;
    if (scan_code == 0x1D)
    {
        control_pressed = !released;
        return;
    }
    if (scan_code == 0x2A || scan_code == 0x36)
    {
        shift_pressed = !released;
        return;
    }
    if (scan_code == 0x3A)
    {
        if (!released)
        {
            caps_lock_enabled = !caps_lock_enabled;
        }
        return;
    }
    if (released)
    {
        return;
    }
    if (scan_code == 0x0E)
    {
        Input::SubmitEvent({Input::EventType::Backspace, '\0'});
    }
    else if (scan_code == 0x1C)
    {
        Input::SubmitEvent({Input::EventType::Enter, '\0'});
    }
    else if (control_pressed)
    {
        if (scan_code == 0x2E)
        {
            Input::SubmitEvent({Input::EventType::Interrupt, '\0'});
        }
    }
    else
    {
        const char character = TranslateScanCode(scan_code);
        if (character != '\0')
        {
            Input::SubmitEvent({Input::EventType::Character, character});
        }
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