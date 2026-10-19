#ifndef FEATHEROS_ARCH_KEYBOARD_H
#define FEATHEROS_ARCH_KEYBOARD_H

#include <stdint.h>

namespace FeatherOS::Arch::Keyboard
{

bool Initialize(const void* platform_data, uint64_t hhdm_offset);

} // namespace FeatherOS::Arch::Keyboard

#endif