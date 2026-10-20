#include "random.h"

#include "kernel/arch/cpu_features.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Random
{
namespace
{

Sync::SpinLock random_lock;
uint64_t state = 0;

uint64_t ReadCycleCounter()
{
#if defined(ARCH_x86_64)
    uint32_t low;
    uint32_t high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<uint64_t>(high) << 32) | low;
#elif defined(ARCH_aarch64)
    uint64_t count;
    asm volatile("isb\n\tmrs %0, CNTPCT_EL0" : "=r"(count));
    return count;
#else
#error "Unsupported architecture for the random source"
#endif
}

} // namespace

// Hardware randomness is mixed in when the CPU has it; otherwise only the cycle counter, which
// is unpredictable enough for address randomization but not cryptographic.
uint64_t Next()
{
    uint64_t hardware = 0;
    Arch::CpuFeatures::ReadHardwareRandom(&hardware);
    Sync::SpinLockGuard guard(random_lock);
    state += 0x9E3779B97F4A7C15 ^ ReadCycleCounter() ^ hardware;
    uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EB;
    return value ^ (value >> 31);
}

} // namespace FeatherOS::Random
