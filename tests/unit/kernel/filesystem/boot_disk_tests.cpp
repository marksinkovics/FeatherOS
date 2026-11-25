#include <catch2/catch_test_macros.hpp>

#include "kernel/filesystem/core_driver.h"
#include "kernel/filesystem/drivers.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/worker.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <vector>

// The boot disk is the disk root= names, whatever its kind: its volumes take the boot slots under
// its own names, every other disk's go to the other slots once the scans are released, and its
// removal kills the boot slots without freeing them.
namespace
{

namespace Drivers = FeatherOS::Filesystem::Drivers;
namespace Storage = FeatherOS::Storage;
namespace VFS = FeatherOS::VFS;
using FeatherOS::Filesystem::CoreDriver;

constexpr uint32_t GptFirstPartition = 64;
constexpr uint32_t GptEntryCount = 128;
constexpr uint32_t GptEntrySize = 128;

class FakeDisk final : public Storage::DiskDriver
{
public:
    FakeDisk(Storage::DiskKind disk_kind, std::vector<uint8_t> bytes, uint32_t size,
             uint16_t controller = 0, uint32_t namespace_number = 1)
        : kind(disk_kind), image(std::move(bytes)), sector_size(size),
          controller_index(controller), namespace_id(namespace_number)
    {
    }

    Storage::DiskInfo Info() override
    {
        Storage::DiskInfo info = {};
        info.kind = kind;
        info.sector_size = sector_size;
        info.sector_count = image.size() / sector_size;
        info.max_transfer_bytes = 64 * 1024;
        info.controller_index = controller_index;
        info.namespace_id = namespace_id;
        return info;
    }

    bool Gone() override
    {
        return false;
    }

    FeatherDisk::Status Read(uint64_t sector, uint32_t count, uint8_t* buffer) override
    {
        std::memcpy(buffer, image.data() + sector * sector_size, uint64_t{count} * sector_size);
        return {};
    }

    FeatherDisk::Status Write(uint64_t sector, uint32_t count, const uint8_t* buffer) override
    {
        std::memcpy(image.data() + sector * sector_size, buffer, uint64_t{count} * sector_size);
        return {};
    }

    FeatherDisk::Status Flush() override
    {
        return {};
    }

    FeatherDisk::Status Stop() override
    {
        return std::unexpected(FeatherDisk::Error::Unsupported);
    }

    void Release() override
    {
        ++releases;
    }

    std::atomic<int> releases = 0;

private:
    Storage::DiskKind kind;
    std::vector<uint8_t> image;
    uint32_t sector_size;
    uint16_t controller_index;
    uint32_t namespace_id;
};

void RunWorker()
{
    while (FeatherOS::Storage::Worker::RunNext())
    {
    }
}

void Put32(uint8_t* at, uint32_t value)
{
    for (int index = 0; index < 4; ++index)
    {
        at[index] = static_cast<uint8_t>(value >> (8 * index));
    }
}

void Put64(uint8_t* at, uint64_t value)
{
    Put32(at, static_cast<uint32_t>(value));
    Put32(at + 4, static_cast<uint32_t>(value >> 32));
}

uint32_t Crc32(const uint8_t* bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t index = 0; index < length; ++index)
    {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB88320U : 0U);
        }
    }
    return ~crc;
}

// A protective MBR and a GUID partition table whose partitions follow one another from sector 64.
std::vector<uint8_t> GptImage(uint32_t sector_size,
                              const std::vector<std::vector<uint8_t>>& partitions)
{
    std::vector<uint64_t> sectors;
    uint64_t total = GptFirstPartition;
    for (const std::vector<uint8_t>& partition : partitions)
    {
        sectors.push_back((partition.size() + sector_size - 1) / sector_size);
        total += sectors.back();
    }
    total += 40;
    std::vector<uint8_t> image(total * sector_size);
    image[446 + 4] = 0xEE;
    Put32(image.data() + 446 + 8, 1);
    Put32(image.data() + 446 + 12, 0xFFFFFFFFU);
    image[510] = 0x55;
    image[511] = 0xAA;

    std::vector<uint8_t> entries(GptEntryCount * GptEntrySize);
    uint64_t first = GptFirstPartition;
    for (size_t index = 0; index < partitions.size(); ++index)
    {
        uint8_t* entry = entries.data() + index * GptEntrySize;
        entry[0] = 0xA2;
        Put64(entry + 32, first);
        Put64(entry + 40, first + sectors[index] - 1);
        std::copy(partitions[index].begin(), partitions[index].end(),
                  image.begin() + static_cast<std::ptrdiff_t>(first * sector_size));
        first += sectors[index];
    }
    std::copy(entries.begin(), entries.end(),
              image.begin() + static_cast<std::ptrdiff_t>(2 * sector_size));

    uint8_t* header = image.data() + sector_size;
    std::memcpy(header, "EFI PART", 8);
    Put32(header + 8, 0x00010000);
    Put32(header + 12, 92);
    Put64(header + 24, 1);
    Put64(header + 32, total - 1);
    Put64(header + 40, 34);
    Put64(header + 48, total - 34);
    Put64(header + 72, 2);
    Put32(header + 80, GptEntryCount);
    Put32(header + 84, GptEntrySize);
    Put32(header + 88, Crc32(entries.data(), entries.size()));
    Put32(header + 16, Crc32(header, 92));
    return image;
}

