#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "kernel/files.h"
#include "kernel/filesystem/core_driver.h"
#include "kernel/filesystem/drivers.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/log_ring.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/worker.h"
#include "kernel/sync/mutex.h"
#include "support/scheduler_stubs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Volumes of USB disks (sda, sdb, ...) beside the boot disk's, opened by the storage worker and
// killed when their disk goes. The tests run the worker's jobs by hand.
namespace
{

namespace Drivers = FeatherOS::Filesystem::Drivers;
namespace Storage = FeatherOS::Storage;
namespace User = FeatherOS::User;
namespace VFS = FeatherOS::VFS;
using FeatherOS::Filesystem::CoreDriver;

constexpr uint32_t SectorBytes = 512;
constexpr uint32_t VolumeSectors = 5000;
constexpr uint32_t FatSectors = 20;
constexpr std::string_view FileText = "usb stick contents";

class FakeUsbDisk final : public Storage::DiskDriver
{
public:
    explicit FakeUsbDisk(std::vector<uint8_t> bytes, uint32_t sector_size = SectorBytes)
        : image(std::move(bytes)), sector_size(sector_size)
    {
    }

    Storage::DiskInfo Info() override
    {
        Storage::DiskInfo info = {};
        info.kind = Storage::DiskKind::Usb;
        info.sector_size = sector_size;
        info.sector_count = image.size() / sector_size;
        info.max_transfer_bytes = 64 * 1024;
        info.removable = removable;
        info.read_only = read_only;
        info.read_only_reason = reason;
        return info;
    }

    bool Gone() override
    {
        return false;
    }

    FeatherDisk::Status Read(uint64_t sector, uint32_t count, uint8_t* buffer) override
    {
        ++reads;
        std::memcpy(buffer, image.data() + sector * sector_size, uint64_t{count} * sector_size);
        if (on_read)
        {
            std::exchange(on_read, nullptr)();
        }
        return {};
    }

    FeatherDisk::Status Write(uint64_t sector, uint32_t count, const uint8_t* buffer) override
    {
        ++writes;
        std::memcpy(image.data() + sector * sector_size, buffer, uint64_t{count} * sector_size);
        std::lock_guard guard(log_mutex);
        write_log.emplace_back(sector, count);
        return {};
    }

    FeatherDisk::Status Flush() override
    {
        const uint64_t number = ++flushes;
        if (fail_flush || (fail_flush_after != 0 && number > fail_flush_after))
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
        return {};
    }

    FeatherDisk::Status Stop() override
    {
        ++stops;
        if (fail_stop)
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
        return {};
    }

    void Release() override
    {
        ++releases;
    }

    uint64_t Calls() const
    {
        return reads + writes + flushes;
    }

    std::atomic<uint64_t> reads = 0;
    std::atomic<uint64_t> writes = 0;
    std::atomic<uint64_t> flushes = 0;
    std::atomic<int> releases = 0;
    std::atomic<bool> removable = true;
    std::atomic<bool> read_only = false;
    Storage::ReadOnlyReason reason = Storage::ReadOnlyReason::None;
    std::atomic<bool> fail_flush = false;
    // Flushes past this count fail; 0 for none.
    std::atomic<uint64_t> fail_flush_after = 0;
    std::atomic<bool> fail_stop = false;
    std::atomic<int> stops = 0;
    // Runs once, inside the first Read that follows.
    std::function<void()> on_read;

    // The (first sector, count) of every Write, in order.
    std::vector<std::pair<uint64_t, uint32_t>> Writes()
    {
        std::lock_guard guard(log_mutex);
        return write_log;
    }

    uint8_t Byte(uint64_t offset) const
    {
        return image[offset];
    }

private:
    std::mutex log_mutex;
    std::vector<std::pair<uint64_t, uint32_t>> write_log;
    std::vector<uint8_t> image;
    uint32_t sector_size;
};

void Put16(uint8_t* at, uint32_t value)
{
    at[0] = static_cast<uint8_t>(value);
    at[1] = static_cast<uint8_t>(value >> 8);
}

void Put32(uint8_t* at, uint32_t value)
{
    Put16(at, value);
    Put16(at + 2, value >> 16);
}

void FormatFat16(std::vector<uint8_t>& image, uint32_t start, uint32_t count)
{
    uint8_t* boot = image.data() + uint64_t{start} * SectorBytes;
    Put16(boot + 11, SectorBytes);
    boot[13] = 1;
    Put16(boot + 14, 1);
    boot[16] = 1;
    Put16(boot + 17, 16);
    Put16(boot + 19, count);
    boot[21] = 0xF8;
    Put16(boot + 22, FatSectors);
    boot[510] = 0x55;
    boot[511] = 0xAA;
    uint8_t* fat = boot + SectorBytes;
    Put16(fat, 0xFFF8);
    Put16(fat + 2, 0xFFFF);
    Put16(fat + 4, 0xFFF8);
    uint8_t* root = fat + FatSectors * SectorBytes;
    std::memcpy(root, "BLOCK   TXT", 11);
    root[11] = 0x20;
    Put16(root + 26, 2);
    Put32(root + 28, static_cast<uint32_t>(FileText.size()));
    std::memcpy(root + SectorBytes, FileText.data(), FileText.size());
}

void SetEntry(std::vector<uint8_t>& image, uint32_t entry, uint8_t type, uint32_t start,
              uint32_t count)
{
    uint8_t* table = image.data() + 446 + 16 * entry;
    table[4] = type;
    Put32(table + 8, start);
    Put32(table + 12, count);
    image[510] = 0x55;
    image[511] = 0xAA;
}

// An MBR with FAT16 volumes of type types[0], types[1]... of count sectors each.
std::vector<uint8_t> PartitionedImage(std::vector<uint8_t> types = {0x06, 0x04},
                                      uint32_t count = VolumeSectors)
{
    std::vector<uint8_t> image((1 + uint64_t{count} * types.size()) * SectorBytes);
    for (uint32_t index = 0; index < types.size(); ++index)
    {
        const uint32_t start = 1 + index * count;
        SetEntry(image, index, types[index], start, count);
        FormatFat16(image, start, count);
    }
    return image;
}

std::vector<uint8_t> SuperfloppyImage()
{
    std::vector<uint8_t> image(uint64_t{VolumeSectors} * SectorBytes);
    FormatFat16(image, 0, VolumeSectors);
    return image;
}

// The storage handlers belong to these tests alone: the USB driver tests count the commands a disk
// receives.
struct Handlers
{
    Handlers()
    {
        if (users++ == 0)
        {
            Storage::Initialize();
            Drivers::AttachStorage();
        }
    }

