#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/filesystem/devices.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/tmpfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/memory/heap.h"
#include "kernel/random.h"
#include "kernel/terminal/terminal.h"
#include "support/heap_failure.h"
#include "support/terminal_device.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// The kernel heap and random source, which the host tests do not build.
namespace FeatherOS::Memory
{

namespace Testing
{
std::size_t failing_zeroed_count = 0;
std::size_t failing_zeroed_size = 0;
} // namespace Testing

void* Allocate(size_t size)
{
    return std::malloc(size);
}

bool Deallocate(void* pointer)
{
    std::free(pointer);
    return true;
}

void* AllocateZeroed(size_t count, size_t size)
{
    if (Testing::failing_zeroed_count != 0 && count == Testing::failing_zeroed_count &&
        size == Testing::failing_zeroed_size)
    {
        return nullptr;
    }
    return std::calloc(count, size);
}

} // namespace FeatherOS::Memory

namespace FeatherOS::Random
{

uint64_t Next()
{
    static uint64_t counter = 0;
    return 0x0123456789ABCDEF + ++counter;
}

} // namespace FeatherOS::Random

namespace
{

// As the kernel does at boot.
[[maybe_unused]] const bool built_in_mounted = FeatherOS::VFS::MountBuiltIn();

using FeatherOS::User::OpenAppend;
using FeatherOS::User::OpenCreate;
using FeatherOS::User::OpenRead;
using FeatherOS::User::OpenTruncate;
using FeatherOS::User::OpenWrite;

constexpr FeatherOS::Tmpfs::Root Tmp = FeatherOS::Tmpfs::Root::Tmp;
constexpr FeatherOS::Tmpfs::Root Etc = FeatherOS::Tmpfs::Root::Etc;

std::vector<std::string> List(const char* path)
{
    std::vector<std::string> names;
    FeatherOS::Filesystem::DirectoryEntry entry{};
    for (uint32_t index = 0; FeatherOS::VFS::ReadDirectory(path, index, &entry); ++index)
    {
        names.push_back(std::string(entry.name) +
                        (entry.type == FeatherOS::User::ModeDirectory ? "/" : ""));
    }
    return names;
}

std::string ReadAll(const char* path)
{
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open(path, &file));
    std::string contents(file.size, '\0');
    uint64_t bytes_read = 0;
    REQUIRE(FeatherOS::VFS::Read(&file, contents.data(), contents.size(), &bytes_read));
    REQUIRE(bytes_read == contents.size());
    FeatherOS::VFS::Close(&file);
    return contents;
}

bool FakeProcessAt(uint32_t index, uint64_t* process_id)
{
    const uint64_t ids[] = {1, 4, 12};
    if (index >= 3)
    {
        return false;
    }
    *process_id = ids[index];
    return true;
}

bool FakeProcessStatus(uint64_t process_id, FeatherOS::Procfs::ProcessStatus* status)
{
    if (process_id != 1 && process_id != 4 && process_id != 12)
    {
        return false;
    }
    std::strcpy(status->name, process_id == 1 ? "init" : "shell");
    status->state = process_id == 12 ? 'R' : 'S';
    status->process_id = process_id;
    status->parent_id = process_id == 1 ? 0 : 1;
    status->group = process_id == 12 ? 4 : process_id;
    status->pages = 37;
    status->page_limit = 1024;
    status->open_files = 2;
    status->cpu_milliseconds = process_id * 10;
    status->last_cpu = 3;
    status->start_milliseconds = 1500;
    status->threads = 3;
    return true;
}

void FakeMemoryStatus(FeatherOS::Procfs::MemoryStatus* status)
{
    status->total_bytes = 256 * 1024 * 1024;
    status->free_bytes = 200 * 1024 * 1024;
    status->kernel_heap_bytes = 64 * 1024;
    status->tmp_bytes = FeatherOS::Tmpfs::UsedBytes();
}

void FakeSystemStatus(FeatherOS::Procfs::SystemStatus* status)
{
    status->uptime_milliseconds = 61000;
    status->cpu_count = 2;
    status->busy_milliseconds[0] = 700;
    status->busy_milliseconds[1] = 30;
}

bool FakeMountAt(uint32_t index, FeatherOS::Procfs::MountStatus* status)
{
    if (index >= 2)
    {
        return false;
    }
    *status = index == 0 ? FeatherOS::Procfs::MountStatus{"initramfs", "initramfs", "/system", true}
                         : FeatherOS::Procfs::MountStatus{"vda1", "fat16", "/", false};
    return true;
}

void UseFakeProcesses()
{
    FeatherOS::Procfs::SetSource(
        {FakeProcessAt, FakeProcessStatus, FakeMemoryStatus, FakeSystemStatus, FakeMountAt});
}

