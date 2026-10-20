#include "process.h"
#include "runtime.h"

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
        WriteText("hardening: failed ");
        WriteLine(name);
        Exit(1);
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
    if (StringEquals(name, "exec-heap"))
    {
        const uint64_t page = ExtendHeap(4096);
        auto* code = reinterpret_cast<volatile uint32_t*>(page);
#if defined(__x86_64__)
        *code = 0xC3;
#elif defined(__aarch64__)
        *code = 0xD65F03C0;
#endif
        reinterpret_cast<void (*)()>(page)();
    }
    else if (StringEquals(name, "write-code"))
    {
        *reinterpret_cast<volatile uint8_t*>(&RunViolation) = 0;
    }
    else if (StringEquals(name, "kernel-read"))
    {
        (void)*reinterpret_cast<volatile uint64_t*>(KernelAddress);
    }
    else if (StringEquals(name, "bad-syscall"))
    {
        Syscall(static_cast<SyscallNumber>(99));
    }
    Exit(0);
}

} // namespace

constexpr char Help[] =
    "usage: hardening\n"
    "Runs 44 checks that the kernel rejects bad syscalls, pointers, ELF files,\n"
    "and memory violations from user programs.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    if (arguments != nullptr && arguments->argument_count == 2)
    {
        RunViolation(arguments->argument_values[1]);
    }

    char buffer[16];
    const auto* kernel_text = reinterpret_cast<const char*>(KernelAddress);
    Check(ReadLine(reinterpret_cast<char*>(KernelAddress), sizeof(buffer), ">") ==
              SyscallErrorInvalidArgument,
          "kernel ReadLine buffer");
    Check(ReadLine(buffer, sizeof(buffer), kernel_text) == SyscallErrorInvalidArgument,
          "kernel ReadLine prompt");
    Check(Spawn(kernel_text) == SyscallErrorInvalidArgument, "kernel Spawn command line");
    Check(Wait(123456, nullptr) == SyscallErrorInvalidArgument, "Wait for a non-child");
    Check(ExtendHeap(UINT64_MAX) == SyscallErrorInvalidArgument, "oversized ExtendHeap");
    Check(Sleep(UINT64_MAX) == SyscallErrorInvalidArgument, "oversized Sleep");
    const uint64_t bad_elf = Spawn("badelf");
    Check(IsSyscallError(bad_elf) && bad_elf != SyscallErrorNotFound, "kernel-half ELF");

    Check(ChildTerminated("hardening exec-heap"), "executing the heap");
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

    WriteText("hardening: ");
    WriteNumber(passed_checks);
    WriteLine(" checks passed");
    Exit(0);
}
