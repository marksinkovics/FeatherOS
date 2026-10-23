#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/tmpfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/memory/heap.h"
#include "kernel/random.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The kernel heap and random source, which the host tests do not build.
namespace FeatherOS::Memory
{

void* Allocate(size_t size)
{
    return std::malloc(size);
}

bool Deallocate(void* pointer)
{
    std::free(pointer);
    return true;
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

using FeatherOS::User::OpenAppend;
using FeatherOS::User::OpenCreate;
using FeatherOS::User::OpenRead;
using FeatherOS::User::OpenTruncate;
using FeatherOS::User::OpenWrite;

std::vector<std::string> List(const char* path)
{
    std::vector<std::string> names;
    FeatherOS::FAT16::DirectoryEntry entry{};
    for (uint32_t index = 0; FeatherOS::VFS::ReadDirectory(path, index, &entry); ++index)
    {
        names.push_back(std::string(entry.name) + (entry.directory ? "/" : ""));
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
    status->pages = 37;
    status->page_limit = 1024;
    status->open_files = 2;
    return true;
}

void FakeMemoryStatus(FeatherOS::Procfs::MemoryStatus* status)
{
    status->total_bytes = 256 * 1024 * 1024;
    status->free_bytes = 200 * 1024 * 1024;
    status->kernel_heap_bytes = 64 * 1024;
    status->tmp_bytes = FeatherOS::Tmpfs::UsedBytes();
}

void UseFakeProcesses()
{
    FeatherOS::Procfs::SetSource({FakeProcessAt, FakeProcessStatus, FakeMemoryStatus});
}

} // namespace

TEST_CASE("tmpfs files grow, shrink, and keep their contents")
{
    FeatherOS::Tmpfs::Reset();
    uint32_t node = 0;
    REQUIRE(FeatherOS::Tmpfs::Create("a.txt", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create("a.txt", &node));
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
    REQUIRE(FeatherOS::Tmpfs::Remove("a.txt"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Open("a.txt", &node));
}

TEST_CASE("tmpfs directories nest, and names are case-sensitive")
{
    FeatherOS::Tmpfs::Reset();
    uint32_t node = 0;
    REQUIRE(FeatherOS::Tmpfs::MakeDirectory("docs"));
    REQUIRE(FeatherOS::Tmpfs::MakeDirectory("docs/old"));
    REQUIRE(FeatherOS::Tmpfs::Create("docs/Plan.txt", &node));
    REQUIRE(FeatherOS::Tmpfs::Create("docs/plan.txt", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create("missing/plan.txt", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create("docs/Plan.txt/x", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create("docs/..", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create(std::string(64, 'n').c_str(), &node));
    REQUIRE(FeatherOS::Tmpfs::IsDirectory(""));
    REQUIRE(FeatherOS::Tmpfs::IsDirectory("docs/old"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::IsDirectory("docs/plan.txt"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Open("docs", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::RemoveDirectory("docs"));
    REQUIRE_FALSE(FeatherOS::Tmpfs::RemoveDirectory(""));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Remove("docs/old"));
    REQUIRE(FeatherOS::Tmpfs::RemoveDirectory("docs/old"));
    REQUIRE(FeatherOS::Tmpfs::Remove("docs/Plan.txt"));
    REQUIRE(FeatherOS::Tmpfs::Remove("docs/plan.txt"));
    REQUIRE(FeatherOS::Tmpfs::RemoveDirectory("docs"));
}

TEST_CASE("tmpfs stops at its node and byte limits")
{
    FeatherOS::Tmpfs::Reset();
    uint32_t node = 0;
    for (uint32_t index = 1; index < FeatherOS::Tmpfs::MaximumNodes; ++index)
    {
        REQUIRE(FeatherOS::Tmpfs::Create(("f" + std::to_string(index)).c_str(), &node));
    }
    REQUIRE_FALSE(FeatherOS::Tmpfs::Create("one-too-many", &node));
    REQUIRE(FeatherOS::Tmpfs::Open("f1", &node));
    std::vector<char> large(FeatherOS::Tmpfs::MaximumBytes - 100, 'x');
    uint64_t count = 0;
    REQUIRE(FeatherOS::Tmpfs::Write(node, 0, large.data(), large.size(), &count));
    REQUIRE(FeatherOS::Tmpfs::Open("f2", &node));
    REQUIRE_FALSE(FeatherOS::Tmpfs::Write(node, 0, large.data(), 1000, &count));
    REQUIRE(count == 100);
    REQUIRE(FeatherOS::Tmpfs::UsedBytes() == FeatherOS::Tmpfs::MaximumBytes);
    FeatherOS::Tmpfs::Reset();
    REQUIRE(FeatherOS::Tmpfs::UsedBytes() == 0);
}

TEST_CASE("/tmp works through file descriptors with the disk's sharing rules")
{
    FeatherOS::Tmpfs::Reset();
    FeatherOS::Files::Table table{};
    REQUIRE(FeatherOS::Files::CheckDirectory("/tmp") == 0);
    REQUIRE(FeatherOS::Files::MakeDirectory("/tmp/work") == 0);
    const uint64_t writer =
        FeatherOS::Files::Open(&table, "/tmp/work/Notes.txt", OpenWrite | OpenCreate);
    REQUIRE(writer == FeatherOS::User::FirstFileDescriptor);
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
}

TEST_CASE("/dev devices read, take writes, and cannot be created or removed")
{
    FeatherOS::Files::Table table{};
    REQUIRE(List("/dev") == std::vector<std::string>{"null", "zero", "random"});
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

TEST_CASE("/proc lists processes and generates their status and meminfo")
{
    UseFakeProcesses();
    REQUIRE(List("/proc") == std::vector<std::string>{"meminfo", "1/", "4/", "12/"});
    REQUIRE(List("/proc/4") == std::vector<std::string>{"status"});
    REQUIRE(ReadAll("/proc/12/status") == "Name:\tshell\nState:\tR (running)\nPid:\t12\nPPid:\t1\n"
                                          "Pages:\t37\nPageLimit:\t1024\nOpenFiles:\t2\n");
    REQUIRE(ReadAll("/proc/meminfo") ==
            "MemTotal:\t262144 kB\nMemFree:\t204800 kB\nKernelHeap:\t64 kB\nTmp:\t0 kB\n");
    REQUIRE(FeatherOS::VFS::IsDirectory("/proc/1"));
    REQUIRE_FALSE(FeatherOS::VFS::IsDirectory("/proc/2"));
    REQUIRE_FALSE(FeatherOS::VFS::IsDirectory("/proc/01"));

    FeatherOS::Files::Table table{};
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
