#include "procfs.h"

#include "kernel/string/format.h"

namespace FeatherOS::Procfs
{
namespace
{

enum class Kind : uint8_t
{
    None,
    Root,
    MemoryInfo,
    SystemStat,
    ProcessDirectory,
    ProcessStatusFile,
};

constexpr char MemoryInfoName[] = "meminfo";
constexpr char SystemStatName[] = "stat";
constexpr char StatusName[] = "status";

Source source = {};

bool Equals(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

// Reads a process ID from the leading digits of path and leaves *rest after them.
bool ParseProcessId(const char* path, uint64_t* process_id, const char** rest)
{
    uint64_t value = 0;
    const char* cursor = path;
    for (; *cursor >= '0' && *cursor <= '9'; ++cursor)
    {
        if (value > (UINT64_MAX - 9) / 10)
        {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(*cursor - '0');
    }
    // Leading zeros would give one process many names.
    if (cursor == path || (path[0] == '0' && cursor - path > 1))
    {
        return false;
    }
    *process_id = value;
    *rest = cursor;
    return true;
}

bool ProcessExists(uint64_t process_id)
{
    ProcessStatus status;
    return source.process_status != nullptr && source.process_status(process_id, &status);
}

Kind Classify(const char* path, uint64_t* process_id)
{
    if (*path == '\0')
    {
        return Kind::Root;
    }
    if (Equals(path, MemoryInfoName))
    {
        return Kind::MemoryInfo;
    }
    if (Equals(path, SystemStatName))
    {
        return Kind::SystemStat;
    }
    const char* rest;
    if (!ParseProcessId(path, process_id, &rest) || !ProcessExists(*process_id))
    {
        return Kind::None;
    }
    if (*rest == '\0')
    {
        return Kind::ProcessDirectory;
    }
    return *rest == '/' && Equals(rest + 1, StatusName) ? Kind::ProcessStatusFile : Kind::None;
}

const char* StateName(char state)
{
    switch (state)
    {
    case 'R':
        return "running";
    case 'S':
        return "sleeping";
    default:
        return "zombie";
    }
}

size_t Length(const char* text)
{
    size_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

void SetEntry(FAT16::DirectoryEntry* entry, const char* name, bool directory)
{
    uint32_t index = 0;
    for (; name[index] != '\0' && index < sizeof(entry->name) - 1; ++index)
    {
        entry->name[index] = name[index];
    }
    entry->name[index] = '\0';
    entry->directory = directory;
    entry->size = 0;
}

} // namespace

void SetSource(const Source& new_source)
{
    source = new_source;
}

bool Generate(const char* path, char* buffer, size_t* length)
{
    uint64_t process_id = 0;
    if (path == nullptr || buffer == nullptr || length == nullptr)
    {
        return false;
    }
    bool formatted = false;
    switch (Classify(path, &process_id))
    {
    case Kind::MemoryInfo:
    {
        if (source.memory_status == nullptr)
        {
            return false;
        }
        MemoryStatus memory = {};
        source.memory_status(&memory);
        formatted = String::FormatTo(buffer, MaximumFileSize,
                                     "MemTotal:\t{} kB\nMemFree:\t{} kB\nKernelHeap:\t{} kB\n"
                                     "Tmp:\t{} kB\n",
                                     memory.total_bytes / 1024, memory.free_bytes / 1024,
                                     memory.kernel_heap_bytes / 1024, memory.tmp_bytes / 1024);
        break;
    }
    case Kind::SystemStat:
    {
        if (source.system_status == nullptr)
        {
            return false;
        }
        SystemStatus system = {};
        source.system_status(&system);
        formatted = String::FormatTo(buffer, MaximumFileSize, "UptimeMs:\t{}\nCpus:\t{}\n",
                                     system.uptime_milliseconds, system.cpu_count);
        size_t used = Length(buffer);
        const uint32_t cpus = system.cpu_count < MaximumCpus ? system.cpu_count : MaximumCpus;
        for (uint32_t cpu = 0; formatted && cpu < cpus; ++cpu)
        {
            formatted = String::FormatTo(buffer + used, MaximumFileSize - used,
                                         "Cpu{}BusyMs:\t{}\n", cpu, system.busy_milliseconds[cpu]);
            used += Length(buffer + used);
        }
        break;
    }
    case Kind::ProcessStatusFile:
    {
        ProcessStatus status;
        if (!source.process_status(process_id, &status))
        {
            return false;
        }
        formatted = String::FormatTo(
            buffer, MaximumFileSize,
            "Name:\t{}\nState:\t{} ({})\nPid:\t{}\nPPid:\t{}\nPgid:\t{}\nPages:\t{}\n"
            "PageLimit:\t{}\nOpenFiles:\t{}\nCpuTimeMs:\t{}\nLastCpu:\t{}\nStartTimeMs:\t{}\n"
            "Threads:\t{}\n",
            status.name, status.state, StateName(status.state), status.process_id, status.parent_id,
            status.group, status.pages, status.page_limit, status.open_files,
            status.cpu_milliseconds, status.last_cpu, status.start_milliseconds, status.threads);
        break;
    }
    default:
        return false;
    }
    if (!formatted)
    {
        return false;
    }
    *length = Length(buffer);
    return true;
}

bool IsDirectory(const char* path)
{
    uint64_t process_id = 0;
    if (path == nullptr)
    {
        return false;
    }
    const Kind kind = Classify(path, &process_id);
    return kind == Kind::Root || kind == Kind::ProcessDirectory;
}

bool ReadDirectory(const char* path, uint32_t index, FAT16::DirectoryEntry* entry)
{
    uint64_t process_id = 0;
    if (path == nullptr || entry == nullptr)
    {
        return false;
    }
    const Kind kind = Classify(path, &process_id);
    if (kind == Kind::ProcessDirectory)
    {
        if (index != 0)
        {
            return false;
        }
        SetEntry(entry, StatusName, false);
        return true;
    }
    if (kind != Kind::Root)
    {
        return false;
    }
    if (index == 0 || index == 1)
    {
        SetEntry(entry, index == 0 ? MemoryInfoName : SystemStatName, false);
        return true;
    }
    if (source.process_at == nullptr || !source.process_at(index - 2, &process_id))
    {
        return false;
    }
    char name[24];
    if (!String::FormatTo(name, sizeof(name), "{}", process_id))
    {
        return false;
    }
    SetEntry(entry, name, true);
    return true;
}

} // namespace FeatherOS::Procfs