// The core's 4096-byte-block ext4 image, which holds hello.txt; empty without e2fsprogs.
std::vector<uint8_t> Ext4Image()
{
    std::string path = FEATHEROS_EXT4_TEST_IMAGE;
    const std::string::size_type name = path.rfind("ext4.img");
    if (name == std::string::npos)
    {
        return {};
    }
    path.replace(name, std::string::npos, "ext4-4k.img");
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        return {};
    }
    std::vector<uint8_t> bytes;
    uint8_t buffer[4096];
    for (size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) != 0;)
    {
        bytes.insert(bytes.end(), buffer, buffer + count);
    }
    std::fclose(file);
    return bytes;
}

// The BPB of a FAT16 volume laid out for 512-byte sectors, which a disk of 4096-byte sectors
// cannot use.
std::vector<uint8_t> Fat16Image(uint32_t sector_size)
{
    std::vector<uint8_t> image(uint64_t{64} * sector_size);
    uint8_t* boot = image.data();
    boot[11] = 0;
    boot[12] = 2;
    boot[13] = 1;
    boot[14] = 1;
    boot[16] = 1;
    boot[17] = 16;
    boot[19] = 0;
    boot[20] = 2;
    boot[21] = 0xF8;
    boot[22] = 20;
    boot[510] = 0x55;
    boot[511] = 0xAA;
    return image;
}

std::string RootSource()
{
    const char* type = nullptr;
    const char* source = nullptr;
    bool read_only = false;
    if (!VFS::DescribeRoot(&type, &source, &read_only))
    {
        return "";
    }
    return std::string(source) + " " + type;
}

// The storage handlers belong to these tests alone.
struct Handlers
{
    Handlers()
    {
        Storage::Initialize();
        Drivers::AttachStorage();
    }

    ~Handlers()
    {
        Storage::SetRemovalHandler(nullptr);
        Storage::SetScanHandler(nullptr);
        Storage::SetVolumeLister(nullptr);
    }

    Handlers(const Handlers&) = delete;
    Handlers& operator=(const Handlers&) = delete;
};

// Makes a disk of its own the boot disk and puts everything back for the tests that follow, which
// expect vda: the volumes it opened are forgotten, dead or not, and vda is the boot disk again.
class BootRig
{
public:
    BootRig(Storage::DiskKind kind, std::vector<uint8_t> image, uint32_t sector_size)
        : previous(Storage::BootDisk()), driver(kind, std::move(image), sector_size),
          disk(Storage::Register(driver))
    {
        REQUIRE(previous != nullptr);
        REQUIRE(disk != nullptr);
        disk->Retain();
        Drivers::SetBootDisk(*disk);
    }

    ~BootRig()
    {
        Drivers::SetBootDisk(*previous);
        VFS::MountBlockDevice();
        for (size_t number = 1; number <= Drivers::MaximumVolumes; ++number)
        {
            Drivers::Volume(number)->ClearDead();
        }
        VFS::MountBlockDevice();
        Storage::Remove(*disk);
        RunWorker();
        disk->Release();
    }

    BootRig(const BootRig&) = delete;
    BootRig& operator=(const BootRig&) = delete;

    Handlers handlers;
    Storage::Disk* previous;
    FakeDisk driver;
    Storage::Disk* disk;
};

