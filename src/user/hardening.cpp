#include "command_line.h"
#include "syscall.h"

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
    "Runs 48 checks that the kernel rejects bad syscalls, pointers, ELF files,\n"
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
    const uint64_t bad_elf = Spawn("badelf");
    Check(IsSyscallError(bad_elf) && bad_elf != SyscallErrorNotFound, "kernel-half ELF");

    Check(ChildTerminated("hardening exec-heap"), "executing the heap");
    Check(ChildTerminated("hardening freed-heap"), "using a page the heap returned");
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
