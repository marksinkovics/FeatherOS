#ifndef FEATHEROS_STORAGE_DISK_DRIVER_H
#define FEATHEROS_STORAGE_DISK_DRIVER_H

#include "fs/error.h"

#include <stdint.h>

namespace FeatherOS::Storage
{

enum class DiskKind : uint8_t
{
    Virtio,
    Usb,
};

constexpr uint32_t MaxDiskModel = 48;
constexpr uint32_t MaxDiskLocation = 16;

struct DiskInfo
{
    DiskKind kind;
    uint32_t sector_size;
    uint64_t sector_count;
    uint32_t max_transfer_bytes;
    bool removable;
    bool read_only;
    char model[MaxDiskModel];
    char location[MaxDiskLocation];
};

// What a transport implements. Calls are synchronous and the driver serializes them itself; a
// buffer holds count sectors, and every sector must lie on the disk.
class DiskDriver
{
public:
    virtual DiskInfo Info() = 0;
    // True once the hardware is gone; the registry refuses such a driver.
    virtual bool Gone() = 0;
    virtual FeatherDisk::Status Read(uint64_t sector, uint32_t count, uint8_t* buffer) = 0;
    virtual FeatherDisk::Status Write(uint64_t sector, uint32_t count, const uint8_t* buffer) = 0;
    virtual FeatherDisk::Status Flush() = 0;
    // Ejects the medium: later calls fail.
    virtual FeatherDisk::Status Stop() = 0;
    // Drops one reference; the last one frees the driver.
    virtual void Release() = 0;

protected:
    DiskDriver() = default;
    ~DiskDriver() = default;
    DiskDriver(const DiskDriver&) = default;
    DiskDriver& operator=(const DiskDriver&) = default;
};

} // namespace FeatherOS::Storage

#endif
