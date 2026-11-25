#include <catch2/catch_test_macros.hpp>

#include "ahci_machine.h"
#include "fake_ahci.h"

#include "kernel/power.h"
#include "kernel/storage/ahci/registers.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace AhciTest;

namespace
{

constexpr uint64_t FourGiB = 1ULL << 32;

size_t Position(const FakeAhci& fake, Event event, size_t from = 0)
{
    const auto& log = fake.Log();
    for (size_t i = from; i < log.size(); i++)
    {
        if (log[i].event == event)
        {
            return i;
        }
    }
    return log.size();
}

} // namespace

TEST_CASE("ahci T19: sectors of both sizes read back what was written", "[ahci][io]")
{
    for (const uint32_t sector : {512U, 4096U})
    {
        Machine m;
        FakeAhci::PortOptions options;
        options.sector_size = sector;
        options.sectors = 4 * 1024 * 1024 / sector;
        m.fake.AddPort(0, options);
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
            REQUIRE(m.fake.PortDisk(0).Read(at, medium));
            CHECK(medium == data);
            at += count + 3;
        }
        CHECK(m.fake.Faults().empty());
        disk->Release();
    }
}

TEST_CASE("ahci T19: a megabyte is eight commands in LBA order", "[ahci][io]")
{
    for (const uint32_t sector : {512U, 4096U})
    {
        Machine m;
        FakeAhci::PortOptions options;
        options.sector_size = sector;
        options.sectors = 2 * 1024 * 1024 / sector;
        m.fake.AddPort(0, options);
        REQUIRE(m.Start());
        const auto data = Pattern(9, 1024 * 1024);
        A::Port& port = *m.hba->DiskPort(0);
        REQUIRE(port.Write(100, 1024 * 1024 / sector, data.data()));
        CHECK(m.fake.Count(Event::Write) == 8);
        CHECK(port.write_commands() == 8);
        uint64_t expected = 100;
        for (const FakeAhci::Record& record : Of(m.fake, {Event::Write}))
        {
            CHECK(record.lba == expected);
            CHECK(record.sectors == 128 * 1024 / sector);
            expected += record.sectors;
        }
        std::vector<uint8_t> back(data.size());
        REQUIRE(port.Read(100, 1024 * 1024 / sector, back.data()));
        CHECK(back == data);
        CHECK(m.fake.Count(Event::Read) == 8);
        CHECK(m.fake.Faults().empty());
    }
}

TEST_CASE("ahci T19: a read past the disk is refused before any command", "[ahci][io]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    std::vector<uint8_t> buffer(512 * 4);
    const uint32_t reads = m.fake.Count(Event::Read);
    CHECK(port.Read(2046, 4, buffer.data()).error() == Error::InvalidArgument);
    CHECK(port.Read(2049, 1, buffer.data()).error() == Error::InvalidArgument);
    CHECK(port.Read(2044, 4, buffer.data()));
    CHECK(m.fake.Count(Event::Read) == reads + 1);
}

TEST_CASE("ahci T19: a short PRDBC fails the read and leaves the port alive", "[ahci][io]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    std::vector<uint8_t> data = Pattern(4, 512 * 8);
    REQUIRE(port.Write(0, 8, data.data()));

    m.fake.Inject(0, FakeAhci::Failure::ShortRead);
    std::vector<uint8_t> back(data.size());
    CHECK(port.Read(0, 8, back.data()).error() == Error::Io);
    CHECK_FALSE(port.Dead());
    CHECK(m.Lines("READ DMA EXT moved 2048 of 4096 bytes") == 1);
    CHECK(removed.empty());

    REQUIRE(port.Read(0, 8, back.data()));
    CHECK(back == data);
}

TEST_CASE("ahci T19: FLUSH CACHE EXT, and FLUSH CACHE when only that exists", "[ahci][io]")
{
    SECTION("extended")
    {
        Machine m;
        m.fake.AddPort(0);
        REQUIRE(m.Start());
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        const auto first = Pattern(1, 512 * 4);
        const auto second = Pattern(2, 512 * 4);
        REQUIRE(disk->Write(0, first));
        REQUIRE(disk->Flush());
        REQUIRE(disk->Write(16, second));
        CHECK(m.fake.Count(Event::FlushExt) == 1);
        CHECK(m.fake.Count(Event::Flush) == 0);
        CHECK(m.hba->DiskPort(0)->flush_commands() == 1);

        m.fake.PortDisk(0).PowerLoss();
        CHECK(m.fake.PortDisk(0).Durable(0, 4) == first);
        CHECK(m.fake.PortDisk(0).Durable(16, 4) == std::vector<uint8_t>(512 * 4, 0));
        disk->Release();
    }
    SECTION("plain")
    {
        Machine m;
        FakeAhci::PortOptions options;
        options.flush_ext = false;
        m.fake.AddPort(0, options);
        REQUIRE(m.Start());
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        const auto data = Pattern(3, 512 * 2);
        REQUIRE(disk->Write(0, data));
        REQUIRE(disk->Flush());
        CHECK(m.fake.Count(Event::FlushExt) == 0);
        CHECK(m.fake.Count(Event::Flush) == 1);
        m.fake.PortDisk(0).PowerLoss();
        CHECK(m.fake.PortDisk(0).Durable(0, 2) == data);
        CHECK(m.fake.Faults().empty());
        disk->Release();
    }
}

