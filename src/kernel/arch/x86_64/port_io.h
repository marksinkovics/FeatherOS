#ifndef FEATHEROS_X86_64_PORT_IO_H
#define FEATHEROS_X86_64_PORT_IO_H

#include <stdint.h>

namespace FeatherOS::Arch::X86_64::PortIo
{

void Write8(uint16_t port, uint8_t value);
void Write16(uint16_t port, uint16_t value);
void Write32(uint16_t port, uint32_t value);
uint8_t Read8(uint16_t port);
uint16_t Read16(uint16_t port);
uint32_t Read32(uint16_t port);

} // namespace FeatherOS::Arch::X86_64::PortIo

#endif
