#include <catch2/catch_test_macros.hpp>

#include "ahci_machine.h"
#include "fake_ahci.h"

#include "kernel/storage/ahci/registers.h"
#include "kernel/storage/ata/identify.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace AhciTest;

namespace
{

uint32_t PortReg(uint32_t port, uint32_t offset)
{
    return A::PortRegister(port, offset);
}

} // namespace

TEST_CASE("ahci bring-up registers one disk", "[ahci][hba]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    CHECK(m.fake.Faults().empty());
    CHECK(m.hba->DiskCount() == 1);
    REQUIRE(m.hba->DiskPort(0) != nullptr);
    CHECK(m.hba->DiskPort(1) == nullptr);
    CHECK(scanned == std::vector<std::string>{"sda"});

    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const S::DiskInfo info = disk->Info();
    CHECK(info.kind == S::DiskKind::Ahci);
    CHECK(info.sector_size == 512);
    CHECK(info.sector_count == 2048);
    CHECK(info.max_transfer_bytes == 128 * 1024);
    CHECK_FALSE(info.removable);
    CHECK_FALSE(info.read_only);
    CHECK(info.read_only_reason == S::ReadOnlyReason::None);
    CHECK(std::string(info.model) == "FAKE SATA DISK");
    CHECK(std::string(info.location) == "00:05.0");
    CHECK(m.Lines(m.Line("QEMU controller, writes allowed")) == 1);
    CHECK(m.Lines(m.Line("00:05.0 1 ports, 32 command slots, MSI-X, 64-bit DMA")) == 1);
    CHECK(m.Lines("Storage: sda: AHCI 00:05.0 port 0, \"FAKE SATA DISK\" \"FEATHER0001\", "
                   "1024 KiB, 512-byte sectors, write cache, rw") == 1);
    disk->Release();
}

TEST_CASE("ahci takes the controller from the firmware", "[ahci][hba]")
{
    SECTION("a release after a few reads, then BB clears")
    {
        Machine m;
        m.fake.options.boh = true;
        m.fake.options.firmware_owns = true;
        m.fake.options.bios_release_reads = 1;
        m.fake.options.bios_busy_reads = 4;
        m.fake.AddPort(0);
        REQUIRE(m.Start());
        CHECK(m.fake.Count(Event::HandoffRequested) == 1);
        CHECK(m.fake.Faults().empty());
        CHECK(m.hba->DiskCount() == 1);
        CHECK(m.Lines(m.Line("firmware did not release the controller, continuing")) == 0);
        CHECK(m.Lines(m.Line("firmware stayed busy, continuing")) == 0);
    }
    SECTION("firmware that never lets go costs 25 ms and bring-up continues")
    {
        Machine m;
        m.fake.options.boh = true;
        m.fake.options.firmware_owns = true;
        m.fake.options.bios_never_releases = true;
        m.fake.AddPort(0);
        REQUIRE(m.Start());
        CHECK(m.Lines(m.Line("firmware did not release the controller, continuing")) == 1);
        CHECK(m.Elapsed() < 500);
        CHECK(m.hba->DiskCount() == 1);
    }
    SECTION("firmware that stays busy costs 2 s and bring-up continues")
    {
        Machine m;
        m.fake.options.boh = true;
        m.fake.options.firmware_owns = true;
        m.fake.options.bios_stays_busy = true;
        m.fake.AddPort(0);
        REQUIRE(m.Start());
        CHECK(m.Lines(m.Line("firmware stayed busy, continuing")) == 1);
        CHECK(m.Elapsed() >= 2000);
        CHECK(m.Elapsed() < 3000);
        CHECK(m.hba->DiskCount() == 1);
    }
    SECTION("no handoff without CAP2.BOH, and CAP2 is not read before AHCI 1.2")
    {
        {
            Machine m;
            m.fake.options.boh = true;
            m.fake.options.version = 0x00010100;
            m.fake.AddPort(0);
            REQUIRE(m.Start());
            CHECK(m.fake.Count(Event::HandoffRequested) == 0);
        }
        Machine plain;
        plain.fake.AddPort(0);
        REQUIRE(plain.Start());
        CHECK(plain.fake.Count(Event::HandoffRequested) == 0);
    }
}

TEST_CASE("ahci stops every port before the HBA reset", "[ahci][hba]")
{
    Machine m;
    FakeAhci::PortOptions running;
    running.engine_running = true;
    m.fake.options.engine_stop_delay_reads = 3;
    m.fake.AddPort(0, running);
    m.fake.AddPort(2, running);
    m.fake.AddPort(3);
    REQUIRE(m.Start());
    CHECK(m.fake.Faults().empty());

    const auto log = Of(m.fake, {Event::EngineStopped, Event::HbaReset});
    REQUIRE(log.size() >= 3);
    CHECK(log[0].event == Event::EngineStopped);
    CHECK(log[1].event == Event::EngineStopped);
    CHECK(log[2].event == Event::HbaReset);
    CHECK(m.hba->DiskCount() == 3);
    CHECK(m.fake.Resets() == 1);
}

