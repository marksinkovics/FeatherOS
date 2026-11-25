#include <catch2/catch_test_macros.hpp>

#include "fake_nvme.h"
#include "nvme_machine.h"
#include "storage_rig.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace NvmeTest;

namespace
{

using FeatherOS::Storage::Testing::ThreadEnvironment;

constexpr uint32_t Sectors = 256;

// Every ladder rung together: the I/O timeout, the abort, the abort's wait, three waits on RDY
// at CAP.TO of 1 s (disable, the re-enable, the disable after a failed reset) and one admin
// command that times out and ends the reset.
constexpr uint64_t RecoveryBoundMs = N::IoTimeoutMs + N::AdminTimeoutMs + N::AbortWaitMs +
                                     3 * 1000 + N::AdminTimeoutMs;

struct Pair
{
    S::Disk* first = nullptr;
    S::Disk* second = nullptr;
};

Pair StartTwo(Machine& m)
{
    m.fake.AddNamespace(1, 512, Sectors);
    m.fake.AddNamespace(2, 512, Sectors);
    REQUIRE(m.Start());
    Pair pair{m.Open(1), m.Open(2)};
    REQUIRE(pair.first != nullptr);
    REQUIRE(pair.second != nullptr);
    return pair;
}

void Release(Pair& pair)
{
    pair.first->Release();
    pair.second->Release();
}

// The controller still works for both namespaces and kept what was written before.
void CheckHealthy(Machine& m, Pair& pair, uint32_t seed)
{
    const auto data = Pattern(seed, 512 * 8);
    std::vector<uint8_t> back(data.size());
    REQUIRE(pair.first->Write(40, data));
    REQUIRE(pair.first->Read(40, back));
    CHECK(back == data);
    REQUIRE(pair.second->Write(40, data));
    REQUIRE(pair.second->Read(40, back));
    CHECK(back == data);
    CHECK(m.fake.Faults().empty());
    CHECK_FALSE(m.controller->Dead());
}

} // namespace

TEST_CASE("nvme T9 an abort that the controller honours frees the command", "[nvme][recovery]")
{
    Machine m;
    Pair pair = StartTwo(m);
    const auto data = Pattern(1, 512 * 4);
    REQUIRE(pair.first->Write(8, data));

    m.fake.HoldIo(FakeNvme::Hold::UntilAborted);
    const uint64_t before = m.env.NowMilliseconds();
    std::vector<uint8_t> back(512 * 4);
    CHECK(pair.first->Read(8, back).error() == Error::Io);
    const uint64_t elapsed = m.env.NowMilliseconds() - before;
    CHECK(elapsed >= N::IoTimeoutMs);
    CHECK(elapsed < N::IoTimeoutMs + 2 * PumpEnvironment::Step);

    CHECK(m.fake.Count(Event::Abort) == 1);
    CHECK(m.controller->AbortCount() == 1);
    CHECK(m.controller->ResetCount() == 0);
    CHECK(m.fake.Count(Event::Enabled) == 1);
    CHECK(LogLines(m.Line("command timed out; aborted")) == 1);
    CHECK(removed.empty());
    CHECK_FALSE(pair.first->Gone());

    REQUIRE(pair.first->Read(8, back));
    CHECK(back == data);
    CheckHealthy(m, pair, 2);
    Release(pair);
}

TEST_CASE("nvme T9 an abort that does not take ends in a controller reset", "[nvme][recovery]")
{
    Machine m;
    Pair pair = StartTwo(m);
    const auto data = Pattern(3, 512 * 4);
    REQUIRE(pair.second->Write(8, data));

    m.fake.HoldIo(FakeNvme::Hold::Never);
    const uint64_t before = m.env.NowMilliseconds();
    std::vector<uint8_t> back(512 * 4);
    CHECK(pair.first->Read(8, back).error() == Error::Io);
    const uint64_t elapsed = m.env.NowMilliseconds() - before;
    CHECK(elapsed >= N::IoTimeoutMs + N::AbortWaitMs);
    CHECK(elapsed <= RecoveryBoundMs);

    CHECK(m.fake.Count(Event::Abort) == 1);
    CHECK(m.controller->ResetCount() == 1);
    CHECK(m.fake.Count(Event::Enabled) == 2);
    CHECK(m.fake.Count(Event::Stopped) == 1);
    CHECK_FALSE(m.fake.HoldActive());
    CHECK(LogLines(m.Line("command timed out; controller reset")) == 1);
    CHECK(removed.empty());
    CHECK_FALSE(pair.first->Gone());
    CHECK_FALSE(pair.second->Gone());

    REQUIRE(pair.second->Read(8, back));
    CHECK(back == data);
    CheckHealthy(m, pair, 4);
    Release(pair);
}