    ~Handlers()
    {
        if (--users == 0)
        {
            Storage::SetRemovalHandler(nullptr);
            Storage::SetScanHandler(nullptr);
            Storage::SetVolumeLister(nullptr);
        }
    }

    Handlers(const Handlers&) = delete;
    Handlers& operator=(const Handlers&) = delete;

    static inline int users = 0;
};

// Registered, with an extra reference so the test can look at the disk after Remove; the worker's
// jobs run before the reference goes.
class Rig
{
public:
    explicit Rig(std::vector<uint8_t> image, uint32_t sector_size = SectorBytes)
        : driver(std::move(image), sector_size), disk(Storage::Register(driver))
    {
        REQUIRE(disk != nullptr);
        disk->Retain();
    }

    ~Rig()
    {
        Storage::Remove(*disk);
        while (FeatherOS::Storage::Worker::RunNext())
        {
        }
        disk->Release();
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void Unplug()
    {
        Storage::Remove(*disk);
    }

    // "sda1" for 1; the disk's own name for 0.
    std::string Source(uint32_t number = 0) const
    {
        char name[Drivers::MaximumSourceLength];
        if (number == 0)
        {
            return disk->Name();
        }
        Storage::PartitionName(disk->Name(), number, name, sizeof(name));
        return name;
    }

    Handlers handlers;
    FakeUsbDisk driver;
    Storage::Disk* disk;
};

void RunWorker()
{
    while (FeatherOS::Storage::Worker::RunNext())
    {
    }
}


std::string ReadFile(const char* path)
{
    VFS::FileHandle file{};
    if (!VFS::Open(path, &file))
    {
        return "<open failed>";
    }
    char buffer[64] = {};
    uint64_t count = 0;
    const bool read = VFS::Read(&file, buffer, sizeof(buffer), &count);
    VFS::Close(&file);
    return read ? std::string(buffer, count) : "<read failed>";
}

std::string Partitions()
{
    char text[FeatherOS::Procfs::MaximumPartitionsFileSize];
    Storage::FormatPartitions(text, sizeof(text));
    return text;
}

bool Listed(const std::string& text, const char* line_start)
{
    return text.find(std::string("\n") + line_start) != std::string::npos ||
           text.rfind(line_start, 0) == 0;
}

bool IsReadOnly(const char* point)
{
    FeatherOS::Procfs::MountStatus status{};
    for (uint32_t index = 0; VFS::DescribeMount(index, &status); ++index)
    {
        if (std::string(status.point) == point)
        {
            return status.read_only;
        }
    }
    return false;
}

const char* SourceAt(const char* point)
{
    return Drivers::SourceOf(VFS::MountedAt(point));
}

CoreDriver& VolumeAt(const char* point)
{
    CoreDriver* volume = Drivers::VolumeOf(VFS::MountedAt(point));
    REQUIRE(volume != nullptr);
    return *volume;
}

// A type nothing registered yet is as good as a source nothing has.
bool Absent(VFS::MountResult result)
{
    return result == VFS::MountResult::NotFound || result == VFS::MountResult::UnknownType;
}

size_t LogLines(const std::string& needle)
{
    static char text[FeatherOS::KernelLog::Capacity];
    const size_t length = FeatherOS::KernelLog::Snapshot(text, sizeof(text));
    const std::string all(text, length);
    size_t count = 0;
    for (size_t at = all.find(needle); at != std::string::npos; at = all.find(needle, at + 1))
    {
        ++count;
    }
    return count;
}

// FAT[1], the status word of a volume that starts at start in an image of FakeUsbDisk.
uint64_t StatusOffset(uint32_t start)
{
    return (uint64_t{start} + 1) * SectorBytes + 2;
}

uint16_t StatusWord(FakeUsbDisk& driver, uint32_t start)
{
    return static_cast<uint16_t>(driver.Byte(StatusOffset(start)) |
                                 (driver.Byte(StatusOffset(start) + 1) << 8));
}

bool Reaches(FakeUsbDisk& driver, uint32_t start)
{
    const uint64_t sector = start + 1;
    for (const auto& [first, count] : driver.Writes())
    {
        if (first <= sector && sector < first + count)
        {
            return true;
        }
    }
    return false;
}

bool Created(const char* path)
{
    VFS::FileHandle file{};
    if (!VFS::Create(path, &file))
    {
        return false;
    }
    // Data allocates a cluster, which writes the FAT sector that holds FAT[1].
    uint64_t written = 0;
    const bool wrote = VFS::Write(&file, "created", 7, &written) && written == 7;
    VFS::Close(&file);
    return wrote;
}

bool NoWorkingDirectory(const char* /*point*/)
{
    return false;
}

bool EveryWorkingDirectory(const char* /*point*/)
{
    return true;
}

bool Mounted(const char* point)
{
    return VFS::MountedAt(point) != nullptr;
}

constexpr auto Mount = [](const char* type, const char* source, const char* point)
{ return VFS::Mount(type, source, point); };

} // namespace

TEST_CASE("the volumes of a USB disk are opened, listed and mounted by name")
{
    Rig rig(PartitionedImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);

    const std::string partitions = Partitions();
    INFO(partitions);
    REQUIRE(Listed(partitions, (rig.Source() + " ").c_str()));
    REQUIRE(Listed(partitions, rig.Source(1).c_str()));
    REQUIRE(Listed(partitions, rig.Source(2).c_str()));
    REQUIRE(partitions.find("2500 512 fat16") != std::string::npos);

    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE(Mount("fat16", ("/dev/" + rig.Source(2)).c_str(), "/usb2") == VFS::MountResult::Done);
    REQUIRE(std::string(SourceAt("/usb1")) == rig.Source(1));
    REQUIRE(std::string(SourceAt("/usb2")) == rig.Source(2));
    REQUIRE(ReadFile("/usb1/BLOCK.TXT") == FileText);
    REQUIRE(ReadFile("/usb2/BLOCK.TXT") == FileText);
    REQUIRE_FALSE(IsReadOnly("/usb1"));
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(VFS::Unmount("/usb2") == VFS::MountResult::Done);

    REQUIRE(Absent(Mount("fat16", "sdz1", "/usb1")));
    REQUIRE(Mount("fat16", "vda9", "/usb1") == VFS::MountResult::NotFound);
}

TEST_CASE("a filesystem on the whole USB disk is the volume named like the disk")
{
    Rig rig(SuperfloppyImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 1);
    const std::string partitions = Partitions();
    INFO(partitions);
    REQUIRE(Listed(partitions, (rig.Source() + " ").c_str()));
    REQUIRE_FALSE(Listed(partitions, rig.Source(1).c_str()));
    REQUIRE(Mount("fat16", rig.Source().c_str(), "/usb0") == VFS::MountResult::Done);
    REQUIRE(ReadFile("/usb0/BLOCK.TXT") == FileText);
    REQUIRE(VFS::Unmount("/usb0") == VFS::MountResult::Done);
}

TEST_CASE("a disk with no filesystem opens no volume")
{
    Rig rig(std::vector<uint8_t>(64 * SectorBytes));
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 0);
    REQUIRE(Absent(Mount("fat16", rig.Source().c_str(), "/usb0")));
}