std::vector<uint8_t> ZeroImage(uint32_t sectors, uint32_t sector_size)
{
    return std::vector<uint8_t>(uint64_t{sectors} * sector_size);
}

std::vector<std::string> scanned;

void OnScan(Storage::Disk& disk)
{
    scanned.emplace_back(disk.Name());
}

void Nothing(void*) {}

struct Registry
{
    Registry()
    {
        registry.Initialize();
        registry.SetScanHandler(OnScan);
        scanned.clear();
    }

    ~Registry()
    {
        registry.SetScanHandler(nullptr);
        RunWorker();
    }

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    Storage::DiskRegistry registry;
};

} // namespace

TEST_CASE("T32: the boot disk is the named disk, else vda, else none")
{
    Registry fixture;
    Storage::DiskRegistry& registry = fixture.registry;
    FakeDisk virtio(Storage::DiskKind::Virtio, ZeroImage(8, 512), 512);
    FakeDisk nvme(Storage::DiskKind::Nvme, ZeroImage(8, 512), 512);
    FakeDisk ahci(Storage::DiskKind::Ahci, ZeroImage(8, 512), 512);

    REQUIRE(registry.BootDisk() == nullptr);
    REQUIRE(registry.ChooseBootDisk(nullptr) == nullptr);
    REQUIRE(registry.ChooseBootDisk("nvme0n1") == nullptr);

    Storage::Disk* const nvme_disk = registry.Register(nvme);
    REQUIRE(nvme_disk != nullptr);
    // No vda: an absent or missing name leaves no boot disk, and nvme0n1 is never chosen unasked.
    CHECK(registry.ChooseBootDisk(nullptr) == nullptr);
    CHECK(registry.ChooseBootDisk("") == nullptr);
    CHECK(registry.ChooseBootDisk("sda") == nullptr);
    Storage::Disk* chosen = registry.ChooseBootDisk("nvme0n1");
    CHECK(chosen == nvme_disk);
    chosen->Release();

    Storage::Disk* const vda = registry.Register(virtio);
    Storage::Disk* const sda = registry.Register(ahci);
    REQUIRE(vda != nullptr);
    REQUIRE(sda != nullptr);
    for (const char* name : {"nvme0n1", "sda", "vda"})
    {
        INFO(name);
        chosen = registry.ChooseBootDisk(name);
        REQUIRE(chosen != nullptr);
        CHECK(std::string(chosen->Name()) == name);
        chosen->Release();
    }
    for (const char* name : {static_cast<const char*>(nullptr), "", "nvme1n1", "sdb", "vdb"})
    {
        chosen = registry.ChooseBootDisk(name);
        CHECK(chosen == vda);
        chosen->Release();
    }

    registry.Remove(*nvme_disk);
    chosen = registry.ChooseBootDisk("nvme0n1");
    CHECK(chosen == vda);
    chosen->Release();

    registry.Remove(*sda);
    registry.Remove(*vda);
    CHECK(registry.ChooseBootDisk("vda") == nullptr);
}

TEST_CASE("T32: SetBootDisk retains the disk, so its removal leaves it and its name behind")
{
    Registry fixture;
    Storage::DiskRegistry& registry = fixture.registry;
    FakeDisk first(Storage::DiskKind::Nvme, ZeroImage(8, 512), 512);
    FakeDisk second(Storage::DiskKind::Nvme, ZeroImage(8, 512), 512);

    Storage::Disk* const boot = registry.Register(first);
    REQUIRE(boot != nullptr);
    registry.SetBootDisk(*boot);
    CHECK(registry.BootDisk() == boot);
    registry.Remove(*boot);
    CHECK(registry.BootDisk() == boot);
    CHECK(boot->Gone());
    CHECK(first.releases == 0);
    CHECK(std::string(boot->Name()) == "nvme0n1");
    // The name stays taken for as long as the boot disk is kept.
    CHECK(registry.Register(second) == nullptr);
    CHECK(registry.Acquire("nvme0n1") == nullptr);
    // The registry's reference, which the kernel never drops.
    boot->Release();
    CHECK(first.releases == 1);
}

