#include <catch2/catch_test_macros.hpp>

#include "kernel/futex.h"
#include "kernel/pipe.h"
#include "kernel/scheduler.h"
#include "support/scheduler_stubs.h"
#include "user/syscall_abi.h"

// R2-07 and R3-06 model the lost wake-up of check-then-block. The signal sender is modelled by the
// interrupt callback itself: it answers "not interrupted", then the "other CPU" marks the signal
// pending and calls WakeTask, as SendSignalLocked does, right after the check. The waiter is not
// asleep yet, so the scheduler keeps the wake-up as a token and its WaitOn returns at once. The
// stub's on_wait is the moment the task goes to sleep: it must never be reached with the signal
// pending (deadline 0, nothing else would wake the task).
// R3-04 and the real scheduler live in code the host build does not contain; the kernel tests
// cover them.

namespace
{

bool signal_pending = false;
int check_calls = 0;
bool slept_with_pending_signal = false;

bool SignalArrivesAfterFirstCheck()
{
    ++check_calls;
    const bool result = signal_pending;
    if (check_calls == 1)
    {
        signal_pending = true;
        FeatherOS::Scheduler::WakeTask(TestScheduler::current_task);
    }
    return result;
}

void Reset()
{
    TestScheduler::Reset();
    signal_pending = false;
    check_calls = 0;
    slept_with_pending_signal = false;
    TestScheduler::on_wait = []()
    {
        slept_with_pending_signal = signal_pending;
        // The real task would stay asleep here; the model returns so the loop can end.
    };
}

} // namespace

TEST_CASE("R2-07 a pipe read does not go to sleep with a signal that arrived after its check",
          "[R2-07]")
{
    Reset();
    FeatherOS::Pipes::Pipe* pipe = FeatherOS::Pipes::Create();
    REQUIRE(pipe != nullptr);
    char buffer[8];
    const uint64_t result =
        FeatherOS::Pipes::Read(pipe, buffer, sizeof buffer, SignalArrivesAfterFirstCheck);
    CHECK(TestScheduler::last_wait_deadline == 0);
    CHECK(TestScheduler::early_wakes == 1);
    CHECK_FALSE(slept_with_pending_signal);
    CHECK(result == FeatherOS::User::SyscallErrorInterrupted);
    FeatherOS::Pipes::CloseEnd(pipe, false);
    FeatherOS::Pipes::CloseEnd(pipe, true);
}

TEST_CASE("R2-07 a pipe write on a full pipe does not go to sleep with a pending signal",
          "[R2-07]")
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
    CHECK(TestScheduler::last_wait_deadline == 0);
    CHECK(TestScheduler::early_wakes == 1);
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
          "[R3-06]")
{
    Reset();
    const uint64_t result = FeatherOS::Futex::Wait(&futex_owner, 0x1000, 7, 0, FutexRead,
                                                   SignalArrivesAfterFirstCheck);
    CHECK(TestScheduler::last_wait_deadline == 0);
    CHECK(TestScheduler::early_wakes == 1);
    CHECK_FALSE(slept_with_pending_signal);
    CHECK(result == FeatherOS::User::SyscallErrorInterrupted);
}
