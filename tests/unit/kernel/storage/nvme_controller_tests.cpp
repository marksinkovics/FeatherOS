#include <catch2/catch_test_macros.hpp>

#include "fake_nvme.h"
#include "nvme_machine.h"
#include "storage_rig.h"

#include "support/scheduler_stubs.h"

#include "kernel/log_ring.h"
#include "kernel/memory/dma.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/nvme/controller.h"
#include "kernel/storage/nvme/queue.h"
#include "kernel/storage/write_policy.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace NvmeTest;


TEST_CASE("nvme bring-up registers one namespace", "[nvme][controller]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, 2048);
    REQUIRE(m.controller == nullptr);
    REQUIRE(m.Start());
    CHECK(m.fake.Faults().empty());
    CHECK(m.fake.IoQueueEntries() == 32);
    CHECK(m.controller->NamespaceCount() == 1);
    CHECK_FALSE(m.controller->Dead());
    CHECK(m.fake.Count(Event::Enabled) == 1);
    CHECK(m.fake.Count(Event::AdminProgrammed) == 1);

    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const S::DiskInfo info = disk->Info();
    CHECK(info.kind == S::DiskKind::Nvme);
    CHECK(info.sector_size == 512);
    CHECK(info.sector_count == 2048);
    CHECK(info.max_transfer_bytes == 128 * 1024);
    CHECK_FALSE(info.read_only);
    CHECK(info.read_only_reason == S::ReadOnlyReason::None);
    CHECK(info.controller_index == m.index);
    CHECK(info.namespace_id == 1);
    CHECK(std::string(info.model) == "Fake NVMe Controller");
    CHECK(std::string(info.location) == "00:04.0");
    CHECK(scanned == std::vector<std::string>{m.DiskName(1)});
    CHECK(LogLines(m.Line("QEMU controller, writes allowed")) == 1);
    CHECK(LogLines(m.Line("00:04.0 \"Fake NVMe Controller\" fw 1.0, MSI-X, 1 I/O queue of 32, "
                          "max transfer 128 KiB, volatile write cache")) == 1);
    disk->Release();
}

TEST_CASE("nvme bring-up disables a controller that firmware left running", "[nvme][controller]")
{
    Machine m;
    m.fake.options.firmware_enabled = true;
    m.fake.AddNamespace(1, 512, 256);
    REQUIRE(m.Start());
    CHECK(m.fake.Faults().empty());
    const auto& events = m.fake.Events();
    REQUIRE(events.front() == Event::Enabled);
    const size_t disabled = Position(events, Event::Disabled);
    const size_t programmed = Position(events, Event::AdminProgrammed);
    const size_t enabled = Position(events, Event::Enabled, 1);
    CHECK(disabled < programmed);
    CHECK(programmed < enabled);
    CHECK(m.fake.Enabled());
}

TEST_CASE("the fake faults on admin registers written while enabled", "[nvme][controller]")
{
    FeatherOS::Testing::PhysicalMemory memory;
    FakeNvme fake(memory);
    fake.options.firmware_enabled = true;
    fake.Write32(N::Reg::Aqa, 0x001F001F);
    REQUIRE(fake.Faults().size() == 1);
    fake.Write64(N::Reg::Asq, 0x1000);
    CHECK(fake.Faults().size() == 3);
}