TEST_CASE("T32: scans wait while deferred, and the boot disk is never scanned as another disk")
{
    Registry fixture;
    Storage::DiskRegistry& registry = fixture.registry;
    FakeDisk virtio(Storage::DiskKind::Virtio, ZeroImage(8, 512), 512);
    FakeDisk nvme(Storage::DiskKind::Nvme, ZeroImage(8, 512), 512);
    FakeDisk ahci(Storage::DiskKind::Ahci, ZeroImage(8, 512), 512);

    registry.DeferScans(true);
    Storage::Disk* const vda = registry.Register(virtio);
    Storage::Disk* const nvme_disk = registry.Register(nvme);
    Storage::Disk* const sda = registry.Register(ahci);
    registry.Scan(*vda);
    registry.Scan(*nvme_disk);
    registry.Scan(*sda);
    registry.SetBootDisk(*nvme_disk);
    RunWorker();
    CHECK(scanned.empty());

    registry.DeferScans(false);
    RunWorker();
    std::sort(scanned.begin(), scanned.end());
    CHECK(scanned == std::vector<std::string>{"sda", "vda"});

    // Nothing is pending now, and a scan after the deferral runs at once.
    registry.DeferScans(true);
    registry.DeferScans(false);
    RunWorker();
    CHECK(scanned.size() == 2);
    registry.Scan(*sda);
    CHECK(scanned.size() == 3);

    for (Storage::Disk* disk : {vda, sda, nvme_disk})
    {
        registry.Remove(*disk);
    }
    nvme_disk->Release();
}

TEST_CASE("T32: a disk removed while its scan waits is not scanned, and a full queue is survived")
{
    Registry fixture;
    Storage::DiskRegistry& registry = fixture.registry;
    FakeDisk gone(Storage::DiskKind::Ahci, ZeroImage(8, 512), 512);
    FakeDisk kept(Storage::DiskKind::Ahci, ZeroImage(8, 512), 512);

    registry.DeferScans(true);
    Storage::Disk* const removed = registry.Register(gone);
    Storage::Disk* const stays = registry.Register(kept);
    registry.Scan(*removed);
    registry.Scan(*stays);
    registry.Remove(*removed);
    CHECK(gone.releases == 1);

    // The worker's queue holds eight jobs.
    for (int index = 0; index < 8; ++index)
    {
        REQUIRE(Storage::Worker::Post(Nothing, nullptr));
    }
    registry.DeferScans(false);
    RunWorker();
    CHECK(scanned.empty());

    registry.Remove(*stays);
    CHECK(kept.releases == 1);
}

TEST_CASE("T32: the boot slots take the boot disk's names, and other disks keep the other slots")
{
    std::vector<uint8_t> ext4 = Ext4Image();
    if (ext4.empty())
    {
        SKIP("no ext4 image from mke2fs");
    }
    BootRig rig(Storage::DiskKind::Nvme, GptImage(4096, {ext4, ext4}), 4096);
    FakeDisk other_driver(Storage::DiskKind::Ahci, ZeroImage(64, 512), 512);

    REQUIRE(std::string(rig.disk->Name()) == "nvme0n1");
    CHECK(std::string(Drivers::PartitionSource(0)) == "nvme0n1");
    CHECK(std::string(Drivers::PartitionSource(1)) == "nvme0n1p1");
    CHECK(std::string(Drivers::PartitionSource(16)) == "nvme0n1p16");
    CHECK(Drivers::PartitionSource(17) == nullptr);

    Storage::DeferScans(true);
    Storage::Disk* const other = Storage::Register(other_driver);
    REQUIRE(other != nullptr);
    other->Retain();
    Storage::Scan(*rig.disk);
    Storage::Scan(*other);

    REQUIRE(VFS::MountBlockDevice());
    CHECK(RootSource() == "nvme0n1p1 ext4");
    CHECK(Drivers::SourceOf(Drivers::Volume(1)) == std::string("nvme0n1p1"));
    CHECK(Drivers::SourceOf(Drivers::Volume(2)) == std::string("nvme0n1p2"));

    const uint64_t boot_reads = rig.disk->Counters().read_sectors;
    const uint64_t other_reads = other->Counters().read_sectors;
    Storage::DeferScans(false);
    RunWorker();
    // The extra disk was scanned as another disk; the boot disk was not.
    CHECK(rig.disk->Counters().read_sectors == boot_reads);
    CHECK(other->Counters().read_sectors > other_reads);

    Storage::Remove(*other);
    RunWorker();
    other->Release();
}

