#include "command_line.h"
#include "process.h"
#include "syscall.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// These checks call the kernel directly, since they test the exact errors it returns.
using namespace FeatherOS::User;

namespace
{

#if defined(__x86_64__)
constexpr uint64_t KernelAddress = 0xFFFF800000000000;
#elif defined(__aarch64__)
constexpr uint64_t KernelAddress = 0xFFFF000000000000;
#endif

uint64_t passed_checks = 0;

void Check(bool passed, const char* name)
{
    if (!passed)
    {
        printf("hardening: failed %s\n", name);
        exit(1);
    }
    ++passed_checks;
}

bool ChildTerminated(const char* command_line)
{
    const uint64_t child = Spawn(command_line);
    uint64_t status = 0;
    return !IsSyscallError(child) && !IsSyscallError(Wait(child, &status)) &&
           (status == ExitStatusFault || status == ExitStatusKernelAccessFault);
}

// Writes the first size bytes of from, or size bytes of 'x' if from is null.
bool WriteFile(const char* path, const char* from, size_t size)
{
    static char buffer[4096];
    const int source = from == nullptr ? -1 : open(from, O_RDONLY);
    const int target = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    memset(buffer, 'x', sizeof(buffer));
    bool written = target >= 0 && (from == nullptr || source >= 0);
    for (size_t done = 0; written && done < size;)
    {
        const size_t part = size - done < sizeof(buffer) ? size - done : sizeof(buffer);
        const ssize_t length =
            source >= 0 ? read(source, buffer, part) : static_cast<ssize_t>(part);
        if (length == 0)
        {
            break;
        }
        written = length > 0 && write(target, buffer, static_cast<size_t>(length)) == length;
        done += static_cast<size_t>(length);
    }
    if (source >= 0)
    {
        close(source);
    }
    return target >= 0 && close(target) == 0 && written;
}

// Programs from files other than the initramfs go through the same checks.
void CheckProgramFiles()
{
    Check(Spawn("/tmp/missing") == SyscallErrorNotFound, "Spawn of a missing file");
    Check(Spawn("/tmp") == SyscallErrorIsDirectory, "Spawn of a directory");
    Check(Spawn("/dev/zero") == SyscallErrorNotExecutable, "Spawn of a device");
    Check(Spawn("hello.txt") == SyscallErrorNotExecutable, "Spawn of a text file");
    Check(WriteFile("/tmp/truncated", "/system/echo", 2048) &&
              Spawn("/tmp/truncated") == SyscallErrorNotExecutable,
          "Spawn of a truncated program");
    Check(WriteFile("/tmp/badelf", "/system/badelf", MaximumProgramSize),
          "copying the kernel-half ELF");
    const uint64_t bad_elf = Spawn("/tmp/badelf");
    Check(IsSyscallError(bad_elf) && bad_elf != SyscallErrorNotFound, "kernel-half ELF from /tmp");
    Check(WriteFile("/tmp/large", nullptr, MaximumProgramSize + 1) &&
              Spawn("/tmp/large") == SyscallErrorNoMemory,
          "Spawn of a file over the size limit");
    char original[MaximumPathLength];
    char long_path[MaximumCommandLineLength - 2];
    memset(long_path, 'p', sizeof(long_path) - 1);
    long_path[0] = '.';
    long_path[1] = '/';
    long_path[sizeof(long_path) - 1] = '\0';
    Check(getcwd(original, sizeof(original)) != nullptr && chdir("/tmp") == 0 &&
              Spawn(long_path) == SyscallErrorNameTooLong && chdir(original) == 0,
          "Spawn of a path too long once resolved");
    Check(unlink("/tmp/truncated") == 0 && unlink("/tmp/badelf") == 0 && unlink("/tmp/large") == 0,
          "removing the program files");
}

volatile uint8_t* shared_page = nullptr;
int reader_started = 0;

// Keeps reading on its own CPU, so its TLB holds the page while the main thread returns it.
void* ReadSharedPage(void*)
{
    for (unsigned long sum = 0;; sum += shared_page[0])
    {
        __atomic_store_n(&reader_started, 1, __ATOMIC_RELEASE);
    }
}

[[noreturn]] void RunViolation(const char* name)
{
    if (strcmp(name, "exec-heap") == 0)
    {
        void* page = sbrk(4096);
        auto* code = static_cast<volatile uint32_t*>(page);
#if defined(__x86_64__)
        *code = 0xC3;
#elif defined(__aarch64__)
        *code = 0xD65F03C0;
#endif
        reinterpret_cast<void (*)()>(page)();
    }
    else if (strcmp(name, "freed-heap") == 0)
    {
        auto* page = static_cast<volatile uint8_t*>(sbrk(2 * 4096));
        page[4096] = 1;
        sbrk(-2 * 4096);
        page[4096] = 2;
    }
    else if (strcmp(name, "freed-heap-threads") == 0)
    {
        auto* pages = static_cast<volatile uint8_t*>(sbrk(2 * 4096));
        shared_page = pages + 4096;
        shared_page[0] = 1;
        pthread_t reader = 0;
        pthread_create(&reader, nullptr, ReadSharedPage, nullptr);
        while (__atomic_load_n(&reader_started, __ATOMIC_ACQUIRE) == 0)
        {
        }
        sbrk(-2 * 4096);
        // Without a shootdown the reader would go on through its stale translation.
        Sleep(500);
    }
    else if (strcmp(name, "write-code") == 0)
    {
        *reinterpret_cast<volatile uint8_t*>(&RunViolation) = 0;
    }
    else if (strcmp(name, "kernel-read") == 0)
    {
        (void)*reinterpret_cast<volatile uint64_t*>(KernelAddress);
    }
    else if (strcmp(name, "bad-syscall") == 0)
    {
        Syscall(static_cast<SyscallNumber>(99));
    }
    exit(0);
}

} // namespace

