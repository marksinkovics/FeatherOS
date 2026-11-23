#include <catch2/catch_test_macros.hpp>

#include "kernel/storage/nvme/command.h"
#include "kernel/storage/nvme/registers.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

namespace
{

using namespace FeatherOS::Storage::Nvme;
using FeatherDisk::Error;

static_assert(std::endian::native == std::endian::little);

// NVMe Base Specification, "Submission Queue Entry - Command Format": DW0 is opcode (7:0), FUSE
// (9:8), PSDT (15:14) and CID (31:16); DW1 NSID; DW2-3 reserved; DW4-5 MPTR; DW6-7 PRP1; DW8-9
// PRP2; DW10-15 command specific. All fields are little-endian, so a byte offset is 4 x the DWORD.
using Expected = std::initializer_list<std::pair<size_t, uint8_t>>;

std::vector<uint8_t> Bytes(const Sqe& sqe)
{
    std::vector<uint8_t> bytes(sizeof(sqe));
    std::memcpy(bytes.data(), &sqe, sizeof(sqe));
    return bytes;
}

std::vector<uint8_t> Sparse(Expected set)
{
    std::vector<uint8_t> bytes(64, 0);
    for (const auto& [offset, value] : set)
    {
        bytes[offset] = value;
    }
    return bytes;
}

constexpr size_t Dw0 = 0;
constexpr size_t Dw1 = 4;
constexpr size_t Prp1 = 24;
constexpr size_t Prp2 = 32;
constexpr size_t Dw10 = 40;
constexpr size_t Dw11 = 44;
constexpr size_t Dw12 = 48;

} // namespace

TEST_CASE("nvme T5: Identify is admin opcode 06h with CNS in CDW10", "[nvme]")
{
    // Figure "Identify command": CDW10 bits 7:0 are CNS, 01h = controller, 00h = namespace
    // (NSID in DW1), 02h = active namespace ID list.
    CHECK(Bytes(Identify(0x1234, 0, 0x01, 0x1122334455667000ULL)) ==
          Sparse({{Dw0, 0x06},
                  {Dw0 + 2, 0x34},
                  {Dw0 + 3, 0x12},
                  {Prp1 + 1, 0x70},
                  {Prp1 + 2, 0x66},
                  {Prp1 + 3, 0x55},
                  {Prp1 + 4, 0x44},
                  {Prp1 + 5, 0x33},
                  {Prp1 + 6, 0x22},
                  {Prp1 + 7, 0x11},
                  {Dw10, 0x01}}));
    CHECK(Bytes(Identify(1, 3, 0x00, 0x4000)) ==
          Sparse({{Dw0, 0x06}, {Dw0 + 2, 0x01}, {Dw1, 3}, {Prp1 + 1, 0x40}, {Dw10, 0x00}}));
    CHECK(Bytes(Identify(2, 0, 0x02, 0x4000)) ==
          Sparse({{Dw0, 0x06}, {Dw0 + 2, 0x02}, {Prp1 + 1, 0x40}, {Dw10, 0x02}}));
}

TEST_CASE("nvme T5: Create I/O Completion Queue", "[nvme]")
{
    // Opcode 05h. CDW10: QSIZE (31:16, zero-based), QID (15:0). CDW11: IV (31:16), IEN (bit 1),
    // PC (bit 0, physically contiguous).
    CHECK(Bytes(CreateIoCq(5, 1, 32, 0x10000, 0, true)) ==
          Sparse({{Dw0, 0x05},
                  {Dw0 + 2, 0x05},
                  {Prp1 + 1, 0x00},
                  {Prp1 + 2, 0x01},
                  {Dw10, 0x01},
                  {Dw10 + 2, 0x1F},
                  {Dw11, 0x03}}));
    CHECK(Bytes(CreateIoCq(6, 2, 2, 0x10000, 1, false)) ==
          Sparse({{Dw0, 0x05},
                  {Dw0 + 2, 0x06},
                  {Prp1 + 2, 0x01},
                  {Dw10, 0x02},
                  {Dw10 + 2, 0x01},
                  {Dw11, 0x01},
                  {Dw11 + 2, 0x01}}));
}

