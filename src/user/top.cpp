#include "command_line.h"
#include "process_table.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

using FeatherOS::User::ProcessInfo;
using FeatherOS::User::SystemInfo;

constexpr char Help[] =
    "usage: top [-n count] [-d milliseconds]\n"
    "Shows how much CPU each process used, busiest first, every second (or -d), count\n"
    "times (or until Ctrl+C). %CPU is of one CPU. Each refresh prints a new table,\n"
    "since the console cannot redraw the screen yet.";

namespace
{

constexpr unsigned long long MinimumDelay = 100;

struct Row
{
    const ProcessInfo* process;
    unsigned long long permille;
};

void SleepMilliseconds(unsigned long long milliseconds)
{
    const timespec duration = {static_cast<time_t>(milliseconds / 1000),
                               static_cast<long>(milliseconds % 1000 * 1000000)};
    nanosleep(&duration, nullptr);
}

const ProcessInfo* FindProcess(const ProcessInfo* table, size_t count, unsigned long long pid)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (table[index].pid == pid)
        {
            return &table[index];
        }
    }
    return nullptr;
}

// Busiest first, then by ID, so the order holds still between refreshes.
void SortRows(Row* rows, size_t count)
{
    for (size_t index = 1; index < count; ++index)
    {
        const Row row = rows[index];
        size_t slot = index;
        while (slot > 0 && (rows[slot - 1].permille < row.permille ||
                            (rows[slot - 1].permille == row.permille &&
                             rows[slot - 1].process->pid > row.process->pid)))
        {
            rows[slot] = rows[slot - 1];
            --slot;
        }
        rows[slot] = row;
    }
}

// A process that started during the interval is measured over the time since it started.
void PrintTable(const ProcessInfo* before, size_t before_count, const SystemInfo& before_system,
                const ProcessInfo* after, size_t after_count, const SystemInfo& after_system)
{
    const unsigned long long wall =
        after_system.uptime_milliseconds - before_system.uptime_milliseconds;
    unsigned long long busy = 0;
    for (unsigned long long cpu = 0; cpu < after_system.cpus && cpu < before_system.cpus; ++cpu)
    {
        busy += after_system.busy_milliseconds[cpu] - before_system.busy_milliseconds[cpu];
    }
    Row rows[FeatherOS::User::MaximumListedProcesses];
    for (size_t index = 0; index < after_count; ++index)
    {
        const ProcessInfo& process = after[index];
        const ProcessInfo* previous = FindProcess(before, before_count, process.pid);
        const unsigned long long used =
            process.cpu_milliseconds - (previous != nullptr ? previous->cpu_milliseconds : 0);
        const unsigned long long since_start =
            after_system.uptime_milliseconds - process.start_milliseconds;
        const unsigned long long span =
            previous != nullptr || since_start > wall ? wall : since_start;
        rows[index] = {&process, FeatherOS::User::CpuPermille(used, span)};
    }
    SortRows(rows, after_count);
    const unsigned long long system_permille =
        FeatherOS::User::CpuPermille(busy, wall * after_system.cpus);
    const unsigned long long uptime = after_system.uptime_milliseconds / 1000;
    printf("top: up %llu:%02llu:%02llu, %zu processes, CPU %llu.%llu%% of %llu\n", uptime / 3600,
           uptime / 60 % 60, uptime % 60, after_count, system_permille / 10, system_permille % 10,
           after_system.cpus);
    puts("  PID S  %CPU CPU     TIME NAME");
    for (size_t index = 0; index < after_count; ++index)
    {
        const ProcessInfo& process = *rows[index].process;
        char time[24];
        FeatherOS::User::FormatCpuTime(process.cpu_milliseconds, time, sizeof(time));
        printf("%5llu %c %3llu.%llu %3llu %8s %s\n", process.pid, process.state,
               rows[index].permille / 10, rows[index].permille % 10, process.last_cpu, time,
               process.name);
    }
}

bool ParseOptions(int argc, char** argv, unsigned long long* count, unsigned long long* delay)
{
    for (int index = 1; index < argc; index += 2)
    {
        unsigned long long value = 0;
        if (index + 1 >= argc || !FeatherOS::User::ParseUnsigned(argv[index + 1], &value))
        {
            return false;
        }
        if (strcmp(argv[index], "-n") == 0 && value > 0)
        {
            *count = value;
        }
        else if (strcmp(argv[index], "-d") == 0 && value >= MinimumDelay)
        {
            *delay = value;
        }
        else
        {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    // 0 refreshes forever.
    unsigned long long count = 0;
    unsigned long long delay = 1000;
    if (!ParseOptions(argc, argv, &count, &delay))
    {
        puts("usage: top [-n count] [-d milliseconds, at least 100]");
        return 1;
    }
    static ProcessInfo tables[2][FeatherOS::User::MaximumListedProcesses];
    SystemInfo systems[2];
    size_t counts[2] = {
        FeatherOS::User::ReadProcessTable(tables[0], FeatherOS::User::MaximumListedProcesses), 0};
    if (counts[0] == 0 || !FeatherOS::User::ReadSystemInfo(&systems[0]))
    {
        puts("top: cannot read /proc");
        return 1;
    }
    for (unsigned long long refresh = 0; count == 0 || refresh < count; ++refresh)
    {
        SleepMilliseconds(delay);
        const size_t now = (refresh + 1) % 2;
        const size_t then = refresh % 2;
        counts[now] =
            FeatherOS::User::ReadProcessTable(tables[now], FeatherOS::User::MaximumListedProcesses);
        if (!FeatherOS::User::ReadSystemInfo(&systems[now]))
        {
            puts("top: cannot read /proc/stat");
            return 1;
        }
        if (refresh > 0)
        {
            putchar('\n');
        }
        PrintTable(tables[then], counts[then], systems[then], tables[now], counts[now],
                   systems[now]);
    }
    return 0;
}
