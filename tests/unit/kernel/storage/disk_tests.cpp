#include <catch2/catch_test_macros.hpp>

#include "kernel/storage/disk.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using FeatherDisk::Error;
using FeatherOS::Storage::Disk;
using FeatherOS::Storage::DiskDriver;
using FeatherOS::Storage::DiskInfo;
using FeatherOS::Storage::DiskKind;
using FeatherOS::Storage::DiskRegistry;
using FeatherOS::Storage::ReadOnlyReason;

struct Call
{
    char operation;
    uint64_t sector;
    uint32_t count;

    bool operator==(const Call&) const = default;
};

class FakeDriver final : public DiskDriver
{
public:
    explicit FakeDriver(DiskKind driver_kind = DiskKind::Usb, uint64_t sectors = 1024,
                        uint32_t size = 512, uint32_t max_transfer = 4096)
        : kind(driver_kind), sector_count(sectors), sector_size(size),
          max_transfer_bytes(max_transfer), image(sectors * size)
    {
        for (size_t index = 0; index < image.size(); ++index)
        {
            image[index] = static_cast<uint8_t>(index * 31 + (index >> 8));
        }
    }

    DiskInfo Info() override
    {
        DiskInfo info = {};
        info.kind = kind;
        info.sector_size = sector_size;
        info.sector_count = sector_count;
        info.max_transfer_bytes = max_transfer_bytes;
        info.removable = kind == DiskKind::Usb;
        info.read_only = read_only.load();
        info.read_only_reason = reason;
        info.controller_index = controller_index;
        info.namespace_id = namespace_id;
        return info;
    }

    bool Gone() override
    {
        return gone.load();
    }

    FeatherDisk::Status Read(uint64_t sector, uint32_t count, uint8_t* buffer) override
    {
        const std::lock_guard guard(lock);
        Record({'r', sector, count});
        if (fail_reads_after >= 0 && static_cast<int>(reads_done) >= fail_reads_after)
        {
            return std::unexpected(Error::Io);
        }
        ++reads_done;
        std::memcpy(buffer, image.data() + sector * sector_size,
                    static_cast<size_t>(count) * sector_size);
        return {};
    }

    FeatherDisk::Status Write(uint64_t sector, uint32_t count, const uint8_t* buffer) override
    {
        const std::lock_guard guard(lock);
        Record({'w', sector, count});
        if (!write_answer)
        {
            return write_answer;
        }
        std::memcpy(image.data() + sector * sector_size, buffer,
                    static_cast<size_t>(count) * sector_size);
        return {};
    }

    FeatherDisk::Status Flush() override
    {
        const std::lock_guard guard(lock);
        Record({'f', 0, 0});
        ++flushes;
        if (fail_flush)
        {
            return std::unexpected(Error::Io);
        }
        return {};
    }

    FeatherDisk::Status Stop() override
    {
        const std::lock_guard guard(lock);
        Record({'s', 0, 0});
        if (fail_stop)
        {
            return std::unexpected(Error::Io);
        }
        return {};
    }

    void Release() override
    {
        ++releases;
    }

    std::vector<Call> Calls()
    {
        const std::lock_guard guard(lock);
        return calls;
    }

    DiskKind kind;
    uint64_t sector_count;
    uint32_t sector_size;
    uint32_t max_transfer_bytes;
    std::vector<uint8_t> image;
    std::atomic<bool> gone = false;
    std::atomic<bool> read_only = false;
    ReadOnlyReason reason = ReadOnlyReason::None;
    uint16_t controller_index = 0;
    uint32_t namespace_id = 1;
    std::atomic<uint32_t> releases = 0;
    std::mutex lock;
    std::vector<Call> calls;
    int fail_reads_after = -1;
    uint32_t reads_done = 0;
    FeatherDisk::Status write_answer;
    bool fail_flush = false;
    bool fail_stop = false;
    uint32_t flushes = 0;
    uint32_t oversized = 0;

private:
    void Record(const Call& call)
    {
        if (static_cast<uint64_t>(call.count) * sector_size > max_transfer_bytes)
        {
            ++oversized;
        }
        calls.push_back(call);
    }
};

