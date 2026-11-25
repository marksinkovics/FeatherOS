#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/network/socket.h"
#include "kernel/pipe.h"
#include "kernel/terminal/pseudo_terminal.h"
#include "kernel/terminal/terminal.h"
#include "support/network_stubs.h"
#include "support/scheduler_stubs.h"

#include <stddef.h>
#include <stdint.h>

#include <string>

// Reproductions for the process-lifetime findings of docs/reviews/2026-11-review.md. Each test
// asserts the correct behaviour, so it fails on the current code when the bug is real.
//
// Trace-only findings (process.cpp is not in the host build, and these are SMP or arch races):
//
// R2-01 CopyToUser/CopyFromUser re-translate after the range check, with no lock.
//   1. Thread A: ReadSyscall -> UserRangeWritable(buf) (process.cpp:1723) passes, every page mapped.
//   2. Thread B (other CPU): sbrk(-4096) -> ShrinkHeap (1287) clears the PTE with
//      Memory::UnmapUserPage under memory_lock (1311), then frees the frame in FreeUnmappedFrames
//      (1271-1282) after the lock is dropped (1321).
//   3. Thread A: CopyToUser (912-924) calls UserByte() again per byte (921);
//      TranslateUserPage (arch/x86_64/virtual_memory.cpp:387) fails, UserByte returns nullptr
//      (853-864) and `*UserByte(...) = bytes[index]` stores to address 0 in ring 0.
//   4. Ring-0 fault -> ReportFatalException (arch/x86_64/exceptions.cpp:558-600): kernel panic from
//      an unprivileged two-thread program. ReleaseStackSlot (1330-1348, ThreadJoin) is the same
//      unmap path for a buffer on a joined thread's stack.
//
// R3-01 AArch64 direct-map copy against a freed frame.
//   1. Thread A: UserByte (process.cpp:853) returns PhysicalBytes(frame) for a user page, or
//      CopyFromUserPaged (954-971) starts a memcpy of up to 4 KiB from it.
//   2. Thread B on another CPU: ShrinkHeap/ReleaseStackSlot unmap the page (1311, 1340) and call
//      FreeUnmappedFrames (1271) -> Memory::ShootDownUserMappings, which is an empty function on
//      AArch64 (arch/aarch64/virtual_memory.cpp:491; the TLBI only drops user-virtual
//      translations, not the direct-map alias) -> Memory::FreePageFrame (1280).
//   3. The frame can now be allocated to another owner while A still reads or writes it through
//      the direct map. No memory_lock or frame reference is held by the copier (853-864, 898-924).
//   x86-64 is held off only because syscalls run with IF=0 and the shootdown waits for an IPI
//   acknowledgement (arch/x86_64/virtual_memory.cpp:511-536).
//
// R2-04 WaitSyscall keeps a ProcessRecord pointer after dropping process_lock.
//   1. Threads T1 and T2 of one parent both call waitpid(child). Each finds the record under
//      process_lock (process.cpp:1219-1227) and keeps `child` and `watch.child` (1236) after the
//      lock is released (1227).
//   2. The child ends. T1 leaves Scheduler::Wait (1248), calls Wait(pid) (1258): FindProcess,
//      Scheduler::Wait, Scheduler::ReapTask (4016-4020), then RemoveProcess -> Memory::Deallocate
//      (4047, 510-531).
//   3. T2 wakes in Scheduler::Wait (scheduler.cpp:457-487), FindTask returns null ("Another waiter
//      may have reaped the task", 480), so it returns false at 1248.
//   4. T2 evaluates ReportReady(watch) at 1253 and reads watch.child->report from the freed record
//      (and, if Spawn reused the block, TakeReport (1207) writes `child->report = None` into an
//      unrelated process). WNOHANG: Scheduler::GetState(child->task) at 1244 reads it too.
//
// R2-05 WaitForThreads marks a running thread as being joined after main's pthread_exit.
//   1. main creates B (slot 0) and A (slot 1), then pthread_exit: thread_exit_only is set, so
//      RunProcess skips ExitProcess (process.cpp:3476-3478) and calls WaitForThreads (3481).
//   2. WaitForThreads (2272-2307) picks the first record with id != 0, task != 0, !joining (B) and
//      sets `thread->joining = true` (2284) without checking that the process is exiting; then
//      Scheduler::Wait(B->task) (2299).
//   3. A calls pthread_join(B): ThreadJoinSyscall finds B (2170-2175), sees `thread->joining` and
//      returns SyscallErrorInvalidArgument (2182-2185). Once WaitForThreads has reaped B and set
//      id = 0 (2304), it returns SyscallErrorNotFound (2178-2181). B's result is dropped, and
//      ThreadDetachSyscall (2226-2229) fails the same way.
//
// R2-06 syscall buffer checks reject not-yet-touched demand-grown stack pages.
//   1. `char buf[16384]; read(fd, buf, sizeof buf)`: only the top stack page is mapped at load;
//      the compiler lowers sp by the frame size without probing, so middle pages are untouched.
//   2. ReadSyscall -> UserRangeWritable (process.cpp:1723, 1705) -> UserByte (853-864) ->
//      Memory::TranslateUserPage fails for the first untouched page; nothing calls
//      HandleUserPageFault/MapStackPageLocked (3745-3760), which only the user-mode page-fault
//      handlers (arch/x86_64/exceptions.cpp:568, arch/aarch64/exceptions.cpp:342) and
//      CopyToCurrentUser (3762-3784, signal frames) reach.
//   3. ReadSyscall returns SyscallErrorBadAddress (EFAULT) although the range is valid stack.

