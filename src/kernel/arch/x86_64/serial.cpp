#include <stdint.h>

#include "kernel/arch/serial.h"

namespace FeatherOS
{
namespace Serial
{

namespace
{

const uint16_t Port = 0x3F8; // Legacy COM1 base I/O port.

void Out(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t In(uint16_t port)
{
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void WriteCharacter(char character)
{
    // Wait for the transmitter holding register to empty, but don't hang forever
    // if no serial device is responding.
    for (uint32_t attempt = 0; attempt < 100000; ++attempt)
    {
        if (In(Port + 5) & 0x20U)
        {
            Out(Port, (uint8_t)character);
            return;
        }
    }
}

} // namespace

void Init(uint64_t hhdm_offset)
{
    (void)hhdm_offset;

    // Disable UART interrupts while configuring the device.
    Out(Port + 1, 0x00);

    // Enable the divisor latch and set divisor 3: 38400 baud from a 115200 clock.
    Out(Port + 3, 0x80);
    Out(Port + 0, 0x03);
    Out(Port + 1, 0x00);

    // Disable the divisor latch; select 8 data bits, no parity, one stop bit.
    Out(Port + 3, 0x03);

    // Enable and clear FIFOs; assert DTR, RTS, and OUT2.
    Out(Port + 2, 0xC7);
    Out(Port + 4, 0x0B);
}

void Write(const char* text)
{
    if (text == nullptr)
    {
        return;
    }

    while (*text != '\0')
    {
        if (*text == '\n')
        {
            // Serial terminals expect CRLF for a new line.
            WriteCharacter('\r');
        }
        WriteCharacter(*text++);
    }
}

bool ReadCharacter(char* character)
{
    if (character == nullptr || (In(Port + 5) & 0x01U) == 0)
    {
        return false;
    }
    *character = static_cast<char>(In(Port));
    return true;
}

} // namespace Serial
} // namespace FeatherOS