TEST_CASE("nvme T5: Create I/O Submission Queue", "[nvme]")
{
    // Opcode 01h. CDW10 as for the CQ. CDW11: CQID (31:16), QPRIO (2:1), PC (bit 0).
    CHECK(Bytes(CreateIoSq(7, 1, 32, 0x20000, 1)) ==
          Sparse({{Dw0, 0x01},
                  {Dw0 + 2, 0x07},
                  {Prp1 + 2, 0x02},
                  {Dw10, 0x01},
                  {Dw10 + 2, 0x1F},
                  {Dw11, 0x01},
                  {Dw11 + 2, 0x01}}));
}

TEST_CASE("nvme T5: Set Features, Number of Queues", "[nvme]")
{
    // Opcode 09h. CDW10: FID 07h. CDW11: NCQR (31:16) and NSQR (15:0), both zero-based.
    CHECK(Bytes(SetQueueCount(8, 0, 0)) ==
          Sparse({{Dw0, 0x09}, {Dw0 + 2, 0x08}, {Dw10, 0x07}}));
    CHECK(Bytes(SetQueueCount(8, 3, 5)) ==
          Sparse({{Dw0, 0x09}, {Dw0 + 2, 0x08}, {Dw10, 0x07}, {Dw11, 0x03}, {Dw11 + 2, 0x05}}));
}

TEST_CASE("nvme T5: Read and Write", "[nvme]")
{
    // NVM command set: Read 02h, Write 01h. CDW10-11 are SLBA (low, high dword), CDW12 bits 15:0
    // are NLB, zero-based; FUA (bit 30) and PRINFO stay clear. PSDT (DW0 bits 15:14) = 0: PRPs.
    CHECK(Bytes(*ReadWrite(false, 0x0102, 1, 0x1122334455667788ULL, 256, 0x2000, 0x3000)) ==
          Sparse({{Dw0, 0x02},
                  {Dw0 + 2, 0x02},
                  {Dw0 + 3, 0x01},
                  {Dw1, 0x01},
                  {Prp1 + 1, 0x20},
                  {Prp2 + 1, 0x30},
                  {Dw10, 0x88},
                  {Dw10 + 1, 0x77},
                  {Dw10 + 2, 0x66},
                  {Dw10 + 3, 0x55},
                  {Dw11, 0x44},
                  {Dw11 + 1, 0x33},
                  {Dw11 + 2, 0x22},
                  {Dw11 + 3, 0x11},
                  {Dw12, 0xFF}}));
    CHECK(Bytes(*ReadWrite(true, 9, 2, 8, 1, 0x2000, 0)) ==
          Sparse({{Dw0, 0x01}, {Dw0 + 2, 0x09}, {Dw1, 0x02}, {Prp1 + 1, 0x20}, {Dw10, 0x08}}));
    CHECK(Bytes(*ReadWrite(false, 9, 1, 0, 65536, 0x2000, 0)) ==
          Sparse({{Dw0, 0x02},
                  {Dw0 + 2, 0x09},
                  {Dw1, 0x01},
                  {Prp1 + 1, 0x20},
                  {Dw12, 0xFF},
                  {Dw12 + 1, 0xFF}}));
}

TEST_CASE("nvme T5: Read and Write refuse a block count the NLB field cannot hold", "[nvme]")
{
    CHECK_FALSE(ReadWrite(false, 1, 1, 0, 0, 0x2000, 0).has_value());
    CHECK_FALSE(ReadWrite(true, 1, 1, 0, 65537, 0x2000, 0).has_value());
}

TEST_CASE("nvme T5: Flush and Abort", "[nvme]")
{
    // Flush: NVM opcode 00h with the NSID. Abort: admin opcode 08h, CDW10 = CID (31:16) of the
    // command to abort and SQID (15:0).
    CHECK(Bytes(Flush(0x0A, 1)) == Sparse({{Dw0 + 2, 0x0A}, {Dw1, 0x01}}));
    CHECK(Bytes(Abort(0x0B, 0x0034, 1)) ==
          Sparse({{Dw0, 0x08}, {Dw0 + 2, 0x0B}, {Dw10, 0x01}, {Dw10 + 2, 0x34}}));
}