struct Registry
{
    Registry()
    {
        registry.Initialize();
    }

    DiskRegistry registry;
};

std::string NameOf(const Disk* disk)
{
    return disk == nullptr ? "(null)" : disk->Name();
}

std::atomic<uint32_t> removal_calls = 0;
std::atomic<Disk*> removed_disk = nullptr;

void OnRemoval(Disk& disk)
{
    ++removal_calls;
    removed_disk = &disk;
}

} // namespace

TEST_CASE("disk T16: names follow probe order, and a letter returns only after the disk is freed",
          "[disk]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    FakeDriver virtio(DiskKind::Virtio);
    FakeDriver first;
    FakeDriver second;
    FakeDriver third;
    FakeDriver fourth;

    Disk* vda = registry.Register(virtio);
    Disk* sda = registry.Register(first);
    Disk* sdb = registry.Register(second);
    REQUIRE(NameOf(vda) == "vda");
    REQUIRE(NameOf(sda) == "sda");
    REQUIRE(NameOf(sdb) == "sdb");
    REQUIRE(registry.Count() == 3);
    REQUIRE(registry.BootDisk() == vda);

    Disk* held = registry.Acquire("sda");
    REQUIRE(held == sda);
    registry.Remove(*sda);
    CHECK(registry.Acquire("sda") == nullptr);
    CHECK(registry.Count() == 2);
    CHECK(first.releases == 0);

    // sda is gone from the registry but a volume still holds it, so its letter stays taken.
    Disk* sdc = registry.Register(third);
    REQUIRE(NameOf(sdc) == "sdc");
    CHECK(held->Gone());

    held->Release();
    CHECK(first.releases == 1);
    Disk* again = registry.Register(fourth);
    REQUIRE(NameOf(again) == "sda");

    registry.Remove(*sdb);
    registry.Remove(*sdc);
    registry.Remove(*again);
    registry.Remove(*vda);
    CHECK(registry.Count() == 0);
    CHECK(second.releases == 1);
    CHECK(third.releases == 1);
    CHECK(fourth.releases == 1);
    CHECK(virtio.releases == 1);
}

TEST_CASE("disk T16: the driver is released once, after the last of registry, holders and acquirers",
          "[disk]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    removal_calls = 0;
    registry.SetRemovalHandler(OnRemoval);
    FakeDriver driver;
    Disk* disk = registry.Register(driver);
    REQUIRE(disk != nullptr);
    disk->Retain();
    Disk* acquired = registry.Acquire("sda");
    REQUIRE(acquired == disk);

    registry.Remove(*disk);
    registry.Remove(*disk);
    CHECK(removal_calls == 1);
    CHECK(removed_disk == disk);
    CHECK(driver.releases == 0);
    disk->Release();
    CHECK(driver.releases == 0);
    acquired->Release();
    CHECK(driver.releases == 1);
    registry.SetRemovalHandler(nullptr);
}

TEST_CASE("disk T16: partitions of a name ending in a digit take a p", "[disk]")
{
    char name[24];
    REQUIRE(FeatherOS::Storage::PartitionName("sda", 1, name, sizeof(name)) == 4);
    CHECK(std::string(name) == "sda1");
    REQUIRE(FeatherOS::Storage::PartitionName("vda", 16, name, sizeof(name)) == 5);
    CHECK(std::string(name) == "vda16");
    REQUIRE(FeatherOS::Storage::PartitionName("nvme0n1", 1, name, sizeof(name)) == 9);
    CHECK(std::string(name) == "nvme0n1p1");
    REQUIRE(FeatherOS::Storage::PartitionName("mmcblk0", 12, name, sizeof(name)) == 10);
    CHECK(std::string(name) == "mmcblk0p12");
    CHECK(FeatherOS::Storage::PartitionName("nvme0n1", 1, name, 9) == 0);
    CHECK(FeatherOS::Storage::PartitionName("nvme0n1", 1, name, 10) == 9);
}

