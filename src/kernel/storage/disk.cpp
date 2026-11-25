#include "disk.h"

#include "kernel/klogger.h"
#include "kernel/storage/worker.h"
#include "kernel/string/format.h"

#include <algorithm>
#include <expected>
#include <stdint.h>

namespace FeatherOS::Storage
{
namespace
{

constexpr char PoolPrefixes[DiskNamePools][3] = {"vd", "sd"};

uint32_t PoolOf(DiskKind kind)
{
    return kind == DiskKind::Virtio ? 0 : 1;
}

const char* KindName(DiskKind kind)
{
    switch (kind)
    {
    case DiskKind::Virtio:
        return "virtio";
    case DiskKind::Usb:
        return "usb";
    case DiskKind::Nvme:
        return "nvme";
    case DiskKind::Ahci:
        return "ahci";
    }
    return "";
}

size_t Length(const char* text)
{
    size_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

bool Equal(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

constinit DiskRegistry global_registry;
constinit KLogger logger;

} // namespace

Disk::Disk(DiskRegistry& registry, DiskDriver& driver) : registry_(registry), driver_(driver) {}

const char* Disk::Name() const
{
    return name_;
}

DiskDriver& Disk::Driver() const
{
    return driver_;
}

DiskInfo Disk::Info() const
{
    DiskInfo info = driver_.Info();
    info.read_only = info.read_only || read_only_.load(std::memory_order_relaxed);
    if (info.read_only && info.read_only_reason == ReadOnlyReason::None)
    {
        info.read_only_reason = ReadOnlyReason::Medium;
    }
    return info;
}

bool Disk::Gone() const
{
    return gone_.load(std::memory_order_acquire);
}

bool Disk::Ejected() const
{
    return ejected_.load(std::memory_order_acquire);
}

DiskCounters Disk::Counters() const
{
    return {read_sectors_.load(std::memory_order_relaxed),
            written_sectors_.load(std::memory_order_relaxed),
            flushes_.load(std::memory_order_relaxed)};
}

void Disk::Retain()
{
    references_.fetch_add(1, std::memory_order_relaxed);
}

void Disk::Release()
{
    if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        registry_.Free(*this);
    }
}

void Disk::MarkGone()
{
    gone_.store(true, std::memory_order_release);
}

uint32_t Disk::SectorSize() const
{
    return Info().sector_size;
}

uint64_t Disk::SectorCount() const
{
    return Info().sector_count;
}

FeatherDisk::Status Disk::Transfer(uint64_t first_sector, uint8_t* in, const uint8_t* out,
                                   uint64_t length)
{
    const DiskInfo info = Info();
    if (info.sector_size == 0 || length % info.sector_size != 0)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    uint64_t remaining = length / info.sector_size;
    if (first_sector > info.sector_count || remaining > info.sector_count - first_sector)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    if (remaining == 0)
    {
        return {};
    }
    if (Gone() || Ejected())
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    if (out != nullptr && info.read_only)
    {
        return std::unexpected(FeatherDisk::Error::ReadOnly);
    }
    const uint32_t per_chunk = std::max<uint32_t>(1, info.max_transfer_bytes / info.sector_size);
    uint64_t sector = first_sector;
    while (remaining != 0)
    {
        const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(remaining, per_chunk));
        const FeatherDisk::Status status =
            out != nullptr ? driver_.Write(sector, count, out) : driver_.Read(sector, count, in);
        if (!status)
        {
            if (status.error() == FeatherDisk::Error::ReadOnly)
            {
                read_only_.store(true, std::memory_order_relaxed);
            }
            return status;
        }
        const uint64_t bytes = static_cast<uint64_t>(count) * info.sector_size;
        if (out != nullptr)
        {
            written_sectors_.fetch_add(count, std::memory_order_relaxed);
            out += bytes;
        }
        else
        {
            read_sectors_.fetch_add(count, std::memory_order_relaxed);
            in += bytes;
        }
        sector += count;
        remaining -= count;
    }
    return {};
}

FeatherDisk::Status Disk::Read(uint64_t first_sector, std::span<uint8_t> buffer)
{
    return Transfer(first_sector, buffer.data(), nullptr, buffer.size());
}

FeatherDisk::Status Disk::Write(uint64_t first_sector, std::span<const uint8_t> data)
{
    return Transfer(first_sector, nullptr, data.data(), data.size());
}

FeatherDisk::Status Disk::Flush()
{
    if (Gone() || Ejected())
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    if (Info().read_only)
    {
        return {};
    }
    flushes_.fetch_add(1, std::memory_order_relaxed);
    return driver_.Flush();
}

FeatherDisk::Status Disk::Eject()
{
    if (Gone() || Ejected())
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    const FeatherDisk::Status status = driver_.Stop();
    if (status)
    {
        ejected_.store(true, std::memory_order_release);
    }
    return status;
}

void Disk::MarkEjected()
{
    ejected_.store(true, std::memory_order_release);
}

bool DiskRegistry::Initialize()
{
    Sync::SpinLockGuard guard(lock_);
    initialized_ = true;
    return true;
}

Disk* DiskRegistry::Register(DiskDriver& driver)
{
    const DiskInfo info = driver.Info();
    const bool nvme = info.kind == DiskKind::Nvme;
    const uint32_t pool = PoolOf(info.kind);
    char nvme_name[MaxDiskName] = {};
    if (nvme && !String::FormatTo(nvme_name, sizeof(nvme_name), "nvme{}n{}",
                                  static_cast<uint32_t>(info.controller_index), info.namespace_id))
    {
        return nullptr;
    }
    auto* disk = new Disk(*this, driver);
    disk->read_only_.store(info.read_only, std::memory_order_relaxed);
    {
        Sync::SpinLockGuard guard(lock_);
        uint32_t slot = MaxDisks;
        for (uint32_t index = 0; index < MaxDisks && slot == MaxDisks; ++index)
        {
            if (disks_[index] == nullptr)
            {
                slot = index;
            }
        }
        uint32_t letter = DiskLetters;
        uint32_t nvme_slot = MaxDisks;
        bool duplicate = false;
        if (nvme)
        {
            for (uint32_t index = 0; index < MaxDisks; ++index)
            {
                const NvmeName& entry = nvme_names_[index];
                if (!entry.used)
                {
                    nvme_slot = std::min(nvme_slot, index);
                }
                else if (entry.controller_index == info.controller_index &&
                         entry.namespace_id == info.namespace_id)
                {
                    duplicate = true;
                }
            }
        }
        else
        {
            for (uint32_t index = 0; index < DiskLetters && letter == DiskLetters; ++index)
            {
                if (!letters_[pool][index])
                {
                    letter = index;
                }
            }
        }
        const bool named = nvme ? nvme_slot != MaxDisks && !duplicate : letter != DiskLetters;
        if (initialized_ && slot != MaxDisks && named && !driver.Gone())
        {
            if (nvme)
            {
                nvme_names_[nvme_slot] = {true, info.controller_index, info.namespace_id};
                disk->nvme_slot_ = static_cast<uint8_t>(nvme_slot);
                std::copy(nvme_name, nvme_name + MaxDiskName, disk->name_);
            }
            else
            {
                letters_[pool][letter] = true;
                disk->lettered_ = true;
                disk->pool_ = static_cast<uint8_t>(pool);
                disk->letter_ = static_cast<uint8_t>(letter);
                disk->name_[0] = PoolPrefixes[pool][0];
                disk->name_[1] = PoolPrefixes[pool][1];
                disk->name_[2] = static_cast<char>('a' + letter);
            }
            disks_[slot] = disk;
            return disk;
        }
    }
    delete disk;
    return nullptr;
}

void DiskRegistry::Remove(Disk& disk)
{
    bool found = false;
    {
        Sync::SpinLockGuard guard(lock_);
        for (Disk*& entry : disks_)
        {
            if (entry == &disk)
            {
                entry = nullptr;
                found = true;
                disk.MarkGone();
            }
        }
    }
    if (!found)
    {
        return;
    }
    const RemovalHandler handler = handler_.load(std::memory_order_acquire);
    if (handler != nullptr)
    {
        handler(disk);
    }
    disk.Release();
}

void DiskRegistry::Free(Disk& disk)
{
    {
        Sync::SpinLockGuard guard(lock_);
        if (disk.lettered_)
        {
            letters_[disk.pool_][disk.letter_] = false;
        }
        else
        {
            nvme_names_[disk.nvme_slot_].used = false;
        }
    }
    disk.driver_.Release();
    delete &disk;
}

Disk* DiskRegistry::Acquire(const char* name)
{
    Sync::SpinLockGuard guard(lock_);
    for (Disk* disk : disks_)
    {
        if (disk != nullptr && Equal(disk->Name(), name))
        {
            disk->Retain();
            return disk;
        }
    }
    return nullptr;
}

Disk* DiskRegistry::ChooseBootDisk(const char* requested)
{
    if (requested != nullptr && requested[0] != '\0')
    {
        if (Disk* named = Acquire(requested))
        {
            return named;
        }
    }
    return Acquire("vda");
}

void DiskRegistry::SetBootDisk(Disk& disk)
{
    disk.Retain();
    Disk* previous;
    {
        Sync::SpinLockGuard guard(lock_);
        previous = boot_disk_;
        boot_disk_ = &disk;
    }
    if (previous != nullptr)
    {
        previous->Release();
    }
}

Disk* DiskRegistry::BootDisk()
{
    Sync::SpinLockGuard guard(lock_);
    return boot_disk_;
}

void DiskRegistry::SetRemovalHandler(RemovalHandler handler)
{
    handler_.store(handler, std::memory_order_release);
}

void DiskRegistry::SetScanHandler(ScanHandler handler)
{
    scan_handler_.store(handler, std::memory_order_release);
}

void DiskRegistry::SetVolumeLister(VolumeLister lister)
{
    lister_.store(lister, std::memory_order_release);
}

void DiskRegistry::Scan(Disk& disk)
{
    {
        Sync::SpinLockGuard guard(lock_);
        if (scans_deferred_)
        {
            disk.scan_pending_.store(true, std::memory_order_relaxed);
            return;
        }
    }
    const ScanHandler handler = scan_handler_.load(std::memory_order_acquire);
    if (handler != nullptr && !disk.Gone())
    {
        handler(disk);
    }
}

void DiskRegistry::DeferScans(bool deferred)
{
    Disk* due[MaxDisks] = {};
    size_t count = 0;
    {
        Sync::SpinLockGuard guard(lock_);
        scans_deferred_ = deferred;
        if (deferred)
        {
            return;
        }
        for (Disk* disk : disks_)
        {
            if (disk != nullptr && disk->scan_pending_.exchange(false, std::memory_order_relaxed) &&
                disk != boot_disk_ && !disk->Gone())
            {
                disk->Retain();
                due[count++] = disk;
            }
        }
    }
    for (size_t index = 0; index < count; ++index)
    {
        if (!Worker::Post(ScanJob, due[index]))
        {
            due[index]->Release();
            logger.Warning("Storage: the worker queue is full, a disk's volumes are not opened");
        }
    }
}

void DiskRegistry::ScanJob(void* context)
{
    auto* disk = static_cast<Disk*>(context);
    disk->registry_.Scan(*disk);
    disk->Release();
}

size_t DiskRegistry::Count()
{
    Sync::SpinLockGuard guard(lock_);
    size_t count = 0;
    for (const Disk* disk : disks_)
    {
        count += disk != nullptr ? 1 : 0;
    }
    return count;
}

bool DiskRegistry::FlushAll()
{
    Disk* held[MaxDisks] = {};
    {
        Sync::SpinLockGuard guard(lock_);
        for (uint32_t index = 0; index < MaxDisks; ++index)
        {
            held[index] = disks_[index];
            if (held[index] != nullptr)
            {
                held[index]->Retain();
            }
        }
    }
    bool all = true;
    for (Disk* disk : held)
    {
        if (disk == nullptr)
        {
            continue;
        }
        if (!disk->Gone() && !disk->Ejected() && !disk->Flush())
        {
            all = false;
        }
        disk->Release();
    }
    return all;
}

size_t DiskRegistry::FormatPartitions(char* out, size_t capacity)
{
    Disk* held[MaxDisks] = {};
    {
        Sync::SpinLockGuard guard(lock_);
        for (uint32_t index = 0; index < MaxDisks; ++index)
        {
            held[index] = disks_[index];
            if (held[index] != nullptr)
            {
                held[index]->Retain();
            }
        }
    }
    const VolumeLister lister = lister_.load(std::memory_order_acquire);
    size_t length = 0;
    for (Disk* disk : held)
    {
        if (disk == nullptr)
        {
            continue;
        }
        const DiskInfo info = disk->Info();
        const uint64_t kibibytes = info.sector_count * info.sector_size / 1024;
        char line[96];
        const char* state = info.removable ? (info.read_only ? " removable ro" : " removable rw")
                                           : (info.read_only ? " ro" : "");
        char padding[8] = "      ";
        padding[Length(disk->Name()) < 6 ? 6 - Length(disk->Name()) : 0] = '\0';
        if (String::FormatTo(line, sizeof(line), "{}{} {} {} {}{}{}\n", disk->Name(), padding,
                             kibibytes, info.sector_size, KindName(info.kind), state,
                             disk->Ejected() ? " ejected" : ""))
        {
            const size_t size = Length(line);
            if (length + size < capacity)
            {
                std::copy(line, line + size, out + length);
                length += size;
            }
        }
        if (lister != nullptr && length + 1 < capacity)
        {
            length += lister(*disk, out + length, capacity - length - 1);
        }
        disk->Release();
    }
    if (capacity != 0)
    {
        out[length] = '\0';
    }
    return length;
}

DiskRegistry& Registry()
{
    return global_registry;
}

bool Initialize()
{
    return global_registry.Initialize();
}

Disk* Register(DiskDriver& driver)
{
    return global_registry.Register(driver);
}

void Remove(Disk& disk)
{
    global_registry.Remove(disk);
}

Disk* Acquire(const char* name)
{
    return global_registry.Acquire(name);
}

Disk* ChooseBootDisk(const char* requested)
{
    return global_registry.ChooseBootDisk(requested);
}

void SetBootDisk(Disk& disk)
{
    global_registry.SetBootDisk(disk);
}

Disk* BootDisk()
{
    return global_registry.BootDisk();
}

void SetRemovalHandler(RemovalHandler handler)
{
    global_registry.SetRemovalHandler(handler);
}

void SetScanHandler(ScanHandler handler)
{
    global_registry.SetScanHandler(handler);
}

void SetVolumeLister(VolumeLister lister)
{
    global_registry.SetVolumeLister(lister);
}

void Scan(Disk& disk)
{
    global_registry.Scan(disk);
}

void DeferScans(bool deferred)
{
    global_registry.DeferScans(deferred);
}

bool FlushAll()
{
    return global_registry.FlushAll();
}

size_t DiskCount()
{
    return global_registry.Count();
}

size_t FormatPartitions(char* out, size_t capacity)
{
    return global_registry.FormatPartitions(out, capacity);
}

size_t PartitionName(const char* disk, uint32_t number, char* out, size_t capacity)
{
    const size_t length = Length(disk);
    const bool separator = length != 0 && disk[length - 1] >= '0' && disk[length - 1] <= '9';
    char digits[10];
    size_t count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + number % 10);
        number /= 10;
    } while (number != 0);
    const size_t total = length + (separator ? 1 : 0) + count;
    if (total + 1 > capacity)
    {
        return 0;
    }
    std::copy(disk, disk + length, out);
    size_t position = length;
    if (separator)
    {
        out[position++] = 'p';
    }
    while (count != 0)
    {
        out[position++] = digits[--count];
    }
    out[position] = '\0';
    return position;
}

} // namespace FeatherOS::Storage
