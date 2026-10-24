#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
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

constexpr size_t MaximumRedirects = 4;
constexpr size_t MaximumStages = 4;
constexpr size_t MaximumPipelineWords = MaximumStages * MaximumWords;
constexpr size_t MaximumCommandLineLength = 64;

// Opens the files the redirections name, close-on-exec so only the copies the file actions make
// reach the child. Returns false after reporting a file that cannot be opened.
bool PrepareRedirects(const Redirect* redirects, size_t count, int* opened,
                      posix_spawn_file_actions_t* actions)
{
    for (size_t index = 0; index < count; ++index)
    {
        opened[index] = -1;
    }
    for (size_t index = 0; index < count; ++index)
    {
        const Redirect& redirect = redirects[index];
        if (redirect.kind == RedirectKind::ErrorToOutput)
        {
            posix_spawn_file_actions_adddup2(actions, STDOUT_FILENO, STDERR_FILENO);
            continue;
        }
        const int flags = redirect.kind == RedirectKind::Input    ? O_RDONLY
                          : redirect.kind == RedirectKind::Append ? O_WRONLY | O_CREAT | O_APPEND
                                                                  : O_WRONLY | O_CREAT | O_TRUNC;
        opened[index] = open(redirect.path, flags | O_CLOEXEC, 0644);
        if (opened[index] < 0)
        {
            printf("shell: %s: %s\n", redirect.path, strerror(errno));
            return false;
        }
        const int target = redirect.kind == RedirectKind::Input   ? STDIN_FILENO
                           : redirect.kind == RedirectKind::Error ? STDERR_FILENO
                                                                  : STDOUT_FILENO;
        posix_spawn_file_actions_adddup2(actions, opened[index], target);
    }
    return true;
}

void CloseRedirects(const int* opened, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (opened[index] >= 0)
        {
            close(opened[index]);
        }
    }
}

void ReportStatus(int status)
{
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

// Starts one command with input and output (-1 for the shell's own) and then its own
// redirections, which win, as in other shells. Returns the child, or 0 after saying why not.
pid_t StartStage(const Stage& stage, int input, int output)
{
    char** words = stage.words;
    size_t count = stage.count;
    Redirect redirects[MaximumRedirects];
    size_t redirect_count = 0;
    const RedirectResult redirected =
        ExtractRedirects(words, stage.quoted, &count, redirects, MaximumRedirects, &redirect_count);
    if (redirected != RedirectResult::Done || count == 0)
    {
        puts(redirected == RedirectResult::TooMany ? "shell: too many redirections"
                                                   : "shell: a redirection needs a file name");
        return 0;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (input >= 0)
    {
        posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
    }
    if (output >= 0)
    {
        posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    }
    int opened[MaximumRedirects];
    if (!PrepareRedirects(redirects, redirect_count, opened, &actions))
    {
        CloseRedirects(opened, redirect_count);
        return 0;
    }
    pid_t child = 0;
    // Programs inherit the shell's environment, which init set up.
    const int error = posix_spawnp(&child, words[0], &actions, nullptr, words, environ);
    CloseRedirects(opened, redirect_count);
    if (error == ENOENT)
    {
        printf("Unknown command: %s\n", words[0]);
    }
    else if (error == E2BIG)
    {
        puts("shell: too many arguments");
    }
    else if (error != 0)
    {
        puts("shell: could not start program");
    }
    return error == 0 ? child : 0;
}

// Runs one command, or a pipeline of up to MaximumStages, and waits for all of them. The shell's
// pipe descriptors are close-on-exec, so each child gets only the ends file actions give it,
// and the reader sees the end of its input once every writer is gone.
void RunCommand(const char* line)
{
    char separated[2 * MaximumCommandLineLength];
    char* words[MaximumPipelineWords + 1];
    bool quoted[MaximumPipelineWords];
    size_t count = 0;
    if (!SeparatePipes(line, separated, sizeof(separated)))
    {
        puts("shell: command too long");
        return;
    }
    const SplitResult split =
        SplitCommandLine(separated, words, MaximumPipelineWords + 1, &count, quoted);
    if (split != SplitResult::Done)
    {
        puts(split == SplitResult::UnclosedQuote ? "shell: unclosed quote"
                                                 : "shell: too many arguments");
        return;
    }
    Stage stages[MaximumStages];
    size_t stage_count = 0;
    const PipelineResult pipeline =
        SplitPipeline(words, quoted, count, stages, MaximumStages, &stage_count);
    if (pipeline != PipelineResult::Done)
    {
        puts(pipeline == PipelineResult::EmptyStage ? "shell: a pipeline has an empty command"
                                                    : "shell: at most 4 commands in a pipeline");
        return;
    }
    int pipes[MaximumStages - 1][2];
    for (size_t index = 0; index + 1 < stage_count; ++index)
    {
        if (pipe2(pipes[index], O_CLOEXEC) != 0)
        {
            puts("shell: cannot create a pipe");
            for (size_t created = 0; created < index; ++created)
            {
                close(pipes[created][0]);
                close(pipes[created][1]);
            }
            return;
        }
    }
    // Last to first, so the first command, the one that may read the console, takes it last.
    pid_t children[MaximumStages] = {};
    for (size_t index = stage_count; index-- > 0;)
    {
        const int input = index > 0 ? pipes[index - 1][0] : -1;
        const int output = index + 1 < stage_count ? pipes[index][1] : -1;
        children[index] = StartStage(stages[index], input, output);
        if (children[index] == 0)
        {
            break;
        }
    }
    for (size_t index = 0; index + 1 < stage_count; ++index)
    {
        close(pipes[index][0]);
        close(pipes[index][1]);
    }
    // As in other shells, the last command's status is the pipeline's.
    for (size_t index = 0; index < stage_count; ++index)
    {
        int status = 0;
        if (children[index] == 0)
        {
            continue;
        }
        if (waitpid(children[index], &status, 0) != children[index])
        {
            puts("shell: could not wait for program");
        }
        else if (index + 1 == stage_count)
        {
            ReportStatus(status);
        }
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
    "which cd changes (cd alone returns to /home) and programs inherit. A command's\n"
    "input and output can be redirected: < file, > file, >> file, 2> file, 2>&1;\n"
    "a | b connects one command's output to the next one's input.";

int main(int argc, char** argv)
{
    if (HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    puts("FeatherOS shell. Type 'help' for commands.");
    char line[MaximumCommandLineLength];
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