#if defined(__SANITIZE_ADDRESS__)
#define REPRO_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define REPRO_HAS_ASAN 1
#endif
#endif

#ifdef REPRO_HAS_ASAN
extern "C" size_t __sanitizer_get_current_allocated_bytes();
#endif

namespace Files = FeatherOS::Files;
namespace Pipes = FeatherOS::Pipes;
namespace PseudoTerminals = FeatherOS::PseudoTerminals;
namespace Sockets = FeatherOS::Network::Sockets;
namespace User = FeatherOS::User;

namespace
{

bool interrupt_now = false;

bool InterruptNow()
{
    return interrupt_now;
}

// The wait hook stands for the other thread, which runs while the caller sleeps.
struct OtherThread
{
    OtherThread()
    {
        TestScheduler::Reset();
        interrupt_now = false;
    }

    ~OtherThread()
    {
        TestScheduler::on_wait = nullptr;
        interrupt_now = false;
    }

    OtherThread(const OtherThread&) = delete;
    OtherThread& operator=(const OtherThread&) = delete;
};

struct DescriptorTable
{
    DescriptorTable()
    {
        Files::OpenConsole(&table, nullptr);
    }

    ~DescriptorTable()
    {
        Files::CloseAll(&table);
    }

    DescriptorTable(const DescriptorTable&) = delete;
    DescriptorTable& operator=(const DescriptorTable&) = delete;

    Files::Table table{};
};

#ifdef REPRO_HAS_ASAN
// A freed object is seen by the drop in live heap bytes, so the test never touches freed memory.
template <typename Body> bool ReleasesAtLeast(size_t bytes, Body&& body)
{
    const size_t before = __sanitizer_get_current_allocated_bytes();
    body();
    const size_t after = __sanitizer_get_current_allocated_bytes();
    return before > after && before - after >= bytes;
}
#endif

constexpr size_t SocketQueueBytes = Sockets::ReceiveQueueLength * Sockets::MaximumDatagramSize;

struct DatagramSocket
{
    DatagramSocket()
    {
        TestNetwork::Reset();
        const uint64_t created =
            Files::CreateSocket(&descriptors.table, User::SocketFamilyIpv4, User::SocketDatagram, 0);
        REQUIRE_FALSE(User::IsSyscallError(created));
        descriptor = created;
        REQUIRE(Files::FindSocket(&descriptors.table, descriptor, &socket, nullptr) == 0);
    }

    DescriptorTable descriptors;
    uint64_t descriptor = 0;
    Sockets::Socket* socket = nullptr;
};

} // namespace

TEST_CASE("R2-03 a pipe stays allocated while a read blocked on it has its descriptors closed",
          "[.repro][R2-03]")
{
#ifndef REPRO_HAS_ASAN
    SKIP("needs AddressSanitizer to see the heap shrink");
#else
    const OtherThread other_thread;
    DescriptorTable descriptors;
    uint64_t reader = 0;
    uint64_t writer = 0;
    REQUIRE(Files::CreatePipe(&descriptors.table, 0, &reader, &writer) == 0);

    TestScheduler::on_wait = [&] {
        const bool freed = ReleasesAtLeast(Pipes::Capacity, [&] {
            Files::Close(&descriptors.table, reader);
            Files::Close(&descriptors.table, writer);
        });
        interrupt_now = true;
        // Throwing keeps the stub from re-taking the freed pipe's lock when the bug is present.
        REQUIRE_FALSE(freed);
    };
    char buffer[16];
    const uint64_t result = Files::Read(&descriptors.table, reader, buffer, sizeof(buffer),
                                        InterruptNow);
    REQUIRE((result == 0 || result == User::SyscallErrorInterrupted));
#endif
}