TEST_CASE("nvme T9 an Abort that fails or never completes ends in a reset", "[nvme][recovery]")
{
    Machine m;
    Pair pair = StartTwo(m);
    std::vector<uint8_t> back(512);
    uint64_t least = N::IoTimeoutMs;

    SECTION("the Abort command fails")
    {
        m.fake.options.fail_abort = true;
    }
    SECTION("the Abort command never completes")
    {
        m.fake.StallAdmin(1);
        least += N::AdminTimeoutMs;
    }
    m.fake.HoldIo(FakeNvme::Hold::UntilAborted);
    const uint64_t before = m.env.NowMilliseconds();
    CHECK(pair.first->Read(0, back).error() == Error::Io);
    const uint64_t elapsed = m.env.NowMilliseconds() - before;
    CHECK(elapsed >= least);
    CHECK(elapsed <= RecoveryBoundMs);

    CHECK(m.controller->AbortCount() == 1);
    CHECK(m.controller->ResetCount() == 1);
    CHECK(m.fake.Count(Event::Enabled) == 2);
    CheckHealthy(m, pair, 5);
    Release(pair);
}

TEST_CASE("nvme T9 a fatal status skips the abort and resets", "[nvme][recovery]")
{
    Machine m;
    Pair pair = StartTwo(m);
    const auto data = Pattern(6, 512 * 2);
    REQUIRE(pair.first->Write(20, data));

    m.fake.HoldIo(FakeNvme::Hold::Never, 1, FakeNvme::Effect::Fatal);
    const uint64_t before = m.env.NowMilliseconds();
    std::vector<uint8_t> back(512 * 2);
    CHECK(pair.first->Read(20, back).error() == Error::Io);
    const uint64_t elapsed = m.env.NowMilliseconds() - before;
    CHECK(elapsed >= N::IoTimeoutMs);
    CHECK(elapsed < N::IoTimeoutMs + 5 * 1000);

    CHECK(m.fake.Count(Event::Abort) == 0);
    CHECK(m.controller->ResetCount() == 1);
    CHECK(LogLines(m.Line("controller reports a fatal status")) == 1);
    CHECK(LogLines(m.Line("command timed out; controller reset")) == 1);
    REQUIRE(pair.first->Read(20, back));
    CHECK(back == data);
    CheckHealthy(m, pair, 7);
    Release(pair);
}

TEST_CASE("nvme T9 a controller that reads all ones is gone at once", "[nvme][recovery]")
{
    Machine m;
    Pair pair = StartTwo(m);

    m.fake.HoldIo(FakeNvme::Hold::Never, 1, FakeNvme::Effect::Unplug);
    const uint64_t before = m.env.NowMilliseconds();
    std::vector<uint8_t> back(512);
    CHECK(pair.first->Read(0, back).error() == Error::Io);
    const uint64_t elapsed = m.env.NowMilliseconds() - before;
    CHECK(elapsed >= N::IoTimeoutMs);
    CHECK(elapsed < N::IoTimeoutMs + 2 * PumpEnvironment::Step);

    CHECK(m.fake.Count(Event::Abort) == 0);
    CHECK(m.controller->ResetCount() == 0);
    CHECK(m.controller->Dead());
    CHECK(removed == (std::vector<std::string>{m.DiskName(1), m.DiskName(2)}));
    CHECK(LogLines(m.Line("controller is gone")) == 1);
    CHECK(LogLines(m.Line("controller failed; its disks are gone")) == 1);

    const uint64_t later = m.env.NowMilliseconds();
    CHECK(pair.second->Read(0, back).error() == Error::Io);
    CHECK(m.env.NowMilliseconds() == later);
    CHECK(m.Open(1) == nullptr);
    Release(pair);
}

TEST_CASE("nvme T9 late DMA lands only before RDY is 0 and never into a reused buffer",
          "[nvme][recovery]")
{
    Machine m;
    m.fake.options.ready_delay_reads = 4;
    Pair pair = StartTwo(m);
    const auto first = Pattern(8, 512 * 4);
    const auto second = Pattern(9, 512 * 4);
    REQUIRE(pair.first->Write(16, first));
    REQUIRE(pair.first->Write(32, second));
    std::vector<uint8_t> back(512 * 4);

    SECTION("a read whose data arrives late")
    {
        m.fake.HoldIo(FakeNvme::Hold::LateDma);
        CHECK(pair.first->Read(16, back).error() == Error::Io);
        REQUIRE(pair.first->Read(32, back));
        CHECK(back == second);
    }
    SECTION("a write whose data is fetched late")
    {
        const auto late = Pattern(10, 512 * 4);
        m.fake.HoldIo(FakeNvme::Hold::LateDma);
        CHECK(pair.first->Write(16, late).error() == Error::Io);
        REQUIRE(pair.first->Read(32, back));
        CHECK(back == second);
        std::vector<uint8_t> on_disk(late.size());
        REQUIRE(m.fake.NamespaceDisk(1).Read(16, on_disk));
        CHECK(on_disk == late);
    }

    const auto& events = m.fake.Events();
    const size_t late = Position(events, Event::LateDma);
    const size_t stopped = Position(events, Event::Stopped);
    const size_t enabled = Position(events, Event::Enabled, stopped);
    REQUIRE(late < events.size());
    CHECK(late < stopped);
    CHECK(stopped < enabled);
    CHECK(m.controller->ResetCount() == 1);
    CHECK(m.fake.Faults().empty());
    CheckHealthy(m, pair, 11);
    Release(pair);
}

