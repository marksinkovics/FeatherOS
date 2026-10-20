#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

namespace
{

// A pointer stored in data needs a RELATIVE relocation, so this only reads correctly if the
// kernel applied the image's relocations at its load address.
const char* const relocated_labels[] = {"relocated"};

} // namespace

// aslr: prints where this run's image, stack, arguments block, and heap landed.
extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    if (!StringEquals(relocated_labels[0], "relocated"))
    {
        WriteLine("aslr: relocations were not applied");
        Exit(1);
    }
    volatile uint64_t stack_marker = 0;
    WriteText("aslr: image=");
    WriteNumber(reinterpret_cast<uint64_t>(&_start));
    WriteText(" stack=");
    WriteNumber(reinterpret_cast<uint64_t>(&stack_marker));
    WriteText(" args=");
    WriteNumber(reinterpret_cast<uint64_t>(arguments));
    WriteText(" heap=");
    WriteNumber(ExtendHeap(0));
    WriteCharacter('\n');
    Exit(0);
}