TEST_CASE("nvme bring-up waits within CAP.TO and refuses", "[nvme][controller]")
{
    SECTION("a controller that never becomes ready")
    {
        Machine m;
        m.fake.options.timeout_units = 1;
        m.fake.options.never_ready = true;
        m.fake.AddNamespace(1, 512, 256);
        REQUIRE_FALSE(m.Start());
        CHECK(m.Elapsed() >= 500);
        CHECK(m.Elapsed() < 700);
        CHECK(m.fake.Faults().empty());
        CHECK_FALSE(m.fake.Enabled());
        CHECK(m.fake.LiveBuffers() == 0);
        CHECK(m.Open() == nullptr);
        CHECK(LogLines(m.Line("controller did not become ready")) == 1);
    }
    SECTION("a longer CAP.TO waits longer")
    {
        Machine m;
        m.fake.options.timeout_units = 6;
        m.fake.options.never_ready = true;
        m.fake.AddNamespace(1, 512, 256);
        REQUIRE_FALSE(m.Start());
        CHECK(m.Elapsed() >= 3000);
        CHECK(m.Elapsed() < 3200);
    }
    SECTION("a controller that never disables")
    {
        Machine m;
        m.fake.options.timeout_units = 1;
        m.fake.options.firmware_enabled = true;
        m.fake.options.never_disable = true;
        m.fake.AddNamespace(1, 512, 256);
        REQUIRE_FALSE(m.Start());
        CHECK(m.Elapsed() >= 500);
        CHECK(m.fake.Faults().empty());
        CHECK(m.fake.Count(Event::AdminProgrammed) == 0);
        CHECK(m.fake.LiveBuffers() == 0);
        CHECK(LogLines(m.Line("controller did not disable")) == 1);
    }
    SECTION("a slow controller still gets there")
    {
        Machine m;
        m.fake.options.ready_delay_reads = 5;
        m.fake.AddNamespace(1, 512, 256);
        REQUIRE(m.Start());
        CHECK(m.fake.Faults().empty());
    }
}

TEST_CASE("nvme bring-up refuses a controller in trouble", "[nvme][controller]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, 256);
    SECTION("fatal status")
    {
        m.fake.options.fatal_on_enable = true;
        REQUIRE_FALSE(m.Start());
        CHECK(LogLines(m.Line("controller reported a fatal status")) == 1);
        CHECK(m.fake.LiveBuffers() == 0);
    }
    SECTION("a register read of all ones")
    {
        m.fake.Unplug();
        REQUIRE_FALSE(m.Start());
        CHECK(LogLines(m.Line("controller is gone")) == 1);
    }
    SECTION("no NVM command set")
    {
        m.fake.options.nvm_command_set = false;
        REQUIRE_FALSE(m.Start());
        CHECK(m.fake.LiveBuffers() == 0);
    }
    SECTION("no 4 KiB pages")
    {
        m.fake.options.min_page_log2 = 1;
        REQUIRE_FALSE(m.Start());
    }
    SECTION("no queue granted")
    {
        m.fake.options.fail_set_queue_count = true;
        REQUIRE_FALSE(m.Start());
        CHECK_FALSE(m.fake.Enabled());
        CHECK(m.fake.LiveBuffers() == 0);
    }
    SECTION("out of memory")
    {
        m.fake.options.allocations_before_failure = 3;
        REQUIRE_FALSE(m.Start());
        CHECK(m.fake.LiveBuffers() == 0);
    }
    SECTION("out of memory for the transfer buffer")
    {
        m.fake.options.allocations_before_failure = 6;
        REQUIRE_FALSE(m.Start());
        CHECK_FALSE(m.fake.Enabled());
        CHECK(m.fake.LiveBuffers() == 0);
    }
}

TEST_CASE("nvme refuses a controller with no namespace to use", "[nvme][controller]")
{
    SECTION("NN is zero")
    {
        Machine m;
        REQUIRE_FALSE(m.Start());
        CHECK(m.fake.LiveBuffers() == 0);
        CHECK_FALSE(m.fake.Enabled());
        CHECK(LogLines(m.Line("no namespaces")) == 1);
    }
    SECTION("every namespace refused")
    {
        Machine m;
        FakeNvme::NamespaceFlags metadata;
        metadata.metadata_bytes = 8;
        m.fake.AddNamespace(1, 512, 64, metadata);
        REQUIRE_FALSE(m.Start());
        CHECK(m.fake.LiveBuffers() == 0);
        CHECK_FALSE(m.fake.Enabled());
        CHECK(LogLines(m.Line("no namespaces")) == 1);
    }
}

