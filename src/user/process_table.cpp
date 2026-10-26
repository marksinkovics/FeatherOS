#include "process_table.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace FeatherOS::User
{

namespace ProcessTableDetail
{

const char* FindValue(const char* text, const char* key)
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

bool ReadNumber(const char* text, const char* key, unsigned long long* value)
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

bool ParseProcessStatus(const char* text, ProcessInfo* info)
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

bool ParseSystemStat(const char* text, SystemInfo* info)
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

void FormatCpuTime(unsigned long long milliseconds, char* text, size_t size)
{
    snprintf(text, size, "%llu:%02llu.%02llu", milliseconds / 60000, milliseconds / 1000 % 60,
             milliseconds % 1000 / 10);
}

unsigned long long CpuPermille(unsigned long long busy, unsigned long long wall)
{
    return wall == 0 ? 0 : (busy * 1000 + wall / 2) / wall;
}

bool ReadSmallFile(const char* path, char* text, size_t size)
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

size_t ReadProcessTable(ProcessInfo* table, size_t capacity)
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

bool ReadSystemInfo(SystemInfo* info)
{
    char text[1024];
    return ReadSmallFile("/proc/stat", text, sizeof(text)) && ParseSystemStat(text, info);
}

} // namespace FeatherOS::User
