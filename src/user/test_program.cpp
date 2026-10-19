#include "process.h"
#include "syscall.h"

#if defined(__x86_64__)
constexpr uint64_t KernelAddress = 0xFFFFFFFF80000000;
constexpr uint64_t OutputCharacter = 'U';
constexpr uint64_t CompletionCharacter = 'u';
#elif defined(__aarch64__)
constexpr uint64_t KernelAddress = 0xFFFFFFFF80000000;
constexpr uint64_t OutputCharacter = 'A';
constexpr uint64_t CompletionCharacter = 'a';
#endif

uint64_t syscall_count = 1;

void WaitForTimerPreemption()
{
    volatile uint64_t counter = 0;
    while (counter < 20000000)
    {
        ++counter;
    }
}

extern "C" [[noreturn]] void _start(const FeatherOS::User::ProgramArguments* arguments)
{
    if (arguments == nullptr)
    {
        FeatherOS::User::Exit(2);
    }
    if (arguments->process_id == 11 && arguments->test_case == 0)
    {
        FeatherOS::User::WriteCharacter(OutputCharacter);
        if (arguments->user_preemption_test != 0)
        {
            WaitForTimerPreemption();
            FeatherOS::User::WriteCharacter(CompletionCharacter);
        }
        ++syscall_count;
        FeatherOS::User::Exit(0);
    }
    if (arguments->process_id != 22 || arguments->test_case != 1)
    {
        FeatherOS::User::Exit(2);
    }

    if (arguments->user_preemption_test != 0)
    {
        FeatherOS::User::WriteCharacter('I');
        WaitForTimerPreemption();
    }

    const auto* kernel_memory = reinterpret_cast<const volatile uint64_t*>(KernelAddress);
    const uint64_t value = *kernel_memory;
    (void)value;
    FeatherOS::User::Exit(1);
}