constexpr char Help[] =
    "usage: hardening\n"
    "Runs 61 checks that the kernel rejects bad syscalls, pointers, program files,\n"
    "and memory violations from user programs.";

int main(int argc, char** argv)
{
    if (HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc == 2)
    {
        RunViolation(argv[1]);
    }

    char buffer[16];
    const auto* kernel_text = reinterpret_cast<const char*>(KernelAddress);
    Check(ReadLine(reinterpret_cast<char*>(KernelAddress), sizeof(buffer), ">") ==
              SyscallErrorBadAddress,
          "kernel ReadLine buffer");
    Check(ReadLine(buffer, sizeof(buffer), kernel_text) == SyscallErrorBadAddress,
          "kernel ReadLine prompt");
    Check(Spawn(kernel_text) == SyscallErrorBadAddress, "kernel Spawn command line");
    Check(Spawn("echo", 0, kernel_text) == SyscallErrorBadAddress, "kernel Spawn environment");
    Check(Wait(123456, nullptr) == SyscallErrorNotChild, "Wait for a non-child");
    Check(ExtendHeap(static_cast<uint64_t>(INT64_MAX)) == SyscallErrorInvalidArgument,
          "oversized ExtendHeap");
    Check(ExtendHeap(static_cast<uint64_t>(INT64_MIN)) == SyscallErrorInvalidArgument,
          "shrinking the heap below its start");
    Check(Sleep(UINT64_MAX) == SyscallErrorInvalidArgument, "oversized Sleep");
    Check(Syscall(SyscallNumber::ThreadCreate, KernelAddress, 0, 0) == SyscallErrorInvalidArgument,
          "a thread starting in the kernel");
    Check(Syscall(SyscallNumber::ThreadJoin, 2, KernelAddress) == SyscallErrorBadAddress,
          "kernel ThreadJoin value");
    const uint64_t bad_elf = Spawn("badelf");
    Check(IsSyscallError(bad_elf) && bad_elf != SyscallErrorNotFound, "kernel-half ELF");
    CheckProgramFiles();

    Check(ChildTerminated("hardening exec-heap"), "executing the heap");
    Check(ChildTerminated("hardening freed-heap"), "using a page the heap returned");
    Check(ChildTerminated("hardening freed-heap-threads"),
          "a thread on another CPU using a page the heap returned");
    Check(ChildTerminated("hardening write-code"), "writing code");
    Check(ChildTerminated("hardening kernel-read"), "reading kernel memory");
    Check(ChildTerminated("hardening bad-syscall"), "invalid syscall number");

    constexpr uint64_t ChildLimit = 16;
    uint64_t children[ChildLimit];
    for (uint64_t& child : children)
    {
        child = Spawn("sleep 300");
        Check(!IsSyscallError(child), "spawning up to the child limit");
    }
    Check(IsSyscallError(Spawn("sleep 300")), "spawning past the child limit");
    Check(Shutdown(ShutdownPowerOff) == SyscallErrorNotPermitted, "shutdown from a program");
    for (const uint64_t child : children)
    {
        uint64_t status;
        Check(!IsSyscallError(Wait(child, &status)) && status == 0, "waiting for children");
    }

    printf("hardening: %llu checks passed\n", static_cast<unsigned long long>(passed_checks));
    return 0;
}