TEST_CASE("nvme uses the doorbell stride the controller states", "[nvme][controller]")
{
    Machine m;
    m.fake.options.doorbell_stride_log2 = 2;
    m.fake.AddNamespace(1, 512, 256);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto data = Pattern(3, 512 * 4);
    REQUIRE(disk->Write(8, data));
    std::vector<uint8_t> back(data.size());
    REQUIRE(disk->Read(8, back));
    CHECK(back == data);
    CHECK(m.fake.Faults().empty());
    disk->Release();
}

TEST_CASE("nvme I/O queue depth is min(MQES + 1, 32)", "[nvme][controller]")
{
    const std::pair<uint32_t, uint32_t> cases[] = {{3, 4}, {15, 16}, {31, 32}, {255, 32}, {65535, 32}};
    for (const auto& [mqes, depth] : cases)
    {
        Machine m;
        m.fake.options.mqes = mqes;
        m.fake.AddNamespace(1, 512, 256);
        REQUIRE(m.Start());
        CHECK(m.fake.IoQueueEntries() == depth);
        CHECK(m.fake.Faults().empty());
    }
}

TEST_CASE("nvme transfers whole sectors of both sizes", "[nvme][io]")
{
    for (const uint32_t sector : {512U, 4096U})
    {
        Machine m;
        const uint64_t sectors = 4 * 1024 * 1024 / sector;
        m.fake.AddNamespace(1, sector, sectors);
        REQUIRE(m.Start());
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        CHECK(disk->SectorSize() == sector);

        const uint32_t counts[] = {1, 2, 3, 7, 8, 9, 33, 64};
        uint64_t at = 5;
        uint32_t seed = 1;
        for (const uint32_t count : counts)
        {
            const auto data = Pattern(seed++, static_cast<size_t>(count) * sector);
            REQUIRE(disk->Write(at, data));
            std::vector<uint8_t> back(data.size());
            REQUIRE(disk->Read(at, back));
            CHECK(back == data);
            std::vector<uint8_t> medium(data.size());
            REQUIRE(m.fake.NamespaceDisk(1).Read(at, medium));
            CHECK(medium == data);
            at += count + 3;
        }
        CHECK(m.fake.Faults().empty());
        disk->Release();
    }
}

TEST_CASE("nvme splits a megabyte into eight commands in LBA order", "[nvme][io]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, 8192);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto data = Pattern(9, 1024 * 1024);
    REQUIRE(disk->Write(100, data));
    CHECK(m.fake.Count(Event::Write) == 8);
    uint64_t expected = 100;
    for (const FakeNvme::IoRecord& record : m.fake.IoLog())
    {
        REQUIRE(record.kind == Event::Write);
        CHECK(record.lba == expected);
        CHECK(record.blocks == 256);
        expected += record.blocks;
    }
    std::vector<uint8_t> back(data.size());
    REQUIRE(disk->Read(100, back));
    CHECK(back == data);
    CHECK(m.fake.Count(Event::Read) == 8);
    CHECK(m.fake.Faults().empty());
    disk->Release();
}

TEST_CASE("nvme limits a command to the controller's MDTS", "[nvme][io]")
{
    const std::pair<uint32_t, uint32_t> cases[] = {{1, 8 * 1024}, {2, 16 * 1024}, {3, 32 * 1024},
                                                   {5, 128 * 1024}, {7, 128 * 1024}, {0, 128 * 1024}};
    for (const auto& [mdts, limit] : cases)
    {
        Machine m;
        m.fake.options.mdts = mdts;
        m.fake.AddNamespace(1, 512, 2048);
        REQUIRE(m.Start());
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        CHECK(disk->Info().max_transfer_bytes == limit);
        const auto data = Pattern(mdts, 200 * 512);
        REQUIRE(disk->Write(1, data));
        std::vector<uint8_t> back(data.size());
        REQUIRE(disk->Read(1, back));
        CHECK(back == data);
        CHECK(m.fake.Count(Event::Write) == (data.size() + limit - 1) / limit);
        CHECK(m.fake.Faults().empty());
        disk->Release();
    }
}

