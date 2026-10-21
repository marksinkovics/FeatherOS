#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

constexpr char Help[] = "usage: ls [directory]\n"
                        "Lists a directory (the current one by default) with file sizes in\n"
                        "bytes; subdirectories end in /. /system holds the read-only boot files.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    const uint64_t count = arguments == nullptr ? 1 : arguments->argument_count;
    const char* name = count == 2 ? arguments->argument_values[1] : ".";
    char path[MaximumPathLength];
    if (count > 2 || !CopyPath(name, path))
    {
        WriteLine("usage: ls [directory]");
        Exit(1);
    }
    constexpr uint64_t NameColumnWidth = 24;
    uint64_t files = 0;
    uint64_t directories = 0;
    uint64_t bytes = 0;
    DirectoryEntry entry;
    for (uint64_t index = 0;; ++index)
    {
        const uint64_t result = ReadDirectory(path, index, &entry);
        if (result == SyscallErrorNotFound)
        {
            break;
        }
        if (IsSyscallError(result))
        {
            WriteText("ls: ");
            WriteText(name);
            WriteLine(": no such directory");
            Exit(1);
        }
        uint64_t length = 0;
        for (; entry.name[length] != '\0'; ++length)
        {
            WriteCharacter(static_cast<uint64_t>(entry.name[length]));
        }
        if (entry.directory)
        {
            WriteCharacter('/');
            ++length;
        }
        do
        {
            WriteCharacter(' ');
        } while (++length < NameColumnWidth);
        if (entry.directory)
        {
            WriteLine("<dir>");
            ++directories;
            continue;
        }
        WriteNumber(entry.size);
        WriteCharacter('\n');
        ++files;
        bytes += entry.size;
    }
    if (directories > 0)
    {
        WriteNumber(directories);
        WriteText(directories == 1 ? " directory, " : " directories, ");
    }
    WriteNumber(files);
    WriteText(files == 1 ? " file, " : " files, ");
    WriteNumber(bytes);
    WriteLine(" bytes");
    Exit(0);
}