TEST_CASE("the fake faults on PxCLB and PxFB written while the engine runs", "[ahci][hba]")
{
    FeatherOS::Testing::PhysicalMemory memory;
    FakeAhci fake(memory);
    fake.AddPort(0);
    fake.Write32(A::Reg::Ghc, A::GhcBit::AhciEnable);
    fake.Write32(PortReg(0, A::PortReg::Clb), 0x1000);
    CHECK(fake.Faults().empty());
    fake.Write32(PortReg(0, A::PortReg::Cmd), A::CmdBit::FisReceiveEnable);
    fake.Write32(PortReg(0, A::PortReg::Fb), 0x2000);
    REQUIRE(fake.Faults().size() == 1);
    fake.Write32(PortReg(0, A::PortReg::Clb), 0x3000);
    CHECK(fake.Faults().size() == 2);
    fake.Write32(A::Reg::Ghc, A::GhcBit::AhciEnable | A::GhcBit::Reset);
    CHECK(fake.Faults().size() == 3);
}

TEST_CASE("ahci programs the command list and FIS area while the port is stopped", "[ahci][hba]")
{
    Machine m;
    m.fake.options.engine_stop_delay_reads = 2;
    FakeAhci::PortOptions running;
    running.engine_running = true;
    m.fake.AddPort(0, running);
    REQUIRE(m.Start());
    CHECK(m.fake.Faults().empty());
    CHECK(m.fake.EngineRunning(0));
    CHECK(m.fake.ReceivingFis(0));
}

TEST_CASE("ahci refuses an HBA whose reset never completes", "[ahci][hba]")
{
    SECTION("never")
    {
        Machine m;
        m.fake.options.hr_never_clears = true;
        m.fake.AddPort(0);
        CHECK_FALSE(m.Start());
        CHECK(m.Lines(m.Line("controller reset did not complete, not supported")) == 1);
        CHECK(m.Elapsed() >= 1000);
        CHECK(m.Elapsed() < 1200);
        CHECK(m.hba->DiskCount() == 0);
        CHECK(m.fake.LiveBuffers() == 0);
        CHECK(scanned.empty());
    }
    SECTION("slowly")
    {
        Machine m;
        m.fake.options.hr_delay_reads = 20;
        m.fake.AddPort(0);
        REQUIRE(m.Start());
        CHECK(m.hba->DiskCount() == 1);
        CHECK(m.fake.Faults().empty());
    }
}

TEST_CASE("ahci spins ports up one at a time", "[ahci][hba]")
{
    SECTION("each port's link settles before the next spin-up")
    {
        Machine m;
        m.fake.options.sss = true;
        FakeAhci::PortOptions slow;
        slow.link_delay_reads = 3;
        m.fake.AddPort(0, slow);
        m.fake.AddPort(1, slow);
        m.fake.AddPort(2, slow);
        REQUIRE(m.Start());
        CHECK(m.fake.Faults().empty());
        const auto log = Of(m.fake, {Event::SpinUp, Event::LinkUp});
        REQUIRE(log.size() == 6);
        for (uint32_t port = 0; port < 3; port++)
        {
            CHECK(log[2 * port].event == Event::SpinUp);
            CHECK(log[2 * port].port == port);
            CHECK(log[2 * port + 1].event == Event::LinkUp);
            CHECK(log[2 * port + 1].port == port);
        }
        CHECK(m.hba->DiskCount() == 3);
    }
    SECTION("an empty port is waited for, not skipped at once")
    {
        Machine m;
        m.fake.options.sss = true;
        FakeAhci::PortOptions empty;
        empty.device = FakeAhci::Device::None;
        m.fake.AddPort(0);
        m.fake.AddPort(1, empty);
        m.fake.AddPort(2);
        REQUIRE(m.Start());
        const auto log = Of(m.fake, {Event::SpinUp, Event::LinkUp});
        REQUIRE(log.size() == 5);
        CHECK(log[2].event == Event::SpinUp);
        CHECK(log[2].port == 1);
        CHECK(log[3].event == Event::SpinUp);
        CHECK(log[3].port == 2);
        CHECK(m.Elapsed() >= 1000);
        CHECK(m.hba->DiskCount() == 2);
        CHECK(m.Lines("port 1:") == 0);
    }
    SECTION("no spin-up without staggered spin-up support")
    {
        Machine m;
        m.fake.AddPort(0);
        m.fake.AddPort(1);
        REQUIRE(m.Start());
        CHECK(m.fake.Count(Event::SpinUp) == 0);
        CHECK(m.hba->DiskCount() == 2);
    }
}