TEST_CASE("nvme T9 a failed reset removes the disks and later calls fail at once",
          "[nvme][recovery]")
{
    Machine m;
    Pair pair = StartTwo(m);
    bool disables = true;

    SECTION("the controller never stops")
    {
        m.fake.options.never_disable = true;
        disables = false;
    }
    SECTION("the controller never becomes ready again")
    {
        m.fake.options.never_ready = true;
    }
    SECTION("the controller refuses the queues")
    {
        m.fake.options.fail_set_queue_count = true;
    }

    m.fake.HoldIo(FakeNvme::Hold::Never);
    const uint64_t before = m.env.NowMilliseconds();
    std::vector<uint8_t> back(512);
    CHECK(pair.first->Read(0, back).error() == Error::Io);
    CHECK(m.env.NowMilliseconds() - before <= RecoveryBoundMs);

    CHECK(m.controller->Dead());
    CHECK(m.controller->ResetCount() == 1);
    CHECK(removed == (std::vector<std::string>{m.DiskName(1), m.DiskName(2)}));
    CHECK(pair.first->Gone());
    CHECK(pair.second->Gone());
    CHECK(LogLines(m.Line("controller failed; its disks are gone")) == 1);
    if (disables)
    {
        CHECK_FALSE(m.fake.Enabled());
    }

    const uint64_t later = m.env.NowMilliseconds();
    CHECK(pair.second->Read(0, back).error() == Error::Io);
    CHECK(pair.first->Write(0, back).error() == Error::Io);
    CHECK(pair.first->Flush().error() == Error::Io);
    CHECK(m.env.NowMilliseconds() == later);
    CHECK(m.Open(2) == nullptr);
    Release(pair);
}

TEST_CASE("nvme T9 a second reset within a minute makes the controller dead", "[nvme][recovery]")
{
    Machine m;
    Pair pair = StartTwo(m);
    std::vector<uint8_t> back(512);

    m.fake.HoldIo(FakeNvme::Hold::Never);
    CHECK(pair.first->Read(0, back).error() == Error::Io);
    REQUIRE(m.controller->ResetCount() == 1);
    REQUIRE_FALSE(m.controller->Dead());

    SECTION("within 60 s")
    {
        m.env.Advance(10 * 1000);
        m.fake.HoldIo(FakeNvme::Hold::Never);
        const uint64_t before = m.env.NowMilliseconds();
        CHECK(pair.first->Read(0, back).error() == Error::Io);
        CHECK(m.env.NowMilliseconds() - before <= RecoveryBoundMs);
        CHECK(m.controller->Dead());
        CHECK(m.controller->ResetCount() == 1);
        CHECK(removed.size() == 2);
        CHECK(pair.second->Read(0, back).error() == Error::Io);
        CHECK(LogLines(m.Line("second reset within 60 s")) == 1);
    }
    SECTION("after 60 s")
    {
        m.env.Advance(N::ResetWindowMs + 1000);
        m.fake.HoldIo(FakeNvme::Hold::Never);
        CHECK(pair.first->Read(0, back).error() == Error::Io);
        CHECK(m.controller->ResetCount() == 2);
        CHECK_FALSE(m.controller->Dead());
        CheckHealthy(m, pair, 12);
    }
    Release(pair);
}

TEST_CASE("nvme T9 a namespace that changed in the reset loses its disk only", "[nvme][recovery]")
{
    Machine m;
    Pair pair = StartTwo(m);
    std::vector<uint8_t> back(512);

    SECTION("it got another size")
    {
        m.fake.AddNamespace(2, 512, Sectors / 2);
    }
    SECTION("it is no longer active")
    {
        FakeNvme::NamespaceFlags flags;
        flags.inactive = true;
        m.fake.AddNamespace(2, 512, Sectors, flags);
    }
    SECTION("it got another sector size")
    {
        m.fake.AddNamespace(2, 4096, Sectors / 8);
    }

    m.fake.HoldIo(FakeNvme::Hold::Never);
    CHECK(pair.first->Read(0, back).error() == Error::Io);

    CHECK_FALSE(m.controller->Dead());
    CHECK(m.controller->ResetCount() == 1);
    CHECK(removed == std::vector<std::string>{m.DiskName(2)});
    CHECK(pair.second->Gone());
    CHECK_FALSE(pair.first->Gone());
    CHECK(pair.second->Read(0, back).error() == Error::Io);
    REQUIRE(pair.first->Write(0, back));
    REQUIRE(pair.first->Read(0, back));
    CHECK(m.fake.Faults().empty());
    Release(pair);
}