TEST_CASE("T33: a 4096-byte-sector disk with a GPT ext4 volume gives the root")
{
    std::vector<uint8_t> ext4 = Ext4Image();
    if (ext4.empty())
    {
        SKIP("no ext4 image from mke2fs");
    }
    BootRig rig(Storage::DiskKind::Nvme, GptImage(4096, {ext4}), 4096);

    bool honored = false;
    REQUIRE(VFS::MountBlockDevice(1, &honored));
    CHECK(honored);
    CHECK(RootSource() == "nvme0n1p1 ext4");
    VFS::FileInfo info{};
    CHECK(VFS::Stat("/hello.txt", &info));
}

TEST_CASE("T33: FAT16 is not usable on 4096-byte sectors, so the default root is the next volume")
{
    std::vector<uint8_t> ext4 = Ext4Image();
    if (ext4.empty())
    {
        SKIP("no ext4 image from mke2fs");
    }
    BootRig rig(Storage::DiskKind::Ahci, GptImage(4096, {Fat16Image(4096), ext4}), 4096);

    bool honored = true;
    REQUIRE(VFS::MountBlockDevice(1, &honored));
    CHECK_FALSE(honored);
    CHECK(RootSource() == "sda2 ext4");
    CHECK(Drivers::Volume(1)->PartitionNumber() == 0);

    REQUIRE(VFS::MountBlockDevice(2, &honored));
    CHECK(honored);
    CHECK(RootSource() == "sda2 ext4");
}

TEST_CASE("T33: a disk with no supported volume reports failure")
{
    BootRig rig(Storage::DiskKind::Nvme, GptImage(4096, {Fat16Image(4096)}), 4096);

    bool honored = true;
    CHECK_FALSE(VFS::MountBlockDevice(1, &honored));
    CHECK_FALSE(honored);
    for (size_t number = 1; number <= Drivers::MaximumVolumes; ++number)
    {
        CHECK(Drivers::Volume(number)->PartitionNumber() == 0);
    }
    CHECK_FALSE(VFS::MountBlockDevice());
}

TEST_CASE("T34: removing the boot disk kills the boot slots, frees none and spares other disks")
{
    std::vector<uint8_t> ext4 = Ext4Image();
    if (ext4.empty())
    {
        SKIP("no ext4 image from mke2fs");
    }
    BootRig rig(Storage::DiskKind::Nvme, GptImage(4096, {ext4, ext4}), 4096);
    FakeDisk usb_driver(Storage::DiskKind::Usb, ext4, 4096);
    FakeDisk replacement(Storage::DiskKind::Nvme, ZeroImage(8, 512), 512);

    Storage::DeferScans(true);
    Storage::Disk* const usb = Storage::Register(usb_driver);
    REQUIRE(usb != nullptr);
    usb->Retain();
    Storage::Scan(*usb);
    REQUIRE(VFS::MountBlockDevice());
    Storage::DeferScans(false);
    RunWorker();
    REQUIRE(RootSource() == "nvme0n1p1 ext4");
    REQUIRE(VFS::Mount("ext4", usb->Name(), "/usb0") == VFS::MountResult::Done);
    VFS::FileInfo info{};
    REQUIRE(VFS::Stat("/hello.txt", &info));
    REQUIRE(VFS::Stat("/usb0/hello.txt", &info));
    REQUIRE_FALSE(Drivers::Volume(1)->Dead());

    Storage::Remove(*rig.disk);
    RunWorker();

    for (size_t number = 1; number <= Drivers::MaximumVolumes; ++number)
    {
        INFO(number);
        CHECK(Drivers::Volume(number)->Dead());
    }
    CHECK(VFS::IsReadOnly("/"));
    CHECK_FALSE(VFS::Stat("/hello.txt", &info));
    CHECK(Drivers::Volume(1)->PartitionNumber() == 1);
    CHECK(Drivers::SourceOf(Drivers::Volume(1)) == std::string("nvme0n1p1"));
    CHECK(Storage::BootDisk() == rig.disk);
    // Its name stays taken: no slot was freed.
    CHECK(Storage::Register(replacement) == nullptr);
    CHECK(rig.driver.releases == 0);

    CHECK(VFS::Stat("/usb0/hello.txt", &info));
    REQUIRE(VFS::Unmount("/usb0") == VFS::MountResult::Done);
    Storage::Remove(*usb);
    RunWorker();
    usb->Release();
}