TEST_CASE("ahci resets a link that shows presence without communication once", "[ahci][hba]")
{
    Machine m;
    FakeAhci::PortOptions stuck;
    stuck.det1_until_comreset = true;
    m.fake.AddPort(0, stuck);
    REQUIRE(m.Start());
    CHECK(m.fake.Count(Event::Comreset) == 1);
    CHECK(m.hba->DiskCount() == 1);
    CHECK(m.fake.Faults().empty());
}

TEST_CASE("ahci does not take the link-up PhyRdy change for an unplug", "[ahci][hba]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    const auto data = Pattern(1, 512);
    CHECK(disk->Write(0, data));
    CHECK_FALSE(m.hba->DiskPort(0)->Dead());
    CHECK(removed.empty());
    disk->Release();
}

TEST_CASE("ahci makes progress with MSI-X, MSI, polling and lost interrupts", "[ahci][hba]")
{
    struct Case
    {
        S::InterruptKind kind;
        bool drop;
        bool expect_interrupts;
    };
    const Case cases[] = {{S::InterruptKind::MsiX, false, true},
                          {S::InterruptKind::Msi, false, true},
                          {S::InterruptKind::Polling, false, false},
                          {S::InterruptKind::MsiX, true, false}};
    for (const Case& c : cases)
    {
        Machine m;
        m.fake.options.interrupts = c.kind;
        m.fake.options.drop_interrupts = c.drop;
        m.fake.options.defer_processing = true;
        m.fake.AddPort(0);
        REQUIRE(m.Start());
        S::Disk* disk = m.Open();
        REQUIRE(disk != nullptr);
        const auto data = Pattern(3, 512 * 4);
        REQUIRE(disk->Write(8, data));
        std::vector<uint8_t> back(data.size());
        REQUIRE(disk->Read(8, back));
        CHECK(back == data);
        CHECK((m.fake.Interrupts() != 0) == c.expect_interrupts);
        CHECK((m.env.Wakes() != 0) == c.expect_interrupts);
        CHECK(m.fake.Faults().empty());
        disk->Release();
    }
}

TEST_CASE("ahci frees what it allocated when memory runs out", "[ahci][hba]")
{
    SECTION("no command list")
    {
        Machine m;
        m.fake.options.allocations_before_failure = 0;
        m.fake.AddPort(0);
        REQUIRE(m.Start());
        CHECK(m.hba->DiskCount() == 0);
        CHECK(m.Lines("AHCI 00:05.0 port 0: out of memory for the command list") == 1);
        CHECK(m.fake.LiveBuffers() == 0);
        CHECK(m.fake.Faults().empty());
    }
    SECTION("no transfer buffer")
    {
        Machine m;
        m.fake.options.allocations_before_failure = 1;
        m.fake.AddPort(0);
        REQUIRE(m.Start());
        CHECK(m.hba->DiskCount() == 0);
        CHECK(m.Lines("AHCI 00:05.0 port 0: out of memory for the transfer buffer") == 1);
        CHECK(m.fake.LiveBuffers() == 0);
        CHECK(m.fake.Faults().empty());
    }
}

TEST_CASE("ahci tears down a running port before freeing its buffers", "[ahci][hba]")
{
    Machine m;
    m.fake.AddPort(0);
    REQUIRE(m.Start());
    CHECK(m.fake.LiveBuffers() == 2);
    m.RemoveDisks();
    m.hba.reset();
    CHECK(m.fake.LiveBuffers() == 0);
    CHECK(m.fake.Faults().empty());
    CHECK_FALSE(m.fake.EngineRunning(0));
    CHECK_FALSE(m.fake.ReceivingFis(0));
}

TEST_CASE("ahci T18: only ATA signatures are accepted", "[ahci][ports]")
{
    Machine m;
    FakeAhci::PortOptions atapi;
    atapi.device = FakeAhci::Device::Atapi;
    FakeAhci::PortOptions multiplier;
    multiplier.device = FakeAhci::Device::PortMultiplier;
    FakeAhci::PortOptions semb;
    semb.device = FakeAhci::Device::Semb;
    FakeAhci::PortOptions empty;
    empty.device = FakeAhci::Device::None;
    m.fake.AddPort(0);
    m.fake.AddPort(1, atapi);
    m.fake.AddPort(2, multiplier);
    m.fake.AddPort(3, semb);
    m.fake.AddPort(4, empty);
    m.fake.AddPort(5);
    REQUIRE(m.Start());
    CHECK(m.fake.Faults().empty());
    CHECK(m.hba->DiskCount() == 2);
    CHECK(m.hba->DiskPort(0) != nullptr);
    CHECK(m.hba->DiskPort(5) != nullptr);
    CHECK(m.Lines("Storage: AHCI 00:05.0 port 1: ATAPI device not supported") == 1);
    CHECK(m.Lines("Storage: AHCI 00:05.0 port 2: port multiplier not supported") == 1);
    CHECK(m.Lines("Storage: AHCI 00:05.0 port 3: enclosure management bridge not supported") ==
          1);
    CHECK(m.Lines("AHCI 00:05.0 port 4") == 0);
    CHECK(m.fake.LiveBuffers() == 4);
    CHECK(m.fake.Count(Event::Identify) == 2);
    CHECK(m.Lines(m.Line("00:05.0 6 ports, 32 command slots, MSI-X, 64-bit DMA")) == 1);
}

