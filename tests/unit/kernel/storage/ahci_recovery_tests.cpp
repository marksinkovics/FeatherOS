#include <catch2/catch_test_macros.hpp>

#include "ahci_machine.h"
#include "fake_ahci.h"
#include "storage_rig.h"

#include "kernel/storage/ahci/registers.h"

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

using namespace AhciTest;

namespace
{

using FeatherOS::Storage::Testing::ThreadEnvironment;

constexpr uint32_t Sectors = 2048;

size_t Position(const FakeAhci& fake, Event event, uint32_t port, size_t from = 0)
{
    const auto& log = fake.Log();
    for (size_t i = from; i < log.size(); i++)
    {
        if (log[i].event == event && log[i].port == port)
        {
            return i;
        }
    }
    return log.size();
}

// A dead port refuses at once and stays stopped.
void CheckDead(Machine& m, A::Port& port)
{
    CHECK(port.Dead());
    CHECK(port.Gone());
    CHECK(removed == std::vector<std::string>{"sda"});
    CHECK(m.Lines("port failed; its disk is gone") == 1);
    const uint64_t later = m.env.NowMilliseconds();
    const uint32_t reads = m.fake.Count(Event::Read);
    std::vector<uint8_t> buffer(512);
    CHECK(port.Read(0, 1, buffer.data()).error() == Error::Io);
    CHECK(port.Write(0, 1, buffer.data()).error() == Error::Io);
    CHECK(port.Flush().error() == Error::Io);
    CHECK(m.env.NowMilliseconds() == later);
    CHECK(m.fake.Count(Event::Read) == reads);
}

} // namespace

