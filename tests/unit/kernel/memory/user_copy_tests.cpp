#include <catch2/catch_test_macros.hpp>

#include "kernel/memory/user_copy.h"
#include "user/syscall_abi.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <sys/mman.h>
#include <thread>
#include <vector>

namespace
{

using namespace FeatherOS;
namespace UserCopy = FeatherOS::Memory::UserCopy;

constexpr uint64_t PageSize = 4096;
constexpr uint64_t Base = 0x40000000;
constexpr size_t PageCount = 4;

struct FakeSpace
{
    Sync::SpinLock lock;
    std::array<std::array<uint8_t, PageSize>, PageCount> pages{};
    std::array<bool, PageCount> mapped{};
    std::array<bool, PageCount> writable{};
    std::vector<size_t> calls;
    int unlocked_calls = 0;
    int resolver_calls = 0;
    bool record = true;
    // When set, resolving this page number unmaps hook_target, as a ShrinkHeap between two holds.
    int hook_on = -1;
    int hook_target = -1;
    // Replaces page 0's backing, and is told when the resolver ran.
    uint8_t* page0_override = nullptr;
    std::atomic<bool>* resolved = nullptr;

    FakeSpace()
    {
        mapped.fill(true);
        writable.fill(true);
    }

    UserCopy::Target Target()
    {
        return {&lock, this, &Resolve};
    }

    static volatile uint8_t* Resolve(void* context, uint64_t page_address, bool write)
    {
        auto* space = static_cast<FakeSpace*>(context);
        if (!space->lock.IsLocked())
        {
            ++space->unlocked_calls;
        }
        ++space->resolver_calls;
        if (page_address < Base || page_address >= Base + PageCount * PageSize)
        {
            return nullptr;
        }
        const size_t page = (page_address - Base) / PageSize;
        if (space->record)
        {
            space->calls.push_back(page);
        }
        const bool hooked = space->hook_on == static_cast<int>(page);
        const bool ok = space->mapped[page] && (!write || space->writable[page]);
        if (hooked)
        {
            space->mapped[static_cast<size_t>(space->hook_target)] = false;
        }
        if (space->resolved != nullptr)
        {
            *space->resolved = true;
        }
        if (page == 0 && space->page0_override != nullptr)
        {
            return ok ? space->page0_override : nullptr;
        }
        return ok ? space->pages[page].data() : nullptr;
    }
};

void Fill(std::vector<uint8_t>& bytes)
{
    for (size_t index = 0; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<uint8_t>(index * 7 + 1);
    }
}

} // namespace

TEST_CASE("user copy T1: copies inside one page and across two and three", "[user_copy]")
{
    for (const size_t size : {1UL, 100UL, 4096UL, 5000UL, 9000UL})
    {
        for (const uint64_t offset : {0UL, 3UL, 4000UL})
        {
            if (offset + size > PageCount * PageSize)
            {
                continue;
            }
            FakeSpace space;
            std::vector<uint8_t> source(size);
            Fill(source);
            REQUIRE(UserCopy::CopyOut(space.Target(), Base + offset, source.data(), size));
            std::vector<uint8_t> back(size);
            REQUIRE(UserCopy::CopyIn(space.Target(), Base + offset, back.data(), size));
            REQUIRE(back == source);
            const auto* flat = space.pages[0].data();
            REQUIRE(std::memcmp(flat + offset, source.data(), size > PageSize - offset ? PageSize - offset : size) == 0);
            REQUIRE(space.unlocked_calls == 0);
        }
    }
}

TEST_CASE("user copy T2: a missing middle page fails after the first", "[user_copy]")
{
    FakeSpace space;
    space.mapped[1] = false;
    std::vector<uint8_t> source(3 * PageSize);
    Fill(source);
    REQUIRE_FALSE(UserCopy::CopyOut(space.Target(), Base, source.data(), source.size()));
    REQUIRE(std::memcmp(space.pages[0].data(), source.data(), PageSize) == 0);
    REQUIRE(space.calls == std::vector<size_t>{0, 1});
    for (const uint8_t byte : space.pages[2])
    {
        REQUIRE(byte == 0);
    }

    space.calls.clear();
    std::vector<uint8_t> back(3 * PageSize);
    REQUIRE_FALSE(UserCopy::CopyIn(space.Target(), Base, back.data(), back.size()));
    REQUIRE(space.calls == std::vector<size_t>{0, 1});
}

