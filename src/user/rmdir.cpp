#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

constexpr char Help[] = "usage: rmdir <directory>\n"
                        "Removes an empty directory on the disk.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    char path[MaximumPathLength];
    if (arguments == nullptr || arguments->argument_count != 2 ||
        !CopyPath(arguments->argument_values[1], path))
    {
        WriteLine("usage: rmdir <directory>");
        Exit(1);
    }
    if (IsSyscallError(RemoveDirectory(path)))
    {
        WriteText("rmdir: ");
        WriteText(arguments->argument_values[1]);
        WriteLine(": not an empty directory");
        Exit(1);
    }
    Exit(0);
}
