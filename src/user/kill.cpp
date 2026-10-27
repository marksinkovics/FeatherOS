#include "command_line.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

constexpr char Help[] =
    "usage: kill [-signal] <pid>...\n"
    "Sends a signal, SIGTERM by default, to each process, or to a whole process group\n"
    "for -group (kill -TERM -20). The signal is a number or a name such as INT, KILL\n"
    "or SIGUSR1. kill -l lists the signals.";

namespace
{

struct SignalName
{
    const char* name;
    int number;
};

constexpr SignalName Names[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"ABRT", SIGABRT}, {"KILL", SIGKILL},
    {"USR1", SIGUSR1}, {"SEGV", SIGSEGV}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM},
    {"TERM", SIGTERM}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT},
};

// A number, or a name with or without its SIG prefix, in any case.
int ParseSignal(const char* text)
{
    unsigned long long number = 0;
    if (FeatherOS::User::ParseUnsigned(text, &number))
    {
        return number < NSIG ? static_cast<int>(number) : -1;
    }
    if (strncasecmp(text, "SIG", 3) == 0)
    {
        text += 3;
    }
    for (const SignalName& entry : Names)
    {
        if (strcasecmp(text, entry.name) == 0)
        {
            return entry.number;
        }
    }
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-l") == 0)
    {
        for (const SignalName& entry : Names)
        {
            printf("%2d %s\n", entry.number, entry.name);
        }
        return 0;
    }
    int first = 1;
    int signal_number = SIGTERM;
    if (argc > 2 && argv[1][0] == '-')
    {
        signal_number = ParseSignal(argv[1] + 1);
        first = 2;
    }
    if (signal_number < 0 || first >= argc)
    {
        puts("usage: kill [-signal] <pid>...");
        return 1;
    }
    int failures = 0;
    for (int index = first; index < argc; ++index)
    {
        const bool group = argv[index][0] == '-';
        unsigned long long target = 0;
        if (!FeatherOS::User::ParseUnsigned(argv[index] + (group ? 1 : 0), &target) ||
            target == 0 || target > 0x7FFFFFFF)
        {
            printf("kill: %s: not a process ID\n", argv[index]);
            ++failures;
            continue;
        }
        const auto pid = static_cast<pid_t>(target);
        if (kill(group ? -pid : pid, signal_number) != 0)
        {
            printf("kill: %s: %s\n", argv[index],
                   errno == ESRCH ? "no such process" : strerror(errno));
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