TEST_CASE("ahci T19: no flush command without an enabled write cache", "[ahci][io]")
{
    Machine m;
    FakeAhci::PortOptions options;
    options.write_cache_enabled = false;
    m.fake.AddPort(0, options);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto data = Pattern(6, 512 * 4);
    REQUIRE(disk->Write(0, data));
    REQUIRE(disk->Flush());
    REQUIRE(disk->Flush());
    CHECK(m.fake.Count(Event::FlushExt) == 0);
    CHECK(m.fake.Count(Event::Flush) == 0);
    CHECK(m.hba->DiskPort(0)->flush_commands() == 0);
    CHECK_FALSE(m.hba->DiskPort(0)->write_cache());
    m.fake.PortDisk(0).PowerLoss();
    CHECK(m.fake.PortDisk(0).Durable(0, 4) == data);
    CHECK(m.Lines("no write cache, rw") == 1);
    disk->Release();
}

TEST_CASE("ahci T19: a failed command is recovered and the port keeps its disk", "[ahci][io]")
{
    Machine m;
    m.fake.AddPort(0);
    m.fake.AddPort(1);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    A::Port& port = *m.hba->DiskPort(0);

    m.fake.Inject(0, FakeAhci::Failure::TaskFile);
    std::vector<uint8_t> buffer(512);
    CHECK(disk->Read(0, buffer).error() == Error::Io);
    CHECK_FALSE(port.Dead());
    CHECK_FALSE(port.Gone());
    CHECK(removed.empty());
    CHECK(m.Lines("Storage: sda: READ DMA EXT failed (status 51h error 40h); port recovered") ==
          1);
    CHECK(m.Lines("port failed; its disk is gone") == 0);
    CHECK(m.fake.EngineRunning(0));
    CHECK(disk->Read(0, buffer));
    CHECK(m.fake.Faults().empty());
    disk->Release();
}

TEST_CASE("ahci T19: a command that never completes fails after 30 s", "[ahci][io]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    m.fake.Inject(0, FakeAhci::Failure::Hang);
    const uint64_t before = m.Elapsed();
    std::vector<uint8_t> buffer(512);
    CHECK(port.Read(0, 1, buffer.data()).error() == Error::Io);
    const uint64_t took = m.Elapsed() - before;
    CHECK(took >= 30000);
    CHECK(took < 31000);
    CHECK_FALSE(port.Dead());
    CHECK(removed.empty());
    CHECK(m.fake.EngineRunning(0));
    CHECK(m.Lines("READ DMA EXT timed out (status 80h error 00h); port recovered") == 1);
    CHECK(port.Read(0, 1, buffer.data()));
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("ahci T19: a flush that never completes fails after 60 s", "[ahci][io]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    A::Port& port = *m.hba->DiskPort(0);
    m.fake.Inject(0, FakeAhci::Failure::Hang);
    const uint64_t before = m.Elapsed();
    CHECK(port.Flush().error() == Error::Io);
    const uint64_t took = m.Elapsed() - before;
    CHECK(took >= 60000);
    CHECK(took < 61000);
    CHECK_FALSE(port.Dead());
    CHECK(port.Flush());
}

TEST_CASE("ahci T21: with S64A clear every DMA address is below 4 GiB", "[ahci][dma]")
{
    Machine m;
    m.fake.options.s64a = false;
    m.fake.AddPort(0);
    m.fake.AddPort(1);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto data = Pattern(7, 512 * 40);
    REQUIRE(disk->Write(3, data));
    std::vector<uint8_t> back(data.size());
    REQUIRE(disk->Read(3, back));
    CHECK(back == data);
    CHECK(m.fake.Faults().empty());
    REQUIRE(m.fake.Limits().size() == 4);
    for (const uint64_t limit : m.fake.Limits())
    {
        CHECK(limit <= FourGiB);
    }
    disk->Release();
}

TEST_CASE("ahci T21: with S64A set there is no address limit and the upper halves are used",
          "[ahci][dma]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    REQUIRE(m.fake.Limits().size() == 2);
    for (const uint64_t limit : m.fake.Limits())
    {
        CHECK(limit == UINT64_MAX);
    }
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto data = Pattern(8, 512 * 4);
    REQUIRE(disk->Write(0, data));
    std::vector<uint8_t> back(data.size());
    REQUIRE(disk->Read(0, back));
    CHECK(back == data);
    CHECK(m.fake.Faults().empty());
    disk->Release();
}

TEST_CASE("ahci T22: no write or flush command reaches a read-only disk", "[ahci][policy]")
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
        m.fake.AddPort(0);
        REQUIRE(m.Start(c.policy));
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        A::Port& port = *m.hba->DiskPort(0);
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
            CHECK(port.Write(0, 2, data.data()).error() == Error::ReadOnly);
            CHECK(port.Flush());
            CHECK(disk->Flush());
            CHECK(m.fake.Count(Event::Write) == 0);
            CHECK(m.fake.Count(Event::FlushExt) == 0);
            CHECK(m.fake.Count(Event::Flush) == 0);
            CHECK(port.write_commands() == 0);
            CHECK(port.flush_commands() == 0);
            CHECK(m.Lines("2, 1024 KiB") == 0);
            CHECK(m.Lines("write cache, ro") == 1);
        }
        else
        {
            CHECK(disk->Write(0, data));
            CHECK(disk->Flush());
            CHECK(m.fake.Count(Event::Write) == 1);
            CHECK(m.fake.Count(Event::FlushExt) == 1);
        }
        CHECK(m.fake.Count(Event::Read) == 1);
        disk->Release();
    }
}

