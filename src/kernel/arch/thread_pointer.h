#ifndef FEATHEROS_ARCH_THREAD_POINTER_H
#define FEATHEROS_ARCH_THREAD_POINTER_H

#include <stdint.h>

// The user thread pointer that thread-local variables are found through: the FS base on x86-64,
// TPIDR_EL0 on AArch64. The kernel never uses either, so it only changes on a context switch.
namespace FeatherOS::Arch::ThreadPointer
{

uint64_t Read();
void Write(uint64_t value);

} // namespace FeatherOS::Arch::ThreadPointer

#endif
