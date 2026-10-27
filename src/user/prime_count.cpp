#include "prime_count.h"

#include <sched.h>
#include <stdio.h>

namespace FeatherOS::User
{
namespace
{

constexpr unsigned CpuMaskBits = 64;
constexpr unsigned long long CpuSampleInterval = 1024;

bool IsOddPrime(unsigned long long candidate)
{
    for (unsigned long long divisor = 3; divisor * divisor <= candidate; divisor += 2)
    {
        if (candidate % divisor == 0)
        {
            return false;
        }
    }
    return true;
}

unsigned long long CurrentCpuBit()
{
    return 1ULL << (static_cast<unsigned>(sched_getcpu()) % CpuMaskBits);
}

} // namespace

PrimeCount CountOddPrimes(unsigned long long index, unsigned long long count,
                          unsigned long long limit)
{
    PrimeCount result = {0, 0};
    unsigned long long tested = 0;
    for (unsigned long long candidate = 3 + 2 * index; candidate < limit; candidate += 2 * count)
    {
        if (tested++ % CpuSampleInterval == 0)
        {
            result.cpus |= CurrentCpuBit();
        }
        result.primes += IsOddPrime(candidate) ? 1 : 0;
    }
    result.cpus |= CurrentCpuBit();
    return result;
}

void WriteCpus(unsigned long long cpus)
{
    bool first = true;
    for (unsigned cpu = 0; cpu < CpuMaskBits; ++cpu)
    {
        if ((cpus & (1ULL << cpu)) != 0)
        {
            printf(first ? "%u" : ",%u", cpu);
            first = false;
        }
    }
}

} // namespace FeatherOS::User
