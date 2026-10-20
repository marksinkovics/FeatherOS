#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

constexpr char Help[] = "usage: cat <file>\n"
                        "Prints a file. A relative name that is missing is also tried in the\n"
                        "initramfs root, so cat hello.txt works from any directory.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    if (arguments == nullptr || arguments->argument_count != 2)
    {
        WriteLine("usage: cat <file>");
        Exit(1);
    }
    const char* name = arguments->argument_values[1];
    char path[MaximumPathLength];
    uint64_t descriptor = CopyPath(name, path) ? OpenFile(path, OpenRead) : SyscallErrorNotFound;
    if (descriptor == SyscallErrorNotFound && name[0] != '/')
    {
        char root_path[MaximumPathLength + 1];
        root_path[0] = '/';
        for (uint64_t index = 0; index < MaximumPathLength; ++index)
        {
            root_path[index + 1] = path[index];
            if (path[index] == '\0')
            {
                break;
            }
        }
        descriptor = OpenFile(root_path, OpenRead);
    }
    if (IsSyscallError(descriptor))
    {
        WriteText("cat: cannot open ");
        WriteLine(name);
        Exit(1);
    }

    char buffer[256];
    char last = '\n';
    for (;;)
    {
        const uint64_t count = ReadFile(descriptor, buffer, sizeof(buffer));
        if (IsSyscallError(count))
        {
            WriteLine("cat: read failed");
            Exit(1);
        }
        if (count == 0)
        {
            break;
        }
        // The console accepts only printable ASCII and newlines.
        for (uint64_t index = 0; index < count; ++index)
        {
            const char character = buffer[index];
            if (character == '\r')
            {
                continue;
            }
            if (character == '\t')
            {
                WriteCharacter(' ');
            }
            else if (character == '\n' || (character >= 0x20 && character <= 0x7E))
            {
                WriteCharacter(static_cast<uint64_t>(character));
            }
            else
            {
                WriteCharacter('?');
            }
            last = character;
        }
    }
    if (last != '\n')
    {
        WriteCharacter('\n');
    }
    CloseFile(descriptor);
    Exit(0);
}
