#include <catch2/catch_test_macros.hpp>

#include "kernel/storage/disk.h"
#include "kernel/storage/nvme_machine.h"

#include <expected>
#include <string>
#include <vector>

// R4-05 (arch/x86_64/block_device.cpp:226) and R4-06 (storage/virtio_disk.cpp:30) are trace-only:
// neither file is built into featheros_unit_tests, and both depend on port I/O and PCI.
//
// R4-05 trace, legacy virtio-blk with queue-size=1024:
//   block_device.cpp:226  Read16(VirtioQueueSize) == 1024, not < QueueSize (256), so it is accepted.
//   block_device.cpp:31-38,91  driver layout: avail at 4096, used at 8192, 3 frames (12288 bytes).
//   block_device.cpp:253  PFN written; the legacy device derives its layout from its own size:
//     avail at 16*1024 = 16384, used at align4096(16384 + 6 + 2*1024) = 20480, both past the frames.
//   First request: the device reads avail.idx from foreign memory, never sees the driver's request;
//     the driver polls its own used.idx (offset 8194) up to PollLimit (line 152-157), then fails.
//   queue-size=512: avail at 8192 (the driver's used ring), used at 12288 (past the allocation).
//
// R4-06 trace, virtio-blk offered VIRTIO_BLK_F_RO (QEMU readonly=on):
//   x86_64/block_device.cpp:222 and aarch64/block_device.cpp:272 record read_only.
//   storage/block_device.h declares no accessor for it, so virtio_disk.cpp:30-38 (Info) cannot
//     report it and DiskInfo::read_only stays false.
//   disk.cpp (Register) copies that false into read_only_; Disk::Transfer's read-only check
//     (disk.cpp:150-153) does not fire; core_driver.cpp:184 sees no protected disk.
//   The arch Transfer refuses the write (x86_64:169, aarch64:217) and virtio_disk.cpp:61-64
//     returns Error::Io instead of ReadOnly, so the disk is registered and mounted as writable.

namespace
{

using namespace NvmeTest;
using FeatherDisk::Error;

constexpr uint32_t Sectors = 256;

class StubDriver final : public S::DiskDriver
{
public:
    S::DiskInfo Info() override
    {
        S::DiskInfo info = {};
        info.kind = S::DiskKind::Usb;
        info.sector_size = 512;
        info.sector_count = 64;
        info.max_transfer_bytes = 4096;
        return info;
    }

    bool Gone() override
    {
        return false;
    }

    FeatherDisk::Status Read(uint64_t, uint32_t, uint8_t*) override
    {
        return {};
    }

    FeatherDisk::Status Write(uint64_t, uint32_t, const uint8_t*) override
    {
        return {};
    }

    FeatherDisk::Status Flush() override
    {
        return {};
    }

    FeatherDisk::Status Stop() override
    {
        return {};
    }

    void Release() override {}
};

} // namespace

TEST_CASE("R4-07 a controller that dies after a revalidation leaves the reused disk alone",
          "[.repro][R4-07]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, Sectors);
    m.fake.AddNamespace(2, 512, Sectors);
    REQUIRE(m.Start());
    S::Disk* first = m.Open(1);
    S::Disk* second = m.Open(2);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    // Namespace 2 changes size across a reset, so Revalidate removes its disk but keeps the pointer.
    m.fake.AddNamespace(2, 512, Sectors / 2);
    m.fake.HoldIo(FakeNvme::Hold::Never);
    std::vector<uint8_t> back(512);
    CHECK(first->Read(0, back).error() == Error::Io);
    REQUIRE(removed == std::vector<std::string>{m.DiskName(2)});
    REQUIRE(second->Gone());

    // The last reference goes, the Disk is deleted and the next new Disk takes the same block.
    const S::Disk* stale = second;
    second->Release();
    second = nullptr;
    StubDriver driver;
    std::vector<S::Disk*> extra;
    S::Disk* held = nullptr;
    for (int i = 0; i < 12 && held == nullptr; i++)
    {
        S::Disk* other = S::Register(driver);
        REQUIRE(other != nullptr);
        S::Disk* ref = S::Acquire(other->Name());
        REQUIRE(ref == other);
        if (other == stale)
        {
            held = ref;
        }
        else
        {
            extra.push_back(ref);
        }
    }
    auto cleanup = [&] {
        for (S::Disk* d : extra)
        {
            S::Remove(*d);
            d->Release();
        }
        if (held != nullptr)
        {
            S::Remove(*held);
            held->Release();
        }
        first->Release();
    };
    // Without address reuse the stale Remove finds no entry and is harmless, so nothing shows.
    if (held == nullptr)
    {
        cleanup();
        SKIP("the allocator did not reuse the freed Disk's address");
    }

    removed.clear();
    m.fake.HoldIo(FakeNvme::Hold::Never, 1, FakeNvme::Effect::Unplug);
    CHECK(first->Read(0, back).error() == Error::Io);
    REQUIRE(m.controller->Dead());

    const bool unrelated_disk_removed = held->Gone();
    CHECK_FALSE(unrelated_disk_removed);
    CHECK(removed == std::vector<std::string>{m.DiskName(1)});

    cleanup();
}