TEST_CASE("FAT16 on a 4096-byte-sector disk is refused")
{
    std::vector<uint8_t> image(uint64_t{VolumeSectors} * SectorBytes);
    FormatFat16(image, 0, VolumeSectors);
    Rig rig(std::move(image), 4096);
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 0);
}

TEST_CASE("an MBR partition of type 0x04 is a FAT16 volume")
{
    Rig rig(PartitionedImage({0x04}));
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 1);
    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE(ReadFile("/usb1/BLOCK.TXT") == FileText);
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
}

TEST_CASE("overlap compares ranges on one disk only, and the boot disk keeps its slots")
{
    FeatherOS::Storage::Disk* const boot = Storage::BootDisk();
    REQUIRE(boot != nullptr);
    Rig rig(PartitionedImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    const CoreDriver& volume = VolumeAt("/usb1");
    const FeatherDisk::Partition::Range same = {1, VolumeSectors, 1};

    REQUIRE(volume.Overlaps(*rig.disk, same));
    REQUIRE_FALSE(volume.Overlaps(*boot, same));
    REQUIRE_FALSE(volume.Overlaps(*rig.disk, {1 + VolumeSectors, 10, 2}));
    REQUIRE(volume.Overlaps(*rig.disk, {VolumeSectors, 10, 2}));

    for (size_t number = 1; number <= Drivers::MaximumVolumes; ++number)
    {
        CoreDriver::Summary summary;
        REQUIRE(Drivers::Volume(number) != &volume);
        REQUIRE_FALSE(Drivers::Volume(number)->Summarize(*rig.disk, &summary));
    }
    REQUIRE(Drivers::Volume(0) == nullptr);
    REQUIRE(Drivers::Volume(Drivers::MaximumVolumes + 1) == nullptr);
    REQUIRE(Drivers::VolumeOf(&volume) == &volume);
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
}

TEST_CASE("a dead volume fails every operation before the device, shows ro, and unmounts with no I/O")
{
    Rig rig(PartitionedImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    VFS::FileHandle open_file{};
    REQUIRE(VFS::Open("/usb1/BLOCK.TXT", &open_file));
    CoreDriver& volume = VolumeAt("/usb1");

    rig.Unplug();
    const uint64_t calls = rig.driver.Calls();
    REQUIRE(volume.Dead());
    REQUIRE(IsReadOnly("/usb1"));
    {
        FeatherOS::Sync::VolumeHoldBase hold(volume.VolumeLock());
        FeatherOS::Filesystem::Node node{};
        uint64_t size = 0;
        REQUIRE_FALSE(volume.Open("BLOCK.TXT", &node, &size));
        REQUIRE(volume.LastError() == User::SyscallErrorInputOutput);
        REQUIRE_FALSE(volume.Sync());
        REQUIRE(volume.LastError() == User::SyscallErrorInputOutput);
        REQUIRE_FALSE(volume.Create("NEW.TXT", &node, &size));
        REQUIRE(volume.LastError() == User::SyscallErrorInputOutput);
    }
    char buffer[8];
    uint64_t count = 0;
    REQUIRE_FALSE(VFS::Read(&open_file, buffer, sizeof(buffer), &count));
    FeatherOS::VFS::FileInfo info{};
    REQUIRE_FALSE(VFS::Stat("/usb1/BLOCK.TXT", &info));
    REQUIRE(ReadFile("/usb1/BLOCK.TXT") == "<open failed>");
    VFS::FileHandle created{};
    REQUIRE_FALSE(VFS::Create("/usb1/NEW.TXT", &created));
    REQUIRE(VFS::LastError() == User::SyscallErrorInputOutput);
    REQUIRE_FALSE(VFS::SyncAll());
    REQUIRE(rig.driver.Calls() == calls);

    REQUIRE(Mount("fat16", rig.Source(2).c_str(), "/usb2") == VFS::MountResult::NoDevice);
    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb2") == VFS::MountResult::NoDevice);

    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Busy);
    VFS::Close(&open_file);
    REQUIRE(Mount("fat16", rig.Source(2).c_str(), "/usb2") == VFS::MountResult::NoDevice);
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(rig.driver.Calls() == calls);

    RunWorker();
    REQUIRE(Absent(Mount("fat16", rig.Source(1).c_str(), "/usb1")));
    REQUIRE(Absent(Mount("fat16", rig.Source(2).c_str(), "/usb2")));
    REQUIRE(rig.driver.releases == 0);
    REQUIRE(rig.driver.Calls() == calls);
}

TEST_CASE("USB T28 a dead volume answers EIO to the system calls that would say not found")
{
    Rig rig(PartitionedImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE_FALSE(VFS::IsDead("/usb1/BLOCK.TXT"));
    FeatherOS::Files::Table table{};
    const uint64_t descriptor = FeatherOS::Files::Open(&table, "/usb1/BLOCK.TXT", User::OpenRead);
    REQUIRE_FALSE(User::IsSyscallError(descriptor));
    const uint64_t written =
        FeatherOS::Files::Open(&table, "/usb1/OPEN.TXT", User::OpenWrite | User::OpenCreate);
    REQUIRE_FALSE(User::IsSyscallError(written));
    REQUIRE(FeatherOS::Files::Open(&table, "/usb1/MISSING.TXT", User::OpenRead) ==
            User::SyscallErrorNotFound);

    rig.Unplug();
    const uint64_t calls = rig.driver.Calls();
    REQUIRE(VFS::IsDead("/usb1/BLOCK.TXT"));
    REQUIRE_FALSE(VFS::IsDead("/tmp"));
    char buffer[8] = {};
    REQUIRE(User::IsSyscallError(FeatherOS::Files::Read(&table, descriptor, buffer, sizeof(buffer))));
    REQUIRE(FeatherOS::Files::Write(&table, written, buffer, sizeof(buffer)) ==
            User::SyscallErrorInputOutput);
    REQUIRE(FeatherOS::Files::MakeDirectory("/usb1/DIR") == User::SyscallErrorInputOutput);
    REQUIRE(FeatherOS::Files::Open(&table, "/usb1/BLOCK.TXT", User::OpenRead) ==
            User::SyscallErrorInputOutput);
    REQUIRE(FeatherOS::Files::Open(&table, "/usb1/NEW.TXT", User::OpenWrite | User::OpenCreate) ==
            User::SyscallErrorInputOutput);
    User::FileStatus status = {};
    REQUIRE(FeatherOS::Files::Stat("/usb1/BLOCK.TXT", &status) == User::SyscallErrorInputOutput);
    User::DirectoryEntry entry = {};
    REQUIRE(FeatherOS::Files::ReadDirectory("/usb1", 0, &entry) == User::SyscallErrorInputOutput);
    REQUIRE(FeatherOS::Files::Remove("/usb1/BLOCK.TXT") == User::SyscallErrorInputOutput);
    FeatherOS::Files::CloseAll(&table);
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(rig.driver.Calls() == calls);
}

TEST_CASE("USB T28 a disk unplugged during the volume scan logs nothing about its volumes")
{
    const size_t before = LogLines("no supported filesystem");
    {
        Rig rig(PartitionedImage());
        rig.driver.on_read = [&rig] { rig.Unplug(); };
        REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 0);
        RunWorker();
        REQUIRE(Absent(Mount("fat16", rig.Source(1).c_str(), "/usb1")));
    }
    REQUIRE(LogLines("no supported filesystem") == before);
}

TEST_CASE("the worker forgets unmounted dead volumes and keeps a mounted one until umount")
{
    std::string reused;
    {
        Rig rig(PartitionedImage());
        REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
        REQUIRE(Mount("fat16", rig.Source(2).c_str(), "/usb2") == VFS::MountResult::Done);
        rig.Unplug();
        RunWorker();
        REQUIRE(Absent(Mount("fat16", rig.Source(1).c_str(), "/usb1")));
        REQUIRE(VolumeAt("/usb2").Dead());
        REQUIRE_FALSE(Listed(Partitions(), rig.Source(2).c_str()));
        REQUIRE(VFS::Unmount("/usb2") == VFS::MountResult::Done);
        REQUIRE(Absent(Mount("fat16", rig.Source(2).c_str(), "/usb2")));
        REQUIRE(rig.driver.releases == 0);
        reused = rig.Source();
    }
    Rig again(PartitionedImage());
    REQUIRE(again.Source() == reused);
    REQUIRE(Drivers::OpenDiskVolumes(*again.disk) == 2);
    REQUIRE(Mount("fat16", again.Source(2).c_str(), "/usb2") == VFS::MountResult::Done);
    REQUIRE(VFS::Unmount("/usb2") == VFS::MountResult::Done);
}

TEST_CASE("the disk is freed once its last volume slot is released")
{
    const Handlers handlers;
    auto* driver = new FakeUsbDisk(PartitionedImage());
    FeatherOS::Storage::Disk* disk = Storage::Register(*driver);
    REQUIRE(disk != nullptr);
    disk->Retain();
    REQUIRE(Drivers::OpenDiskVolumes(*disk) == 2);
    char source[Drivers::MaximumSourceLength];
    Storage::PartitionName(disk->Name(), 1, source, sizeof(source));
    REQUIRE(Mount("fat16", source, "/usb1") == VFS::MountResult::Done);
    Storage::Remove(*disk);
    disk->Release();
    RunWorker();
    REQUIRE(driver->releases == 0);
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(driver->releases == 1);
    delete driver;
}

TEST_CASE("more volumes than slots are not opened")
{
    const Handlers handlers;
    std::vector<std::unique_ptr<Rig>> rigs;
    size_t opened = 0;
    for (int disk = 0; disk < 5; ++disk)
    {
        rigs.push_back(std::make_unique<Rig>(PartitionedImage({0x06, 0x06, 0x06, 0x06}, 4200)));
        opened += Drivers::OpenDiskVolumes(*rigs.back()->disk);
    }
    REQUIRE(opened == Drivers::MaximumOtherVolumes);
}

TEST_CASE("the storage worker scanning while an unplug happens leaves no volume behind",
          "[concurrency]")
{
    const Handlers handlers;
    for (int round = 0; round < 40; ++round)
    {
        auto* driver = new FakeUsbDisk(PartitionedImage());
        FeatherOS::Storage::Disk* disk = Storage::Register(*driver);
        REQUIRE(disk != nullptr);
        disk->Retain();
        std::atomic<size_t> opened = 0;
        std::thread scanner([&] { opened = Drivers::OpenDiskVolumes(*disk); });
        std::thread unplug(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::microseconds(round * 60));
                Storage::Remove(*disk);
            });
        scanner.join();
        unplug.join();
        RunWorker();
        char source[Drivers::MaximumSourceLength];
        Storage::PartitionName(disk->Name(), 1, source, sizeof(source));
        REQUIRE(Absent(Mount("fat16", source, "/usb1")));
        disk->Release();
        REQUIRE(driver->releases == 1);
        delete driver;
    }
}

