#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

constexpr char Help[] = "usage: write [-a] <file> <text...>\n"
                        "Writes the text and a newline to a file on the disk, replacing it;\n"
                        "-a appends instead.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    const uint64_t count = arguments == nullptr ? 0 : arguments->argument_count;
    const bool append = count > 1 && StringEquals(arguments->argument_values[1], "-a");
    const uint64_t name_index = append ? 2 : 1;
    char path[MaximumPathLength];
    if (count < name_index + 2 || !CopyPath(arguments->argument_values[name_index], path))
    {
        WriteLine("usage: write [-a] <file> <text...>");
        Exit(1);
    }
    const uint64_t descriptor =
        OpenFile(path, OpenWrite | OpenCreate | (append ? OpenAppend : OpenTruncate));
    if (IsSyscallError(descriptor))
    {
        WriteText("write: cannot open ");
        WriteLine(arguments->argument_values[name_index]);
        Exit(1);
    }
    for (uint64_t index = name_index + 1; index < count; ++index)
    {
        const char* word = arguments->argument_values[index];
        uint64_t length = 0;
        while (word[length] != '\0')
        {
            ++length;
        }
        const char* separator = index + 1 < count ? " " : "\n";
        if (WriteFile(descriptor, word, length) != length ||
            WriteFile(descriptor, separator, 1) != 1)
        {
            WriteLine("write: the disk is full");
            Exit(1);
        }
    }
    CloseFile(descriptor);
    Exit(0);
}
