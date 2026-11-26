#include "keyboard.h"

#include "acpi.h"
#include "i8042.h"
#include "interrupts.h"
#include "kernel/arch/keyboard.h"
#include "kernel/boot_options.h"
#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"
#include "kernel/klogger.h"
#include "kernel/string/format.h"

namespace FeatherOS::Arch::X86_64
{
namespace
{

constexpr uint16_t KeyboardDataPort = 0x60;
constexpr uint16_t KeyboardStatusPort = I8042::CommandPort;
constexpr uint16_t KeyboardCommandPort = I8042::CommandPort;
constexpr uint8_t EnableFirstPortCommand = 0xAE;

constinit KLogger logger;
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
    case 0x38:
        return Input::KeyCode::RightAlt;
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
    case 0x5B:
        return Input::KeyCode::LeftMeta;
    case 0x5C:
        return Input::KeyCode::RightMeta;
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
    if (code != 0)
    {
        Input::SubmitKey(keyboard, code, pressed);
    }
}

class HardwarePorts final : public I8042::Ports
{
public:
    uint8_t Read8(uint16_t port) override
    {
        return ReadPort(port);
    }

    void Write8(uint16_t port, uint8_t value) override
    {
        WritePort(port, value);
    }
};

void LogFadtFlag()
{
    uint16_t flags;
    if (!Acpi::BootArchitectureFlags(&flags))
    {
        logger.Info("ACPI: FADT 8042 flag unknown");
    }
    else if ((flags & Acpi::BootArchitecture8042) != 0)
    {
        logger.Info("ACPI: FADT 8042 flag set");
    }
    else
    {
        logger.Info("ACPI: FADT 8042 flag clear");
    }
}

void LogSetup(const I8042::Setup& setup)
{
    char message[80];
    if (String::FormatTo(
            message, sizeof(message), "Keyboard: i8042 config 0x{}{}, was 0x{}{}, flushed {} bytes",
            "0123456789abcdef"[setup.config >> 4], "0123456789abcdef"[setup.config & 0xF],
            "0123456789abcdef"[setup.original >> 4], "0123456789abcdef"[setup.original & 0xF],
            setup.flushed))
    {
        logger.Info(message);
    }
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
    LogFadtFlag();
    HardwarePorts ports;
    const I8042::Setup setup = I8042::Configure(ports);
    if (setup.status == I8042::Status::NoController)
    {
        logger.Info("Keyboard: no i8042 controller");
        return false;
    }
    if (setup.status == I8042::Status::TimedOut)
    {
        logger.Warning("Keyboard: i8042 did not answer");
        return false;
    }
    LogSetup(setup);
    if (!I8042::WaitForInputEmpty(ports))
    {
        logger.Warning("Keyboard: i8042 did not answer");
        return false;
    }
    RegisterInterruptHandler(1, OnKeyboardInterrupt);
    WritePort(KeyboardCommandPort, EnableFirstPortCommand);
    uint32_t flushed;
    (void)I8042::Flush(ports, &flushed);
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
    if (BootOptions::SelectedPs2() == BootOptions::Ps2Mode::Off)
    {
        return false;
    }
    return X86_64::InitializeKeyboard();
}

} // namespace FeatherOS::Arch::Keyboard