TEST_CASE("tasks reading two volumes of a drive while a third unplugs it never see wrong data",
          "[concurrency]")
{
    Rig rig(PartitionedImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE(Mount("fat16", rig.Source(2).c_str(), "/usb2") == VFS::MountResult::Done);

    std::atomic<bool> stop = false;
    std::atomic<int> good = 0;
    std::atomic<int> corrupt = 0;
    const auto reader = [&](const char* path)
    {
        while (!stop)
        {
            const std::string text = ReadFile(path);
            if (text == FileText)
            {
                ++good;
            }
            else if (text.rfind("<", 0) != 0)
            {
                ++corrupt;
            }
        }
    };
    std::thread first(reader, "/usb1/BLOCK.TXT");
    std::thread second(reader, "/usb2/BLOCK.TXT");
    std::thread unplug(
        [&]
        {
            while (good < 20)
            {
                std::this_thread::yield();
            }
            rig.Unplug();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            stop = true;
        });
    first.join();
    second.join();
    unplug.join();
    REQUIRE(corrupt == 0);
    REQUIRE(ReadFile("/usb1/BLOCK.TXT") == "<open failed>");
    REQUIRE(ReadFile("/usb2/BLOCK.TXT") == "<open failed>");
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(VFS::Unmount("/usb2") == VFS::MountResult::Done);
}

TEST_CASE("detaching a disk takes no volume lock", "[concurrency]")
{
    Rig rig(PartitionedImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    CoreDriver& volume = VolumeAt("/usb1");
    const int violations = TestScheduler::volume_violations;

    std::atomic<bool> holding = false;
    std::atomic<bool> release = false;
    std::thread holder(
        [&]
        {
            FeatherOS::Sync::VolumeHoldBase hold(volume.VolumeLock());
            holding = true;
            while (!release)
            {
                std::this_thread::yield();
            }
        });
    while (!holding)
    {
        std::this_thread::yield();
    }
    std::atomic<bool> removed = false;
    std::thread manager(
        [&]
        {
            rig.Unplug();
            removed = true;
        });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!removed && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    const bool finished = removed;
    release = true;
    holder.join();
    manager.join();
    REQUIRE(finished);
    REQUIRE(volume.Dead());
    REQUIRE(TestScheduler::volume_violations == violations);
    RunWorker();
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
}

TEST_CASE("USB T33 a removable FAT16 volume with a status bit clear mounts read-only and is never written")
{
    struct Bit
    {
        const char* name;
        uint8_t high_byte_mask;
    };
    const Bit bit = GENERATE(Bit{"bit 15", 0x80}, Bit{"bit 14", 0x40}, Bit{"both", 0xC0});
    INFO(bit.name);
    std::vector<uint8_t> image = PartitionedImage();
    constexpr uint32_t FirstStart = 1;
    constexpr uint32_t SecondStart = 1 + VolumeSectors;
    image[StatusOffset(FirstStart) + 1] &= static_cast<uint8_t>(~bit.high_byte_mask);
    Rig rig(image);
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    const std::string message = "VFS: " + rig.Source(1) +
                                " was not cleanly unmounted (FAT16 dirty bits); mounted "
                                "read-only, run fsck.fat";
    const size_t logged = LogLines(message);

    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE(LogLines(message) == logged + 1);
    REQUIRE(IsReadOnly("/usb1"));
    REQUIRE(ReadFile("/usb1/BLOCK.TXT") == FileText);
    REQUIRE_FALSE(Created("/usb1/NEW.TXT"));
    REQUIRE(VFS::LastError() == User::SyscallErrorReadOnly);
    REQUIRE(VFS::Remount("/usb1", FeatherOS::Filesystem::MountMode::ReadWrite) ==
            VFS::MountResult::ReadOnly);
    REQUIRE(IsReadOnly("/usb1"));
    REQUIRE_FALSE(Created("/usb1/NEW.TXT"));
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(rig.driver.Writes().empty());

    REQUIRE(Mount("fat16", rig.Source(2).c_str(), "/usb2") == VFS::MountResult::Done);
    REQUIRE_FALSE(IsReadOnly("/usb2"));
    REQUIRE(Created("/usb2/NEW.TXT"));
    REQUIRE(VFS::Unmount("/usb2") == VFS::MountResult::Done);
    REQUIRE_FALSE(rig.driver.Writes().empty());
    REQUIRE(Reaches(rig.driver, SecondStart));
    REQUIRE_FALSE(Reaches(rig.driver, FirstStart));
    REQUIRE(StatusWord(rig.driver, SecondStart) == 0xFFFF);
    REQUIRE((StatusWord(rig.driver, FirstStart) & (bit.high_byte_mask << 8)) == 0);
}

TEST_CASE("USB T33 the same dirty volume on a non-removable disk mounts read-write and keeps its bits")
{
    std::vector<uint8_t> image = PartitionedImage();
    image[StatusOffset(1) + 1] &= 0x3F;
    Rig rig(image);
    rig.driver.removable = false;
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    const size_t logged = LogLines("was not cleanly unmounted");

    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE_FALSE(IsReadOnly("/usb1"));
    REQUIRE(Created("/usb1/NEW.TXT"));
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(LogLines("was not cleanly unmounted") == logged);
    REQUIRE(Reaches(rig.driver, 1));
    REQUIRE(StatusWord(rig.driver, 1) == 0x3FFF);
}

TEST_CASE("USB T33 a removable FAT16 volume that was never dirty stays writable and keeps clean bits")
{
    Rig rig(SuperfloppyImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 1);
    REQUIRE(Mount("fat16", rig.Source().c_str(), "/usb0") == VFS::MountResult::Done);
    REQUIRE_FALSE(IsReadOnly("/usb0"));
    REQUIRE(Created("/usb0/NEW.TXT"));
    REQUIRE(VFS::Unmount("/usb0") == VFS::MountResult::Done);
    REQUIRE(StatusWord(rig.driver, 0) == 0xFFFF);
}

TEST_CASE("USB I13 a write-protected disk's volumes mount read-only and receive no write")
{
    Rig rig(PartitionedImage());
    rig.driver.read_only = true;
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    const std::string message = "VFS: " + rig.Source(1) + " is write-protected, mounted read-only";
    const size_t logged = LogLines(message);

    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE(LogLines(message) == logged + 1);
    REQUIRE(IsReadOnly("/usb1"));
    REQUIRE(ReadFile("/usb1/BLOCK.TXT") == FileText);
    REQUIRE_FALSE(Created("/usb1/NEW.TXT"));
    REQUIRE(VFS::LastError() == User::SyscallErrorReadOnly);
    REQUIRE(VFS::Remount("/usb1", FeatherOS::Filesystem::MountMode::ReadWrite) ==
            VFS::MountResult::ReadOnly);
    REQUIRE(IsReadOnly("/usb1"));
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(rig.driver.Writes().empty());
}

TEST_CASE("M16 I22 a policy read-only disk's volume logs the policy, not write protection")
{
    Rig rig(PartitionedImage());
    rig.driver.read_only = true;
    rig.driver.reason = Storage::ReadOnlyReason::Policy;
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    const std::string policy = "VFS: " + rig.Source(1) +
                               " is read-only by the storage policy (storage=rw to write), "
                               "mounted read-only";
    const std::string protection = "VFS: " + rig.Source(1) + " is write-protected";
    const size_t logged = LogLines(policy);
    const size_t protected_lines = LogLines(protection);

    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE(LogLines(policy) == logged + 1);
    REQUIRE(LogLines(protection) == protected_lines);
    REQUIRE(IsReadOnly("/usb1"));
    REQUIRE(VFS::Remount("/usb1", FeatherOS::Filesystem::MountMode::ReadWrite) ==
            VFS::MountResult::ReadOnly);
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(rig.driver.Writes().empty());
}

TEST_CASE("USB I13 a write-protected superfloppy is named like its disk in the log")
{
    Rig rig(SuperfloppyImage());
    rig.driver.read_only = true;
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 1);
    const std::string message = "VFS: " + rig.Source() + " is write-protected, mounted read-only";
    const size_t logged = LogLines(message);
    REQUIRE(Mount("fat16", rig.Source().c_str(), "/usb0") == VFS::MountResult::Done);
    REQUIRE(LogLines(message) == logged + 1);
    REQUIRE(IsReadOnly("/usb0"));
    REQUIRE(VFS::Unmount("/usb0") == VFS::MountResult::Done);
}

TEST_CASE("USB T20 sync and unmount of a USB volume each reach the drive's flush, and a failure is reported")
{
    Rig rig(PartitionedImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE(Created("/usb1/NEW.TXT"));

    uint64_t flushes = rig.driver.flushes;
    REQUIRE(VFS::SyncAll());
    REQUIRE(rig.driver.flushes == flushes + 1);
    REQUIRE(VFS::SyncAll());
    REQUIRE(rig.driver.flushes == flushes + 2);

    rig.driver.fail_flush = true;
    REQUIRE_FALSE(VFS::SyncAll());
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Failed);
    REQUIRE(VFS::MountedAt("/usb1") != nullptr);
    REQUIRE(ReadFile("/usb1/BLOCK.TXT") == FileText);

    rig.driver.fail_flush = false;
    flushes = rig.driver.flushes;
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(rig.driver.flushes == flushes + 1);
    REQUIRE(VFS::MountedAt("/usb1") == nullptr);
}

TEST_CASE("USB T20 detaching every mount at shutdown flushes each volume")
{
    Rig rig(PartitionedImage());
    REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
    REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
    REQUIRE(Mount("fat16", rig.Source(2).c_str(), "/usb2") == VFS::MountResult::Done);
    const uint64_t flushes = rig.driver.flushes;
    REQUIRE(VFS::DetachAll(false));
    REQUIRE(rig.driver.flushes == flushes + 2);
    REQUIRE(rig.disk->Flush());
    REQUIRE(rig.driver.flushes == flushes + 3);
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(VFS::Unmount("/usb2") == VFS::MountResult::Done);
}

namespace
{

struct EjectRig
{
    EjectRig() : rig(PartitionedImage())
    {
        REQUIRE(Drivers::OpenDiskVolumes(*rig.disk) == 2);
        REQUIRE(Mount("fat16", rig.Source(1).c_str(), "/usb1") == VFS::MountResult::Done);
        REQUIRE(Mount("fat16", rig.Source(2).c_str(), "/usb2") == VFS::MountResult::Done);
    }

    ~EjectRig()
    {
        for (const char* point : {"/usb1", "/usb2"})
        {
            if (Mounted(point))
            {
                VFS::Unmount(point);
            }
        }
    }

    EjectRig(const EjectRig&) = delete;
    EjectRig& operator=(const EjectRig&) = delete;

    VFS::MountResult Eject(VFS::MountInUse in_use = nullptr)
    {
        return VFS::EjectDisk(rig.Source().c_str(), in_use);
    }

    Rig rig;
};

} // namespace

TEST_CASE("USB T21 I25 one busy mount means no mount of the disk is unmounted")
{
    EjectRig fixture;
    VFS::FileHandle open_file{};
    REQUIRE(VFS::Open("/usb2/BLOCK.TXT", &open_file));
    const uint64_t flushes = fixture.rig.driver.flushes;

    REQUIRE(fixture.Eject() == VFS::MountResult::Busy);
    REQUIRE(Mounted("/usb1"));
    REQUIRE(Mounted("/usb2"));
    REQUIRE(fixture.rig.driver.flushes == flushes);
    REQUIRE(fixture.rig.driver.stops == 0);
    REQUIRE_FALSE(fixture.rig.disk->Ejected());
    REQUIRE(ReadFile("/usb1/BLOCK.TXT") == FileText);

    VFS::Close(&open_file);
    REQUIRE(fixture.Eject(EveryWorkingDirectory) == VFS::MountResult::Busy);
    REQUIRE(Mounted("/usb1"));
    REQUIRE(Mounted("/usb2"));
    REQUIRE(fixture.rig.driver.stops == 0);

    REQUIRE(Mount("fat16", fixture.rig.Source(1).c_str(), "/usb3") == VFS::MountResult::Busy);
    REQUIRE(VFS::Unmount("/usb1") == VFS::MountResult::Done);
    REQUIRE(Mount("fat16", fixture.rig.Source(1).c_str(), "/usb3") == VFS::MountResult::Done);
    REQUIRE(VFS::Unmount("/usb3") == VFS::MountResult::Done);
}

TEST_CASE("USB T21 eject unmounts, flushes, stops and marks the disk ejected; a second call is 0")
{
    EjectRig fixture;
    REQUIRE(Created("/usb1/NEW.TXT"));
    const uint64_t flushes = fixture.rig.driver.flushes;
    const size_t logged = LogLines("Storage: sda: ejected, safe to unplug");

    REQUIRE(fixture.Eject(NoWorkingDirectory) == VFS::MountResult::Done);
    REQUIRE_FALSE(Mounted("/usb1"));
    REQUIRE_FALSE(Mounted("/usb2"));
    REQUIRE(fixture.rig.driver.flushes >= flushes + 3);
    REQUIRE(fixture.rig.driver.stops == 1);
    REQUIRE(fixture.rig.disk->Ejected());
    REQUIRE(LogLines("Storage: sda: ejected, safe to unplug") == logged + 1);
    REQUIRE(Created("/usb1/NEW.TXT") == false);

    const std::string partitions = Partitions();
    INFO(partitions);
    REQUIRE(partitions.find("ejected") != std::string::npos);
    REQUIRE_FALSE(Listed(partitions, fixture.rig.Source(1).c_str()));

    REQUIRE(Mount("fat16", fixture.rig.Source(1).c_str(), "/usb1") == VFS::MountResult::NoDevice);
    REQUIRE(Mount("fat16", fixture.rig.Source().c_str(), "/usb1") == VFS::MountResult::NoDevice);
    REQUIRE(Absent(Mount("fat16", "sdz1", "/usb1")));

    const uint64_t calls = fixture.rig.driver.Calls();
    REQUIRE(fixture.Eject() == VFS::MountResult::Done);
    REQUIRE(fixture.rig.driver.stops == 1);
    REQUIRE(fixture.rig.driver.Calls() == calls);
    REQUIRE(VFS::EjectDisk(("/dev/" + fixture.rig.Source()).c_str()) == VFS::MountResult::Done);
}

TEST_CASE("USB T21 a disk that is not removable or unknown cannot be ejected")
{
    EjectRig fixture;
    REQUIRE(VFS::EjectDisk("sdz") == VFS::MountResult::NotFound);
    REQUIRE(VFS::EjectDisk("sda1") == VFS::MountResult::NotFound);
    REQUIRE(VFS::EjectDisk("") == VFS::MountResult::NotFound);
    fixture.rig.driver.removable = false;
    REQUIRE(fixture.Eject() == VFS::MountResult::NotSupported);
    REQUIRE(Mounted("/usb1"));
    REQUIRE(fixture.rig.driver.stops == 0);
    REQUIRE_FALSE(fixture.rig.disk->Ejected());
    fixture.rig.driver.removable = true;
    fixture.rig.Unplug();
    REQUIRE(fixture.Eject() == VFS::MountResult::NotFound);
}

TEST_CASE("USB T21 eject answers the system call's errors")
{
    EjectRig fixture;
    const std::string name = fixture.rig.Source();
    REQUIRE(FeatherOS::Files::Eject(name.c_str(), EveryWorkingDirectory) == User::SyscallErrorBusy);
    REQUIRE(Mounted("/usb1"));
    REQUIRE(FeatherOS::Files::Eject("sdz", NoWorkingDirectory) == User::SyscallErrorNotFound);
    fixture.rig.driver.removable = false;
    REQUIRE(FeatherOS::Files::Eject(name.c_str(), NoWorkingDirectory) ==
            User::SyscallErrorOperationNotSupported);
    fixture.rig.driver.removable = true;
    fixture.rig.driver.fail_flush = true;
    REQUIRE(FeatherOS::Files::Eject(name.c_str(), NoWorkingDirectory) ==
            User::SyscallErrorInputOutput);
    fixture.rig.driver.fail_flush = false;
    REQUIRE(FeatherOS::Files::Eject(name.c_str(), NoWorkingDirectory) == 0);
    REQUIRE(FeatherOS::Files::Eject(name.c_str(), NoWorkingDirectory) == 0);
    REQUIRE(FeatherOS::Files::Eject(nullptr, NoWorkingDirectory) == User::SyscallErrorInvalidArgument);
}

TEST_CASE("USB T21 I24 a flush that fails does not stop or mark the drive, and the mounts done stay unmounted")
{
    SECTION("the first sync fails: every mount stays")
    {
        EjectRig fixture;
        fixture.rig.driver.fail_flush = true;
        REQUIRE(fixture.Eject() == VFS::MountResult::Failed);
        REQUIRE(Mounted("/usb1"));
        REQUIRE(Mounted("/usb2"));
        REQUIRE(fixture.rig.driver.stops == 0);
        REQUIRE_FALSE(fixture.rig.disk->Ejected());
        fixture.rig.driver.fail_flush = false;
        REQUIRE(ReadFile("/usb1/BLOCK.TXT") == FileText);
        REQUIRE(fixture.Eject() == VFS::MountResult::Done);
        REQUIRE(fixture.rig.driver.stops == 1);
    }
    SECTION("the final flush fails: the mounts stay unmounted")
    {
        EjectRig fixture;
        fixture.rig.driver.fail_flush_after = fixture.rig.driver.flushes + 2;
        REQUIRE(fixture.Eject() == VFS::MountResult::Failed);
        REQUIRE_FALSE(Mounted("/usb1"));
        REQUIRE_FALSE(Mounted("/usb2"));
        REQUIRE(fixture.rig.driver.stops == 0);
        REQUIRE_FALSE(fixture.rig.disk->Ejected());
        fixture.rig.driver.fail_flush_after = 0;
        REQUIRE(fixture.Eject() == VFS::MountResult::Done);
        REQUIRE(fixture.rig.driver.stops == 1);
        REQUIRE(fixture.rig.disk->Ejected());
    }
}

TEST_CASE("USB T21 a drive that refuses to stop is logged and the disk is ejected anyway")
{
    EjectRig fixture;
    fixture.rig.driver.fail_stop = true;
    const size_t refused = LogLines("Storage: sda: the drive refused to stop");
    REQUIRE(fixture.Eject() == VFS::MountResult::Done);
    REQUIRE(fixture.rig.driver.stops == 1);
    REQUIRE(fixture.rig.disk->Ejected());
    REQUIRE(LogLines("Storage: sda: the drive refused to stop") == refused + 1);
    REQUIRE(Mount("fat16", fixture.rig.Source(1).c_str(), "/usb1") == VFS::MountResult::NoDevice);
}
