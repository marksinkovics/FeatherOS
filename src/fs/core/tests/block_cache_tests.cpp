// SPDX-License-Identifier: MIT OR Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "common/block_cache.h"
#include "tests/memory_device.h"

#include <array>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace
{

using FeatherDisk::BlockCache;
using FeatherDisk::Error;
using FeatherDisk::Tests::MemoryDevice;

constexpr std::uint32_t BlockSize = 1024;

// Every byte of block n is n plus its offset, modulo 256.
void Number(MemoryDevice& device)
{
    for (std::uint64_t sector = 0; sector < device.SectorCount(); ++sector)
    {
        for (std::size_t offset = 0; offset < 512; ++offset)
        {
            device.Sector(sector)[offset] =
                static_cast<std::uint8_t>(sector / 2 + (sector % 2) * 512 + offset);
        }
    }
}

std::uint8_t Expected(std::uint64_t block, std::size_t offset)
{
    return static_cast<std::uint8_t>(block + offset);
}

} // namespace

TEST_CASE("the block cache copies parts of blocks and keeps them for later reads")
{
    MemoryDevice device(512, 128);
    Number(device);
    BlockCache cache;
    REQUIRE(cache.Initialize(device, BlockSize, 4).has_value());
    REQUIRE(cache.BlockSize() == BlockSize);
    std::vector<std::uint8_t> scratch(BlockSize);

    std::array<std::uint8_t, 4> part{};
    REQUIRE(cache.Read(7, 1020, part, scratch).has_value());
    REQUIRE(part == std::array<std::uint8_t, 4>{Expected(7, 1020), Expected(7, 1021),
                                                Expected(7, 1022), Expected(7, 1023)});

    // A hit never reaches the device.
    device.FailReadsFrom(0);
    part = {};
    REQUIRE(cache.Read(7, 0, part, scratch).has_value());
    REQUIRE(part[3] == Expected(7, 3));
    REQUIRE(cache.Read(8, 0, part, scratch).error() == Error::Io);
}

TEST_CASE("the block cache replaces the least recently used block")
{
    MemoryDevice device(512, 128);
    Number(device);
    BlockCache cache;
    REQUIRE(cache.Initialize(device, BlockSize, 2).has_value());
    std::vector<std::uint8_t> scratch(BlockSize);
    std::array<std::uint8_t, 1> byte{};

    REQUIRE(cache.Read(1, 0, byte, scratch).has_value());
    REQUIRE(cache.Read(2, 0, byte, scratch).has_value());
    REQUIRE(cache.Read(1, 0, byte, scratch).has_value());
    // Block 2 is the older one now, so block 3 takes its place.
    REQUIRE(cache.Read(3, 0, byte, scratch).has_value());
    device.FailReadsFrom(0);
    REQUIRE(cache.Read(1, 5, byte, scratch).has_value());
    REQUIRE(byte[0] == Expected(1, 5));
    REQUIRE(cache.Read(3, 9, byte, scratch).has_value());
    REQUIRE(byte[0] == Expected(3, 9));
    REQUIRE(cache.Read(2, 0, byte, scratch).error() == Error::Io);
}

TEST_CASE("the block cache reads whole blocks into the caller's buffer and bypasses itself")
{
    MemoryDevice device(512, 128);
    Number(device);
    BlockCache cache;
    REQUIRE(cache.Initialize(device, BlockSize, 4).has_value());
    std::vector<std::uint8_t> block(BlockSize);
    REQUIRE(cache.Read(9, 0, block, block).has_value());
    REQUIRE(block[1000] == Expected(9, 1000));

    std::vector<std::uint8_t> run(3 * BlockSize);
    REQUIRE(cache.ReadUncached(20, run).has_value());
    REQUIRE(run[0] == Expected(20, 0));
    REQUIRE(run[2 * BlockSize + 17] == Expected(22, 17));
    device.FailReadsFrom(0);
    REQUIRE(cache.ReadUncached(20, std::span(run).first(BlockSize)).error() == Error::Io);
}

