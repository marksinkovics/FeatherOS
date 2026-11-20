#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/scheduler.h"
#include "support/filesystem_support.h"
#include "support/scheduler_stubs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Tasks on two ext4 volumes at "/a" and "/b" and on /tmp. Catch2's assertions are not thread-safe,
// so a task records what it saw and the test thread checks it.
namespace
{

namespace Files = FeatherOS::Files;
namespace User = FeatherOS::User;
namespace VFS = FeatherOS::VFS;
using FeatherOS::Filesystem::MountMode;
using FilesystemSupport::TwoExt4Volumes;
using std::chrono::milliseconds;

constexpr auto Patience = std::chrono::seconds(5);

bool WaitFor(const std::function<bool()>& condition, milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

class Latch
{
public:
    void Wait()
    {
        std::unique_lock lock(mutex);
        released_signal.wait(lock, [this] { return released; });
    }
    void Release()
    {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        released_signal.notify_all();
    }
    void Reset()
    {
        const std::lock_guard lock(mutex);
        released = false;
    }

private:
    std::mutex mutex;
    std::condition_variable released_signal;
    bool released = false;
};

// A kernel task for the host: its own task number, so volume locks tell it from the others.
class Task
{
public:
    Task(uint32_t number, std::function<void()> body)
        : thread(
              [number, body = std::move(body)]
              {
                  TestScheduler::current_task = number;
                  body();
              })
    {
    }
    ~Task()
    {
        Join();
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    void Join()
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

private:
    std::thread thread;
};

std::string Pattern(size_t length, char first)
{
    std::string text(length, '\0');
    for (size_t index = 0; index < length; ++index)
    {
        text[index] = static_cast<char>(first + index % 23);
    }
    return text;
}

bool NoProcess(uint32_t, uint64_t*)
{
    return false;
}

bool NoProcessStatus(uint64_t, FeatherOS::Procfs::ProcessStatus*)
{
    return false;
}

void NoStatus(FeatherOS::Procfs::MemoryStatus* status)
{
    *status = {};
}

void NoSystemStatus(FeatherOS::Procfs::SystemStatus* status)
{
    *status = {};
}

// What a task of its own has: a table with the console.
struct TaskFiles
{
    TaskFiles() : ready(Files::OpenConsole(&table, nullptr)) {}
    ~TaskFiles()
    {
        Files::CloseAll(&table);
    }
    TaskFiles(const TaskFiles&) = delete;
    TaskFiles& operator=(const TaskFiles&) = delete;

    Files::Table table{};
    bool ready;
};

uint64_t WriteFile(Files::Table* table, const char* path, const std::string& contents)
{
    const uint64_t descriptor =
        Files::Open(table, path, User::OpenWrite | User::OpenCreate | User::OpenTruncate);
    if (User::IsSyscallError(descriptor))
    {
        return descriptor;
    }
    uint64_t result = 0;
    if (!contents.empty())
    {
        result = Files::Write(table, descriptor, contents.data(), contents.size());
        result = result == contents.size() ? 0 : result;
    }
    const uint64_t closed = Files::Close(table, descriptor);
    return result != 0 ? result : closed;
}

// The file's bytes, or the error that stopped reading them.
uint64_t ReadFile(Files::Table* table, const char* path, std::string* contents)
{
    const uint64_t descriptor = Files::Open(table, path, User::OpenRead);
    if (User::IsSyscallError(descriptor))
    {
        return descriptor;
    }
    contents->clear();
    char buffer[1024];
    uint64_t result = 0;
    for (;;)
    {
        const uint64_t count = Files::Read(table, descriptor, buffer, sizeof(buffer));
        if (User::IsSyscallError(count))
        {
            result = count;
            break;
        }
        if (count == 0)
        {
            break;
        }
        contents->append(buffer, count);
    }
    Files::Close(table, descriptor);
    return result;
}

Latch park;
std::atomic<uint32_t> parked_task = 0;
std::atomic<bool> parked_now = false;
std::atomic<const char*> parked_point = "open.created";

// Mounts the two volumes and puts everything back when the test ends.
class TwoVolumes
{
public:
    TwoVolumes()
    {
        TestScheduler::Reset();
        park.Reset();
        parked_now = false;
        parked_point = "open.created";
        FeatherOS::Procfs::SetSource(
            {NoProcess, NoProcessStatus, NoStatus, NoSystemStatus, VFS::DescribeMount});
        if (!FilesystemSupport::AttachTwoExt4Volumes(&volumes))
        {
            SKIP("no ext4 image from mke2fs");
        }
        REQUIRE(VFS::MountBlockDevice());
        REQUIRE(VFS::Mount("ext4", "vda3", "/a") == VFS::MountResult::Done);
        REQUIRE(VFS::Mount("ext4", "vda4", "/b") == VFS::MountResult::Done);
    }
    ~TwoVolumes()
    {
        FilesystemSupport::SetWriteHook(nullptr);
        Files::Testing::SetPauseHook(nullptr);
        VFS::Unmount("/a");
        VFS::Unmount("/b");
    }
    TwoVolumes(const TwoVolumes&) = delete;
    TwoVolumes& operator=(const TwoVolumes&) = delete;

    TwoExt4Volumes volumes{};
};

// Parks the one task, once, at the point the test names.
void ParkInOpen(const char* point)
{
    if (std::strcmp(point, parked_point.load()) == 0 &&
        TestScheduler::current_task == parked_task.load() && !parked_now.exchange(true))
    {
        park.Wait();
    }
}

} // namespace

TEST_CASE("work on one volume does not wait for another volume's lock",
          "[concurrency][volume_locks]")
{
    const TwoVolumes volumes;
    {
        TaskFiles setup;
        REQUIRE(WriteFile(&setup.table, "/a/parked.txt", "x") == 0);
    }
    parked_task = 100;
    Files::Testing::SetPauseHook(ParkInOpen);

    std::atomic<uint64_t> parked_result = 1;
    Task parked(100,
                [&]
                {
                    TaskFiles files;
                    parked_result = Files::Open(&files.table, "/a/parked.txt", User::OpenWrite);
                    Files::Close(&files.table, parked_result);
                    parked_result = 0;
                });
    REQUIRE(WaitFor([] { return parked_now.load(); }, Patience));

    std::atomic<bool> other_done = false;
    std::atomic<int> other_failures = 0;
    Task other(101,
               [&]
               {
                   TaskFiles files;
                   const auto check = [&](bool good)
                   {
                       if (!good)
                       {
                           ++other_failures;
                       }
                   };
                   check(WriteFile(&files.table, "/b/own.txt", "from b") == 0);
                   std::string contents;
                   check(ReadFile(&files.table, "/b/own.txt", &contents) == 0 &&
                         contents == "from b");
                   User::DirectoryEntry entry;
                   check(Files::ReadDirectory("/", 0, &entry) == 0);
                   check(Files::ReadDirectory("/b", 0, &entry) == 0);
                   check(WriteFile(&files.table, "/tmp/own.txt", "from tmp") == 0);
                   check(ReadFile(&files.table, "/tmp/own.txt", &contents) == 0 &&
                         contents == "from tmp");
                   check(ReadFile(&files.table, "/proc/mounts", &contents) == 0 &&
                         contents.find("/a") != std::string::npos);
                   other_done = true;
               });
    std::atomic<bool> sync_done = false;
    Task syncing(102,
                 [&]
                 {
                     Files::SyncPeriodic();
                     sync_done = true;
                 });
    std::atomic<bool> control_done = false;
    Task control(103,
                 [&]
                 {
                     TaskFiles files;
                     WriteFile(&files.table, "/a/y.txt", "y");
                     control_done = true;
                 });

    REQUIRE(WaitFor([&] { return other_done.load(); }, Patience));
    REQUIRE(other_failures == 0);
    std::this_thread::sleep_for(milliseconds(200));
    REQUIRE_FALSE(sync_done);
    REQUIRE_FALSE(control_done);

    park.Release();
    parked.Join();
    syncing.Join();
    control.Join();
    REQUIRE(parked_result == 0);
    REQUIRE(sync_done);
    REQUIRE(control_done);
    REQUIRE(TestScheduler::volume_violations == 0);
}

TEST_CASE("a descriptor in use survives a close and a reopen on another thread",
          "[concurrency][pinning]")
{
    const TwoVolumes volumes;
    TaskFiles files;
    REQUIRE(files.ready);
    REQUIRE(WriteFile(&files.table, "/a/pinned.txt", std::string(16, 'x')) == 0);
    constexpr int Rounds = 5000;
    const uint64_t descriptor = Files::Open(&files.table, "/a/pinned.txt", User::OpenRead);
    REQUIRE_FALSE(User::IsSyscallError(descriptor));

    std::atomic<bool> finished = false;
    std::atomic<int> bad_results = 0;
    std::atomic<int> reads = 0;
    Task reader(110,
                [&]
                {
                    while (!finished.load())
                    {
                        char buffer[8];
                        const uint64_t count =
                            Files::Read(&files.table, descriptor, buffer, sizeof(buffer));
                        if (User::IsSyscallError(count))
                        {
                            bad_results += count == User::SyscallErrorBadDescriptor ? 0 : 1;
                        }
                        else
                        {
                            for (uint64_t index = 0; index < count; ++index)
                            {
                                bad_results += buffer[index] == 'x' ? 0 : 1;
                            }
                            ++reads;
                        }
                        Files::Seek(&files.table, descriptor, 0, User::SeekFromStart);
                    }
                });
    std::atomic<int> reopen_failures = 0;
    Task reopener(111,
                  [&]
                  {
                      for (int round = 0; round < Rounds; ++round)
                      {
                          Files::Close(&files.table, descriptor);
                          if (Files::Open(&files.table, "/a/pinned.txt", User::OpenRead) !=
                              descriptor)
                          {
                              ++reopen_failures;
                          }
                      }
                      finished = true;
                  });
    reopener.Join();
    reader.Join();
    REQUIRE(bad_results == 0);
    REQUIRE(reopen_failures == 0);
    REQUIRE(Files::CountOpen(&files.table) == 4);

    // A read paused after it found its file still has it when the descriptor is closed.
    parked_point = "read.pinned";
    parked_task = 112;
    parked_now = false;
    Files::Testing::SetPauseHook(ParkInOpen);
    std::atomic<uint64_t> paused_read = 0;
    Task pausing(112,
                 [&]
                 {
                     char buffer[8];
                     paused_read = Files::Read(&files.table, descriptor, buffer, sizeof(buffer));
                 });
    REQUIRE(WaitFor([] { return parked_now.load(); }, Patience));
    REQUIRE(Files::Close(&files.table, descriptor) == 0);
    park.Release();
    pausing.Join();
    REQUIRE(paused_read == 8);
    REQUIRE(Files::CountOpen(&files.table) == 3);
    REQUIRE(TestScheduler::volume_violations == 0);
}

TEST_CASE("an open holds its volume from the lookup to the count of its use",
          "[concurrency][compound]")
{
    const TwoVolumes volumes;
    parked_task = 120;
    Files::Testing::SetPauseHook(ParkInOpen);

    std::atomic<uint64_t> opened = 0;
    std::atomic<bool> open_done = false;
    std::atomic<bool> removed_done = false;
    std::atomic<uint64_t> removed = 0;
    Task opening(120,
                 [&]
                 {
                     TaskFiles own;
                     opened = Files::Open(&own.table, "/a/f.txt",
                                          User::OpenWrite | User::OpenCreate);
                     open_done = true;
                     WaitFor([&] { return removed_done.load(); }, Patience);
                     Files::Close(&own.table, opened);
                 });
    REQUIRE(WaitFor([] { return parked_now.load(); }, Patience));
    Task removing(121,
                  [&]
                  {
                      removed = Files::Remove("/a/f.txt");
                      removed_done = true;
                  });
    std::this_thread::sleep_for(milliseconds(200));
    REQUIRE_FALSE(removed_done);
    REQUIRE_FALSE(open_done);
    park.Release();
    REQUIRE(WaitFor([&] { return removed_done.load(); }, Patience));
    opening.Join();
    removing.Join();
    REQUIRE_FALSE(User::IsSyscallError(opened));
    REQUIRE(removed == User::SyscallErrorBusy);
    REQUIRE(Files::Remove("/a/f.txt") == 0);
    REQUIRE(TestScheduler::volume_violations == 0);
}

TEST_CASE("a task that holds one volume's lock and touches another is a violation",
          "[concurrency][one_lock]")
{
    const TwoVolumes volumes;
    TaskFiles files;
    REQUIRE(files.ready);
    REQUIRE(WriteFile(&files.table, "/b/x", "x") == 0);
    REQUIRE(TestScheduler::volume_violations == 0);

    void* const hold = Files::Testing::HoldVolume("/a");
    User::FileStatus status{};
    REQUIRE(Files::Stat("/b/x", &status) == 0);
    Files::Testing::ReleaseVolume(hold);
    REQUIRE(TestScheduler::volume_violations == 1);

    TestScheduler::volume_violations = 0;
    void* const same = Files::Testing::HoldVolume("/a");
    REQUIRE(Files::Stat("/a/hello.txt", &status) == 0);
    Files::Testing::ReleaseVolume(same);
    REQUIRE(TestScheduler::volume_violations == 0);
}

TEST_CASE("each task reads back the error of its own failure", "[concurrency][errors]")
{
    const TwoVolumes volumes;
    REQUIRE(Files::Remount("/b", MountMode::ReadOnly) == 0);
    constexpr int Rounds = 3000;
    std::atomic<int> wrong = 0;
    const auto body = [&](const char* path, uint64_t expected)
    {
        for (int round = 0; round < Rounds; ++round)
        {
            if (VFS::Remove(path) || VFS::LastError() != expected)
            {
                ++wrong;
            }
        }
    };
    {
        Task first(130, [&] { body("/b/hello.txt", User::SyscallErrorReadOnly); });
        Task second(131, [&] { body("/a/missing.txt", User::SyscallErrorNotFound); });
    }
    REQUIRE(wrong == 0);
    REQUIRE(Files::Remount("/b", MountMode::ReadWrite) == 0);
}

namespace
{

constexpr int Workers = 4;
constexpr int Operations = 3000;
constexpr int FilesPerVolume = 6;
constexpr const char* Roots[] = {"/a", "/b", "/tmp"};

struct Expected
{
    std::map<std::string, std::string> files;
    int failures = 0;
};

void Work(int worker, Expected* expected)
{
    TaskFiles files;
    std::mt19937 random(static_cast<uint32_t>(worker) + 1);
    for (int operation = 0; operation < Operations; ++operation)
    {
        const char* const root = Roots[random() % 3];
        const std::string path =
            std::string(root) + "/w" + std::to_string(worker) + "-" + std::to_string(random() % FilesPerVolume);
        const uint32_t action = random() % 10;
        const auto known = expected->files.find(path);
        if (action < 6)
        {
            const std::string contents =
                Pattern(1 + random() % 3000, static_cast<char>('a' + worker)) + std::to_string(operation);
            const uint64_t written = WriteFile(&files.table, path.c_str(), contents);
            if (written == 0)
            {
                expected->files[path] = contents;
            }
            else if (written != User::SyscallErrorReadOnly)
            {
                ++expected->failures;
            }
        }
        else if (action < 8)
        {
            std::string contents;
            const uint64_t read = ReadFile(&files.table, path.c_str(), &contents);
            if (known == expected->files.end())
            {
                expected->failures += read == User::SyscallErrorNotFound ? 0 : 1;
            }
            else
            {
                expected->failures += read == 0 && contents == known->second ? 0 : 1;
            }
        }
        else if (action == 8)
        {
            const uint64_t removed = Files::Remove(path.c_str());
            if (removed == 0)
            {
                expected->failures += known == expected->files.end() ? 1 : 0;
                expected->files.erase(path);
            }
            else if (removed != User::SyscallErrorReadOnly &&
                     !(removed == User::SyscallErrorNotFound && known == expected->files.end()))
            {
                ++expected->failures;
            }
        }
        else
        {
            User::FileStatus status{};
            const uint64_t stat = Files::Stat(path.c_str(), &status);
            expected->failures += (stat == 0) == (known != expected->files.end()) ? 0 : 1;
        }
    }
}

} // namespace

TEST_CASE("workers on two volumes and /tmp keep their files while syncs, /proc reads and remounts run",
          "[concurrency][stress]")
{
    const TwoVolumes volumes;
    Expected expected[Workers];
    std::atomic<bool> working = true;
    std::atomic<int> background_failures = 0;
    {
        std::vector<std::unique_ptr<Task>> workers;
        Task syncing(150,
                     [&]
                     {
                         while (working.load())
                         {
                             background_failures += Files::SyncPeriodic() == 0 ? 0 : 1;
                             std::this_thread::sleep_for(milliseconds(1));
                         }
                     });
        Task reading(151,
                     [&]
                     {
                         TaskFiles files;
                         std::string contents;
                         while (working.load())
                         {
                             background_failures += ReadFile(&files.table, "/proc/mounts", &contents) == 0 &&
                                                            contents.find("/b") != std::string::npos
                                                        ? 0
                                                        : 1;
                         }
                     });
        Task remounting(152,
                        [&]
                        {
                            while (working.load())
                            {
                                const uint64_t made_read_only =
                                    Files::Remount("/b", MountMode::ReadOnly);
                                background_failures +=
                                    made_read_only == 0 || made_read_only == User::SyscallErrorBusy ? 0 : 1;
                                std::this_thread::sleep_for(milliseconds(1));
                                const uint64_t back = Files::Remount("/b", MountMode::ReadWrite);
                                background_failures +=
                                    back == 0 || back == User::SyscallErrorBusy ? 0 : 1;
                            }
                        });
        for (int worker = 0; worker < Workers; ++worker)
        {
            workers.push_back(std::make_unique<Task>(
                160 + static_cast<uint32_t>(worker), [&, worker] { Work(worker, &expected[worker]); }));
        }
        for (auto& worker : workers)
        {
            worker->Join();
        }
        working = false;
    }
    REQUIRE(background_failures == 0);
    REQUIRE(Files::Remount("/b", MountMode::ReadWrite) == 0);

    TaskFiles files;
    for (const Expected& worker : expected)
    {
        REQUIRE(worker.failures == 0);
        for (const auto& [path, contents] : worker.files)
        {
            std::string found;
            REQUIRE(ReadFile(&files.table, path.c_str(), &found) == 0);
            REQUIRE(found == contents);
        }
    }

    REQUIRE(VFS::DetachAll());
    REQUIRE_FALSE(FilesystemSupport::NeedsRecovery(volumes.volumes.first_start));
    REQUIRE_FALSE(FilesystemSupport::NeedsRecovery(volumes.volumes.second_start));
    const auto first = FilesystemSupport::CopyRange(volumes.volumes.first_start, volumes.volumes.first_end);
    const auto second =
        FilesystemSupport::CopyRange(volumes.volumes.second_start, volumes.volumes.second_end);
    for (const Expected& worker : expected)
    {
        for (const auto& [path, contents] : worker.files)
        {
            if (path.starts_with("/a/") || path.starts_with("/b/"))
            {
                const auto& copy = path.starts_with("/a/") ? first : second;
                REQUIRE(FilesystemSupport::ReadFromCopy(copy, path.c_str() + 3) == contents);
            }
        }
    }
    REQUIRE(TestScheduler::volume_violations == 0);
}
