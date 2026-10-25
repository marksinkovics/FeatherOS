#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <featheros.h>
#include <signal.h>
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

constexpr size_t MaximumRedirects = 4;
constexpr size_t MaximumStages = 4;
constexpr size_t MaximumPipelineWords = MaximumStages * MaximumWords;
constexpr size_t MaximumCommandLineLength = 64;
constexpr size_t MaximumJobs = 8;

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

// The program name in /proc/<pid>/status, or "?" once the process is gone.
void ProcessName(pid_t process, char* name, size_t size)
{
    snprintf(name, size, "?");
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/status", process);
    FILE* status = fopen(path, "r");
    if (status == nullptr)
    {
        return;
    }
    char line[64];
    if (fgets(line, sizeof(line), status) != nullptr && strncmp(line, "Name:\t", 6) == 0)
    {
        line[strcspn(line, "\n")] = '\0';
        snprintf(name, size, "%s", line + 6);
    }
    fclose(status);
}

// Says who took the console, if someone did, since a shell that cannot read has to exit and
// the reason would otherwise be lost.
void ReportReadFailure(int error)
{
    const pid_t owner = tcgetpgrp(STDIN_FILENO);
    if (error == EIO && owner > 0 && owner != getpgrp())
    {
        char name[40];
        ProcessName(owner, name, sizeof(name));
        printf("shell: lost the console to process group %d (%s): %s\n", owner, name,
               strerror(error));
        return;
    }
    printf("shell: could not read input: %s\n", strerror(error));
}

// Silent for Ctrl+C and for a pipeline's writer whose reader left, as in other shells.
void ReportStatus(int status)
{
    if (WIFSIGNALED(status))
    {
        const int signal_number = WTERMSIG(status);
        if (signal_number == SIGSEGV)
        {
            puts("shell: program terminated by a CPU exception");
        }
        else if (signal_number != SIGINT && signal_number != SIGPIPE)
        {
            printf("shell: %s\n", strsignal(signal_number));
        }
    }
    else if (WEXITSTATUS(status) != 0)
    {
        printf("exit status %d\n", WEXITSTATUS(status));
    }
}

// Starts one command with input and output (-1 for the shell's own) and then its own
// redirections, which win, as in other shells. It joins group, or leads a new one for 0.
// Returns the child, or 0 after saying why not.
pid_t StartStage(const Stage& stage, int input, int output, pid_t group, bool background)
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
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(
        &attributes,
        static_cast<short>(POSIX_SPAWN_SETPGROUP | (background ? POSIX_SPAWN_BACKGROUND : 0)));
    posix_spawnattr_setpgroup(&attributes, group);
    pid_t child = 0;
    // Programs inherit the shell's environment, which init set up.
    const int error = posix_spawnp(&child, words[0], &actions, &attributes, words, environ);
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

// A command or pipeline running in the background, until all its processes are reaped.
struct Job
{
    int number;
    pid_t group;
    pid_t members[MaximumStages];
    size_t member_count;
    // The last member's status, which is the job's.
    int status;
    char command[MaximumCommandLineLength];
};

Job jobs[MaximumJobs];

Job* FreeJob()
{
    for (Job& job : jobs)
    {
        if (job.number == 0)
        {
            return &job;
        }
    }
    return nullptr;
}

// The lowest number no other job has, as other shells number them.
int NextJobNumber()
{
    for (int number = 1;; ++number)
    {
        bool taken = false;
        for (const Job& job : jobs)
        {
            taken = taken || job.number == number;
        }
        if (!taken)
        {
            return number;
        }
    }
}

// Reaps what has finished, or waits for all of it with block. True once the job is over.
bool ReapJob(Job& job, bool block)
{
    for (size_t index = 0; index < job.member_count; ++index)
    {
        if (job.members[index] == 0)
        {
            continue;
        }
        int status = 0;
        const pid_t reaped = waitpid(job.members[index], &status, block ? 0 : WNOHANG);
        if (reaped == 0)
        {
            continue;
        }
        if (reaped == job.members[index] && index + 1 == job.member_count)
        {
            job.status = status;
        }
        job.members[index] = 0;
    }
    for (size_t index = 0; index < job.member_count; ++index)
    {
        if (job.members[index] != 0)
        {
            return false;
        }
    }
    return true;
}

void ReportFinishedJob(Job& job)
{
    if (WIFSIGNALED(job.status))
    {
        printf("[%d] %s  %s\n", job.number, strsignal(WTERMSIG(job.status)), job.command);
    }
    else if (WEXITSTATUS(job.status) != 0)
    {
        printf("[%d] Exit %d  %s\n", job.number, WEXITSTATUS(job.status), job.command);
    }
    else
    {
        printf("[%d] Done  %s\n", job.number, job.command);
    }
    job.number = 0;
}

// Before each prompt, as other shells report finished background jobs.
void ReapJobs()
{
    for (Job& job : jobs)
    {
        if (job.number != 0 && ReapJob(job, false))
        {
            ReportFinishedJob(job);
        }
    }
}

void ListJobs()
{
    ReapJobs();
    for (const Job& job : jobs)
    {
        if (job.number != 0)
        {
            printf("[%d] Running  %s\n", job.number, job.command);
        }
    }
}

