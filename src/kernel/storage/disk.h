#ifndef FEATHEROS_STORAGE_DISK_H
#define FEATHEROS_STORAGE_DISK_H

#include "fs/block_device.h"
#include "kernel/storage/disk_driver.h"
#include "kernel/sync/spinlock.h"
#include "user/thread_safety.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Storage
{

constexpr uint32_t MaxDisks = 8;
constexpr uint32_t MaxDiskName = 16;
constexpr uint32_t DiskLetters = 26;
constexpr uint32_t DiskNamePools = 2;

// Counted in the disk, below any cache.
struct DiskCounters
{
    uint64_t read_sectors;
    uint64_t written_sectors;
    uint64_t flushes;
};

class DiskRegistry;

// A registered disk as the filesystem core sees it. Counted by the registry until Remove, by each
// open volume and by each Acquire; the last Release frees it and drops the driver's reference.
class Disk final : public FeatherDisk::BlockDevice
{
public:
    const char* Name() const;
    DiskInfo Info() const;
    DiskDriver& Driver() const;
    // Remove was called: every transfer fails.
    bool Gone() const;
    bool Ejected() const;
    DiskCounters Counters() const;
    void Retain();
    void Release();
    FeatherDisk::Status Eject();

    uint32_t SectorSize() const override;
    uint64_t SectorCount() const override;
    FeatherDisk::Status Read(uint64_t first_sector, std::span<uint8_t> buffer) override;
    FeatherDisk::Status Write(uint64_t first_sector, std::span<const uint8_t> data) override;
    FeatherDisk::Status Flush() override;

private:
    friend class DiskRegistry;

    Disk(DiskRegistry& registry, DiskDriver& driver);
    ~Disk() = default;

    void MarkGone();
    FeatherDisk::Status Transfer(uint64_t first_sector, uint8_t* in, const uint8_t* out,
                                 uint64_t length);

    DiskRegistry& registry_;
    DiskDriver& driver_;
    std::atomic<uint32_t> references_ = 1;
    std::atomic<bool> gone_ = false;
    std::atomic<bool> ejected_ = false;
    std::atomic<bool> read_only_ = false;
    std::atomic<uint64_t> read_sectors_ = 0;
    std::atomic<uint64_t> written_sectors_ = 0;
    std::atomic<uint64_t> flushes_ = 0;
    char name_[MaxDiskName] = {};
    uint8_t pool_ = 0;
    uint8_t letter_ = 0;
};

using RemovalHandler = void (*)(Disk&);
using ScanHandler = void (*)(Disk&);
// Appends the whole lines of disk's volumes to out, at most capacity bytes; returns their length.
using VolumeLister = size_t (*)(Disk& disk, char* out, size_t capacity);

class DiskRegistry
{
public:
    constexpr DiskRegistry() = default;

    bool Initialize();
    // Takes over one reference of the driver when it returns a disk; null, with the driver
    // untouched, when it is already gone, the registry is full or no name is left.
    Disk* Register(DiskDriver& driver);
    // Idempotent. Calls the removal handler once, with no lock held.
    void Remove(Disk& disk);
    // Takes a reference on the disk called name; the caller releases it.
    Disk* Acquire(const char* name);
    // The disk registered as vda, with no reference taken: virtio disks are never removed.
    Disk* BootDisk();
    void SetRemovalHandler(RemovalHandler handler);
    void SetScanHandler(ScanHandler handler);
    void SetVolumeLister(VolumeLister lister);
    // What a disk's owner calls once it is registered and no command is in progress: the volumes
    // on it are opened. A gone disk is skipped.
    void Scan(Disk& disk);
    // Flushes every registered disk that is neither gone nor ejected; false when one failed.
    bool FlushAll();
    size_t Count();
    size_t FormatPartitions(char* out, size_t capacity);

private:
    friend class Disk;

    void Free(Disk& disk);

    Sync::SpinLock lock_;
    bool initialized_ GUARDED_BY(lock_) = false;
    Disk* disks_[MaxDisks] GUARDED_BY(lock_) = {};
    bool letters_[DiskNamePools][DiskLetters] GUARDED_BY(lock_) = {};
    std::atomic<RemovalHandler> handler_ = nullptr;
    std::atomic<ScanHandler> scan_handler_ = nullptr;
    std::atomic<VolumeLister> lister_ = nullptr;
};

DiskRegistry& Registry();

bool Initialize();
Disk* Register(DiskDriver& driver);
void Remove(Disk& disk);
Disk* Acquire(const char* name);
Disk* BootDisk();
void SetRemovalHandler(RemovalHandler handler);
void SetScanHandler(ScanHandler handler);
void SetVolumeLister(VolumeLister lister);
void Scan(Disk& disk);
bool FlushAll();
size_t DiskCount();
size_t FormatPartitions(char* out, size_t capacity);

// "sda" and 1 give "sda1"; a name ending in a digit takes a "p" first, "nvme0n1p1". Returns the
// length, or 0 when it does not fit.
size_t PartitionName(const char* disk, uint32_t number, char* out, size_t capacity);

} // namespace FeatherOS::Storage

#endif
