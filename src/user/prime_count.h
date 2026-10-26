#ifndef FEATHEROS_USER_PRIME_COUNT_H
#define FEATHEROS_USER_PRIME_COUNT_H

// The work primes and threads share. Worker index of count tests every count-th odd number from
// 3 by trial division, so all workers get similar numbers and finish together. It uses no C
// library state, so threads may run it at once.
namespace FeatherOS::User
{

struct PrimeCount
{
    unsigned long long primes;
    // Bit n is set when the worker ran on CPU n at some point.
    unsigned long long cpus;
};

PrimeCount CountOddPrimes(unsigned long long index, unsigned long long count,
                          unsigned long long limit);
// Prints the set bits of cpus as "0,2,3".
void WriteCpus(unsigned long long cpus);

} // namespace FeatherOS::User

#endif
