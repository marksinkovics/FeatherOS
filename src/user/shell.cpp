#include "command_line.h"

#include <errno.h>
#include <featheros.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace FeatherOS::User;

namespace
{

constexpr size_t MaximumWords = 16;
constexpr size_t MaximumPathLength = 64;
constexpr int SignalInterrupt = 2;

// Programs inherit the shell's environment, which init set up.
void RunCommand(char* line)
{
    char* words[MaximumWords + 1];
    size_t count = 0;
    const SplitResult split = SplitCommandLine(line, words, MaximumWords + 1, &count);
    if (split == SplitResult::UnclosedQuote)
    {
        puts("shell: unclosed quote");
        return;
    }
    if (split == SplitResult::TooManyWords)
    {
        puts("shell: too many arguments");
        return;
    }
    pid_t child = 0;
    const int error = posix_spawnp(&child, words[0], nullptr, nullptr, words, environ);
    if (error == ENOENT)
    {
        printf("Unknown command: %s\n", words[0]);
        return;
    }
    if (error == E2BIG)
    {
        puts("shell: too many arguments");
        return;
    }
    if (error != 0)
    {
        puts("shell: could not start program");
        return;
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child)
    {
        puts("shell: could not wait for program");
        return;
    }
    if (WIFSIGNALED(status))
    {
        if (WTERMSIG(status) != SignalInterrupt)
        {
            puts("shell: program terminated by a CPU exception");
        }
    }
    else if (WEXITSTATUS(status) != 0)
    {
        printf("exit status %d\n", WEXITSTATUS(status));
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
    size_t length = 0;
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
    const char* target = length == 0 ? "/home" : path;
    if (chdir(target) != 0)
    {
        printf("cd: %s: no such directory\n", target);
    }
}

} // namespace

constexpr char Help[] =
    "usage: shell\n"
    "Runs programs by name and waits for each one. Type help for the commands;\n"
    "Ctrl+C stops the running command. The prompt shows the working directory,\n"
    "which cd changes (cd alone returns to /home) and programs inherit.";

int main(int argc, char** argv)
{
    if (HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    puts("FeatherOS shell. Type 'help' for commands.");
    char line[64];
    // "<working directory>> ", such as "/home/docs> ".
    char prompt[MaximumPathLength + 3];
    for (;;)
    {
        if (getcwd(prompt, MaximumPathLength) == nullptr)
        {
            prompt[0] = '\0';
        }
        strcat(prompt, "> ");
        if (featheros_read_line(prompt, line, sizeof(line)) < 0)
        {
            puts("shell: could not read input");
            return 1;
        }
        char* command = line;
        while (*command == ' ')
        {
            ++command;
        }
        if (*command == '\0')
        {
            continue;
        }
        if (strcmp(command, "cd") == 0 || strncmp(command, "cd ", 3) == 0)
        {
            ChangeDirectoryCommand(command + 2);
            continue;
        }
        if (strcmp(command, "shutdown") == 0 || strcmp(command, "reboot") == 0)
        {
            // Returns only if the kernel refuses.
            reboot(command[0] == 's' ? RB_POWER_OFF : RB_AUTOBOOT);
            printf("%s: not permitted\n", command);
            continue;
        }
        if (strcmp(command, "exit") == 0)
        {
            // init restarts the console service.
            return 0;
        }
        if (strcmp(command, "pwd") == 0)
        {
            char directory[MaximumPathLength];
            puts(getcwd(directory, sizeof(directory)) == nullptr ? "?" : directory);
            continue;
        }
        if (strcmp(command, "help") == 0)
        {
            puts(
                "Commands: calc [expression], date, echo <words...>, fanout <n>, "
                "aslr, hardening, memtest [overflow|exhaust], primes <workers> [limit], "
                "sleep <ms>, upper, uptime, filetest, fptest, libctest, ls, cat <file>, write [-a] "
                "<file> <text>, "
                "rm <file>, mkdir <dir>, rmdir <dir>, env, cd [dir], pwd, exit, shutdown, reboot, "
                "help");
            puts("Run <command> --help to see what a command does.");
            continue;
        }
        RunCommand(command);
    }
}
