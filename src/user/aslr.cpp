#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

// aslr: prints where this run's stack, arguments block, and heap landed.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    volatile uint64_t stack_marker = 0;
    WriteText("aslr: stack=");
    WriteNumber(reinterpret_cast<uint64_t>(&stack_marker));
    WriteText(" args=");
    WriteNumber(reinterpret_cast<uint64_t>(arguments));
    WriteText(" heap=");
    WriteNumber(ExtendHeap(0));
    WriteCharacter('\n');
    Exit(0);
}
