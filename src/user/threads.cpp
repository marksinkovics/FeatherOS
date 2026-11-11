#include "command_line.h"
#include "prime_count.h"

#include <chrono>
#include <pthread.h>
#include <print>
#include <stdio.h>

using namespace FeatherOS::User;

constexpr char Help[] =
    "usage: threads <threads 1-15> [limit]\n"
    "Counts the primes below limit (default 10000000, at most 1000000000) with threads\n"
    "of one process, and shows the CPUs each ran on. primes does the same with worker\n"
    "processes.";

namespace
{

// The main thread only waits, so up to 15 more fit in a process.
constexpr unsigned long long MaximumWorkers = 15;
constexpr unsigned long long DefaultLimit = 10000000;
constexpr unsigned long long MaximumLimit = 1000000000;

struct Work
{
    unsigned long long index;
    unsigned long long count;
    unsigned long long limit;
    PrimeCount result;
};

void* RunWorker(void* argument)
{
    auto* work = static_cast<Work*>(argument);
    work->result = CountOddPrimes(work->index, work->count, work->limit);
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    if (HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    unsigned long long workers = 0;
    unsigned long long limit = DefaultLimit;
    if ((argc != 2 && argc != 3) || !ParseUnsigned(argv[1], &workers) || workers == 0 ||
        workers > MaximumWorkers ||
        (argc == 3 && (!ParseUnsigned(argv[2], &limit) || limit > MaximumLimit)))
    {
        puts("usage: threads <threads 1-15> [limit up to 1000000000]");
        return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    Work work[MaximumWorkers];
    pthread_t threads[MaximumWorkers];
    for (unsigned long long index = 0; index < workers; ++index)
    {
        work[index] = {index, workers, limit, {0, 0}};
        if (pthread_create(&threads[index], nullptr, RunWorker, &work[index]) != 0)
        {
            puts("threads: could not start a thread");
            return 1;
        }
    }
    // 2 is the only even prime; the threads test odd numbers.
    unsigned long long total = limit > 2 ? 1 : 0;
    for (unsigned long long index = 0; index < workers; ++index)
    {
        pthread_join(threads[index], nullptr);
        total += work[index].result.primes;
        std::print("threads: thread {} found {} on CPU ", index, work[index].result.primes);
        WriteCpus(work[index].result.cpus);
        putchar('\n');
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::println("threads: {} primes below {} with {} {} in {} ms", total, limit, workers,
                 workers == 1 ? "thread" : "threads", elapsed.count());
    return 0;
}
