#include "command_line.h"
#include "process_table.h"

#include <stdio.h>

constexpr char Help[] =
    "usage: ps\n"
    "Lists every process from /proc: its ID, parent and process group, state\n"
    "(R running, S sleeping or waiting, Z exited), the CPU it last ran on, its\n"
    "CPU time, and its name.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 1)
    {
        puts("usage: ps");
        return 1;
    }
    FeatherOS::User::ProcessInfo table[FeatherOS::User::MaximumListedProcesses];
    const size_t count =
        FeatherOS::User::ReadProcessTable(table, FeatherOS::User::MaximumListedProcesses);
    if (count == 0)
    {
        puts("ps: cannot read /proc");
        return 1;
    }
    puts("  PID  PPID  PGID S CPU     TIME NAME");
    for (size_t index = 0; index < count; ++index)
    {
        const FeatherOS::User::ProcessInfo& process = table[index];
        char time[24];
        FeatherOS::User::FormatCpuTime(process.cpu_milliseconds, time, sizeof(time));
        printf("%5llu %5llu %5llu %c %3llu %8s %s\n", process.pid, process.parent, process.group,
               process.state, process.last_cpu, time, process.name);
    }
    return 0;
}