TEST_CASE("R2-03 a pipe stays allocated while a write blocked on it has its descriptors closed",
          "[.repro][R2-03]")
{
#ifndef REPRO_HAS_ASAN
    SKIP("needs AddressSanitizer to see the heap shrink");
#else
    const OtherThread other_thread;
    DescriptorTable descriptors;
    uint64_t reader = 0;
    uint64_t writer = 0;
    REQUIRE(Files::CreatePipe(&descriptors.table, 0, &reader, &writer) == 0);
    const char fill[Pipes::Capacity] = {};
    REQUIRE(Files::Write(&descriptors.table, writer, fill, sizeof(fill)) == sizeof(fill));

    TestScheduler::on_wait = [&] {
        const bool freed = ReleasesAtLeast(Pipes::Capacity, [&] {
            Files::Close(&descriptors.table, writer);
            Files::Close(&descriptors.table, reader);
        });
        interrupt_now = true;
        REQUIRE_FALSE(freed);
    };
    const uint64_t result =
        Files::Write(&descriptors.table, writer, fill, 1, InterruptNow);
    REQUIRE((result == User::SyscallErrorBrokenPipe || result == User::SyscallErrorInterrupted));
#endif
}

TEST_CASE("R2-03 a pty master's pipe stays allocated while a read blocked on it has the pair "
          "closed",
          "[.repro][R2-03]")
{
#ifndef REPRO_HAS_ASAN
    SKIP("needs AddressSanitizer to see the heap shrink");
#else
    const OtherThread other_thread;
    struct HookReset
    {
        ~HookReset()
        {
            FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr, nullptr, nullptr});
        }
    } const hook_reset;
    FeatherOS::Terminals::SetProcessHooks({nullptr, InterruptNow, nullptr, nullptr});
    DescriptorTable descriptors;
    constexpr uint64_t ReadWrite = User::OpenRead | User::OpenWrite;
    const uint64_t master = Files::Open(&descriptors.table, "/dev/ptmx", ReadWrite);
    REQUIRE_FALSE(User::IsSyscallError(master));
    PseudoTerminals::Pair* pair = nullptr;
    REQUIRE(Files::FindPseudoTerminal(&descriptors.table, master, &pair) == 0);
    PseudoTerminals::SetLocked(pair, false);
    const std::string slave_path = "/dev/pts/" + std::to_string(PseudoTerminals::Number(pair));
    const uint64_t slave = Files::Open(&descriptors.table, slave_path.c_str(), ReadWrite);
    REQUIRE_FALSE(User::IsSyscallError(slave));

    TestScheduler::on_wait = [&] {
        const bool freed = ReleasesAtLeast(Pipes::Capacity, [&] {
            Files::Close(&descriptors.table, master);
            Files::Close(&descriptors.table, slave);
        });
        interrupt_now = true;
        REQUIRE_FALSE(freed);
    };
    char buffer[16];
    const uint64_t result =
        Files::Read(&descriptors.table, master, buffer, sizeof(buffer), InterruptNow);
    REQUIRE((result == 0 || result == User::SyscallErrorInterrupted));
#endif
}

TEST_CASE("R5-01 a socket stays allocated while a receive blocked on it has its descriptor closed",
          "[.repro][R5-01]")
{
#ifndef REPRO_HAS_ASAN
    SKIP("needs AddressSanitizer to see the heap shrink");
#else
    const OtherThread other_thread;
    DatagramSocket fixture;

    TestScheduler::on_wait = [&] {
        const bool freed = ReleasesAtLeast(SocketQueueBytes, [&] {
            Files::Close(&fixture.descriptors.table, fixture.descriptor);
        });
        interrupt_now = true;
        // Throwing keeps ReceiveFrom from re-reading socket->queue_length from freed memory.
        REQUIRE_FALSE(freed);
    };
    uint8_t buffer[64];
    const uint64_t result =
        Sockets::ReceiveFrom(fixture.socket, buffer, sizeof(buffer), nullptr, InterruptNow);
    REQUIRE(result == User::SyscallErrorInterrupted);
#endif
}

TEST_CASE("R2-02 the socket a syscall looked up stays allocated when its descriptor is closed "
          "meanwhile",
          "[.repro][R2-02]")
{
#ifndef REPRO_HAS_ASAN
    SKIP("needs AddressSanitizer to see the heap shrink");
#else
    const OtherThread other_thread;
    DatagramSocket fixture;

    // FindSocket is what every socket syscall in process.cpp does first; the call then uses the
    // pointer, so a close in another thread at this point must not free it.
    const bool freed = ReleasesAtLeast(SocketQueueBytes, [&] {
        Files::Close(&fixture.descriptors.table, fixture.descriptor);
    });
    REQUIRE_FALSE(freed);
#endif
}