TEST_CASE("nvme T5: the completion entry layout", "[nvme]")
{
    // "Completion Queue Entry": DW0 command specific; DW2 SQ Head Pointer (15:0) and SQ Identifier
    // (31:16); DW3 Command Identifier (15:0), Phase Tag (16) and Status Field (31:17).
    const std::array<uint8_t, 16> raw = {0x78, 0x56, 0x34, 0x12, 0,    0,    0,    0,
                                         0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x09, 0x07};
    Cqe cqe;
    std::memcpy(&cqe, raw.data(), sizeof(cqe));
    CHECK(cqe.result == 0x12345678);
    CHECK(cqe.sq_head == 0x0102);
    CHECK(cqe.sq_id == 0x0304);
    CHECK(CompletionCid(cqe) == 0x0506);
    CHECK(CompletionPhase(cqe));
    CHECK(CompletionStatus(cqe) == 0x384);
    cqe.status = 0x0708;
    CHECK_FALSE(CompletionPhase(cqe));
    CHECK(CompletionStatus(cqe) == 0x384);
}

namespace
{

constexpr uint64_t Base = 0x40000000;

std::vector<uint64_t> Pages(size_t count, uint64_t base = Base)
{
    std::vector<uint64_t> pages(count);
    for (size_t i = 0; i < count; i++)
    {
        pages[i] = base + i * 4096;
    }
    return pages;
}

constexpr uint64_t ListPhysical = 0x7000000;

} // namespace

TEST_CASE("nvme T5: BuildPrps of one page uses PRP1 only", "[nvme]")
{
    const auto pages = Pages(1);
    std::vector<uint64_t> list(512, 0xDEAD);
    uint64_t prp1 = 1;
    uint64_t prp2 = 1;
    for (const uint32_t length : {512U, 4096U})
    {
        CAPTURE(length);
        REQUIRE(BuildPrps(pages, 0, length, ListPhysical, list, &prp1, &prp2));
        CHECK(prp1 == Base);
        CHECK(prp2 == 0);
        CHECK(list[0] == 0xDEAD);
    }
}

TEST_CASE("nvme T5: BuildPrps of two pages puts the second in PRP2", "[nvme]")
{
    const auto pages = Pages(2);
    std::vector<uint64_t> list(512, 0xDEAD);
    uint64_t prp1 = 0;
    uint64_t prp2 = 0;
    REQUIRE(BuildPrps(pages, 0, 8192, ListPhysical, list, &prp1, &prp2));
    CHECK(prp1 == Base);
    CHECK(prp2 == Base + 4096);
    CHECK(list[0] == 0xDEAD);

    REQUIRE(BuildPrps(pages, 512, 4096, ListPhysical, list, &prp1, &prp2));
    CHECK(prp1 == Base + 512);
    CHECK(prp2 == Base + 4096);
}

TEST_CASE("nvme T5: BuildPrps of 128 KiB uses a list of 31 entries", "[nvme]")
{
    const auto pages = Pages(32);
    std::vector<uint64_t> list(512, 0xDEAD);
    uint64_t prp1 = 0;
    uint64_t prp2 = 0;
    REQUIRE(BuildPrps(pages, 0, 128 * 1024, ListPhysical, list, &prp1, &prp2));
    CHECK(prp1 == Base);
    CHECK(prp2 == ListPhysical);
    for (size_t i = 0; i < 31; i++)
    {
        CAPTURE(i);
        CHECK(list[i] == Base + (i + 1) * 4096);
    }
    CHECK(list[31] == 0xDEAD);
}

TEST_CASE("nvme T5: BuildPrps counts the first page's offset", "[nvme]")
{
    std::vector<uint64_t> list(512, 0xDEAD);
    uint64_t prp1 = 0;
    uint64_t prp2 = 0;

    const auto pages = Pages(33);
    REQUIRE(BuildPrps(pages, 0x200, 128 * 1024, ListPhysical, list, &prp1, &prp2));
    CHECK(prp1 == Base + 0x200);
    CHECK(prp2 == ListPhysical);
    CHECK(list[31] == Base + 32 * 4096);
    CHECK(list[32] == 0xDEAD);

    const auto too_few = Pages(32);
    CHECK_FALSE(BuildPrps(too_few, 0x200, 128 * 1024, ListPhysical, list, &prp1, &prp2));

    const auto three = Pages(3);
    REQUIRE(BuildPrps(three, 4092, 4100, ListPhysical, list, &prp1, &prp2));
    CHECK(prp1 == Base + 4092);
    CHECK(prp2 == Base + 4096);
    REQUIRE(BuildPrps(three, 4092, 4101, ListPhysical, list, &prp1, &prp2));
    CHECK(prp2 == ListPhysical);
    CHECK(list[0] == Base + 4096);
    CHECK(list[1] == Base + 8192);
}

