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

// A built-in, because only the shell's own working directory should change. One pair of double
// quotes around the directory is dropped, as program arguments drop theirs.
void ChangeDirectoryCommand(const char* argument)
{
    while (*argument == ' ')
    {
        ++argument;
    }
    char path[MaximumPathLength];
    uint64_t length = 0;
    for (const char* character = argument; *character != '\0' && length < sizeof(path) - 1;
         ++character)
    {
        if (*character != '"')
        {
            path[length++] = *character;
        }
    }
    while (length > 0 && path[length - 1] == ' ')
    {
        --length;
    }
    path[length] = '\0';
    const char* target = length == 0 ? "/disk" : path;
    if (IsSyscallError(ChangeDirectory(target)))
    {
        WriteText("cd: ");
        WriteText(target);
        WriteLine(": no such directory");
    }
}

// "<working directory>> ", such as "/disk/docs> ".
void BuildPrompt(char (&prompt)[MaximumPathLength + 3])
{
    const uint64_t length = CurrentDirectory(prompt, MaximumPathLength);
    const uint64_t end = IsSyscallError(length) ? 0 : length;
    prompt[end] = '>';
    prompt[end + 1] = ' ';
    prompt[end + 2] = '\0';
}

} // namespace

constexpr char Help[] =
    "usage: shell\n"
    "Runs programs by name and waits for each one. Type help for the commands;\n"
    "Ctrl+C stops the running command. The prompt shows the working directory,\n"
    "which cd changes (cd alone returns to /disk) and programs inherit.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    WriteLine("FeatherOS shell. Type 'help' for commands.");
    char line[MaximumCommandLineLength];
    char prompt[MaximumPathLength + 3];
    for (;;)
    {
        BuildPrompt(prompt);
        if (IsSyscallError(ReadLine(line, sizeof(line), prompt)))
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
        if (StringEquals(command, "cd") ||
            (command[0] == 'c' && command[1] == 'd' && command[2] == ' '))
        {
            ChangeDirectoryCommand(command + 2);
            continue;
        }
        if (StringEquals(command, "shutdown") || StringEquals(command, "reboot"))
        {
            // Returns only if the kernel refuses.
            Shutdown(command[0] == 's' ? ShutdownPowerOff : ShutdownRestart);
            WriteText(command);
            WriteLine(": not permitted");
            continue;
        }
        if (StringEquals(command, "exit"))
        {
            // init restarts the console service.
            Exit(0);
        }
        if (StringEquals(command, "pwd"))
        {
            char directory[MaximumPathLength];
            WriteLine(IsSyscallError(CurrentDirectory(directory, sizeof(directory))) ? "?"
                                                                                     : directory);
            continue;
        }
        if (StringEquals(command, "help"))
        {
            WriteLine(
                "Commands: calc [expression], date, echo <words...>, fanout <n>, "
                "aslr, hardening, memtest [overflow|exhaust], primes <workers> [limit], "
                "sleep <ms>, upper, uptime, filetest, ls, cat <file>, write [-a] <file> <text>, "
                "rm <file>, mkdir <dir>, rmdir <dir>, cd [dir], pwd, exit, shutdown, reboot, "
                "help");
            WriteLine("Run <command> --help to see what a command does.");
            continue;
        }
        RunCommand(command);
    }
}
