#include "port_io.h"

namespace FeatherOS::Arch::X86_64::PortIo
{

void Write8(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

void Write16(uint16_t port, uint16_t value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void Write32(uint16_t port, uint32_t value)
{
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t Read8(uint16_t port)
{
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint16_t Read16(uint16_t port)
{
    uint16_t value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t Read32(uint16_t port)
{
    uint32_t value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

} // namespace FeatherOS::Arch::X86_64::PortIo
