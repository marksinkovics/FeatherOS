#include <catch2/catch_test_macros.hpp>

#include "model_disk.h"

#include <vector>

using FeatherOS::Storage::Testing::ModelDisk;

namespace
{

std::vector<uint8_t> Sectors(uint32_t sector_size, uint32_t count, uint8_t value)
{
    return std::vector<uint8_t>(static_cast<size_t>(sector_size) * count, value);
}

std::vector<uint8_t> ReadBack(const ModelDisk& disk, uint64_t lba, uint32_t count)
{
    std::vector<uint8_t> bytes(static_cast<size_t>(disk.SectorSize()) * count);
    REQUIRE(disk.Read(lba, bytes));
    return bytes;
}

} // namespace

TEST_CASE("a volatile cache loses unflushed writes at power loss", "[storage][model_disk]")
{
    ModelDisk disk(512, 16, true);
    REQUIRE(disk.Write(2, Sectors(512, 2, 0xAA)));
    REQUIRE(ReadBack(disk, 2, 2) == Sectors(512, 2, 0xAA));
    REQUIRE(disk.Durable(2, 2) == Sectors(512, 2, 0));

    disk.PowerLoss();
    REQUIRE(ReadBack(disk, 2, 2) == Sectors(512, 2, 0));
    REQUIRE(disk.WriteCommands() == 1);
    REQUIRE(disk.FlushCommands() == 0);
}

TEST_CASE("a flushed write survives power loss and a later one does not", "[storage][model_disk]")
{
    ModelDisk disk(4096, 8, true);
    REQUIRE(disk.Write(1, Sectors(4096, 1, 0x11)));
    disk.Flush();
    REQUIRE(disk.Write(1, Sectors(4096, 1, 0x22)));
    REQUIRE(ReadBack(disk, 1, 1) == Sectors(4096, 1, 0x22));

    disk.PowerLoss();
    REQUIRE(ReadBack(disk, 1, 1) == Sectors(4096, 1, 0x11));
    REQUIRE(disk.Durable(1, 1) == Sectors(4096, 1, 0x11));
    REQUIRE(disk.WriteCommands() == 2);
    REQUIRE(disk.FlushCommands() == 1);
}

TEST_CASE("without a volatile cache every write is durable", "[storage][model_disk]")
{
    ModelDisk disk(512, 4, false);
    REQUIRE(disk.Write(0, Sectors(512, 1, 0x5A)));
    REQUIRE(disk.Durable(0, 1) == Sectors(512, 1, 0x5A));
    disk.PowerLoss();
    REQUIRE(ReadBack(disk, 0, 1) == Sectors(512, 1, 0x5A));
}

TEST_CASE("the model refuses ranges outside the disk and partial sectors", "[storage][model_disk]")
{
    ModelDisk disk(512, 4, true);
    std::vector<uint8_t> bytes(512);
    REQUIRE_FALSE(disk.Write(4, Sectors(512, 1, 1)));
    REQUIRE_FALSE(disk.Write(3, Sectors(512, 2, 1)));
    REQUIRE_FALSE(disk.Write(0, std::vector<uint8_t>(100, 1)));
    REQUIRE_FALSE(disk.Read(4, bytes));
    REQUIRE(disk.WriteCommands() == 0);
    REQUIRE(disk.Write(3, Sectors(512, 1, 1)));
}
