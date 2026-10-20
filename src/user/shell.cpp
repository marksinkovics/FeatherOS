#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

namespace
{

// Writes the first word of line, which names the command.
void WriteCommandName(const char* line)
{
    for (; *line != '\0' && *line != ' '; ++line)
    {
        WriteCharacter(static_cast<uint64_t>(*line));
    }
}

void RunCommand(const char* line)
{
    const uint64_t process_id = Spawn(line);
    if (process_id == SyscallErrorNotFound)
    {
        WriteText("Unknown command: ");
        WriteCommandName(line);
        WriteCharacter('\n');
        return;
    }
    if (process_id == SyscallErrorInvalidArgument)
    {
        WriteLine("shell: too many arguments");
        return;
    }
    if (IsSyscallError(process_id))
    {
        WriteLine("shell: could not start program");
        return;
    }

    uint64_t status = 0;
    if (IsSyscallError(Wait(process_id, &status)))
    {
        WriteLine("shell: could not wait for program");
        return;
    }
    if (status == ExitStatusInterrupted)
    {
        return;
    }
    if (status == ExitStatusFault || status == ExitStatusKernelAccessFault)
    {
        WriteLine("shell: program terminated by a CPU exception");
    }
    else if (status != 0)
    {
        WriteText("exit status ");
        WriteNumber(status);
        WriteCharacter('\n');
    }
}

} // namespace

// The interactive shell: runs initramfs programs by name and waits for each to exit.
extern "C" [[noreturn]] void _start(const ProgramArguments*)
{
    WriteLine("FeatherOS shell. Type 'help' for commands.");
    char line[MaximumCommandLineLength];
    for (;;)
    {
        if (IsSyscallError(ReadLine(line, sizeof(line), "> ")))
        {
            WriteLine("shell: could not read input");
            Exit(1);
        }
        const char* command = line;
        while (*command == ' ')
        {
            ++command;
        }
        if (*command == '\0')
        {
            continue;
        }
        if (StringEquals(command, "help"))
        {
            WriteLine("Commands: calc [expression], date, echo <words...>, fanout <n>, "
                      "aslr, hardening, memtest [overflow|exhaust], primes <workers> [limit], "
                      "sleep <ms>, upper, uptime, help");
            continue;
        }
        RunCommand(command);
    }
}