TEST_CASE("disk T16: a drive that is already gone is not registered, and takes no letter", "[disk]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    FakeDriver gone;
    gone.gone = true;
    CHECK(registry.Register(gone) == nullptr);
    CHECK(gone.releases == 0);
    CHECK(registry.Count() == 0);

    FakeDriver healthy;
    Disk* disk = registry.Register(healthy);
    REQUIRE(NameOf(disk) == "sda");
    registry.Remove(*disk);
}

TEST_CASE("disk T16: the registry refuses a disk before Initialize and past MaxDisks", "[disk]")
{
    DiskRegistry cold;
    FakeDriver early;
    CHECK(cold.Register(early) == nullptr);
    CHECK(early.releases == 0);

    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    std::vector<std::unique_ptr<FakeDriver>> drivers;
    std::vector<Disk*> disks;
    for (uint32_t index = 0; index < FeatherOS::Storage::MaxDisks; ++index)
    {
        drivers.push_back(std::make_unique<FakeDriver>());
        disks.push_back(registry.Register(*drivers.back()));
        REQUIRE(disks.back() != nullptr);
    }
    CHECK(NameOf(disks.back()) == "sdp");
    FakeDriver extra;
    CHECK(registry.Register(extra) == nullptr);
    CHECK(extra.releases == 0);

    registry.Remove(*disks[3]);
    Disk* replacement = registry.Register(extra);
    REQUIRE(replacement != nullptr);
    CHECK(NameOf(replacement) == "sdd");
    registry.Remove(*replacement);
    for (size_t index = 0; index < disks.size(); ++index)
    {
        if (index != 3)
        {
            registry.Remove(*disks[index]);
        }
    }
    for (const auto& driver : drivers)
    {
        CHECK(driver->releases == 1);
    }
}

TEST_CASE("disk T16: FlushAll flushes each live disk once and reports a failure", "[disk]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    FakeDriver virtio(DiskKind::Virtio);
    FakeDriver first;
    FakeDriver second;
    FakeDriver third;
    FakeDriver fourth;
    Disk* vda = registry.Register(virtio);
    Disk* sda = registry.Register(first);
    Disk* sdb = registry.Register(second);
    Disk* sdc = registry.Register(third);
    Disk* sdd = registry.Register(fourth);
    REQUIRE(sdd != nullptr);

    REQUIRE(sdb->Eject());
    registry.Remove(*sdc);
    CHECK(registry.FlushAll());
    CHECK(virtio.flushes == 1);
    CHECK(first.flushes == 1);
    CHECK(second.flushes == 0);
    CHECK(third.flushes == 0);
    CHECK(fourth.flushes == 1);
    CHECK(vda->Counters().flushes == 1);

    first.fail_flush = true;
    CHECK_FALSE(registry.FlushAll());
    CHECK(first.flushes == 2);
    CHECK(fourth.flushes == 2);
    CHECK(sda->Counters().flushes == 2);

    registry.Remove(*vda);
    registry.Remove(*sda);
    registry.Remove(*sdb);
    registry.Remove(*sdd);
}

TEST_CASE("disk T1: NVMe names follow controller and namespace, partitions take a p", "[disk]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    FakeDriver first(DiskKind::Nvme);
    FakeDriver second(DiskKind::Nvme);
    second.namespace_id = 2;
    FakeDriver other(DiskKind::Nvme);
    other.controller_index = 1;
    FakeDriver twin(DiskKind::Nvme);
    Disk* n1 = registry.Register(first);
    Disk* n2 = registry.Register(second);
    Disk* n3 = registry.Register(other);
    REQUIRE(NameOf(n1) == "nvme0n1");
    REQUIRE(NameOf(n2) == "nvme0n2");
    REQUIRE(NameOf(n3) == "nvme1n1");

    char name[16];
    REQUIRE(FeatherOS::Storage::PartitionName(n1->Name(), 1, name, sizeof(name)) == 9);
    CHECK(std::string(name) == "nvme0n1p1");
    REQUIRE(FeatherOS::Storage::PartitionName(n3->Name(), 12, name, sizeof(name)) == 10);
    CHECK(std::string(name) == "nvme1n1p12");

    CHECK(registry.Register(twin) == nullptr);
    CHECK(twin.releases == 0);
    CHECK(registry.Count() == 3);
    CHECK(registry.Acquire("nvme0n2") == n2);
    n2->Release();

    n1->Retain();
    registry.Remove(*n1);
    CHECK(registry.Register(twin) == nullptr);
    n1->Release();
    Disk* again = registry.Register(twin);
    REQUIRE(again != nullptr);
    CHECK(NameOf(again) == "nvme0n1");

    registry.Remove(*again);
    registry.Remove(*n2);
    registry.Remove(*n3);
}