TEST_CASE("user copy T3: a read-only page rejects writes but not reads", "[user_copy]")
{
    FakeSpace space;
    space.writable[0] = false;
    uint8_t byte = 9;
    REQUIRE_FALSE(UserCopy::CopyOut(space.Target(), Base + 10, &byte, 1));
    REQUIRE(UserCopy::CopyIn(space.Target(), Base + 10, &byte, 1));
    REQUIRE(UserCopy::RangeMapped(space.Target(), Base, 16, false));
    REQUIRE_FALSE(UserCopy::RangeMapped(space.Target(), Base, 16, true));
}

TEST_CASE("user copy T4: zero size, wrapping ranges and the range check", "[user_copy]")
{
    FakeSpace space;
    uint8_t byte = 0;
    REQUIRE(UserCopy::CopyIn(space.Target(), 0, &byte, 0));
    REQUIRE(UserCopy::CopyOut(space.Target(), 0, &byte, 0));
    REQUIRE(UserCopy::RangeMapped(space.Target(), 0, 0, true));
    REQUIRE(space.calls.empty());
    REQUIRE(space.resolver_calls == 0);

    REQUIRE_FALSE(UserCopy::CopyIn(space.Target(), UINT64_MAX - 3, &byte, 8));
    REQUIRE_FALSE(UserCopy::CopyOut(space.Target(), UINT64_MAX, &byte, 2));
    REQUIRE_FALSE(UserCopy::RangeMapped(space.Target(), UINT64_MAX - 1, 4, false));
    REQUIRE(space.calls.empty());
    REQUIRE(space.resolver_calls == 0);

    REQUIRE(UserCopy::RangeMapped(space.Target(), Base + 100, 2 * PageSize + 5, false));
    REQUIRE(space.calls == std::vector<size_t>{0, 1, 2});
    REQUIRE_FALSE(UserCopy::CopyIn(space.Target(), 0, &byte, 1));
}

TEST_CASE("user copy T5: every page is resolved under the lock and again after an unmap",
          "[user_copy]")
{
    FakeSpace space;
    std::vector<uint8_t> source(2 * PageSize, 0xAB);
    space.hook_on = 0;
    space.hook_target = 1;
    REQUIRE_FALSE(UserCopy::CopyOut(space.Target(), Base, source.data(), source.size()));
    REQUIRE(space.unlocked_calls == 0);
    for (const uint8_t byte : space.pages[1])
    {
        REQUIRE(byte == 0);
    }
    REQUIRE_FALSE(space.lock.IsLocked());
}

namespace
{

std::atomic<bool> intruder_has_lock = false;
std::atomic<bool> intruder_had_lock_during_copy = false;
uint8_t* faulting_page = nullptr;

// The copy's first touch of the page lands here, with the memory lock still held by a correct
// copy; a copy that already dropped it lets the intruder in while it waits.
void OnPageFault(int, siginfo_t*, void*)
{
    const auto start = std::chrono::steady_clock::now();
    while (!intruder_has_lock &&
           std::chrono::steady_clock::now() - start < std::chrono::milliseconds(30))
    {
    }
    intruder_had_lock_during_copy = intruder_has_lock.load();
    mprotect(faulting_page, PageSize, PROT_READ | PROT_WRITE);
}

} // namespace

