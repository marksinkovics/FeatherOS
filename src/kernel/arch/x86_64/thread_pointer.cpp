#include "kernel/arch/thread_pointer.h"

namespace FeatherOS::Arch::ThreadPointer
{
namespace
{

// IA32_FS_BASE. User code cannot change it: CR4.FSGSBASE stays clear, so WRFSBASE faults.
constexpr uint32_t FsBaseRegister = 0xC0000100;

} // namespace

uint64_t Read()
{
    uint32_t low;
    uint32_t high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(FsBaseRegister));
    return (static_cast<uint64_t>(high) << 32) | low;
}

void Write(uint64_t value)
{
    asm volatile("wrmsr"
                 :
                 : "c"(FsBaseRegister), "a"(static_cast<uint32_t>(value)),
                   "d"(static_cast<uint32_t>(value >> 32)));
}

} // namespace FeatherOS::Arch::ThreadPointer