TEST_CASE("disk T1: NVMe disks take no sd letter and AHCI shares them with USB", "[disk]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    FakeDriver nvme(DiskKind::Nvme);
    FakeDriver ahci_a(DiskKind::Ahci);
    FakeDriver ahci_b(DiskKind::Ahci);
    FakeDriver usb_a(DiskKind::Usb);
    FakeDriver usb_b(DiskKind::Usb);
    FakeDriver virtio(DiskKind::Virtio);
    Disk* n = registry.Register(nvme);
    Disk* a = registry.Register(ahci_a);
    Disk* b = registry.Register(ahci_b);
    Disk* u = registry.Register(usb_a);
    REQUIRE(NameOf(n) == "nvme0n1");
    REQUIRE(NameOf(a) == "sda");
    REQUIRE(NameOf(b) == "sdb");
    REQUIRE(NameOf(u) == "sdc");

    registry.Remove(*a);
    Disk* hole = registry.Register(usb_b);
    CHECK(NameOf(hole) == "sda");
    Disk* v = registry.Register(virtio);
    CHECK(NameOf(v) == "vda");
    CHECK(registry.BootDisk() == v);

    for (Disk* disk : {n, b, u, hole, v})
    {
        registry.Remove(*disk);
    }
}

TEST_CASE("disk T1: sixteen disks fit and the seventeenth is refused", "[disk]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    std::vector<std::unique_ptr<FakeDriver>> drivers;
    std::vector<Disk*> disks;
    for (uint32_t index = 0; index < 16; ++index)
    {
        drivers.push_back(std::make_unique<FakeDriver>(index % 2 == 0 ? DiskKind::Ahci
                                                                     : DiskKind::Nvme));
        drivers.back()->controller_index = static_cast<uint16_t>(index);
        disks.push_back(registry.Register(*drivers.back()));
        REQUIRE(disks.back() != nullptr);
    }
    CHECK(FeatherOS::Storage::MaxDisks == 16);
    CHECK(NameOf(disks[14]) == "sdh");
    CHECK(NameOf(disks[15]) == "nvme15n1");
    FakeDriver extra;
    CHECK(registry.Register(extra) == nullptr);
    for (Disk* disk : disks)
    {
        registry.Remove(*disk);
    }
}

TEST_CASE("disk T1: the partition listing names the NVMe and AHCI kinds", "[disk]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    FakeDriver nvme(DiskKind::Nvme, 2048);
    FakeDriver ahci(DiskKind::Ahci, 4096);
    ahci.read_only = true;
    ahci.reason = ReadOnlyReason::Policy;
    Disk* n = registry.Register(nvme);
    Disk* a = registry.Register(ahci);
    char text[256];
    registry.FormatPartitions(text, sizeof(text));
    CHECK(std::string(text) == "nvme0n1 1024 512 nvme\n"
                               "sda    2048 512 ahci ro\n");
    CHECK(a->Info().read_only_reason == ReadOnlyReason::Policy);
    CHECK(n->Info().read_only_reason == ReadOnlyReason::None);
    registry.Remove(*n);
    registry.Remove(*a);
}

