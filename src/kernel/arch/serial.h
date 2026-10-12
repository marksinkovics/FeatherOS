#ifndef FEATHEROS_ARCH_SERIAL_H
#define FEATHEROS_ARCH_SERIAL_H

#include <stdint.h>

namespace FeatherOS
{
namespace Serial
{

void Init(uint64_t hhdm_offset);
void Write(const char* text);

} // namespace Serial
} // namespace FeatherOS

#endif