namespace
{

// Real threads against one fake whose device work runs on a thread of its own, so a lock the
// interrupt path or a wait callback must not take shows up as a deadlock the watchdog reports.
class Watchdog
{
public:
    explicit Watchdog(std::chrono::seconds limit)
        : thread_(
              [this, limit]
              {
                  std::unique_lock lock(mutex_);
                  if (!signal_.wait_for(lock, limit, [this] { return done_; }))
                  {
                      std::fprintf(stderr, "nvme T23: no progress, probably a deadlock\n");
                      std::abort();
                  }
              })
    {
    }

    ~Watchdog()
    {
        {
            const std::lock_guard lock(mutex_);
            done_ = true;
        }
        signal_.notify_all();
        thread_.join();
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable signal_;
    bool done_ = false;
    std::thread thread_;
};

} // namespace

TEST_CASE("nvme T23 two namespaces, the interrupt path and the device thread", "[nvme][concurrency]")
{
    constexpr uint32_t Rounds = 150;
    constexpr uint32_t Workers = 2;

    S::Initialize();
    S::SetScanHandler(nullptr);
    S::SetRemovalHandler(nullptr);
    FeatherOS::Testing::PhysicalMemory memory;
    FakeNvme fake(memory);
    ThreadEnvironment env(20);
    fake.options.defer_processing = true;
    fake.AddNamespace(1, 512, Sectors);
    fake.AddNamespace(2, 512, Sectors);
    std::unique_ptr<N::Controller> controller;
    S::Disk* disks[Workers] = {};
    std::atomic<bool> stop = false;
    std::atomic<uint32_t> progress = 0;
    std::atomic<uint32_t> io_failures = 0;
    std::atomic<uint32_t> mismatches = 0;

    {
        const Watchdog watchdog(std::chrono::seconds(120));
        std::thread device(
            [&]
            {
                while (!stop.load(std::memory_order_acquire))
                {
                    fake.Pump();
                    std::this_thread::yield();
                }
            });
        controller.reset(N::Controller::Create(fake, env, 90, S::WritePolicy::Allowed, "00:04.0"));
        REQUIRE(controller->Start());
        disks[0] = S::Acquire("nvme90n1");
        disks[1] = S::Acquire("nvme90n2");
        REQUIRE(disks[0] != nullptr);
        REQUIRE(disks[1] != nullptr);

        std::thread interrupter(
            [&]
            {
                while (!stop.load(std::memory_order_acquire))
                {
                    N::Controller::OnInterrupt(controller.get());
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            });
        std::vector<std::thread> workers;
        for (uint32_t w = 0; w < Workers; ++w)
        {
            workers.emplace_back(
                [&, w]
                {
                    std::vector<uint8_t> back(512 * 8);
                    for (uint32_t round = 0; round < Rounds; ++round)
                    {
                        const uint64_t lba = 8 * (round % 16);
                        const auto data = Pattern(w * 1000 + round, back.size());
                        if (!disks[w]->Write(lba, data) || !disks[w]->Read(lba, back))
                        {
                            io_failures.fetch_add(1);
                        }
                        else if (back != data)
                        {
                            mismatches.fetch_add(1);
                        }
                        if (round % 10 == 0 && !disks[w]->Flush())
                        {
                            io_failures.fetch_add(1);
                        }
                        progress.fetch_add(1);
                    }
                });
        }

        while (progress.load() < Rounds / 2)
        {
            std::this_thread::yield();
        }
        // One command hangs while the other namespace's thread queues behind its recovery.
        fake.HoldIo(FakeNvme::Hold::UntilAborted);
        for (std::thread& worker : workers)
        {
            worker.join();
        }
        stop.store(true, std::memory_order_release);
        device.join();
        interrupter.join();
    }

    CHECK(mismatches.load() == 0);
    CHECK(io_failures.load() == 1);
    CHECK(controller->AbortCount() == 1);
    CHECK(controller->ResetCount() == 0);
    CHECK_FALSE(controller->Dead());
    CHECK(fake.Faults().empty());

    for (S::Disk* disk : disks)
    {
        S::Remove(*disk);
        disk->Release();
    }
    controller.reset();
}