TEST_CASE("nvme T5: BuildPrps refuses a list longer than one page (I13)", "[nvme]")
{
    std::vector<uint64_t> list(2048, 0);
    uint64_t prp1 = 0;
    uint64_t prp2 = 0;

    const auto exact = Pages(513);
    REQUIRE(BuildPrps(exact, 0, 513 * 4096, ListPhysical, list, &prp1, &prp2));
    CHECK(prp2 == ListPhysical);
    CHECK(list[511] == Base + 512 * 4096);

    const auto over = Pages(514);
    CHECK_FALSE(BuildPrps(over, 0, 514 * 4096, ListPhysical, list, &prp1, &prp2));
    CHECK_FALSE(BuildPrps(over, 0x200, 513 * 4096, ListPhysical, list, &prp1, &prp2));
}

TEST_CASE("nvme T5: BuildPrps refuses a list buffer that is too small", "[nvme]")
{
    const auto pages = Pages(32);
    std::vector<uint64_t> list(30, 0);
    uint64_t prp1 = 0;
    uint64_t prp2 = 0;
    CHECK_FALSE(BuildPrps(pages, 0, 128 * 1024, ListPhysical, list, &prp1, &prp2));
}

TEST_CASE("nvme T5: BuildPrps refuses an unaligned page after the first (I13)", "[nvme]")
{
    std::vector<uint64_t> list(512, 0);
    uint64_t prp1 = 0;
    uint64_t prp2 = 0;

    auto two = Pages(2);
    two[1] += 8;
    CHECK_FALSE(BuildPrps(two, 0, 8192, ListPhysical, list, &prp1, &prp2));

    auto many = Pages(8);
    many[5] += 512;
    CHECK_FALSE(BuildPrps(many, 0, 8 * 4096, ListPhysical, list, &prp1, &prp2));

    auto first = Pages(2);
    first[0] += 512;
    CHECK_FALSE(BuildPrps(first, 0, 8192, ListPhysical, list, &prp1, &prp2));

    const auto fine = Pages(8);
    CHECK_FALSE(BuildPrps(fine, 0, 8 * 4096, ListPhysical + 8, list, &prp1, &prp2));
}

TEST_CASE("nvme T5: BuildPrps refuses a bad offset or length", "[nvme]")
{
    const auto pages = Pages(4);
    std::vector<uint64_t> list(512, 0);
    uint64_t prp1 = 0;
    uint64_t prp2 = 0;
    CHECK_FALSE(BuildPrps(pages, 0, 0, ListPhysical, list, &prp1, &prp2));
    CHECK_FALSE(BuildPrps(pages, 4096, 512, ListPhysical, list, &prp1, &prp2));
    CHECK_FALSE(BuildPrps(pages, 2, 512, ListPhysical, list, &prp1, &prp2));
    CHECK_FALSE(BuildPrps(std::span<const uint64_t>(), 0, 512, ListPhysical, list, &prp1, &prp2));
}