TEST_CASE("nvme wraps its queues", "[nvme][io]")
{
    Machine m;
    m.fake.options.mqes = 3;
    m.fake.AddNamespace(1, 512, 512);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    for (uint32_t i = 0; i < 200; i++)
    {
        const auto data = Pattern(i, 512);
        REQUIRE(disk->Write(i % 512, data));
        std::vector<uint8_t> back(512);
        REQUIRE(disk->Read(i % 512, back));
        REQUIRE(back == data);
    }
    CHECK(m.fake.IoCommands() == 400);
    CHECK(m.fake.Faults().empty());
    disk->Release();
}

TEST_CASE("nvme drops a stale completion and finishes the right command", "[nvme][io]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, 512);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto data = Pattern(4, 512 * 8);
    REQUIRE(disk->Write(0, data));

    m.fake.InjectStale(0x7777);
    std::vector<uint8_t> back(data.size());
    REQUIRE(disk->Read(0, back));
    CHECK(back == data);
    CHECK(m.controller->StaleCount() == 1);
    CHECK(LogLines(m.Line("stale completion cid 30583")) == 1);

    m.fake.InjectStale(0x1234);
    m.fake.InjectStale(0x1235);
    REQUIRE(disk->Write(8, data));
    CHECK(m.controller->StaleCount() == 3);
    CHECK(LogLines(m.Line("stale completion")) == 1);
    for (uint32_t i = 0; i < 40; i++)
    {
        REQUIRE(disk->Read(0, back));
    }
    CHECK(back == data);
    CHECK(m.fake.Faults().empty());
    disk->Release();
}

TEST_CASE("nvme refuses a read past the namespace", "[nvme][io]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, 64);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    std::vector<uint8_t> back(512 * 8);
    auto& driver = disk->Driver();
    CHECK(driver.Read(60, 8, back.data()).error() == Error::InvalidArgument);
    CHECK(driver.Read(64, 1, back.data()).error() == Error::InvalidArgument);
    CHECK(driver.Read(56, 8, back.data()));
    disk->Release();
}

TEST_CASE("nvme keeps unflushed writes out of the durable medium", "[nvme][durability]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, 256);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto first = Pattern(1, 512 * 4);
    const auto second = Pattern(2, 512 * 4);
    REQUIRE(disk->Write(0, first));
    REQUIRE(disk->Flush());
    REQUIRE(disk->Write(16, second));
    CHECK(m.fake.Count(Event::Flush) == 1);

    m.fake.NamespaceDisk(1).PowerLoss();
    CHECK(m.fake.NamespaceDisk(1).Durable(0, 4) == first);
    CHECK(m.fake.NamespaceDisk(1).Durable(16, 4) == std::vector<uint8_t>(512 * 4, 0));
    disk->Release();
}

TEST_CASE("nvme sends no Flush without a volatile write cache", "[nvme][durability]")
{
    Machine m;
    m.fake.options.volatile_cache = false;
    m.fake.AddNamespace(1, 512, 256);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto data = Pattern(6, 512 * 4);
    REQUIRE(disk->Write(0, data));
    REQUIRE(disk->Flush());
    REQUIRE(disk->Flush());
    CHECK(m.fake.Count(Event::Flush) == 0);
    m.fake.NamespaceDisk(1).PowerLoss();
    CHECK(m.fake.NamespaceDisk(1).Durable(0, 4) == data);
    disk->Release();
}