bool NoWorkingDirectory(const char* /*point*/)
{
    return false;
}

bool EveryWorkingDirectory(const char* /*point*/)
{
    return true;
}

} // namespace

TEST_CASE("tmpfs files grow, shrink, and keep their contents")
{
    FeatherOS::Tmpfs::Reset();
    uint32_t node = 0;
    REQUIRE(FeatherOS::Tmpfs::Create(Tmp, "a.txt", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create(Tmp, "a.txt", &node));
    std::string text(5000, '\0');
    for (size_t index = 0; index < text.size(); ++index)
    {
        text[index] = static_cast<char>('a' + index % 26);
    }
    uint64_t count = 0;
    REQUIRE(FeatherOS::Tmpfs::Write(node, 0, text.data(), 3000, &count));
    REQUIRE(FeatherOS::Tmpfs::Write(node, 3000, text.data() + 3000, 2000, &count));
    REQUIRE(FeatherOS::Tmpfs::Size(node) == 5000);
    REQUIRE_FALSE(FeatherOS::Tmpfs::Write(node, 5001, "x", 1, &count));
    std::string back(5000, '\0');
    REQUIRE(FeatherOS::Tmpfs::Read(node, 0, back.data(), back.size(), &count));
    REQUIRE(count == 5000);
    REQUIRE(back == text);
    REQUIRE(FeatherOS::Tmpfs::Truncate(node, 10));
    REQUIRE(FeatherOS::Tmpfs::Size(node) == 10);
    REQUIRE_FALSE(FeatherOS::Tmpfs::Truncate(node, 20));
    REQUIRE(FeatherOS::Tmpfs::Truncate(node, 0));
    REQUIRE(FeatherOS::Tmpfs::UsedBytes() == 0);
    REQUIRE(FeatherOS::Tmpfs::Remove(Tmp, "a.txt"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Open(Tmp, "a.txt", &node));
}

TEST_CASE("tmpfs directories nest, and names are case-sensitive")
{
    FeatherOS::Tmpfs::Reset();
    uint32_t node = 0;
    REQUIRE(FeatherOS::Tmpfs::MakeDirectory(Tmp, "docs"));
    REQUIRE(FeatherOS::Tmpfs::MakeDirectory(Tmp, "docs/old"));
    REQUIRE(FeatherOS::Tmpfs::Create(Tmp, "docs/Plan.txt", &node));
    REQUIRE(FeatherOS::Tmpfs::Create(Tmp, "docs/plan.txt", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create(Tmp, "missing/plan.txt", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create(Tmp, "docs/Plan.txt/x", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create(Tmp, "docs/..", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create(Tmp, std::string(256, 'n').c_str(), &node));
    REQUIRE(FeatherOS::Tmpfs::Create(Tmp, std::string(255, 'n').c_str(), &node));
    REQUIRE(FeatherOS::Tmpfs::Remove(Tmp, std::string(255, 'n').c_str()));
    REQUIRE(FeatherOS::Tmpfs::IsDirectory(Tmp, ""));
    REQUIRE(FeatherOS::Tmpfs::IsDirectory(Tmp, "docs/old"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::IsDirectory(Tmp, "docs/plan.txt"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Open(Tmp, "docs", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::RemoveDirectory(Tmp, "docs"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::RemoveDirectory(Tmp, ""));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Remove(Tmp, "docs/old"));
    REQUIRE(FeatherOS::Tmpfs::RemoveDirectory(Tmp, "docs/old"));
    REQUIRE(FeatherOS::Tmpfs::Remove(Tmp, "docs/Plan.txt"));
    REQUIRE(FeatherOS::Tmpfs::Remove(Tmp, "docs/plan.txt"));
    REQUIRE(FeatherOS::Tmpfs::RemoveDirectory(Tmp, "docs"));
}

TEST_CASE("tmpfs renames move nodes, but never into themselves")
{
    FeatherOS::Tmpfs::Reset();
    uint32_t node = 0;
    uint32_t moved = 0;
    REQUIRE(FeatherOS::Tmpfs::MakeDirectory(Tmp, "docs"));
    REQUIRE(FeatherOS::Tmpfs::MakeDirectory(Tmp, "docs/old"));
    REQUIRE(FeatherOS::Tmpfs::Create(Tmp, "docs/old/a.txt", &node));
    REQUIRE(FeatherOS::Tmpfs::Create(Tmp, "b.txt", &moved));
    REQUIRE(FeatherOS::Tmpfs::Rename(Tmp, "docs/old", "archive"));
    REQUIRE(FeatherOS::Tmpfs::Open(Tmp, "archive/a.txt", &moved));
    REQUIRE(moved == node);
    REQUIRE(FeatherOS::Tmpfs::Rename(Tmp, "archive", "docs/archive"));
    REQUIRE(FeatherOS::Tmpfs::Rename(Tmp, "b.txt", "docs/B.txt"));
    std::vector<std::string> names = List("/tmp/docs");
    std::sort(names.begin(), names.end());
    REQUIRE(names == std::vector<std::string>{"B.txt", "archive/"});
    REQUIRE_FALSE(FeatherOS::Tmpfs::Rename(Tmp, "docs", "docs/archive/inner"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Rename(Tmp, "docs/B.txt", "docs/archive"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Rename(Tmp, "docs/B.txt", "missing/B.txt"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Rename(Tmp, "", "root"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Rename(Tmp, "gone", "here"));
}

TEST_CASE("tmpfs stops at its node and byte limits")
{
    FeatherOS::Tmpfs::Reset();
    uint32_t node = 0;
    for (uint32_t index = 0; index < FeatherOS::Tmpfs::MaximumEntries; ++index)
    {
        REQUIRE(FeatherOS::Tmpfs::Create(Tmp, ("f" + std::to_string(index)).c_str(), &node));
    }
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create(Tmp, "one-too-many", &node));
    REQUIRE(FeatherOS::Tmpfs::Open(Tmp, "f1", &node));
    std::vector<char> large(FeatherOS::Tmpfs::MaximumBytes - 100, 'x');
    uint64_t count = 0;
    REQUIRE(FeatherOS::Tmpfs::Write(node, 0, large.data(), large.size(), &count));
    REQUIRE(FeatherOS::Tmpfs::Open(Tmp, "f2", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Write(node, 0, large.data(), 1000, &count));
    REQUIRE(count == 100);
    REQUIRE(FeatherOS::Tmpfs::UsedBytes() == FeatherOS::Tmpfs::MaximumBytes);
    FeatherOS::Tmpfs::Reset();
    REQUIRE(FeatherOS::Tmpfs::UsedBytes() == 0);
}

TEST_CASE("tmpfs roots keep separate trees but share the limits")
{
    FeatherOS::Tmpfs::Reset();
    uint32_t tmp_node = 0;
    uint32_t etc_node = 0;
    REQUIRE(FeatherOS::Tmpfs::Create(Tmp, "hosts", &tmp_node));
    REQUIRE(FeatherOS::Tmpfs::Create(Etc, "hosts", &etc_node));
    REQUIRE(tmp_node != etc_node);
    REQUIRE(FeatherOS::Tmpfs::MakeDirectory(Etc, "conf"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::IsDirectory(Tmp, "conf"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::RemoveDirectory(Etc, ""));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Rename(Etc, "", "root"));
    REQUIRE(FeatherOS::Tmpfs::Rename(Etc, "hosts", "conf/hosts"));
    REQUIRE(List("/etc") == std::vector<std::string>{"conf/"});
    REQUIRE(List("/etc/conf") == std::vector<std::string>{"hosts"});
    REQUIRE(List("/tmp") == std::vector<std::string>{"hosts"});
    REQUIRE_FALSE(FeatherOS::VFS::IsReadOnly("/etc/conf/hosts"));
    REQUIRE(FeatherOS::Files::Rename("/etc/conf/hosts", "/tmp/moved") ==
            FeatherOS::User::SyscallErrorCrossDevice);

    uint32_t node = 0;
    for (uint32_t index = 3; index < FeatherOS::Tmpfs::MaximumEntries; ++index)
    {
        REQUIRE(FeatherOS::Tmpfs::Create(Tmp, ("f" + std::to_string(index)).c_str(), &node));
    }
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create(Etc, "full", &node));
    REQUIRE(FeatherOS::Tmpfs::Remove(Tmp, "hosts"));
    REQUIRE(FeatherOS::Tmpfs::Create(Etc, "full", &node));
}

TEST_CASE("/tmp works through file descriptors with the disk's sharing rules")
{
    FeatherOS::Tmpfs::Reset();
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    REQUIRE(FeatherOS::Files::CheckDirectory("/tmp") == 0);
    REQUIRE(FeatherOS::Files::MakeDirectory("/tmp/work") == 0);
    const uint64_t writer =
        FeatherOS::Files::Open(&table, "/tmp/work/Notes.txt", OpenWrite | OpenCreate);
    REQUIRE(writer == FeatherOS::User::StandardError + 1);
    REQUIRE(FeatherOS::Files::Write(&table, writer, "hello tmp", 9) == 9);
    REQUIRE(FeatherOS::Files::Open(&table, "/tmp/work/Notes.txt", OpenRead) ==
            FeatherOS::User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Remove("/tmp/work/Notes.txt") == FeatherOS::User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Close(&table, writer) == 0);
    REQUIRE(ReadAll("/tmp/work/Notes.txt") == "hello tmp");
    REQUIRE(FeatherOS::Files::Open(&table, "/tmp/work/notes.txt", OpenRead) ==
            FeatherOS::User::SyscallErrorNotFound);

    const uint64_t appender =
        FeatherOS::Files::Open(&table, "/tmp/work/Notes.txt", OpenWrite | OpenAppend);
    REQUIRE(FeatherOS::Files::Write(&table, appender, "!", 1) == 1);
    REQUIRE(FeatherOS::Files::Close(&table, appender) == 0);
    const uint64_t truncator =
        FeatherOS::Files::Open(&table, "/tmp/work/Notes.txt", OpenWrite | OpenTruncate);
    FeatherOS::User::FileStatus status{};
    REQUIRE(FeatherOS::Files::StatDescriptor(&table, truncator, &status) == 0);
    REQUIRE(status.size == 0);
    REQUIRE_FALSE(status.read_only);
    REQUIRE(FeatherOS::Files::Close(&table, truncator) == 0);

    REQUIRE(List("/tmp") == std::vector<std::string>{"work/"});
    REQUIRE(List("/tmp/work") == std::vector<std::string>{"Notes.txt"});
    REQUIRE(FeatherOS::Files::RemoveDirectory("/tmp/work") ==
            FeatherOS::User::SyscallErrorNotEmpty);
    REQUIRE(FeatherOS::Files::Remove("/tmp/work/Notes.txt") == 0);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/tmp/work") == 0);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/tmp") == FeatherOS::User::SyscallErrorBusy);

    const uint64_t open_file = FeatherOS::Files::Open(&table, "/tmp/a.txt", OpenWrite | OpenCreate);
    REQUIRE(FeatherOS::Files::Rename("/tmp/a.txt", "/tmp/b.txt") ==
            FeatherOS::User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Close(&table, open_file) == 0);
    REQUIRE(FeatherOS::Files::Rename("/tmp/a.txt", "/tmp/b.txt") == 0);
    REQUIRE(List("/tmp") == std::vector<std::string>{"b.txt"});
}

TEST_CASE("/tmp holds 200-byte names and paths near PATH_MAX, listed whole")
{
    namespace User = FeatherOS::User;
    FeatherOS::Tmpfs::Reset();
    const std::string name(200, 'n');
    std::string path = "/tmp";
    for (int level = 0; level < 4; ++level)
    {
        path += "/" + name;
        REQUIRE(FeatherOS::Files::MakeDirectory(path.c_str()) == 0);
    }
    const std::string directory = path;
    path += "/" + name;
    REQUIRE(path.size() == 1009);
    FeatherOS::Files::Table table{};
    const uint64_t file =
        FeatherOS::Files::Open(&table, path.c_str(), OpenWrite | OpenCreate | OpenTruncate);
    REQUIRE_FALSE(User::IsSyscallError(file));
    REQUIRE(FeatherOS::Files::Write(&table, file, "far", 3) == 3);
    FeatherOS::Files::CloseAll(&table);
    REQUIRE(ReadAll(path.c_str()) == "far");
    REQUIRE(List(directory.c_str()) == std::vector<std::string>{name});

    User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat(path.c_str(), &status) == 0);
    REQUIRE(status.mode == (User::ModeRegular | 0644));
    REQUIRE(status.size == 3);
    REQUIRE(status.link_count == 1);
    User::DirectoryEntry entry{};
    REQUIRE(FeatherOS::Files::ReadDirectory(directory.c_str(), 0, &entry) == 0);
    REQUIRE(std::string(entry.name) == name);
    REQUIRE(entry.node == status.node);
    REQUIRE(entry.type == User::ModeRegular >> User::EntryTypeShift);
    REQUIRE(entry.size == 3);
    REQUIRE(FeatherOS::Files::Stat(directory.c_str(), &status) == 0);
    REQUIRE(status.mode == (User::ModeDirectory | 0755));
    // Five more characters would pass PATH_MAX.
    REQUIRE(FeatherOS::Files::MakeDirectory((path.substr(0, 1004) + "/xxxxxxxxxxxxxxx").c_str()) !=
            0);
    FeatherOS::Tmpfs::Reset();
}

TEST_CASE("the virtual filesystems report fixed modes, and mount points their roots' nodes")
{
    namespace User = FeatherOS::User;
    User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/dev/null", &status) == 0);
    REQUIRE(status.mode == (User::ModeCharacterDevice | 0666));
    REQUIRE(status.node != 0);
    const uint64_t dev_device = status.device;
    REQUIRE(dev_device != 0);
    REQUIRE(FeatherOS::Files::Stat("/dev/zero", &status) == 0);
    REQUIRE(status.device == dev_device);
    REQUIRE(FeatherOS::Files::Stat("/proc", &status) == 0);
    REQUIRE(status.mode == (User::ModeDirectory | 0555));
    REQUIRE(status.device != dev_device);
    REQUIRE(FeatherOS::Files::Stat("/proc/mounts", &status) == 0);
    REQUIRE(status.mode == (User::ModeRegular | 0444));
    REQUIRE(FeatherOS::Files::Stat("/tmp", &status) == 0);
    REQUIRE(status.mode == (User::ModeDirectory | 0755));

    std::map<std::string, uint64_t> nodes;
    User::DirectoryEntry entry{};
    for (uint64_t index = 0; FeatherOS::Files::ReadDirectory("/", index, &entry) == 0; ++index)
    {
        nodes[entry.name] = entry.node;
    }
    for (const char* point : {"dev", "proc", "tmp", "etc"})
    {
        REQUIRE(FeatherOS::Files::Stat((std::string("/") + point).c_str(), &status) == 0);
        REQUIRE(nodes.at(point) == status.node);
    }
    REQUIRE(FeatherOS::Files::ReadDirectory("/dev", 0, &entry) == 0);
    REQUIRE(std::string(entry.name) == "null");
    REQUIRE(entry.type == User::ModeCharacterDevice >> User::EntryTypeShift);
    REQUIRE(FeatherOS::Files::Stat("/dev/null", &status) == 0);
    REQUIRE(entry.node == status.node);
}

TEST_CASE("Spawn reads program files in pieces and keeps them from changing meanwhile")
{
    FeatherOS::Tmpfs::Reset();
    FeatherOS::Files::Table table{};
    std::string contents(5000, 'p');
    contents[4000] = 'q';
    const uint64_t writer =
        FeatherOS::Files::Open(&table, "/tmp/tool", OpenWrite | OpenCreate);
    REQUIRE(FeatherOS::Files::Write(&table, writer, contents.data(), contents.size()) ==
            contents.size());
    {
        FeatherOS::Files::ProgramFile program;
        REQUIRE(program.Open("/tmp/tool") == FeatherOS::User::SyscallErrorBusy);
    }
    REQUIRE(FeatherOS::Files::Close(&table, writer) == 0);
    {
        FeatherOS::Files::ProgramFile program;
        REQUIRE(program.Open("/tmp/tool") == 0);
        REQUIRE(program.Size() == contents.size());
        char piece[3] = {};
        REQUIRE(program.ReadAt(3999, piece, sizeof(piece)));
        REQUIRE(std::string(piece, sizeof(piece)) == "pqp");
        REQUIRE(program.ReadAt(0, piece, sizeof(piece)));
        REQUIRE(std::string(piece, sizeof(piece)) == "ppp");
        REQUIRE_FALSE(program.ReadAt(contents.size() - 1, piece, sizeof(piece)));
        REQUIRE(FeatherOS::Files::Remove("/tmp/tool") == FeatherOS::User::SyscallErrorBusy);
        REQUIRE(FeatherOS::User::IsSyscallError(
            FeatherOS::Files::Open(&table, "/tmp/tool", OpenWrite)));
    }
    REQUIRE(FeatherOS::Files::Remove("/tmp/tool") == 0);

    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(
        FeatherOS::Files::Open(&table, "/tmp/empty", OpenWrite | OpenCreate)));
    FeatherOS::Files::CloseAll(&table);
    FeatherOS::Files::ProgramFile program;
    REQUIRE(program.Open("/tmp/empty") == FeatherOS::User::SyscallErrorNotExecutable);
    REQUIRE(program.Open("/tmp/missing") == FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(program.Open("/tmp") == FeatherOS::User::SyscallErrorIsDirectory);
    REQUIRE(program.Open("/dev/zero") == FeatherOS::User::SyscallErrorNotExecutable);
    REQUIRE(program.Size() == 0);
}

TEST_CASE("/dev devices read, take writes, and cannot be created or removed")
{
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    REQUIRE(List("/dev") ==
            std::vector<std::string>{"null", "zero", "random", "tty", "ptmx", "pts/"});
    const uint64_t null_device =
        FeatherOS::Files::Open(&table, "/dev/null", OpenRead | OpenWrite | OpenTruncate);
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(null_device));
    char buffer[32];
    REQUIRE(FeatherOS::Files::Write(&table, null_device, "discarded", 9) == 9);
    REQUIRE(FeatherOS::Files::Read(&table, null_device, buffer, sizeof(buffer)) == 0);
    REQUIRE(FeatherOS::Files::Seek(&table, null_device, 100, FeatherOS::User::SeekFromStart) ==
            100);

    const uint64_t zero = FeatherOS::Files::Open(&table, "/dev/zero", OpenRead);
    std::memset(buffer, 0x55, sizeof(buffer));
    REQUIRE(FeatherOS::Files::Read(&table, zero, buffer, sizeof(buffer)) == sizeof(buffer));
    REQUIRE(std::all_of(buffer, buffer + sizeof(buffer), [](char byte) { return byte == 0; }));

    const uint64_t random = FeatherOS::Files::Open(&table, "/dev/random", OpenRead);
    char first[16];
    char second[16];
    REQUIRE(FeatherOS::Files::Read(&table, random, first, sizeof(first)) == sizeof(first));
    REQUIRE(FeatherOS::Files::Read(&table, random, second, sizeof(second)) == sizeof(second));
    REQUIRE(std::memcmp(first, second, sizeof(first)) != 0);

    FeatherOS::User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/dev/zero", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Device);
    REQUIRE_FALSE(status.read_only);
    REQUIRE(FeatherOS::Files::StatDescriptor(&table, random, &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Device);
    REQUIRE(FeatherOS::Files::Stat("/dev", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Directory);
    REQUIRE(status.read_only);
    REQUIRE(FeatherOS::Files::Open(&table, "/dev/new", OpenWrite | OpenCreate) ==
            FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Remove("/dev/null") == FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::MakeDirectory("/dev/sub") == FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/dev") == FeatherOS::User::SyscallErrorReadOnly);
    FeatherOS::Files::CloseAll(&table);
}

TEST_CASE("/dev/tty opens the controlling terminal, and terminal descriptors name their terminal")
{
    TestTerminalDevice device;
    FeatherOS::Terminals::Terminal console(device.Device());
    FeatherOS::Terminals::Terminal controlling(device.Device());
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, &console);
    FeatherOS::Terminals::Terminal* found = nullptr;
    REQUIRE(FeatherOS::Files::FindTerminal(&table, 2, &found) == 0);
    REQUIRE(found == &console);

    REQUIRE(FeatherOS::Files::Open(&table, "/dev/tty", OpenRead | OpenWrite) ==
            FeatherOS::User::SyscallErrorNoDevice);
    const uint64_t tty =
        FeatherOS::Files::Open(&table, "/dev/tty", OpenRead | OpenWrite, &controlling);
    REQUIRE(tty == 3);
    REQUIRE(FeatherOS::Files::KindOf(&table, tty) == FeatherOS::Files::Kind::Terminal);
    REQUIRE(FeatherOS::Files::FindTerminal(&table, tty, &found) == 0);
    REQUIRE(found == &controlling);
    FeatherOS::User::FileStatus status{};
    REQUIRE(FeatherOS::Files::StatDescriptor(&table, tty, &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Terminal);
    REQUIRE(FeatherOS::Files::Seek(&table, tty, 0, FeatherOS::User::SeekFromStart) ==
            FeatherOS::User::SyscallErrorNotSeekable);
    REQUIRE(FeatherOS::Files::StatusFlags(&table, tty, true, FeatherOS::User::OpenNonBlocking) ==
            FeatherOS::User::SyscallErrorInvalidArgument);

    FeatherOS::Files::Table child{};
    FeatherOS::Files::Inherit(&child, &table);
    REQUIRE(FeatherOS::Files::Close(&table, tty) == 0);
    REQUIRE(FeatherOS::Files::FindTerminal(&child, tty, &found) == 0);
    REQUIRE(found == &controlling);

    const uint64_t null_device = FeatherOS::Files::Open(&table, "/dev/null", OpenRead);
    REQUIRE(FeatherOS::Files::FindTerminal(&table, null_device, &found) ==
            FeatherOS::User::SyscallErrorNotTerminal);
    REQUIRE(FeatherOS::Files::FindTerminal(&table, 20, &found) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    FeatherOS::Files::CloseAll(&child);
    FeatherOS::Files::CloseAll(&table);
}

TEST_CASE("/dev/ttyN opens a virtual console's terminal")
{
    TestTerminalDevice device;
    FeatherOS::Terminals::Terminal first(device.Device());
    FeatherOS::Terminals::Terminal second(device.Device());
    FeatherOS::Terminals::Terminal* const consoles[] = {&first, &second};
    FeatherOS::Devices::SetConsoles(consoles, 2);
    REQUIRE(List("/dev") == std::vector<std::string>{"null", "zero", "random", "tty", "ptmx",
                                                     "tty1", "tty2", "pts/"});
    FeatherOS::Files::Table table{};
    const uint64_t opened = FeatherOS::Files::Open(&table, "/dev/tty2", OpenRead | OpenWrite);
    REQUIRE(opened == 0);
    REQUIRE(FeatherOS::Files::KindOf(&table, opened) == FeatherOS::Files::Kind::Terminal);
    FeatherOS::Terminals::Terminal* found = nullptr;
    REQUIRE(FeatherOS::Files::FindTerminal(&table, opened, &found) == 0);
    REQUIRE(found == &second);
    FeatherOS::User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/dev/tty1", &status) == 0);
    for (const char* missing : {"/dev/tty0", "/dev/tty3", "/dev/tty12", "/dev/ttyx"})
    {
        REQUIRE(FeatherOS::Files::Open(&table, missing, OpenRead) ==
                FeatherOS::User::SyscallErrorNotFound);
    }
    FeatherOS::Files::CloseAll(&table);
    FeatherOS::Devices::SetConsoles(nullptr, 0);
    REQUIRE(List("/dev") ==
            std::vector<std::string>{"null", "zero", "random", "tty", "ptmx", "pts/"});
}

TEST_CASE("/proc lists processes and generates their status and meminfo")
{
    UseFakeProcesses();
    REQUIRE(List("/proc") ==
            std::vector<std::string>{"meminfo", "stat", "mounts", "1/", "4/", "12/"});
    REQUIRE(List("/proc/4") == std::vector<std::string>{"status"});
    REQUIRE(ReadAll("/proc/12/status") ==
            "Name:\tshell\nState:\tR (running)\nPid:\t12\nPPid:\t1\nPgid:\t4\nPages:\t37\n"
            "PageLimit:\t1024\nOpenFiles:\t2\nCpuTimeMs:\t120\nLastCpu:\t3\n"
            "StartTimeMs:\t1500\nThreads:\t3\n");
    REQUIRE(ReadAll("/proc/stat") ==
            "UptimeMs:\t61000\nCpus:\t2\nCpu0BusyMs:\t700\nCpu1BusyMs:\t30\n");
    REQUIRE(ReadAll("/proc/meminfo") ==
            "MemTotal:\t262144 kB\nMemFree:\t204800 kB\nKernelHeap:\t64 kB\nTmp:\t0 kB\n");
    REQUIRE(FeatherOS::VFS::IsDirectory("/proc/1"));
    REQUIRE_FALSE(FeatherOS::VFS::IsDirectory("/proc/2"));
    REQUIRE_FALSE(FeatherOS::VFS::IsDirectory("/proc/01"));

    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    FeatherOS::User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/proc/1/status", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Regular);
    REQUIRE(status.read_only);
    REQUIRE(status.size == ReadAll("/proc/1/status").size());
    REQUIRE(FeatherOS::Files::Open(&table, "/proc/2/status", OpenRead) ==
            FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Open(&table, "/proc/meminfo", OpenWrite) ==
            FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Open(&table, "/proc/new", OpenWrite | OpenCreate) ==
            FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Remove("/proc/meminfo") == FeatherOS::User::SyscallErrorReadOnly);
    const uint64_t reader = FeatherOS::Files::Open(&table, "/proc/4/status", OpenRead);
    char text[16] = {};
    REQUIRE(FeatherOS::Files::Read(&table, reader, text, 10) == 10);
    REQUIRE(std::string(text) == "Name:\tshel");
    REQUIRE(FeatherOS::Files::Close(&table, reader) == 0);
}

TEST_CASE("/proc/mounts has a line per mount, in mount order")
{
    UseFakeProcesses();
    REQUIRE(ReadAll("/proc/mounts") == "initramfs /system initramfs ro 0 0\nvda1 / fat16 rw 0 0\n");

    FeatherOS::Procfs::SetSource({FakeProcessAt, FakeProcessStatus, FakeMemoryStatus,
                                  FakeSystemStatus, FeatherOS::VFS::DescribeMount});
    const std::string mounts = ReadAll("/proc/mounts");
    for (const char* line :
         {"initramfs /system initramfs ro 0 0\n", "etc /etc tmpfs rw 0 0\n",
          "devfs /dev devfs rw 0 0\n", "procfs /proc procfs ro 0 0\n", "tmp /tmp tmpfs rw 0 0\n"})
    {
        REQUIRE(mounts.find(line) != std::string::npos);
    }
    REQUIRE(mounts.find("devfs") < mounts.find("procfs"));
    UseFakeProcesses();
}

TEST_CASE("unmounting /tmp waits for its users, and mounting it again keeps its files")
{
    using FeatherOS::User::SyscallErrorBusy;
    using FeatherOS::User::SyscallErrorInvalidArgument;
    using FeatherOS::User::SyscallErrorNotDirectory;
    using FeatherOS::User::SyscallErrorNotFound;
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    const uint64_t writer =
        FeatherOS::Files::Open(&table, "/tmp/mounted.txt", OpenWrite | OpenCreate | OpenTruncate);
    REQUIRE(FeatherOS::Files::Write(&table, writer, "kept", 4) == 4);

    REQUIRE(FeatherOS::Files::Unmount("/tmp", NoWorkingDirectory) == SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Close(&table, writer) == 0);
    REQUIRE(FeatherOS::Files::Unmount("/tmp", EveryWorkingDirectory) == SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Unmount("/nonexistent", NoWorkingDirectory) == SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Unmount("/dev/null", NoWorkingDirectory) ==
            SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Unmount("/tmp", NoWorkingDirectory) == 0);
    REQUIRE(FeatherOS::Files::CheckDirectory("/tmp") != 0);
    REQUIRE(FeatherOS::Files::Open(&table, "/tmp/mounted.txt", OpenRead) == SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Unmount("/tmp", NoWorkingDirectory) == SyscallErrorNotFound);

    REQUIRE(FeatherOS::Files::Mount("tmp", "/tmp", "nosuchfs") ==
            FeatherOS::User::SyscallErrorUnknownFilesystemType);
    REQUIRE(FeatherOS::Files::Mount("nosuch", "/tmp", "tmpfs") == SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Mount("tmp", "/no/such", "tmpfs") == SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Mount("tmp", "/dev/null", "tmpfs") == SyscallErrorNotDirectory);
    REQUIRE(FeatherOS::Files::Mount("tmp", "/dev", "tmpfs") == SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Mount("etc", "/elsewhere", "tmpfs") == SyscallErrorBusy);

    // A mount inside another keeps the outer one mounted.
    REQUIRE(FeatherOS::Files::Mount("tmp", "/dev/scratch", "tmpfs") == 0);
    REQUIRE(ReadAll("/dev/scratch/mounted.txt") == "kept");
    REQUIRE(FeatherOS::Files::Unmount("/dev", NoWorkingDirectory) == SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Unmount("/dev/scratch", NoWorkingDirectory) == 0);

    REQUIRE(FeatherOS::Files::Mount("tmp", "/tmp", "tmpfs") == 0);
    REQUIRE(ReadAll("/tmp/mounted.txt") == "kept");
    REQUIRE(FeatherOS::Files::Mount("tmp", "/tmp", "tmpfs") == SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Remove("/tmp/mounted.txt") == 0);
}

TEST_CASE("remount changes /tmp, and answers for the mounts that cannot change")
{
    namespace User = FeatherOS::User;
    using FeatherOS::Files::Remount;
    using FeatherOS::Filesystem::MountMode;
    REQUIRE(FeatherOS::Files::MakeDirectory("/tmp/inside") == 0);
    REQUIRE(Remount("/tmp/inside", MountMode::ReadOnly) == User::SyscallErrorInvalidArgument);
    REQUIRE(Remount("/no/such", MountMode::ReadOnly) == User::SyscallErrorNotFound);
    REQUIRE(Remount("/system", MountMode::ReadOnly) == 0);
    REQUIRE(Remount("/system", MountMode::ReadWrite) == User::SyscallErrorReadOnly);
    REQUIRE(Remount("/proc", MountMode::ReadOnly) == 0);
    REQUIRE(Remount("/proc", MountMode::ReadWrite) == User::SyscallErrorReadOnly);
    REQUIRE(Remount("/dev", MountMode::ReadOnly) == User::SyscallErrorInvalidArgument);
    REQUIRE(Remount("/dev", MountMode::ReadWrite) == User::SyscallErrorInvalidArgument);

    FeatherOS::Files::Table table{};
    const uint64_t writer =
        FeatherOS::Files::Open(&table, "/tmp/kept.txt", OpenWrite | OpenCreate | OpenTruncate);
    REQUIRE_FALSE(User::IsSyscallError(writer));
    REQUIRE(FeatherOS::Files::Write(&table, writer, "kept", 4) == 4);
    REQUIRE(Remount("/tmp", MountMode::ReadOnly) == User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Close(&table, writer) == 0);

    REQUIRE(Remount("/tmp", MountMode::ReadOnly) == 0);
    REQUIRE(Remount("/tmp", MountMode::ReadOnly) == 0);
    REQUIRE(FeatherOS::Files::Open(&table, "/tmp/new.txt", OpenWrite | OpenCreate) ==
            User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Remove("/tmp/kept.txt") == User::SyscallErrorReadOnly);
    REQUIRE(ReadAll("/tmp/kept.txt") == "kept");
    REQUIRE(Remount("/tmp", MountMode::ReadWrite) == 0);
    REQUIRE(Remount("/tmp", MountMode::ReadWrite) == 0);
    REQUIRE(ReadAll("/tmp/kept.txt") == "kept");
    REQUIRE(FeatherOS::Files::Remove("/tmp/kept.txt") == 0);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/tmp/inside") == 0);
}