TEST_CASE("nvme T5: MapStatus", "[nvme]")
{
    // Status field = SCT (10:8) and SC (7:0), with CRD (12:11), M (13) and DNR (14) above. Generic
    // status: 00h success, 20h namespace write-protected, 80h LBA out of range.
    const auto status = [](uint8_t sct, uint8_t sc, uint16_t flags = 0)
    { return MapStatus(static_cast<uint16_t>(flags | (sct << 8) | sc)); };

    CHECK(status(0, 0x00).has_value());
    CHECK(status(0, 0x00, 0x4000).has_value());
    CHECK(status(0, 0x00, 0x2000).has_value());

    REQUIRE_FALSE(status(0, 0x80).has_value());
    CHECK(status(0, 0x80).error() == Error::InvalidArgument);
    CHECK(status(0, 0x80, 0x4000).error() == Error::InvalidArgument);
    REQUIRE_FALSE(status(0, 0x20).has_value());
    CHECK(status(0, 0x20).error() == Error::ReadOnly);

    for (const uint8_t sc : {0x01, 0x02, 0x04, 0x06, 0x07, 0x81, 0xFF})
    {
        CAPTURE(sc);
        REQUIRE_FALSE(status(0, sc).has_value());
        CHECK(status(0, sc).error() == Error::Io);
    }
    for (const uint8_t sct : {1, 2, 3, 7})
    {
        CAPTURE(sct);
        REQUIRE_FALSE(status(sct, 0x00).has_value());
        CHECK(status(sct, 0x00).error() == Error::Io);
        CHECK(status(sct, 0x80).error() == Error::Io);
        CHECK(status(sct, 0x20).error() == Error::Io);
    }
}

TEST_CASE("nvme T5: CAP decoding", "[nvme]")
{
    // CAP: MQES 15:0, CQR 16, TO 31:24 (500 ms units), DSTRD 35:32, CSS 44:37, MPSMIN 51:48,
    // MPSMAX 55:52. QEMU reports MQES 2047, TO 7, CSS 1, MPSMAX 4.
    const uint64_t cap = 0x7FFULL | (1ULL << 16) | (7ULL << 24) | (2ULL << 32) | (1ULL << 37) |
                         (4ULL << 52);
    const Capabilities c = DecodeCap(cap);
    CHECK(c.max_queue_entries == 2048);
    CHECK(c.contiguous_queues_required);
    CHECK(c.timeout_ms == 3500);
    CHECK(c.doorbell_stride == 16);
    CHECK(c.nvm_command_set);
    CHECK(c.min_page_bytes == 4096);
    CHECK(c.max_page_bytes == 65536);
    CHECK(Supports4KiBPages(c));

    const Capabilities zero = DecodeCap(0);
    CHECK(zero.max_queue_entries == 1);
    CHECK_FALSE(zero.contiguous_queues_required);
    CHECK(zero.timeout_ms == 0);
    CHECK(zero.doorbell_stride == 4);
    CHECK_FALSE(zero.nvm_command_set);

    CHECK_FALSE(Supports4KiBPages(DecodeCap(1ULL << 48)));
    CHECK(DecodeCap(0xFFULL << 24).timeout_ms == 127500);
    CHECK(DecodeCap(0xFULL << 32).doorbell_stride == (4U << 15));
}

TEST_CASE("nvme T5: doorbells, controller configuration and version", "[nvme]")
{
    CHECK(SqTailDoorbell(0, 4) == 0x1000);
    CHECK(CqHeadDoorbell(0, 4) == 0x1004);
    CHECK(SqTailDoorbell(1, 4) == 0x1008);
    CHECK(CqHeadDoorbell(1, 4) == 0x100C);
    CHECK(SqTailDoorbell(1, 16) == 0x1020);
    CHECK(CqHeadDoorbell(1, 16) == 0x1030);

    // CC: EN bit 0, IOSQES 19:16 = 6, IOCQES 23:20 = 4; AQA: ACQS 27:16, ASQS 11:0 (zero-based).
    CHECK(ControllerCc(1) == 0x00460001);
    CHECK(ControllerCc(0) == 0x00460000);
    CHECK(AdminQueueAttributes(32, 32) == 0x001F001F);
    CHECK(CcBit::ShutdownNormal == 0x4000);
    CHECK(CstsBit::ShutdownComplete == 0x8);

    // VS: MJR 31:16, MNR 15:8, TER 7:0.
    const Version v = DecodeVersion(0x00010400);
    CHECK(v.major == 1);
    CHECK(v.minor == 4);
    CHECK(v.tertiary == 0);
    CHECK(HasActiveNamespaceList(v));
    CHECK(HasActiveNamespaceList(DecodeVersion(0x00010100)));
    CHECK(HasActiveNamespaceList(DecodeVersion(0x00020000)));
    CHECK_FALSE(HasActiveNamespaceList(DecodeVersion(0x00010000)));
    CHECK_FALSE(HasActiveNamespaceList(DecodeVersion(0x00000900)));
}
