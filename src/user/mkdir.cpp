#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

constexpr char Help[] = "usage: mkdir <directory>\n"
                        "Creates a directory on the disk.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    char path[MaximumPathLength];
    if (arguments == nullptr || arguments->argument_count != 2 ||
        !CopyPath(arguments->argument_values[1], path))
    {
        WriteLine("usage: mkdir <directory>");
        Exit(1);
    }
    if (IsSyscallError(MakeDirectory(path)))
    {
        WriteText("mkdir: ");
        WriteText(arguments->argument_values[1]);
        WriteLine(": cannot create the directory");
        Exit(1);
    }
    Exit(0);
}
