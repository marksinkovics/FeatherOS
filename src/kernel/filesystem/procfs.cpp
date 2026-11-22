#include "procfs.h"

#include "kernel/memory/heap.h"
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
    Mounts,
    UsbDevices,
    KernelLog,
    ProcessDirectory,
    ProcessStatusFile,
};

constexpr char MemoryInfoName[] = "meminfo";
constexpr char SystemStatName[] = "stat";
constexpr char MountsName[] = "mounts";
constexpr char UsbName[] = "usb";
constexpr char KernelLogName[] = "kmsg";
constexpr const char* FixedNames[] = {MemoryInfoName, SystemStatName, MountsName, UsbName,
                                      KernelLogName};
constexpr Kind FixedKinds[] = {Kind::MemoryInfo, Kind::SystemStat, Kind::Mounts, Kind::UsbDevices,
                               Kind::KernelLog};
constexpr uint32_t FixedCount = sizeof(FixedNames) / sizeof(FixedNames[0]);
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
    if (Equals(path, MountsName))
    {
        return Kind::Mounts;
    }
    if (Equals(path, UsbName))
    {
        return Kind::UsbDevices;
    }
    if (Equals(path, KernelLogName))
    {
        return Kind::KernelLog;
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
    case 'T':
        return "stopped";
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

// The root is 1 and the fixed files follow it; each process has two numbers past them.
constexpr uint64_t RootSerialNumber = 1;
constexpr uint64_t FirstProcessSerialNumber = 16;

uint64_t SerialNumber(Kind kind, uint64_t process_id)
{
    switch (kind)
    {
    case Kind::Root:
        return RootSerialNumber;
    case Kind::MemoryInfo:
        return RootSerialNumber + 1;
    case Kind::SystemStat:
        return RootSerialNumber + 2;
    case Kind::Mounts:
        return RootSerialNumber + 3;
    case Kind::UsbDevices:
        return RootSerialNumber + 4;
    case Kind::KernelLog:
        return RootSerialNumber + 5;
    case Kind::ProcessDirectory:
        return FirstProcessSerialNumber + 2 * process_id;
    case Kind::ProcessStatusFile:
        return FirstProcessSerialNumber + 2 * process_id + 1;
    case Kind::None:
        break;
    }
    return 0;
}

void SetEntry(Filesystem::DirectoryEntry* entry, const char* name, Kind kind,
              uint64_t process_id = 0)
{
    uint32_t index = 0;
    for (; name[index] != '\0' && index < sizeof(entry->name) - 1; ++index)
    {
        entry->name[index] = name[index];
    }
    entry->name[index] = '\0';
    const bool directory = kind == Kind::Root || kind == Kind::ProcessDirectory;
    entry->type = directory ? User::ModeDirectory : User::ModeRegular;
    entry->node = SerialNumber(kind, process_id);
    entry->size = 0;
}

// One line per mount, as Linux's /proc/mounts: "<source> <point> <type> <ro|rw> 0 0".
bool FormatMounts(char* buffer)
{
    buffer[0] = '\0';
    size_t used = 0;
    const Memory::HeapObject<MountStatus> mount;
    if (!mount)
    {
        return false;
    }
    for (uint32_t index = 0; source.mount_at != nullptr && source.mount_at(index, &*mount); ++index)
    {
        if (!String::FormatTo(buffer + used, MaximumFileSize - used, "{} {} {} {} 0 0\n",
                              mount->source, mount->point, mount->type,
                              mount->read_only ? "ro" : "rw"))
        {
            return false;
        }
        used += Length(buffer + used);
    }
    return true;
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
                                     "Tmp:\t{} kB\nCached:\t{} kB\n",
                                     memory.total_bytes / 1024, memory.free_bytes / 1024,
                                     memory.kernel_heap_bytes / 1024, memory.tmp_bytes / 1024,
                                     memory.cached_bytes / 1024);
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
    case Kind::Mounts:
        formatted = FormatMounts(buffer);
        break;
    case Kind::UsbDevices:
        buffer[0] = '\0';
        if (source.usb_devices != nullptr)
        {
            source.usb_devices(buffer, MaximumFileSize);
        }
        formatted = true;
        break;
    case Kind::KernelLog:
        buffer[0] = '\0';
        if (source.kernel_log != nullptr)
        {
            source.kernel_log(buffer, MaximumKernelLogFileSize);
        }
        formatted = true;
        break;
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

size_t FileCapacity(const char* path)
{
    uint64_t process_id = 0;
    return path != nullptr && Classify(path, &process_id) == Kind::KernelLog
               ? MaximumKernelLogFileSize
               : MaximumFileSize;
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

bool Find(const char* path, uint64_t* serial_number, bool* directory)
{
    uint64_t process_id = 0;
    if (path == nullptr || serial_number == nullptr || directory == nullptr)
    {
        return false;
    }
    const Kind kind = Classify(path, &process_id);
    *serial_number = SerialNumber(kind, process_id);
    *directory = kind == Kind::Root || kind == Kind::ProcessDirectory;
    return kind != Kind::None;
}

bool ReadDirectory(const char* path, uint32_t index, Filesystem::DirectoryEntry* entry)
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
        SetEntry(entry, StatusName, Kind::ProcessStatusFile, process_id);
        return true;
    }
    if (kind != Kind::Root)
    {
        return false;
    }
    if (index < FixedCount)
    {
        SetEntry(entry, FixedNames[index], FixedKinds[index]);
        return true;
    }
    if (source.process_at == nullptr || !source.process_at(index - FixedCount, &process_id))
    {
        return false;
    }
    char name[24];
    if (!String::FormatTo(name, sizeof(name), "{}", process_id))
    {
        return false;
    }
    SetEntry(entry, name, Kind::ProcessDirectory, process_id);
    return true;
}

} // namespace FeatherOS::Procfs