TEST_CASE("nvme shuts down after the last flush", "[nvme][shutdown]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, 256);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto data = Pattern(8, 512 * 2);
    REQUIRE(disk->Write(0, data));
    REQUIRE(disk->Flush());
    m.controller->Shutdown();

    const auto& events = m.fake.Events();
    CHECK(events.back() == Event::ShutdownRequested);
    CHECK(events[events.size() - 2] == Event::Flush);
    CHECK(m.fake.Count(Event::ShutdownRequested) == 1);
    CHECK(LogLines(m.Line("shutdown complete")) == 1);

    const uint32_t commands = m.fake.IoCommands();
    std::vector<uint8_t> back(data.size());
    CHECK(disk->Read(0, back).error() == Error::Io);
    CHECK(disk->Write(0, data).error() == Error::Io);
    CHECK(disk->Flush().error() == Error::Io);
    CHECK(m.fake.IoCommands() == commands);

    m.controller->Shutdown();
    CHECK(m.fake.Count(Event::ShutdownRequested) == 1);
    disk->Release();
}

TEST_CASE("nvme waits for SHST and warns when it never comes", "[nvme][shutdown]")
{
    SECTION("a slow shutdown still completes")
    {
        Machine m;
        m.fake.options.shutdown_delay_reads = 40;
        m.fake.AddNamespace(1, 512, 64);
        REQUIRE(m.Start());
        m.controller->Shutdown();
        CHECK(LogLines(m.Line("shutdown complete")) == 1);
    }
    SECTION("the budget is 5 s")
    {
        Machine m;
        m.fake.options.never_shutdown = true;
        m.fake.AddNamespace(1, 512, 64);
        REQUIRE(m.Start());
        const uint64_t before = m.env.NowMilliseconds();
        m.controller->Shutdown();
        CHECK(m.env.NowMilliseconds() - before >= 5000);
        CHECK(m.env.NowMilliseconds() - before < 5200);
        CHECK(LogLines(m.Line("shutdown did not complete in 5000 ms")) == 1);
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        std::vector<uint8_t> back(512);
        CHECK(disk->Read(0, back).error() == Error::Io);
        disk->Release();
    }
    SECTION("RTD3E stretches the budget, up to 60 s")
    {
        Machine m;
        m.fake.options.never_shutdown = true;
        m.fake.options.rtd3_entry_latency_us = 20'000'000;
        m.fake.AddNamespace(1, 512, 64);
        REQUIRE(m.Start());
        const uint64_t before = m.env.NowMilliseconds();
        m.controller->Shutdown();
        CHECK(m.env.NowMilliseconds() - before >= 20000);
        CHECK(m.env.NowMilliseconds() - before < 20200);
    }
    SECTION("the cap is 60 s")
    {
        Machine m;
        m.fake.options.never_shutdown = true;
        m.fake.options.rtd3_entry_latency_us = 200'000'000;
        m.fake.AddNamespace(1, 512, 64);
        REQUIRE(m.Start());
        const uint64_t before = m.env.NowMilliseconds();
        m.controller->Shutdown();
        CHECK(m.env.NowMilliseconds() - before >= 60000);
        CHECK(m.env.NowMilliseconds() - before < 60200);
    }
}

TEST_CASE("nvme sends no write or flush to a read-only disk", "[nvme][policy]")
{
    struct Case
    {
        S::WritePolicy policy;
        bool read_only;
    };
    const Case cases[] = {{S::WritePolicy::Allowed, false},
                          {S::WritePolicy::AllowedByOption, false},
                          {S::WritePolicy::ReadOnlyHardware, true},
                          {S::WritePolicy::ReadOnlyOption, true}};
    for (const Case& c : cases)
    {
        Machine m;
        m.fake.AddNamespace(1, 512, 256);
        REQUIRE(m.Start(c.policy));
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        const S::DiskInfo info = disk->Info();
        CHECK(info.read_only == c.read_only);
        CHECK(info.read_only_reason ==
              (c.read_only ? S::ReadOnlyReason::Policy : S::ReadOnlyReason::None));

        const auto data = Pattern(5, 512 * 2);
        std::vector<uint8_t> back(data.size());
        CHECK(disk->Read(0, back));
        if (c.read_only)
        {
            CHECK(disk->Write(0, data).error() == Error::ReadOnly);
            // Straight to the driver, as a caller that skipped the registry would.
            CHECK(disk->Driver().Write(0, 2, data.data()).error() == Error::ReadOnly);
            CHECK(disk->Driver().Flush());
            CHECK(disk->Flush());
            CHECK(m.fake.Count(Event::Write) == 0);
            CHECK(m.fake.Count(Event::Flush) == 0);
        }
        else
        {
            CHECK(disk->Write(0, data));
            CHECK(disk->Flush());
            CHECK(m.fake.Count(Event::Write) == 1);
            CHECK(m.fake.Count(Event::Flush) == 1);
        }
        CHECK(m.fake.Count(Event::Read) == 1);
        disk->Release();
    }
}

