#include <catch2/catch_test_macros.hpp>

#include "kernel/futex.h"
#include "support/scheduler_stubs.h"
#include "user/syscall_abi.h"

#include <map>

namespace
{

using FeatherOS::Futex::Wait;
using FeatherOS::Futex::Wake;

std::map<uint64_t, uint32_t> words;
bool interrupt_now = false;
const int owner = 0;
const int other_owner = 0;

bool ReadWord(const void*, uint64_t address, uint32_t* value)
{
    const auto found = words.find(address);
    if (found == words.end())
    {
        return false;
    }
    *value = found->second;
    return true;
}

bool Interrupted()
{
    return interrupt_now;
}

void Reset()
{
    TestScheduler::Reset();
    words = {{0x1000, 7}, {0x2000, 0}};
    interrupt_now = false;
}

} // namespace

TEST_CASE("a futex wait returns at once when the word changed or cannot be read")
{
    Reset();
    REQUIRE(Wait(&owner, 0x1000, 8, 0, ReadWord, Interrupted) ==
            FeatherOS::User::SyscallErrorWouldBlock);
    REQUIRE(Wait(&owner, 0x3000, 0, 0, ReadWord, Interrupted) ==
            FeatherOS::User::SyscallErrorBadAddress);
    REQUIRE(TestScheduler::wait_count == 0);
    REQUIRE(Wake(&owner, 0x1000, 1) == 0);
}

TEST_CASE("a futex wake ends the wait for that word of that process only")
{
    Reset();
    int calls = 0;
    TestScheduler::on_wait = [&]()
    {
        ++calls;
        // Neither another process's word nor another word of this one wakes it.
        REQUIRE(Wake(&other_owner, 0x1000, 10) == 0);
        REQUIRE(Wake(&owner, 0x2000, 10) == 0);
        if (calls == 2)
        {
            REQUIRE(Wake(&owner, 0x1000, 10) == 1);
        }
    };
    REQUIRE(Wait(&owner, 0x1000, 7, 0, ReadWord, Interrupted) == 0);
    REQUIRE(calls == 2);
    REQUIRE(TestScheduler::woken_tasks == std::vector<uint32_t>{1});
    REQUIRE(Wake(&owner, 0x1000, 10) == 0);
}

TEST_CASE("a futex wake of one waiter takes the oldest first")
{
    Reset();
    int first_calls = 0;
    TestScheduler::on_wait = [&]()
    {
        if (++first_calls != 1)
        {
            return;
        }
        // While task 1 waits, task 2 starts waiting too, and each wake ends one wait.
        TestScheduler::current_task = 2;
        int second_calls = 0;
        TestScheduler::on_wait = [&]()
        {
            ++second_calls;
            REQUIRE(Wake(&owner, 0x1000, 1) == 1);
        };
        REQUIRE(Wait(&owner, 0x1000, 7, 0, ReadWord, Interrupted) == 0);
        REQUIRE(second_calls == 2);
        TestScheduler::on_wait = nullptr;
    };
    REQUIRE(Wait(&owner, 0x1000, 7, 0, ReadWord, Interrupted) == 0);
    REQUIRE(TestScheduler::woken_tasks == std::vector<uint32_t>{1, 2});
}

TEST_CASE("a futex wait gives up at its deadline or for a signal")
{
    Reset();
    TestScheduler::ticks = 100;
    TestScheduler::on_wait = []() { TestScheduler::ticks = 150; };
    REQUIRE(Wait(&owner, 0x1000, 7, 150, ReadWord, Interrupted) ==
            FeatherOS::User::SyscallErrorTimedOut);
    REQUIRE(TestScheduler::last_wait_deadline == 150);
    // The waiter left the queue, so a wake finds nobody.
    REQUIRE(Wake(&owner, 0x1000, 1) == 0);

    TestScheduler::on_wait = []() { interrupt_now = true; };
    REQUIRE(Wait(&owner, 0x1000, 7, 0, ReadWord, Interrupted) ==
            FeatherOS::User::SyscallErrorInterrupted);
    REQUIRE(Wake(&owner, 0x1000, 1) == 0);
}
