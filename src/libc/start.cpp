#include "libc/environment.h"
#include "user/process.h"

#include <stdint.h>
#include <stdlib.h>

extern "C" int main(int argc, char** argv);

namespace
{

using InitFunction = void (*)();

// The kernel maps the arguments read-only, but C lets a program modify its argv strings (strtok
// on an argument, for example), so they are copied.
char argument_storage[FeatherOS::User::MaximumCommandLineLength];
char* argument_values[FeatherOS::User::MaximumArgumentCount + 1];

int CopyArguments(const FeatherOS::User::ProgramArguments& arguments)
{
    for (uint64_t index = 0; index < sizeof(argument_storage); ++index)
    {
        argument_storage[index] = arguments.argument_storage[index];
    }
    uint64_t count = 0;
    for (; count < arguments.argument_count && count < FeatherOS::User::MaximumArgumentCount;
         ++count)
    {
        const auto offset =
            static_cast<uint64_t>(arguments.argument_values[count] - arguments.argument_storage);
        argument_values[count] = argument_storage + offset;
    }
    argument_values[count] = nullptr;
    return static_cast<int>(count);
}

} // namespace

extern "C" const InitFunction __preinit_array_start[] __attribute__((visibility("hidden")));
extern "C" const InitFunction __preinit_array_end[] __attribute__((visibility("hidden")));
extern "C" const InitFunction __init_array_start[] __attribute__((visibility("hidden")));
extern "C" const InitFunction __init_array_end[] __attribute__((visibility("hidden")));

extern "C" [[noreturn]] void _start(const FeatherOS::User::ProgramArguments* arguments)
{
    const int count = CopyArguments(*arguments);
    FeatherOS::LibC::LoadEnvironment(arguments->environment);
    // Global constructors, including C++ static objects, run before main.
    for (const InitFunction* function = __preinit_array_start; function != __preinit_array_end;
         ++function)
    {
        (*function)();
    }
    for (const InitFunction* function = __init_array_start; function != __init_array_end;
         ++function)
    {
        (*function)();
    }
    exit(main(count, argument_values));
}
