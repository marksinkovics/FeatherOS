#include <catch2/catch_test_macros.hpp>

#include "kernel/futex.h"
#include "kernel/pipe.h"
#include "support/scheduler_stubs.h"
#include "user/syscall_abi.h"

// R2-07 and R3-06 model the lost wake-up of check-then-block. The real Scheduler::WaitOn
// (scheduler.cpp:560-585) sets Blocked under scheduler_lock without re-checking any condition, and
// Scheduler::WakeTask (598-611) drops a wake-up for a task that is still Running. The signal
// sender is modelled by the interrupt callback itself: it answers "not interrupted", then the
// "other CPU" marks the signal pending right after the check, where WakeTask would find the task
// Running and do nothing. The stub's on_wait is the moment the task goes to sleep: a signal that is
// already pending then is one nothing will ever wake the task for (deadline 0). The tests assert
// the correct behaviour, that a waiter never goes to sleep with an undelivered signal pending.
// R3-04, R3-05 and R3-07 live in code the host build does not contain (scheduler.cpp, cpu.cpp,
// memory/kernel_stack.cpp); their traces are in the report, not here.

namespace
{

bool signal_pending = false;
int check_calls = 0;
bool slept_with_pending_signal = false;
uint64_t sleep_deadline = 1;

bool SignalArrivesAfterFirstCheck()
{
    ++check_calls;
    const bool result = signal_pending;
    if (check_calls == 1)
    {
        signal_pending = true;
    }
    return result;
}

void Reset()
{
    TestScheduler::Reset();
    signal_pending = false;
    check_calls = 0;
    slept_with_pending_signal = false;
    sleep_deadline = 1;
    TestScheduler::on_wait = []()
    {
        slept_with_pending_signal = signal_pending;
        sleep_deadline = TestScheduler::last_wait_deadline;
        // The real task would stay asleep here; the model returns so the loop can end.
    };
}

} // namespace

TEST_CASE("R2-07 a pipe read does not go to sleep with a signal that arrived after its check",
          "[.repro][R2-07]")
{
    Reset();
    FeatherOS::Pipes::Pipe* pipe = FeatherOS::Pipes::Create();
    REQUIRE(pipe != nullptr);
    char buffer[8];
    const uint64_t result =
        FeatherOS::Pipes::Read(pipe, buffer, sizeof buffer, SignalArrivesAfterFirstCheck);
    CHECK(sleep_deadline == 0);
    CHECK_FALSE(slept_with_pending_signal);
    CHECK(result == FeatherOS::User::SyscallErrorInterrupted);
    FeatherOS::Pipes::CloseEnd(pipe, false);
    FeatherOS::Pipes::CloseEnd(pipe, true);
}

TEST_CASE("R2-07 a pipe write on a full pipe does not go to sleep with a pending signal",
          "[.repro][R2-07]")
{
    Reset();
    FeatherOS::Pipes::Pipe* pipe = FeatherOS::Pipes::Create();
    REQUIRE(pipe != nullptr);
    const std::vector<uint8_t> block(FeatherOS::Pipes::Capacity, 0x5A);
    REQUIRE(FeatherOS::Pipes::Write(pipe, block.data(), block.size(), nullptr) == block.size());
    signal_pending = false;
    check_calls = 0;
    const uint8_t extra = 1;
    const uint64_t result =
        FeatherOS::Pipes::Write(pipe, &extra, 1, SignalArrivesAfterFirstCheck);
    CHECK(sleep_deadline == 0);
    CHECK_FALSE(slept_with_pending_signal);
    CHECK(result == FeatherOS::User::SyscallErrorInterrupted);
    FeatherOS::Pipes::CloseEnd(pipe, false);
    FeatherOS::Pipes::CloseEnd(pipe, true);
}

namespace
{

const int futex_owner = 0;

bool FutexRead(const void*, uint64_t, uint32_t* value)
{
    *value = 7;
    return true;
}

} // namespace

TEST_CASE("R3-06 a futex wait does not go to sleep with a signal that arrived after its check",
          "[.repro][R3-06]")
{
    Reset();
    const uint64_t result = FeatherOS::Futex::Wait(&futex_owner, 0x1000, 7, 0, FutexRead,
                                                   SignalArrivesAfterFirstCheck);
    CHECK(sleep_deadline == 0);
    CHECK_FALSE(slept_with_pending_signal);
    CHECK(result == FeatherOS::User::SyscallErrorInterrupted);
}
