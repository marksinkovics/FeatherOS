#include <catch2/catch_test_macros.hpp>

#include "kernel/storage/block_device.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/virtio_disk.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

namespace S = FeatherOS::Storage;
using FeatherDisk::Error;

struct FakeVirtio
{
    bool offers_read_only = false;
    bool initialized = true;
    std::vector<uint64_t> writes;
};

FakeVirtio fake;

S::Disk* scanned = nullptr;

void OnScan(S::Disk& disk)
{
    scanned = &disk;
}

class VirtioRegistered
{
public:
    explicit VirtioRegistered(bool read_only)
    {
        fake = {};
        fake.offers_read_only = read_only;
        scanned = nullptr;
        S::Initialize();
        S::SetScanHandler(OnScan);
        REQUIRE(FeatherOS::BlockDevice::RegisterVirtioDisk());
        disk = scanned;
        REQUIRE(disk != nullptr);
        disk->Retain();
    }

    VirtioRegistered(const VirtioRegistered&) = delete;
    VirtioRegistered& operator=(const VirtioRegistered&) = delete;

    ~VirtioRegistered()
    {
        S::Remove(*disk);
        disk->Release();
        S::SetScanHandler(nullptr);
    }

    S::Disk* disk = nullptr;
};

} // namespace

namespace FeatherOS::BlockDevice
{

bool Initialize(const void*, uint64_t)
{
    return fake.initialized;
}

uint64_t SectorCount()
{
    return 64;
}

bool IsReadOnly()
{
    return fake.offers_read_only;
}

bool ReadSector(uint64_t, void* buffer)
{
    std::memset(buffer, 0, SectorSize);
    return true;
}

bool WriteSector(uint64_t sector, const void*)
{
    if (fake.offers_read_only)
    {
        return false;
    }
    fake.writes.push_back(sector);
    return true;
}

bool Flush()
{
    return true;
}

} // namespace FeatherOS::BlockDevice

TEST_CASE("R4-06 a virtio-blk disk that offers VIRTIO_BLK_F_RO is registered read-only", "[R4-06]")
{
    const VirtioRegistered registered(true);
    const S::DiskInfo info = registered.disk->Info();
    CHECK(info.read_only);
    CHECK(info.read_only_reason == S::ReadOnlyReason::Medium);

    std::array<uint8_t, 512> sector{};
    CHECK(registered.disk->Write(3, sector).error() == Error::ReadOnly);
    CHECK(fake.writes.empty());
    CHECK(registered.disk->Read(3, sector));
}

TEST_CASE("R4-06 a virtio-blk disk without the read-only feature stays writable", "[R4-06]")
{
    const VirtioRegistered registered(false);
    const S::DiskInfo info = registered.disk->Info();
    CHECK_FALSE(info.read_only);
    CHECK(info.read_only_reason == S::ReadOnlyReason::None);

    std::array<uint8_t, 512> sector{};
    CHECK(registered.disk->Write(3, sector));
    CHECK(fake.writes == std::vector<uint64_t>{3});
}