TEST_CASE("nvme logs why a controller is read-only", "[nvme][policy]")
{
    {
        Machine m;
        m.fake.AddNamespace(1, 512, 64);
        REQUIRE(m.Start(S::WritePolicy::ReadOnlyHardware));
        CHECK(LogLines(m.Line("real hardware, read-only (boot with storage=rw to write)")) == 1);
        CHECK(LogLines("Storage: " + m.DiskName(1) + ": 32 KiB, 512-byte sectors, ro") == 1);
    }
    {
        Machine m;
        m.fake.AddNamespace(1, 512, 64);
        REQUIRE(m.Start(S::WritePolicy::ReadOnlyOption));
        CHECK(LogLines(m.Line("storage=ro, read-only")) == 1);
    }
    {
        Machine m;
        m.fake.AddNamespace(1, 512, 64);
        REQUIRE(m.Start(S::WritePolicy::AllowedByOption));
        CHECK(LogLines(m.Line("storage=rw, writes allowed on real hardware")) == 1);
    }
}

TEST_CASE("nvme marks a write-protected namespace read-only by the medium", "[nvme][policy]")
{
    Machine m;
    FakeNvme::NamespaceFlags flags;
    flags.write_protected = true;
    m.fake.AddNamespace(1, 512, 64, flags);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    CHECK(disk->Info().read_only);
    CHECK(disk->Info().read_only_reason == S::ReadOnlyReason::Medium);
    const auto data = Pattern(1, 512);
    CHECK(disk->Driver().Write(0, 1, data.data()).error() == Error::ReadOnly);
    CHECK(m.fake.Count(Event::Write) == 0);
    disk->Release();
}

