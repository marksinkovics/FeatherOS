#include <catch2/catch_test_macros.hpp>

#include "kernel/storage/disk.h"
#include "kernel/storage/nvme_machine.h"

#include <expected>
#include <string>
#include <vector>

namespace
{

using namespace NvmeTest;
using FeatherDisk::Error;

constexpr uint32_t Sectors = 256;

} // namespace

TEST_CASE("R4-07 a controller drops the disk it removed at revalidation", "[R4-07]")
{
    Machine m;
    m.fake.AddNamespace(1, 512, Sectors);
    m.fake.AddNamespace(2, 512, Sectors);
    REQUIRE(m.Start());
    S::Disk* first = m.Open(1);
    S::Disk* second = m.Open(2);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(m.controller->DiskAt(0) == first);
    REQUIRE(m.controller->DiskAt(1) == second);

    // Namespace 2 changes size across a reset, so Revalidate removes its disk.
    m.fake.AddNamespace(2, 512, Sectors / 2);
    m.fake.HoldIo(FakeNvme::Hold::Never);
    std::vector<uint8_t> back(512);
    CHECK(first->Read(0, back).error() == Error::Io);
    REQUIRE(removed == std::vector<std::string>{m.DiskName(2)});
    REQUIRE(second->Gone());
    CHECK(m.controller->DiskAt(0) == first);
    CHECK(m.controller->DiskAt(1) == nullptr);

    // A freed Disk's address may be reused by an unrelated disk; the dead controller must not
    // remove by it.
    second->Release();
    removed.clear();
    m.fake.HoldIo(FakeNvme::Hold::Never, 1, FakeNvme::Effect::Unplug);
    CHECK(first->Read(0, back).error() == Error::Io);
    REQUIRE(m.controller->Dead());
    CHECK(removed == std::vector<std::string>{m.DiskName(1)});
    CHECK(m.controller->DiskAt(0) == nullptr);

    first->Release();
}