TEST_CASE("disk T2: a policy read-only disk refuses writes and skips the flush", "[disk]")
{
    Registry fixture;
    FakeDriver policy(DiskKind::Nvme);
    policy.read_only = true;
    policy.reason = ReadOnlyReason::Policy;
    Disk* disk = fixture.registry.Register(policy);
    REQUIRE(disk != nullptr);
    std::vector<uint8_t> buffer(512);
    auto write = disk->Write(0, buffer);
    REQUIRE_FALSE(write);
    CHECK(write.error() == Error::ReadOnly);
    CHECK(disk->Flush());
    CHECK(policy.Calls().empty());
    CHECK(policy.flushes == 0);
    CHECK(disk->Counters().flushes == 0);
    CHECK(disk->Info().read_only_reason == ReadOnlyReason::Policy);
    CHECK(disk->Read(0, buffer));

    FakeDriver medium;
    medium.read_only = true;
    medium.reason = ReadOnlyReason::Medium;
    Disk* locked = fixture.registry.Register(medium);
    REQUIRE(locked != nullptr);
    CHECK(locked->Flush());
    CHECK(medium.flushes == 0);
    CHECK(locked->Info().read_only_reason == ReadOnlyReason::Medium);

    FakeDriver open;
    Disk* writable = fixture.registry.Register(open);
    REQUIRE(writable != nullptr);
    CHECK(writable->Write(0, buffer));
    CHECK(writable->Flush());
    CHECK(open.flushes == 1);
    CHECK(writable->Info().read_only_reason == ReadOnlyReason::None);

    fixture.registry.Remove(*disk);
    fixture.registry.Remove(*locked);
    fixture.registry.Remove(*writable);
}

TEST_CASE("disk T16: the partition listing shows one line per disk", "[disk]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    FakeDriver virtio(DiskKind::Virtio, 524288);
    FakeDriver stick(DiskKind::Usb, 131072);
    FakeDriver locked(DiskKind::Usb, 4096);
    locked.read_only = true;
    Disk* vda = registry.Register(virtio);
    Disk* sda = registry.Register(stick);
    Disk* sdb = registry.Register(locked);

    char text[512];
    const size_t length = registry.FormatPartitions(text, sizeof(text));
    CHECK(length == std::strlen(text));
    CHECK(std::string(text) == "vda    262144 512 virtio\n"
                               "sda    65536 512 usb removable rw\n"
                               "sdb    2048 512 usb removable ro\n");
    REQUIRE(sda->Eject());
    registry.FormatPartitions(text, sizeof(text));
    CHECK(std::string(text).find("sda    65536 512 usb removable rw ejected\n") !=
          std::string::npos);

    char small[24];
    const size_t cut = registry.FormatPartitions(small, sizeof(small));
    CHECK(cut == std::strlen(small));
    CHECK(cut < sizeof(small));

    registry.Remove(*vda);
    registry.Remove(*sda);
    registry.Remove(*sdb);
}

TEST_CASE("disk T17: transfers are split at max_transfer_bytes, in order", "[disk]")
{
    Registry fixture;
    FakeDriver driver(DiskKind::Usb, 1024, 512, 4096);
    Disk* disk = fixture.registry.Register(driver);
    REQUIRE(disk != nullptr);

    std::vector<uint8_t> buffer(20 * 512);
    REQUIRE(disk->Read(100, buffer));
    CHECK(driver.Calls() == std::vector<Call>{{'r', 100, 8}, {'r', 108, 8}, {'r', 116, 4}});
    CHECK(std::equal(buffer.begin(), buffer.end(), driver.image.begin() + 100 * 512));
    CHECK(driver.oversized == 0);

    std::vector<uint8_t> pattern(17 * 512);
    for (size_t index = 0; index < pattern.size(); ++index)
    {
        pattern[index] = static_cast<uint8_t>(index ^ 0x5A);
    }
    driver.calls.clear();
    REQUIRE(disk->Write(1007, pattern));
    CHECK(driver.Calls() == std::vector<Call>{{'w', 1007, 8}, {'w', 1015, 8}, {'w', 1023, 1}});
    CHECK(std::equal(pattern.begin(), pattern.end(), driver.image.begin() + 1007 * 512));
    CHECK(driver.oversized == 0);

    const auto counters = disk->Counters();
    CHECK(counters.read_sectors == 20);
    CHECK(counters.written_sectors == 17);
    fixture.registry.Remove(*disk);
}