TEST_CASE("the block cache writes through and keeps its copies current")
{
    MemoryDevice device(512, 128);
    Number(device);
    BlockCache cache;
    REQUIRE(cache.Initialize(device, BlockSize, 4).has_value());
    std::vector<std::uint8_t> scratch(BlockSize);
    std::array<std::uint8_t, 1> byte{};
    REQUIRE(cache.Read(5, 0, byte, scratch).has_value());
    REQUIRE(cache.Read(6, 0, byte, scratch).has_value());

    // Blocks 5 to 7 at once: the device takes them, and cached 5 and 6 take the new bytes.
    std::vector<std::uint8_t> written(3 * BlockSize, 0xA5);
    written[BlockSize + 3] = 0x5A;
    REQUIRE(cache.Write(5, written).has_value());
    REQUIRE(device.Sector(10)[0] == 0xA5);
    REQUIRE(device.Sector(14)[1023 - 512] == 0xA5);
    device.FailReadsFrom(0);
    REQUIRE(cache.Read(6, 3, byte, scratch).has_value());
    REQUIRE(byte[0] == 0x5A);
    REQUIRE(cache.Read(5, 1000, byte, scratch).has_value());
    REQUIRE(byte[0] == 0xA5);
    // A block that was not cached stays out of the cache.
    REQUIRE(cache.Read(7, 0, byte, scratch).error() == Error::Io);

    // A failed write leaves the device in doubt, so the cached copy goes.
    device.read_only = true;
    REQUIRE(cache.Write(5, std::span(written).first(BlockSize)).error() == Error::ReadOnly);
    REQUIRE(cache.Read(5, 0, byte, scratch).error() == Error::Io);
    REQUIRE(cache.Write(5, std::span(written).first(100)).error() == Error::InvalidArgument);
}

TEST_CASE("the block cache refuses bad sizes and offsets")
{
    MemoryDevice device(512, 16);
    Number(device);
    BlockCache unused;
    std::vector<std::uint8_t> scratch(BlockSize);
    std::array<std::uint8_t, 4> part{};
    REQUIRE(unused.Read(0, 0, part, scratch).error() == Error::InvalidArgument);

    BlockCache cache;
    REQUIRE(cache.Initialize(device, 768, 4).error() == Error::InvalidArgument);
    REQUIRE(cache.Initialize(device, 256, 4).error() == Error::InvalidArgument);
    REQUIRE(cache.Initialize(device, BlockSize, 0).has_value());
    REQUIRE(cache.Initialize(device, BlockSize, 4).error() == Error::InvalidArgument);
    REQUIRE(cache.Read(0, 1021, part, scratch).error() == Error::InvalidArgument);
    REQUIRE(cache.Read(0, 2048, part, scratch).error() == Error::InvalidArgument);
    std::vector<std::uint8_t> short_scratch(512);
    REQUIRE(cache.Read(0, 0, part, short_scratch).error() == Error::InvalidArgument);
    std::vector<std::uint8_t> partial(BlockSize + 1);
    REQUIRE(cache.ReadUncached(0, partial).error() == Error::InvalidArgument);
}

TEST_CASE("the block cache serves several threads at once")
{
    MemoryDevice device(512, 128);
    Number(device);
    BlockCache cache;
    REQUIRE(cache.Initialize(device, BlockSize, 4).has_value());
    std::array<bool, 4> correct{};
    std::vector<std::thread> threads;
    for (std::size_t thread = 0; thread < correct.size(); ++thread)
    {
        threads.emplace_back(
            [&cache, &correct, thread]
            {
                std::vector<std::uint8_t> scratch(BlockSize);
                std::array<std::uint8_t, 2> part{};
                bool all = true;
                for (std::uint64_t round = 0; round < 2000; ++round)
                {
                    const std::uint64_t block = (round * 7 + thread) % 16;
                    const std::size_t offset = (round * 13) % (BlockSize - 1);
                    all = all && cache.Read(block, offset, part, scratch).has_value() &&
                          part[0] == Expected(block, offset) &&
                          part[1] == Expected(block, offset + 1);
                }
                correct[thread] = all;
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    REQUIRE(correct == std::array<bool, 4>{true, true, true, true});
}
