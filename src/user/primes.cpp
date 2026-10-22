#include "command_line.h"
#include "process.h"
#include "syscall.h"

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

using namespace FeatherOS::User;

namespace
{

constexpr uint64_t MaximumWorkers = 16;
constexpr uint64_t DefaultLimit = 10000000;
constexpr uint64_t MaximumLimit = 1000000000;
// Workers return their result as an exit status, since there are no pipes yet: the prime count
// above a mask of the CPUs they ran on. That needs the kernel's 64-bit status, which exit and
// waitpid cut to 8 bits, so workers are started, ended and waited for with raw syscalls.
constexpr uint64_t CpuMaskBits = 8;
constexpr uint64_t CpuSampleInterval = 1024;

bool IsOddPrime(uint64_t candidate)
{
    for (uint64_t divisor = 3; divisor * divisor <= candidate; divisor += 2)
    {
        if (candidate % divisor == 0)
        {
            return false;
        }
    }
    return true;
}

// Worker index of count tests every count-th odd number from 3, so all workers get similar
// numbers and finish together.
[[noreturn]] void RunWorker(uint64_t index, uint64_t count, uint64_t limit)
{
    uint64_t primes = 0;
    uint64_t cpus = 0;
    uint64_t tested = 0;
    for (uint64_t candidate = 3 + 2 * index; candidate < limit; candidate += 2 * count)
    {
        if (tested++ % CpuSampleInterval == 0)
        {
            cpus |= 1ULL << (static_cast<uint64_t>(sched_getcpu()) % CpuMaskBits);
        }
        primes += IsOddPrime(candidate) ? 1 : 0;
    }
    cpus |= 1ULL << (static_cast<uint64_t>(sched_getcpu()) % CpuMaskBits);
    Exit(primes << CpuMaskBits | cpus);
}

void WriteCpus(uint64_t cpus)
{
    bool first = true;
    for (uint64_t cpu = 0; cpu < CpuMaskBits; ++cpu)
    {
        if ((cpus & (1ULL << cpu)) != 0)
        {
            printf(first ? "%llu" : ",%llu", static_cast<unsigned long long>(cpu));
            first = false;
        }
    }
}

uint64_t MillisecondsSinceBoot()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

} // namespace

constexpr char Help[] =
    "usage: primes <workers 1-16> [limit]\n"
    "Counts the primes below limit (default 10000000, at most 1000000000) with\n"
    "worker processes and shows the CPUs each ran on. Compare primes 1 with primes 4.";

int main(int argc, char** argv)
{
    if (HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    unsigned long long workers = 0;
    unsigned long long limit = DefaultLimit;
    unsigned long long index = 0;
    if (argc == 5 && strcmp(argv[1], "worker") == 0 && ParseUnsigned(argv[2], &index) &&
        ParseUnsigned(argv[3], &workers) && ParseUnsigned(argv[4], &limit))
    {
        RunWorker(index, workers, limit);
    }
    if ((argc != 2 && argc != 3) || !ParseUnsigned(argv[1], &workers) || workers == 0 ||
        workers > MaximumWorkers ||
        (argc == 3 && (!ParseUnsigned(argv[2], &limit) || limit > MaximumLimit)))
    {
        puts("usage: primes <workers 1-16> [limit up to 1000000000]");
        return 1;
    }

    const uint64_t start = MillisecondsSinceBoot();
    uint64_t children[MaximumWorkers];
    for (uint64_t worker = 0; worker < workers; ++worker)
    {
        char command[MaximumCommandLineLength];
        snprintf(command, sizeof(command), "primes worker %llu %llu %llu",
                 static_cast<unsigned long long>(worker), workers, limit);
        children[worker] = Spawn(command);
        if (IsSyscallError(children[worker]))
        {
            puts("primes: could not start a worker");
            return 1;
        }
    }

    // 2 is the only even prime; the workers test odd numbers.
    uint64_t total = limit > 2 ? 1 : 0;
    for (uint64_t worker = 0; worker < workers; ++worker)
    {
        uint64_t status;
        if (IsSyscallError(Wait(children[worker], &status)) || status == ExitStatusFault ||
            status == ExitStatusKernelAccessFault)
        {
            puts("primes: a worker failed");
            return 1;
        }
        const uint64_t primes = status >> CpuMaskBits;
        total += primes;
        printf("primes: worker %llu found %llu on CPU ", static_cast<unsigned long long>(worker),
               static_cast<unsigned long long>(primes));
        WriteCpus(status & ((1ULL << CpuMaskBits) - 1));
        putchar('\n');
    }
    const uint64_t elapsed = MillisecondsSinceBoot() - start;
    printf("primes: %llu primes below %llu with %llu %s in %llu ms\n",
           static_cast<unsigned long long>(total), limit, workers,
           workers == 1 ? "worker" : "workers", static_cast<unsigned long long>(elapsed));
    return 0;
}