TEST_CASE("disk T17: a sector of 4096 bytes and a transfer limit below one sector", "[disk]")
{
    Registry fixture;
    FakeDriver large(DiskKind::Usb, 64, 4096, 65536);
    Disk* disk = fixture.registry.Register(large);
    REQUIRE(disk != nullptr);
    CHECK(disk->SectorSize() == 4096);
    CHECK(disk->SectorCount() == 64);
    std::vector<uint8_t> buffer(40 * 4096);
    REQUIRE(disk->Read(0, buffer));
    CHECK(large.Calls() == std::vector<Call>{{'r', 0, 16}, {'r', 16, 16}, {'r', 32, 8}});

    FakeDriver tiny(DiskKind::Usb, 16, 512, 100);
    Disk* narrow = fixture.registry.Register(tiny);
    REQUIRE(narrow != nullptr);
    std::vector<uint8_t> two(2 * 512);
    REQUIRE(narrow->Read(3, two));
    CHECK(tiny.Calls() == std::vector<Call>{{'r', 3, 1}, {'r', 4, 1}});
    fixture.registry.Remove(*disk);
    fixture.registry.Remove(*narrow);
}

TEST_CASE("disk T17: a transfer past the end or of a partial sector is refused", "[disk]")
{
    Registry fixture;
    FakeDriver driver(DiskKind::Usb, 64);
    Disk* disk = fixture.registry.Register(driver);
    REQUIRE(disk != nullptr);
    std::vector<uint8_t> one(512);
    std::vector<uint8_t> two(1024);
    std::vector<uint8_t> partial(100);

    auto refused = [](const FeatherDisk::Status& status)
    { return !status && status.error() == Error::InvalidArgument; };
    CHECK(refused(disk->Read(64, one)));
    CHECK(refused(disk->Read(63, two)));
    CHECK(refused(disk->Write(64, one)));
    CHECK(refused(disk->Write(63, two)));
    CHECK(refused(disk->Read(0, partial)));
    CHECK(refused(disk->Read(~uint64_t{0}, one)));
    CHECK(driver.Calls().empty());

    CHECK(disk->Read(63, one));
    CHECK(disk->Read(64, std::span<uint8_t>()));
    CHECK(driver.Calls() == std::vector<Call>{{'r', 63, 1}});
    fixture.registry.Remove(*disk);
}

TEST_CASE("disk T17: a gone or ejected disk fails with Io and touches no driver", "[disk]")
{
    Registry fixture;
    FakeDriver driver;
    Disk* disk = fixture.registry.Register(driver);
    REQUIRE(disk != nullptr);
    std::vector<uint8_t> buffer(512);
    CHECK_FALSE(disk->Ejected());

    driver.fail_stop = true;
    CHECK_FALSE(disk->Eject());
    CHECK_FALSE(disk->Ejected());
    CHECK(disk->Read(0, buffer));
    driver.fail_stop = false;
    REQUIRE(disk->Eject());
    CHECK(disk->Ejected());
    driver.calls.clear();
    auto io = [](const FeatherDisk::Status& status) { return !status && status.error() == Error::Io; };
    CHECK(io(disk->Read(0, buffer)));
    CHECK(io(disk->Write(0, buffer)));
    CHECK(io(disk->Flush()));
    CHECK(io(disk->Eject()));
    CHECK(driver.Calls().empty());
    fixture.registry.Remove(*disk);

    FakeDriver other;
    Disk* held = fixture.registry.Register(other);
    REQUIRE(held != nullptr);
    held->Retain();
    fixture.registry.Remove(*held);
    CHECK(held->Gone());
    CHECK(io(held->Read(0, buffer)));
    CHECK(io(held->Write(0, buffer)));
    CHECK(io(held->Flush()));
    CHECK(other.Calls().empty());
    held->Release();
    CHECK(other.releases == 1);
}

