#ifndef FEATHEROS_USER_PROCESS_TABLE_H
#define FEATHEROS_USER_PROCESS_TABLE_H

#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
inline const char* FindValue(const char* text, const char* key)
{
    const size_t key_length = strlen(key);
    for (const char* line = text; *line != '\0';)
    {
        if (strncmp(line, key, key_length) == 0 && line[key_length] == ':' &&
            line[key_length + 1] == '\t')
        {
            return line + key_length + 2;
        }
        const char* end = strchr(line, '\n');
        if (end == nullptr)
        {
            break;
        }
        line = end + 1;
    }
    return nullptr;
}

inline bool ReadNumber(const char* text, const char* key, unsigned long long* value)
{
    const char* found = FindValue(text, key);
    if (found == nullptr || *found < '0' || *found > '9')
    {
        return false;
    }
    char* end = nullptr;
    *value = strtoull(found, &end, 10);
    return *end == '\n' || *end == '\0';
}

} // namespace ProcessTableDetail

// False if a field is missing, as when the status came from a different kernel.
inline bool ParseProcessStatus(const char* text, ProcessInfo* info)
{
    using namespace ProcessTableDetail;
    const char* name = FindValue(text, "Name");
    const char* state = FindValue(text, "State");
    if (name == nullptr || state == nullptr || !ReadNumber(text, "Pid", &info->pid) ||
        !ReadNumber(text, "PPid", &info->parent) || !ReadNumber(text, "Pgid", &info->group) ||
        !ReadNumber(text, "Pages", &info->pages) ||
        !ReadNumber(text, "CpuTimeMs", &info->cpu_milliseconds) ||
        !ReadNumber(text, "LastCpu", &info->last_cpu) ||
        !ReadNumber(text, "StartTimeMs", &info->start_milliseconds))
    {
        return false;
    }
    size_t length = strcspn(name, "\n");
    length = length < sizeof(info->name) - 1 ? length : sizeof(info->name) - 1;
    memcpy(info->name, name, length);
    info->name[length] = '\0';
    info->state = *state;
    return true;
}

inline bool ParseSystemStat(const char* text, SystemInfo* info)
{
    using namespace ProcessTableDetail;
    if (!ReadNumber(text, "UptimeMs", &info->uptime_milliseconds) ||
        !ReadNumber(text, "Cpus", &info->cpus) || info->cpus == 0)
    {
        return false;
    }
    info->cpus = info->cpus < MaximumListedCpus ? info->cpus : MaximumListedCpus;
    for (unsigned long long cpu = 0; cpu < info->cpus; ++cpu)
    {
        char key[24];
        snprintf(key, sizeof(key), "Cpu%lluBusyMs", cpu);
        if (!ReadNumber(text, key, &info->busy_milliseconds[cpu]))
        {
            return false;
        }
    }
    return true;
}

// "M:SS.hh", as ps shows CPU time: minutes, seconds and hundredths.
inline void FormatCpuTime(unsigned long long milliseconds, char* text, size_t size)
{
    snprintf(text, size, "%llu:%02llu.%02llu", milliseconds / 60000, milliseconds / 1000 % 60,
             milliseconds % 1000 / 10);
}

// Tenths of a percent of one CPU: busy for all of wall gives 1000.
inline unsigned long long CpuPermille(unsigned long long busy, unsigned long long wall)
{
    return wall == 0 ? 0 : (busy * 1000 + wall / 2) / wall;
}

inline bool ReadSmallFile(const char* path, char* text, size_t size)
{
    FILE* file = fopen(path, "r");
    if (file == nullptr)
    {
        return false;
    }
    const size_t length = fread(text, 1, size - 1, file);
    fclose(file);
    text[length] = '\0';
    return length > 0;
}

// Every process /proc lists, in ascending ID; processes that exit while it is read are left out.
inline size_t ReadProcessTable(ProcessInfo* table, size_t capacity)
{
    DIR* directory = opendir("/proc");
    if (directory == nullptr)
    {
        return 0;
    }
    size_t count = 0;
    for (const dirent* entry = readdir(directory); entry != nullptr && count < capacity;
         entry = readdir(directory))
    {
        if (entry->d_type != DT_DIR)
        {
            continue;
        }
        char path[48];
        char text[1024];
        snprintf(path, sizeof(path), "/proc/%s/status", entry->d_name);
        if (ReadSmallFile(path, text, sizeof(text)) && ParseProcessStatus(text, &table[count]))
        {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

inline bool ReadSystemInfo(SystemInfo* info)
{
    char text[1024];
    return ReadSmallFile("/proc/stat", text, sizeof(text)) && ParseSystemStat(text, info);
}

} // namespace FeatherOS::User

#endif
