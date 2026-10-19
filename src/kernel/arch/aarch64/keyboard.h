#ifndef FEATHEROS_AARCH64_KEYBOARD_H
#define FEATHEROS_AARCH64_KEYBOARD_H

#include <stdint.h>

namespace FeatherOS::Arch::AArch64
{

bool InitializeKeyboard(const void* device_tree, uint64_t hhdm_offset);

} // namespace FeatherOS::Arch::AArch64

#endif