TEST_CASE("disk T17: a read-only disk refuses writes and learns it from the driver", "[disk]")
{
    Registry fixture;
    FakeDriver locked;
    locked.read_only = true;
    Disk* disk = fixture.registry.Register(locked);
    REQUIRE(disk != nullptr);
    std::vector<uint8_t> buffer(512);
    CHECK(disk->Info().read_only);
    auto read_only = [](const FeatherDisk::Status& status)
    { return !status && status.error() == Error::ReadOnly; };
    CHECK(read_only(disk->Write(0, buffer)));
    CHECK(locked.Calls().empty());
    CHECK(disk->Read(0, buffer));

    FakeDriver late;
    late.write_answer = std::unexpected(Error::ReadOnly);
    Disk* learner = fixture.registry.Register(late);
    REQUIRE(learner != nullptr);
    CHECK_FALSE(learner->Info().read_only);
    CHECK(read_only(learner->Write(0, buffer)));
    CHECK(learner->Info().read_only);
    CHECK(read_only(learner->Write(0, buffer)));
    CHECK(late.Calls().size() == 1);
    CHECK(learner->Counters().written_sectors == 0);
    fixture.registry.Remove(*disk);
    fixture.registry.Remove(*learner);
}

TEST_CASE("disk T17: a failing chunk stops the transfer and counts only what completed", "[disk]")
{
    Registry fixture;
    FakeDriver driver;
    driver.fail_reads_after = 1;
    Disk* disk = fixture.registry.Register(driver);
    REQUIRE(disk != nullptr);
    std::vector<uint8_t> buffer(20 * 512);
    const FeatherDisk::Status status = disk->Read(0, buffer);
    REQUIRE_FALSE(status);
    CHECK(status.error() == Error::Io);
    CHECK(driver.Calls() == std::vector<Call>{{'r', 0, 8}, {'r', 8, 8}});
    CHECK(disk->Counters().read_sectors == 8);
    fixture.registry.Remove(*disk);
}

TEST_CASE("disk T16: several tasks register, acquire, flush and remove at once",
          "[disk][concurrency]")
{
    Registry fixture;
    DiskRegistry& registry = fixture.registry;
    constexpr uint32_t Threads = 4;
    constexpr uint32_t Rounds = 200;
    std::vector<std::unique_ptr<FakeDriver>> drivers;
    for (uint32_t index = 0; index < Threads * Rounds; ++index)
    {
        drivers.push_back(std::make_unique<FakeDriver>());
    }
    std::atomic<uint32_t> registered = 0;
    std::atomic<uint32_t> failures = 0;

    std::vector<std::thread> threads;
    for (uint32_t thread = 0; thread < Threads; ++thread)
    {
        threads.emplace_back(
            [&, thread]
            {
                std::vector<uint8_t> buffer(512);
                for (uint32_t round = 0; round < Rounds; ++round)
                {
                    FakeDriver& driver = *drivers[thread * Rounds + round];
                    Disk* disk = registry.Register(driver);
                    if (disk != nullptr)
                    {
                        ++registered;
                        if (!disk->Read(0, buffer) || !disk->Flush())
                        {
                            ++failures;
                        }
                        registry.Remove(*disk);
                    }
                    else if (driver.releases != 0)
                    {
                        ++failures;
                    }
                    Disk* other = registry.Acquire("sda");
                    if (other != nullptr)
                    {
                        other->Read(0, buffer);
                        other->Release();
                    }
                    registry.FlushAll();
                    registry.FormatPartitions(reinterpret_cast<char*>(buffer.data()), 256);
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(failures == 0);
    CHECK(registry.Count() == 0);
    uint32_t released = 0;
    for (const auto& driver : drivers)
    {
        CHECK(driver->releases <= 1);
        released += driver->releases;
    }
    CHECK(released == registered);
}