TEST_CASE("ahci T22: the log says why an HBA is read-only", "[ahci][policy]")
{
    {
        Machine m;
        m.fake.AddPort(0);
        REQUIRE(m.Start(S::WritePolicy::ReadOnlyHardware));
        CHECK(m.Lines(m.Line("real hardware, read-only (boot with storage=rw to write)")) == 1);
    }
    {
        Machine m;
        m.fake.AddPort(0);
        REQUIRE(m.Start(S::WritePolicy::ReadOnlyOption));
        CHECK(m.Lines(m.Line("storage=ro, read-only")) == 1);
    }
    {
        Machine m;
        m.fake.AddPort(0);
        REQUIRE(m.Start(S::WritePolicy::AllowedByOption));
        CHECK(m.Lines(m.Line("storage=rw, writes allowed on real hardware")) == 1);
    }
}

TEST_CASE("ahci T22: STANDBY IMMEDIATE only at power-off, after the last flush", "[ahci][shutdown]")
{
    SECTION("power-off")
    {
        Machine m;
        m.fake.AddPort(0);
        m.fake.AddPort(1);
        REQUIRE(m.Start());
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        REQUIRE(disk->Write(0, Pattern(8, 512 * 2)));
        REQUIRE(S::FlushAll());
        m.hba->Shutdown(FeatherOS::Power::Action::PowerOff);

        CHECK(m.fake.Count(Event::Standby) == 2);
        CHECK(m.fake.Count(Event::Standby, 0) == 1);
        CHECK(m.fake.Count(Event::Standby, 1) == 1);
        CHECK(Position(m.fake, Event::FlushExt) < Position(m.fake, Event::Standby));
        CHECK(m.hba->DiskPort(0)->standby_commands() == 1);

        const size_t log = m.fake.Log().size();
        std::vector<uint8_t> buffer(1024);
        CHECK(disk->Read(0, buffer).error() == Error::Io);
        CHECK(disk->Write(0, buffer).error() == Error::Io);
        CHECK(disk->Flush().error() == Error::Io);
        CHECK(m.fake.Log().size() == log);

        m.hba->Shutdown(FeatherOS::Power::Action::PowerOff);
        CHECK(m.fake.Count(Event::Standby) == 2);
        disk->Release();
    }
    SECTION("restart")
    {
        Machine m;
        m.fake.AddPort(0);
        REQUIRE(m.Start());
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        m.hba->Shutdown(FeatherOS::Power::Action::Restart);
        CHECK(m.fake.Count(Event::Standby) == 0);
        std::vector<uint8_t> buffer(512);
        CHECK(disk->Read(0, buffer).error() == Error::Io);
        m.hba->Shutdown(FeatherOS::Power::Action::PowerOff);
        CHECK(m.fake.Count(Event::Standby) == 0);
        disk->Release();
    }
    SECTION("a read-only disk is parked too")
    {
        Machine m;
        m.fake.AddPort(0);
        REQUIRE(m.Start(S::WritePolicy::ReadOnlyHardware));
        m.hba->Shutdown(FeatherOS::Power::Action::PowerOff);
        CHECK(m.fake.Count(Event::Standby) == 1);
        CHECK(m.fake.Count(Event::Write) == 0);
        CHECK(m.fake.Count(Event::FlushExt) == 0);
    }
    SECTION("a dead port is skipped")
    {
        Machine m;
        m.fake.AddPort(0);
        m.fake.AddPort(1);
        REQUIRE(m.Start());
        m.fake.RaisePhyChange(0);
        std::vector<uint8_t> buffer(512);
        CHECK(m.hba->DiskPort(0)->Read(0, 1, buffer.data()).error() == Error::Io);
        CHECK(m.hba->DiskPort(0)->Dead());
        m.hba->Shutdown(FeatherOS::Power::Action::PowerOff);
        CHECK(m.fake.Count(Event::Standby, 0) == 0);
        CHECK(m.fake.Count(Event::Standby, 1) == 1);
    }
    SECTION("before Start there is nothing to do")
    {
        Machine m;
        m.fake.AddPort(0);
        m.hba.reset(A::Hba::Create(m.fake, m.env, m.index, S::WritePolicy::Allowed, "00:05.0"));
        m.hba->Shutdown(FeatherOS::Power::Action::PowerOff);
        CHECK(m.fake.Count(Event::Standby) == 0);
    }
}
