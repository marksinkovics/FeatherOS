#include "command_line.h"
#include "prime_count.h"

#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using namespace FeatherOS::User;

namespace
{

constexpr unsigned long long MaximumWorkers = 16;
constexpr unsigned long long DefaultLimit = 10000000;
constexpr unsigned long long MaximumLimit = 1000000000;

// Each worker writes one line to its pipe: its prime count and a mask of the CPUs it ran on.
int RunWorker(unsigned long long index, unsigned long long count, unsigned long long limit)
{
    const PrimeCount result = CountOddPrimes(index, count, limit);
    printf("%llu %llu\n", result.primes, result.cpus);
    return 0;
}

uint64_t MillisecondsSinceBoot()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

// Starts worker index with its output going into a new pipe; returns the pipe's read end, or -1.
int StartWorker(unsigned long long index, unsigned long long workers, unsigned long long limit,
                pid_t* child)
{
    char index_text[24];
    char workers_text[24];
    char limit_text[24];
    snprintf(index_text, sizeof(index_text), "%llu", index);
    snprintf(workers_text, sizeof(workers_text), "%llu", workers);
    snprintf(limit_text, sizeof(limit_text), "%llu", limit);
    char name[] = "primes";
    char mode[] = "worker";
    char* arguments[] = {name, mode, index_text, workers_text, limit_text, nullptr};
    // Close-on-exec, so the workers get only the copy at their standard output, and the read
    // ends see the end of the data once each worker exits.
    int ends[2];
    if (pipe2(ends, O_CLOEXEC) != 0)
    {
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
    const int error = posix_spawn(child, "primes", &actions, nullptr, arguments, environ);
    close(ends[1]);
    if (error != 0)
    {
        close(ends[0]);
        return -1;
    }
    return ends[0];
}

// Reads the worker's line and reaps it; false if it did not finish its count.
bool CollectWorker(int output, pid_t child, unsigned long long* primes, unsigned long long* cpus)
{
    FILE* stream = fdopen(output, "r");
    const bool parsed = stream != nullptr && fscanf(stream, "%llu %llu", primes, cpus) == 2;
    if (stream != nullptr)
    {
        fclose(stream);
    }
    else
    {
        close(output);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           parsed;
}

} // namespace

constexpr char Help[] =
    "usage: primes <workers 1-16> [limit]\n"
    "Counts the primes below limit (default 10000000, at most 1000000000) with\n"
    "worker processes, which report through pipes, and shows the CPUs each ran on.\n"
    "Compare primes 1 with primes 4.";

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
        ParseUnsigned(argv[3], &workers) && ParseUnsigned(argv[4], &limit) && workers > 0)
    {
        return RunWorker(index, workers, limit);
    }
    if ((argc != 2 && argc != 3) || !ParseUnsigned(argv[1], &workers) || workers == 0 ||
        workers > MaximumWorkers ||
        (argc == 3 && (!ParseUnsigned(argv[2], &limit) || limit > MaximumLimit)))
    {
        puts("usage: primes <workers 1-16> [limit up to 1000000000]");
        return 1;
    }

    const uint64_t start = MillisecondsSinceBoot();
    pid_t children[MaximumWorkers];
    int outputs[MaximumWorkers];
    for (unsigned long long worker = 0; worker < workers; ++worker)
    {
        outputs[worker] = StartWorker(worker, workers, limit, &children[worker]);
        if (outputs[worker] < 0)
        {
            puts("primes: could not start a worker");
            return 1;
        }
    }

    // 2 is the only even prime; the workers test odd numbers.
    unsigned long long total = limit > 2 ? 1 : 0;
    for (unsigned long long worker = 0; worker < workers; ++worker)
    {
        unsigned long long primes = 0;
        unsigned long long cpus = 0;
        if (!CollectWorker(outputs[worker], children[worker], &primes, &cpus))
        {
            puts("primes: a worker failed");
            return 1;
        }
        total += primes;
        printf("primes: worker %llu found %llu on CPU ", worker, primes);
        WriteCpus(cpus);
        putchar('\n');
    }
    const uint64_t elapsed = MillisecondsSinceBoot() - start;
    printf("primes: %llu primes below %llu with %llu %s in %llu ms\n", total, limit, workers,
           workers == 1 ? "worker" : "workers", static_cast<unsigned long long>(elapsed));
    return 0;
}