TEST_CASE("ahci T18: disks are named in port order", "[ahci][ports]")
{
    Machine m;
    m.fake.AddPort(1);
    m.fake.AddPort(3);
    m.fake.AddPort(4);
    REQUIRE(m.Start());
    CHECK(scanned == std::vector<std::string>{"sda", "sdb", "sdc"});
    S::Disk* second = m.Open(1);
    REQUIRE(second != nullptr);
    CHECK(second->Driver().Info().kind == S::DiskKind::Ahci);
    CHECK(&second->Driver() == m.hba->DiskPort(3));
    second->Release();
}

TEST_CASE("ahci T18: a port whose device stays busy for 10 s is skipped", "[ahci][ports]")
{
    SECTION("forever")
    {
        Machine m;
        FakeAhci::PortOptions busy;
        busy.busy_forever = true;
        m.fake.AddPort(0, busy);
        m.fake.AddPort(1);
        REQUIRE(m.Start());
        CHECK(m.Lines("Storage: AHCI 00:05.0 port 0: device not ready") == 1);
        CHECK(m.Elapsed() >= 10000);
        CHECK(m.Elapsed() < 11000);
        CHECK(m.hba->DiskCount() == 1);
        CHECK(m.hba->DiskPort(1) != nullptr);
        CHECK(scanned == std::vector<std::string>{"sda"});
        CHECK(m.fake.LiveBuffers() == 2);
        CHECK(m.fake.Faults().empty());
    }
    SECTION("for a while")
    {
        Machine m;
        FakeAhci::PortOptions busy;
        busy.busy_reads = 1000;
        m.fake.AddPort(0, busy);
        REQUIRE(m.Start());
        CHECK(m.hba->DiskCount() == 1);
        CHECK(m.Elapsed() >= 2000);
        CHECK(m.Elapsed() < 10000);
    }
}

TEST_CASE("ahci T18: a disk the identify data refuses is not registered", "[ahci][ports]")
{
    Machine m;
    FakeAhci::PortOptions no_lba48;
    no_lba48.lba48 = false;
    m.fake.AddPort(0, no_lba48);
    m.fake.AddPort(1);
    REQUIRE(m.Start());
    CHECK(m.hba->DiskCount() == 1);
    CHECK(m.Lines(std::string("Storage: AHCI 00:05.0 port 0: ") +
                   FeatherOS::Storage::Ata::RefusalText(FeatherOS::Storage::Ata::Refusal::NoLba48)) ==
          1);
    CHECK(m.fake.LiveBuffers() == 2);
    CHECK(m.fake.Faults().empty());
    CHECK_FALSE(m.fake.EngineRunning(0));
    CHECK_FALSE(m.fake.ReceivingFis(0));
}

TEST_CASE("ahci T18: a 4096-byte disk is accepted", "[ahci][ports]")
{
    Machine m;
    FakeAhci::PortOptions four_k;
    four_k.sector_size = 4096;
    four_k.sectors = 512;
    m.fake.AddPort(0, four_k);
    REQUIRE(m.Start());
    S::Disk* disk = m.Open();
    REQUIRE(disk != nullptr);
    CHECK(disk->Info().sector_size == 4096);
    CHECK(disk->Info().sector_count == 512);
    CHECK(m.Lines("2048 KiB, 4096-byte sectors, write cache, rw") == 1);
    disk->Release();
}

TEST_CASE("ahci reports an HBA with no ports or no disks", "[ahci][ports]")
{
    SECTION("no disks")
    {
        Machine m;
        FakeAhci::PortOptions empty;
        empty.device = FakeAhci::Device::None;
        m.fake.AddPort(0, empty);
        REQUIRE(m.Start());
        CHECK(m.Lines(m.Line("no disks")) == 1);
        CHECK(m.fake.LiveBuffers() == 0);
    }
    SECTION("no port implemented")
    {
        Machine m;
        CHECK_FALSE(m.Start());
        CHECK(m.Lines(m.Line("no ports implemented")) == 1);
    }
}