// fg [n]: gives the console to job n, or the newest job, and waits for it.
void ForegroundCommand(const char* argument)
{
    while (*argument == ' ' || *argument == '%')
    {
        ++argument;
    }
    unsigned long long wanted = 0;
    Job* chosen = nullptr;
    const bool numbered = *argument != '\0';
    if (numbered && !ParseUnsigned(argument, &wanted))
    {
        puts("usage: fg [job number]");
        return;
    }
    for (Job& job : jobs)
    {
        const bool matches = numbered ? job.number == static_cast<int>(wanted) : job.number != 0;
        if (matches && (chosen == nullptr || job.number > chosen->number))
        {
            chosen = &job;
        }
    }
    if (chosen == nullptr)
    {
        puts("fg: no such job");
        return;
    }
    puts(chosen->command);
    if (tcsetpgrp(STDIN_FILENO, chosen->group) != 0 && errno != EINVAL)
    {
        printf("fg: %s\n", strerror(errno));
        return;
    }
    // Reaping the group's last process gives the console back to the shell.
    ReapJob(*chosen, true);
    ReportStatus(chosen->status);
    chosen->number = 0;
}

// wait: waits for every background job.
void WaitForJobs()
{
    for (Job& job : jobs)
    {
        if (job.number != 0)
        {
            ReapJob(job, true);
            ReportFinishedJob(job);
        }
    }
}

// kill %n names job n's process group, as in other shells; the kill program gets -group.
// Other commands keep %n as it is.
bool ExpandJobReferences(char** words, const bool* quoted, size_t count, char (*storage)[24])
{
    if (count == 0 || strcmp(words[0], "kill") != 0)
    {
        return true;
    }
    for (size_t index = 1; index < count; ++index)
    {
        unsigned long long number = 0;
        if (quoted[index] || words[index][0] != '%' || !ParseUnsigned(words[index] + 1, &number))
        {
            continue;
        }
        const Job* found = nullptr;
        for (const Job& job : jobs)
        {
            found = job.number != 0 && job.number == static_cast<int>(number) ? &job : found;
        }
        if (found == nullptr)
        {
            printf("kill: %s: no such job\n", words[index]);
            return false;
        }
        snprintf(storage[index], sizeof(storage[index]), "-%d", found->group);
        words[index] = storage[index];
    }
    return true;
}

// Runs one command, or a pipeline of up to MaximumStages, as one process group, so Ctrl+C stops
// all of it. The shell waits for it unless it ends in &. The shell's pipe descriptors are
// close-on-exec, so each child gets only the ends file actions give it, and the reader sees the
// end of its input once every writer is gone.
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
    static char job_references[MaximumPipelineWords][24];
    if (!ExpandJobReferences(words, quoted, count, job_references))
    {
        return;
    }
    const bool background = TakeBackgroundMarker(words, quoted, &count);
    Job* job = background ? FreeJob() : nullptr;
    if (background && job == nullptr)
    {
        puts("shell: too many background jobs");
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
    // The first command leads the group, and the rest join it.
    pid_t children[MaximumStages] = {};
    size_t started = 0;
    for (; started < stage_count; ++started)
    {
        const int input = started > 0 ? pipes[started - 1][0] : -1;
        const int output = started + 1 < stage_count ? pipes[started][1] : -1;
        children[started] = StartStage(stages[started], input, output, children[0], background);
        if (children[started] == 0)
        {
            break;
        }
    }
    for (size_t index = 0; index + 1 < stage_count; ++index)
    {
        close(pipes[index][0]);
        close(pipes[index][1]);
    }
    if (background && started > 0)
    {
        job->number = NextJobNumber();
        job->group = children[0];
        job->member_count = started;
        job->status = 0;
        for (size_t index = 0; index < started; ++index)
        {
            job->members[index] = children[index];
        }
        snprintf(job->command, sizeof(job->command), "%s", line);
        printf("[%d] %d\n", job->number, children[0]);
        return;
    }
    // As in other shells, the last command's status is the pipeline's.
    for (size_t index = 0; index < started; ++index)
    {
        int status = 0;
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
    "a | b connects one command's output to the next one's input, and a trailing &\n"
    "runs a command in the background: jobs lists those, fg [n] brings one back,\n"
    "and wait waits for all of them. kill %n signals job n.";

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
        ReapJobs();
        if (featheros_read_line(prompt, line, sizeof(line)) < 0)
        {
            ReportReadFailure(errno);
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
        if (strcmp(command, "jobs") == 0)
        {
            ListJobs();
            continue;
        }
        if (strcmp(command, "fg") == 0 || strncmp(command, "fg ", 3) == 0)
        {
            ForegroundCommand(command + 2);
            continue;
        }
        if (strcmp(command, "wait") == 0)
        {
            WaitForJobs();
            continue;
        }
        if (strcmp(command, "help") == 0)
        {
            puts(
                "Commands: calc [expression], date, echo <words...>, fanout <n>, "
                "aslr, hardening, memtest [overflow|exhaust], primes <workers> [limit], "
                "sleep <ms>, upper, uptime, filetest, fptest, libctest, ls, cat <file>, write [-a] "
                "<file> <text>, "
                "rm <file>, mkdir <dir>, rmdir <dir>, env, kill [-signal] <pid>, ps, top [-n "
                "count], cd [dir], pwd, "
                "jobs, "
                "fg [n], wait, "
                "exit, "
                "shutdown, reboot, help");
            puts("Run <command> --help to see what a command does.");
            continue;
        }
        RunCommand(command);
    }
}