TEST_CASE("ahci T20: a task file error stops the engine, clears the port and starts it again",
          "[ahci][recovery]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    const auto data = Pattern(1, 512 * 4);
    REQUIRE(port.Write(8, 4, data.data()));

    m.fake.Inject(0, FakeAhci::Failure::TaskFile);
    std::vector<uint8_t> back(512 * 4);
    CHECK(port.Read(8, 4, back.data()).error() == Error::Io);

    CHECK_FALSE(port.Dead());
    CHECK(m.fake.Count(Event::Comreset) == 0);
    CHECK(m.fake.Count(Event::EngineStarted) == 2);
    const size_t stopped = Position(m.fake, Event::EngineStopped, 0);
    const size_t halted = Position(m.fake, Event::EngineHalted, 0, stopped);
    const size_t restarted = Position(m.fake, Event::EngineStarted, 0, halted);
    CHECK(stopped < halted);
    CHECK(halted < restarted);
    CHECK(restarted < m.fake.Log().size());
    CHECK(m.Lines("READ DMA EXT failed (status 51h error 40h); port recovered") == 1);
    CHECK(removed.empty());

    REQUIRE(port.Read(8, 4, back.data()));
    CHECK(back == data);
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("ahci T20: a command that hangs the device ends in a COMRESET", "[ahci][recovery]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    const auto data = Pattern(2, 512 * 4);
    REQUIRE(port.Write(8, 4, data.data()));

    m.fake.Inject(0, FakeAhci::Failure::Hang);
    std::vector<uint8_t> back(512 * 4);
    const uint64_t before = m.Elapsed();
    CHECK(port.Read(8, 4, back.data()).error() == Error::Io);
    const uint64_t took = m.Elapsed() - before;
    CHECK(took >= A::IoTimeoutMs + A::ResetHoldMs);
    CHECK(took < A::IoTimeoutMs + 1000);

    CHECK_FALSE(port.Dead());
    CHECK(m.fake.Count(Event::Comreset) == 1);
    const size_t halted = Position(m.fake, Event::EngineHalted, 0);
    const size_t reset = Position(m.fake, Event::Comreset, 0, halted);
    const size_t restarted = Position(m.fake, Event::EngineStarted, 0, reset);
    CHECK(halted < reset);
    CHECK(reset < restarted);
    CHECK(restarted < m.fake.Log().size());
    CHECK(m.Lines("READ DMA EXT timed out (status 80h error 00h); port recovered") == 1);

    // The PRCS the reset itself raised must not look like an unplug.
    REQUIRE(port.Read(8, 4, back.data()));
    CHECK(back == data);
    REQUIRE(port.Write(16, 4, data.data()));
    CHECK_FALSE(port.Dead());
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("ahci T20: late DMA lands only before CR is 0 and never into a reused buffer",
          "[ahci][recovery]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    const auto first = Pattern(3, 512 * 4);
    const auto second = Pattern(4, 512 * 4);
    REQUIRE(port.Write(16, 4, first.data()));
    REQUIRE(port.Write(32, 4, second.data()));
    m.fake.options.engine_stop_delay_reads = 4;

    m.fake.Inject(0, FakeAhci::Failure::LateDma);
    std::vector<uint8_t> back(512 * 4);
    CHECK(port.Read(16, 4, back.data()).error() == Error::Io);

    const size_t stopped = Position(m.fake, Event::EngineStopped, 0);
    const size_t late = Position(m.fake, Event::LateDma, 0, stopped);
    const size_t restarted = Position(m.fake, Event::EngineStarted, 0, stopped);
    REQUIRE(late < m.fake.Log().size());
    CHECK(late < restarted);
    CHECK(restarted < m.fake.Log().size());

    REQUIRE(port.Read(32, 4, back.data()));
    CHECK(back == second);
    REQUIRE(port.Read(16, 4, back.data()));
    CHECK(back == first);
    CHECK_FALSE(port.Dead());
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("ahci T20: the third recovery within a minute makes the port dead", "[ahci][recovery]")
{
    Machine m;
    m.fake.AddPort(0);
    m.fake.AddPort(1);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    std::vector<uint8_t> back(512);
    m.fake.Inject(0, FakeAhci::Failure::TaskFile, 3);

    SECTION("within 60 s")
    {
        CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
        m.env.Advance(20 * 1000);
        CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
        CHECK_FALSE(port.Dead());
        m.env.Advance(20 * 1000);
        CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
        CheckDead(m, port);
        CHECK(m.fake.Count(Event::EngineStarted, 0) == 3);
        CHECK(m.Lines("no recovery: three recoveries within 60 s") == 1);
        CHECK(m.Lines("port recovered") == 2);
        S::Disk* other = m.Open(1);
        REQUIRE(other != nullptr);
        CHECK(other->Read(0, back));
        other->Release();
    }
    SECTION("spread over more than 60 s")
    {
        CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
        m.env.Advance(31 * 1000);
        CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
        m.env.Advance(31 * 1000);
        CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
        CHECK_FALSE(port.Dead());
        CHECK(m.Lines("port recovered") == 3);
        CHECK(port.Read(0, 1, back.data()));
    }
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("ahci T20: a port reporting a PHY change is dead without a reset", "[ahci][recovery]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    m.fake.RaisePhyChange(0);

    std::vector<uint8_t> back(512);
    CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
    CheckDead(m, port);
    CHECK(m.fake.Count(Event::Comreset) == 0);
    CHECK(m.fake.Count(Event::Read) == 0);
    CHECK(m.Lines("no recovery: the PHY changed state") == 1);
    CHECK_FALSE(m.fake.EngineRunning(0));
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("ahci T20: a COMRESET that does not bring the port back makes it dead",
          "[ahci][recovery]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    std::vector<uint8_t> back(512);
    uint64_t took = 0;
    const char* reason = "";

    SECTION("no link")
    {
        m.fake.LoseLinkOnComreset(0);
        reason = "no recovery: no link after COMRESET";
        took = A::IoTimeoutMs + A::LinkWaitMs;
    }
    SECTION("the device stays busy")
    {
        m.fake.WedgeOnComreset(0);
        reason = "no recovery: the device stayed busy after COMRESET";
        took = A::IoTimeoutMs + A::ReadyWaitMs;
    }
    SECTION("another device answers")
    {
        m.fake.ChangeSignatureOnComreset(0, 0xEB140101);
        reason = "no recovery: the device signature changed";
        took = A::IoTimeoutMs;
    }
    m.fake.Inject(0, FakeAhci::Failure::Hang);
    const uint64_t before = m.Elapsed();
    CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
    CHECK(m.Elapsed() - before >= took);
    CHECK(m.Elapsed() - before < took + 1000);
    CheckDead(m, port);
    CHECK(m.fake.Count(Event::Comreset) == 1);
    CHECK(m.fake.Count(Event::EngineStarted) == 1);
    CHECK(m.Lines(reason) == 1);
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("ahci T20: an engine that does not stop pins the buffers and the port is dead",
          "[ahci][recovery]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    m.fake.options.engine_never_stops = true;
    m.fake.Inject(0, FakeAhci::Failure::TaskFile);

    std::vector<uint8_t> back(512);
    CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
    CHECK(port.Dead());
    CHECK(m.Lines("no recovery: the command engine did not stop") == 1);
    m.hba.reset();
    CHECK(m.fake.LiveBuffers() == 2);
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("ahci T20: the interrupt handler only reads and wakes", "[ahci][recovery]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    std::vector<uint8_t> back(512);
    REQUIRE(port.Read(0, 1, back.data()));
    m.fake.Inject(0, FakeAhci::Failure::TaskFile);
    CHECK(port.Read(0, 1, back.data()).error() == Error::Io);
    REQUIRE(port.Read(0, 1, back.data()));
    CHECK(m.fake.Interrupts() > 0);
    CHECK(m.env.Wakes() > 0);
    CHECK(m.fake.Faults().empty());
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
                      std::fprintf(stderr, "ahci T23: no progress, probably a deadlock\n");
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

TEST_CASE("ahci T23 two ports, the interrupt path and the device thread", "[ahci][concurrency]")
{
    constexpr uint32_t Rounds = 150;
    constexpr uint32_t Workers = 2;

    S::Initialize();
    S::SetScanHandler(nullptr);
    S::SetRemovalHandler(nullptr);
    FeatherOS::Testing::PhysicalMemory memory;
    FakeAhci fake(memory);
    ThreadEnvironment env(20);
    fake.options.defer_processing = true;
    fake.AddPort(0);
    fake.AddPort(1);
    std::unique_ptr<A::Hba> hba;
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
        hba.reset(A::Hba::Create(fake, env, 90, S::WritePolicy::Allowed, "00:05.0"));
        REQUIRE(hba->Start());
        disks[0] = S::Acquire("sda");
        disks[1] = S::Acquire("sdb");
        REQUIRE(disks[0] != nullptr);
        REQUIRE(disks[1] != nullptr);

        std::thread interrupter(
            [&]
            {
                while (!stop.load(std::memory_order_acquire))
                {
                    A::Hba::OnInterrupt(hba.get());
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
        // One port recovers from an error while the other one's thread queues behind a hang that
        // ends in a COMRESET.
        fake.Inject(0, FakeAhci::Failure::TaskFile);
        fake.Inject(1, FakeAhci::Failure::Hang);
        for (std::thread& worker : workers)
        {
            worker.join();
        }
        stop.store(true, std::memory_order_release);
        device.join();
        interrupter.join();
    }

    CHECK(mismatches.load() == 0);
    CHECK(io_failures.load() == 2);
    CHECK(fake.Count(FakeAhci::Event::Comreset) == 1);
    CHECK(fake.Count(FakeAhci::Event::Comreset, 1) == 1);
    CHECK_FALSE(hba->DiskPort(0)->Dead());
    CHECK_FALSE(hba->DiskPort(1)->Dead());
    CHECK(fake.Faults().empty());

    for (S::Disk* disk : disks)
    {
        S::Remove(*disk);
        disk->Release();
    }
    hba.reset();
}
