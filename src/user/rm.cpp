#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

constexpr char Help[] = "usage: rm <file>\n"
                        "Deletes a file on the disk.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    char path[MaximumPathLength];
    if (arguments == nullptr || arguments->argument_count != 2 ||
        !CopyPath(arguments->argument_values[1], path))
    {
        WriteLine("usage: rm <file>");
        Exit(1);
    }
    const uint64_t result = RemoveFile(path);
    if (result == 0)
    {
        Exit(0);
    }
    WriteText("rm: ");
    WriteText(arguments->argument_values[1]);
    WriteLine(result == SyscallErrorNotFound ? ": no such file"
              : result == SyscallErrorBusy   ? ": the file is open"
                                             : ": cannot remove");
    Exit(1);
}
