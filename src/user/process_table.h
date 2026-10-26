#ifndef FEATHEROS_USER_PROCESS_TABLE_H
#define FEATHEROS_USER_PROCESS_TABLE_H

#include <stddef.h>

// What ps and top read from /proc: each process's status and the system's CPU counters.
namespace FeatherOS::User
{

constexpr size_t MaximumListedProcesses = 64;
constexpr size_t MaximumListedCpus = 16;

struct ProcessInfo
{
    unsigned long long pid;
    unsigned long long parent;
    unsigned long long group;
    unsigned long long pages;
    unsigned long long cpu_milliseconds;
    unsigned long long start_milliseconds;
    unsigned long long last_cpu;
    char state;
    char name[40];
};

struct SystemInfo
{
    unsigned long long uptime_milliseconds;
    unsigned long long cpus;
    unsigned long long busy_milliseconds[MaximumListedCpus];
};

namespace ProcessTableDetail
{

// The value after "Key:\t" on the line that starts with key, or nullptr.
const char* FindValue(const char* text, const char* key);

bool ReadNumber(const char* text, const char* key, unsigned long long* value);

} // namespace ProcessTableDetail

// False if a field is missing, as when the status came from a different kernel.
bool ParseProcessStatus(const char* text, ProcessInfo* info);

bool ParseSystemStat(const char* text, SystemInfo* info);

// "M:SS.hh", as ps shows CPU time: minutes, seconds and hundredths.
void FormatCpuTime(unsigned long long milliseconds, char* text, size_t size);

// Tenths of a percent of one CPU: busy for all of wall gives 1000.
unsigned long long CpuPermille(unsigned long long busy, unsigned long long wall);

bool ReadSmallFile(const char* path, char* text, size_t size);

// Every process /proc lists, in ascending ID; processes that exit while it is read are left out.
size_t ReadProcessTable(ProcessInfo* table, size_t capacity);

bool ReadSystemInfo(SystemInfo* info);

} // namespace FeatherOS::User

#endif