TEST_CASE("user copy T5b: the lock is held until the page has been copied", "[user_copy]")
{
    void* memory = mmap(nullptr, PageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(memory != MAP_FAILED);
    faulting_page = static_cast<uint8_t*>(memory);
    struct sigaction action = {};
    action.sa_sigaction = OnPageFault;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    struct sigaction old_segv = {};
    struct sigaction old_bus = {};
    REQUIRE(sigaction(SIGSEGV, &action, &old_segv) == 0);
    REQUIRE(sigaction(SIGBUS, &action, &old_bus) == 0);

    for (const bool write : {false, true})
    {
        std::memset(memory, 0x33, PageSize);
        mprotect(memory, PageSize, PROT_NONE);
        FakeSpace space;
        std::atomic<bool> resolved = false;
        space.resolved = &resolved;
        space.page0_override = faulting_page;
        intruder_has_lock = false;
        intruder_had_lock_during_copy = false;
        std::thread intruder(
            [&]
            {
                while (!resolved)
                {
                }
                space.lock.Lock();
                intruder_has_lock = true;
                space.lock.Unlock();
            });
        uint8_t buffer[64];
        std::memset(buffer, 0x44, sizeof(buffer));
        const bool copied = write ? UserCopy::CopyOut(space.Target(), Base, buffer, sizeof(buffer))
                                  : UserCopy::CopyIn(space.Target(), Base, buffer, sizeof(buffer));
        intruder.join();
        REQUIRE(copied);
        REQUIRE_FALSE(intruder_had_lock_during_copy);
        REQUIRE(buffer[0] == (write ? 0x44 : 0x33));
        REQUIRE(faulting_page[0] == (write ? 0x44 : 0x33));
    }

    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);
    munmap(memory, PageSize);
}

TEST_CASE("user copy T7: prefix and string copies", "[user_copy]")
{
    FakeSpace space;
    space.mapped[1] = false;
    std::vector<uint8_t> prefix(2 * PageSize);
    REQUIRE(UserCopy::CopyInPrefix(space.Target(), Base + PageSize - 10, prefix.data(),
                                   prefix.size()) == 10);
    REQUIRE(UserCopy::CopyInPrefix(space.Target(), Base + PageSize, prefix.data(), 5) == 0);
    REQUIRE(UserCopy::CopyInPrefix(space.Target(), Base, prefix.data(), 0) == 0);
    REQUIRE(UserCopy::CopyInPrefix(space.Target(), UINT64_MAX - 3, prefix.data(), 16) == 0);

    char text[32];
    const char hello[] = "hello";
    std::memcpy(space.pages[0].data() + PageSize - 6, hello, sizeof(hello));
    REQUIRE(UserCopy::CopyStringIn(space.Target(), Base + PageSize - 6, text, sizeof(text)) == 0);
    REQUIRE(std::strcmp(text, "hello") == 0);

    std::memset(space.pages[0].data() + PageSize - 3, 'x', 3);
    REQUIRE(UserCopy::CopyStringIn(space.Target(), Base + PageSize - 3, text, sizeof(text)) ==
            User::SyscallErrorBadAddress);

    space.mapped[1] = true;
    std::memset(space.pages[1].data(), 'y', 32);
    REQUIRE(UserCopy::CopyStringIn(space.Target(), Base + PageSize - 3, text, 8) ==
            User::SyscallErrorInvalidArgument);
    space.pages[1][2] = '\0';
    REQUIRE(UserCopy::CopyStringIn(space.Target(), Base + PageSize - 3, text, 8) == 0);
    REQUIRE(std::strcmp(text, "xxxyy") == 0);
}

TEST_CASE("user copy T6: copies never see a page while it is being rebuilt",
          "[user_copy][concurrency]")
{
    FakeSpace space;
    space.record = false;
    constexpr uint8_t Pattern = 0x5A;
    constexpr uint8_t Poison = 0xEE;
    space.pages[0].fill(Pattern);
    std::atomic<bool> done = false;

    std::thread unmapper(
        [&]
        {
            while (!done)
            {
                {
                    Sync::SpinLockGuard guard(space.lock);
                    space.mapped[0] = false;
                }
                // Plain writes outside the lock: a copy still touching the page would race.
                space.pages[0].fill(Poison);
                {
                    Sync::SpinLockGuard guard(space.lock);
                    space.pages[0].fill(Pattern);
                    space.mapped[0] = true;
                }
            }
        });

    uint64_t good = 0;
    uint64_t torn = 0;
    std::array<uint8_t, 2048> data;
    std::array<uint8_t, 2048> source;
    source.fill(Pattern);
    for (int round = 0; round < 20000; ++round)
    {
        if (UserCopy::CopyIn(space.Target(), Base + 100, data.data(), data.size()))
        {
            ++good;
            for (const uint8_t byte : data)
            {
                torn += byte != Pattern;
            }
        }
        UserCopy::CopyOut(space.Target(), Base + 1000, source.data(), 2048);
    }
    done = true;
    unmapper.join();
    REQUIRE(good > 0);
    REQUIRE(torn == 0);
}
