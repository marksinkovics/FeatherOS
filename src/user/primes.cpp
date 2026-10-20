#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

namespace
{

constexpr uint64_t MaximumWorkers = 16;
constexpr uint64_t DefaultLimit = 10000000;
constexpr uint64_t MaximumLimit = 1000000000;
// Workers return their result as an exit status, since there are no pipes yet: the prime count
// above a mask of the CPUs they ran on.
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
            cpus |= 1ULL << (CurrentCpu() % CpuMaskBits);
        }
        primes += IsOddPrime(candidate) ? 1 : 0;
    }
    cpus |= 1ULL << (CurrentCpu() % CpuMaskBits);
    Exit(primes << CpuMaskBits | cpus);
}

void Append(char* buffer, uint64_t* length, const char* text)
{
    while (*text != '\0')
    {
        buffer[(*length)++] = *text++;
    }
}

void AppendNumber(char* buffer, uint64_t* length, uint64_t value)
{
    char digits[20];
    uint32_t count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0)
    {
        buffer[(*length)++] = digits[--count];
    }
}

void WriteCpus(uint64_t cpus)
{
    bool first = true;
    for (uint64_t cpu = 0; cpu < CpuMaskBits; ++cpu)
    {
        if ((cpus & (1ULL << cpu)) != 0)
        {
            WriteText(first ? "" : ",");
            WriteNumber(cpu);
            first = false;
        }
    }
}

} // namespace

// primes <workers> [limit]: counts the primes below limit, split across worker processes.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    uint64_t workers = 0;
    uint64_t limit = DefaultLimit;
    const uint64_t count = arguments == nullptr ? 0 : arguments->argument_count;
    const char* const* values = arguments == nullptr ? nullptr : arguments->argument_values;
    uint64_t index;
    if (count == 5 && StringEquals(values[1], "worker") && ParseUnsignedNumber(values[2], &index) &&
        ParseUnsignedNumber(values[3], &workers) && ParseUnsignedNumber(values[4], &limit))
    {
        RunWorker(index, workers, limit);
    }
    if ((count != 2 && count != 3) || !ParseUnsignedNumber(values[1], &workers) || workers == 0 ||
        workers > MaximumWorkers ||
        (count == 3 && (!ParseUnsignedNumber(values[2], &limit) || limit > MaximumLimit)))
    {
        WriteLine("usage: primes <workers 1-16> [limit up to 1000000000]");
        Exit(1);
    }

    const uint64_t start = ReadUptime();
    uint64_t children[MaximumWorkers];
    for (uint64_t worker = 0; worker < workers; ++worker)
    {
        char command[MaximumCommandLineLength];
        uint64_t length = 0;
        Append(command, &length, "primes worker ");
        AppendNumber(command, &length, worker);
        Append(command, &length, " ");
        AppendNumber(command, &length, workers);
        Append(command, &length, " ");
        AppendNumber(command, &length, limit);
        command[length] = '\0';
        children[worker] = Spawn(command);
        if (IsSyscallError(children[worker]))
        {
            WriteLine("primes: could not start a worker");
            Exit(1);
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
            WriteLine("primes: a worker failed");
            Exit(1);
        }
        const uint64_t primes = status >> CpuMaskBits;
        total += primes;
        WriteText("primes: worker ");
        WriteNumber(worker);
        WriteText(" found ");
        WriteNumber(primes);
        WriteText(" on CPU ");
        WriteCpus(status & ((1ULL << CpuMaskBits) - 1));
        WriteCharacter('\n');
    }
    const uint64_t elapsed = ReadUptime() - start;

    WriteText("primes: ");
    WriteNumber(total);
    WriteText(" primes below ");
    WriteNumber(limit);
    WriteText(" with ");
    WriteNumber(workers);
    WriteText(workers == 1 ? " worker in " : " workers in ");
    WriteNumber(elapsed);
    WriteLine(" ms");
    Exit(0);
}