TEST_CASE("nvme registers the active namespaces of a 1.4 controller", "[nvme][namespaces]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, 64);
    m.fake.AddNamespace(2, 4096, 32);
    m.fake.AddNamespace(5, 512, 128);
    REQUIRE(m.Start());
    CHECK(m.controller->NamespaceCount() == 3);
    CHECK(scanned == (std::vector<std::string>{m.DiskName(1), m.DiskName(2), m.DiskName(5)}));
    for (const uint32_t nsid : {1U, 2U, 5U})
    {
        S::Disk* disk = m.Open(nsid);
        REQUIRE(disk != nullptr);
        CHECK(disk->Info().namespace_id == nsid);
        disk->Release();
    }
    CHECK(m.Open(3) == nullptr);
    S::Disk* four_k = m.Open(2);
    REQUIRE(four_k != nullptr);
    CHECK(four_k->Info().sector_size == 4096);
    CHECK(four_k->Info().sector_count == 32);
    four_k->Release();
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("nvme 1.0 tries the namespaces up to NN", "[nvme][namespaces]")
{
    Machine m;
    m.fake.options.version = 0x00010000;
    m.fake.AddNamespace(1, 512, 64);
    m.fake.AddNamespace(2, 512, 64);
    m.fake.AddNamespace(4, 512, 64);
    REQUIRE(m.Start());
    CHECK(m.controller->NamespaceCount() == 3);
    CHECK(scanned == (std::vector<std::string>{m.DiskName(1), m.DiskName(2), m.DiskName(4)}));
    CHECK(LogLines(m.Line("namespace 3")) == 0);
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("nvme skips namespaces it cannot use", "[nvme][namespaces]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, 64);
    FakeNvme::NamespaceFlags metadata;
    metadata.metadata_bytes = 8;
    m.fake.AddNamespace(2, 512, 64, metadata);
    FakeNvme::NamespaceFlags protection;
    protection.protection = 1;
    m.fake.AddNamespace(3, 512, 64, protection);
    m.fake.AddNamespace(4, 512, 64);
    REQUIRE(m.Start());
    CHECK(m.controller->NamespaceCount() == 2);
    CHECK(scanned == (std::vector<std::string>{m.DiskName(1), m.DiskName(4)}));
    CHECK(m.Open(2) == nullptr);
    CHECK(LogLines(m.Line("namespace 2: LBA format with metadata not supported")) == 1);
    CHECK(LogLines(m.Line("namespace 3: protection information not supported")) == 1);
}

TEST_CASE("nvme registers at most eight namespaces", "[nvme][namespaces]")
{
    Machine m;
    for (uint32_t nsid = 1; nsid <= 10; nsid++)
    {
        m.fake.AddNamespace(nsid, 512, 16);
    }
    REQUIRE(m.Start());
    CHECK(m.controller->NamespaceCount() == 8);
    CHECK(m.Open(9) == nullptr);
    S::Disk* last = m.Open(8);
    REQUIRE(last != nullptr);
    last->Release();
}

TEST_CASE("nvme makes progress with MSI-X, MSI, polling and lost interrupts", "[nvme][interrupts]")
{
    const auto exercise = [](Machine& m)
    {
        m.fake.AddNamespace(1, 512, 64);
        REQUIRE(m.Start());
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        const auto data = Pattern(2, 512 * 4);
        REQUIRE(disk->Write(0, data));
        std::vector<uint8_t> back(data.size());
        REQUIRE(disk->Read(0, back));
        CHECK(back == data);
        CHECK(m.fake.Faults().empty());
        disk->Release();
    };

    SECTION("MSI-X wakes the waiter")
    {
        Machine m;
        exercise(m);
        CHECK(m.fake.Interrupts() > 0);
        CHECK(m.env.Wakes() == m.fake.Interrupts());
        CHECK(LogLines(m.Line("00:04.0 \"Fake NVMe Controller\" fw 1.0, MSI-X")) == 1);
    }
    SECTION("MSI wakes the waiter")
    {
        Machine m;
        m.fake.options.interrupts = S::InterruptKind::Msi;
        exercise(m);
        CHECK(m.env.Wakes() > 0);
        CHECK(LogLines(m.Line("00:04.0 \"Fake NVMe Controller\" fw 1.0, MSI,")) == 1);
    }
    SECTION("polling never wakes, and the completion is still found")
    {
        Machine m;
        m.fake.options.interrupts = S::InterruptKind::Polling;
        m.fake.options.defer_processing = true;
        exercise(m);
        CHECK(m.fake.Interrupts() == 0);
        CHECK(m.env.Wakes() == 0);
        CHECK(LogLines(m.Line("00:04.0 \"Fake NVMe Controller\" fw 1.0, polling,")) == 1);
    }
    SECTION("a lost interrupt costs a tick, not the command")
    {
        Machine m;
        m.fake.options.drop_interrupts = true;
        m.fake.options.defer_processing = true;
        exercise(m);
        CHECK(m.env.Wakes() == 0);
    }
}

TEST_CASE("nvme queue pair tracks phase, wraps and rings the head doorbell", "[nvme][queue]")
{
    class Recorder final : public S::ControllerHardware
    {
    public:
        uint32_t Read32(uint32_t) override { return 0; }
        void Write32(uint32_t offset, uint32_t value) override { writes.emplace_back(offset, value); }
        uint64_t Read64(uint32_t) override { return 0; }
        void Write64(uint32_t, uint64_t) override {}
        uint32_t ReadConfig32(uint16_t) override { return 0; }
        bool AllocateDma(uint64_t, uint64_t, FeatherOS::Memory::Dma::Buffer*) override { return false; }
        void FreeDma(const FeatherOS::Memory::Dma::Buffer&) override {}
        S::InterruptKind SetUpInterrupts(void (*)(void*), void*) override
        {
            return S::InterruptKind::Polling;
        }
        std::vector<std::pair<uint32_t, uint32_t>> writes;
    };

    FeatherOS::Testing::PhysicalMemory memory;
    Recorder hardware;
    const FeatherOS::Memory::Dma::Buffer sq = memory.Allocate(4096);
    const FeatherOS::Memory::Dma::Buffer cq = memory.Allocate(4096);
    constexpr uint32_t Entries = 8;
    N::QueuePair queue;
    queue.Initialize(hardware, 1, Entries, 4, sq, cq);

    uint32_t phase = 1;
    uint32_t slot = 0;
    const auto complete = [&](uint16_t cid, uint16_t sq_head)
    {
        auto* entry = reinterpret_cast<volatile uint32_t*>(cq.address) + slot * 4;
        entry[2] = sq_head;
        entry[3] = cid | (phase << 16);
        if (++slot == Entries)
        {
            slot = 0;
            phase ^= 1;
        }
    };

    N::Sqe command{};
    command.cdw0 = 0x02;
    uint16_t submitted = 0;
    for (uint32_t round = 0; round < 70000; round++)
    {
        const uint16_t cid = queue.Submit(command);
        REQUIRE(cid != N::NoCid);
        REQUIRE(cid == submitted);
        submitted = static_cast<uint16_t>(submitted + 1);
        if (submitted == N::NoCid)
        {
            submitted = 0;
        }
        const auto* stored = reinterpret_cast<volatile const uint32_t*>(sq.address) +
                             ((round % Entries) * (sizeof(N::Sqe) / 4));
        REQUIRE((stored[0] >> 16) == cid);

        if (round % 5 == 0)
        {
            complete(static_cast<uint16_t>(cid ^ 0x5555), static_cast<uint16_t>((round + 1) % Entries));
        }
        complete(cid, static_cast<uint16_t>((round + 1) % Entries));
        N::Cqe cqe{};
        N::StaleCompletions stale{};
        REQUIRE(queue.Reap(cid, &cqe, &stale));
        REQUIRE(cqe.cid == cid);
        REQUIRE(stale.count == (round % 5 == 0 ? 1U : 0U));
        N::StaleCompletions again{};
        REQUIRE_FALSE(queue.Reap(cid, &cqe, &again));
        REQUIRE(again.count == 0);
    }
    // The last head doorbell names the next slot to read.
    CHECK(hardware.writes.back().first == N::CqHeadDoorbell(1, 4));
    CHECK(hardware.writes.back().second == slot);
}

TEST_CASE("nvme queue pair refuses a submission when the ring is full", "[nvme][queue]")
{
    class Quiet final : public S::ControllerHardware
    {
    public:
        uint32_t Read32(uint32_t) override { return 0; }
        void Write32(uint32_t, uint32_t) override {}
        uint64_t Read64(uint32_t) override { return 0; }
        void Write64(uint32_t, uint64_t) override {}
        uint32_t ReadConfig32(uint16_t) override { return 0; }
        bool AllocateDma(uint64_t, uint64_t, FeatherOS::Memory::Dma::Buffer*) override { return false; }
        void FreeDma(const FeatherOS::Memory::Dma::Buffer&) override {}
        S::InterruptKind SetUpInterrupts(void (*)(void*), void*) override
        {
            return S::InterruptKind::Polling;
        }
    };

    FeatherOS::Testing::PhysicalMemory memory;
    Quiet hardware;
    N::QueuePair queue;
    queue.Initialize(hardware, 0, 4, 4, memory.Allocate(4096), memory.Allocate(4096));
    N::Sqe command{};
    command.cdw0 = 0x06;
    CHECK(queue.Submit(command) == 0);
    CHECK(queue.Submit(command) == 1);
    CHECK(queue.Submit(command) == 2);
    CHECK(queue.Submit(command) == N::NoCid);
}
