#ifndef FEATHEROS_STORAGE_NVME_COMMAND_H
#define FEATHEROS_STORAGE_NVME_COMMAND_H

#include "fs/error.h"

#include <optional>
#include <span>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Storage::Nvme
{

struct Sqe
{
    uint32_t cdw0;
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

static_assert(sizeof(Sqe) == 64);
static_assert(offsetof(Sqe, nsid) == 4);
static_assert(offsetof(Sqe, metadata) == 16);
static_assert(offsetof(Sqe, prp1) == 24);
static_assert(offsetof(Sqe, prp2) == 32);
static_assert(offsetof(Sqe, cdw10) == 40);
static_assert(offsetof(Sqe, cdw15) == 60);

// DW3 bits 31:16 are the status word: phase in bit 0, status field above it.
struct Cqe
{
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
};

static_assert(sizeof(Cqe) == 16);
static_assert(offsetof(Cqe, sq_head) == 8);
static_assert(offsetof(Cqe, sq_id) == 10);
static_assert(offsetof(Cqe, cid) == 12);
static_assert(offsetof(Cqe, status) == 14);

namespace AdminOpcode
{
constexpr uint8_t CreateIoSq = 0x01;
constexpr uint8_t CreateIoCq = 0x05;
constexpr uint8_t Identify = 0x06;
constexpr uint8_t Abort = 0x08;
constexpr uint8_t SetFeatures = 0x09;
} // namespace AdminOpcode

namespace NvmOpcode
{
constexpr uint8_t Flush = 0x00;
constexpr uint8_t Write = 0x01;
constexpr uint8_t Read = 0x02;
} // namespace NvmOpcode

namespace IdentifyCns
{
constexpr uint8_t Namespace = 0x00;
constexpr uint8_t Controller = 0x01;
constexpr uint8_t ActiveNamespaces = 0x02;
} // namespace IdentifyCns

constexpr uint8_t NumberOfQueuesFeature = 0x07;
constexpr uint32_t PageBytes = 4096;
constexpr uint32_t PrpEntriesPerPage = PageBytes / sizeof(uint64_t);
constexpr uint32_t MaxBlocksPerCommand = 65536;

uint16_t CompletionCid(const Cqe& cqe);
bool CompletionPhase(const Cqe& cqe);
uint16_t CompletionStatus(const Cqe& cqe);

Sqe Identify(uint16_t cid, uint32_t nsid, uint8_t cns, uint64_t prp1);
Sqe CreateIoCq(uint16_t cid, uint16_t qid, uint16_t entries, uint64_t prp1, uint16_t vector,
               bool interrupts);
Sqe CreateIoSq(uint16_t cid, uint16_t qid, uint16_t entries, uint64_t prp1, uint16_t cqid);
Sqe SetQueueCount(uint16_t cid, uint16_t submission_zero_based, uint16_t completion_zero_based);
std::optional<Sqe> ReadWrite(bool write, uint16_t cid, uint32_t nsid, uint64_t lba, uint32_t blocks,
                             uint64_t prp1, uint64_t prp2);
Sqe Flush(uint16_t cid, uint32_t nsid);
Sqe Abort(uint16_t cid, uint16_t target_cid, uint16_t sqid);

bool BuildPrps(std::span<const uint64_t> pages, uint32_t offset, uint32_t length,
               uint64_t list_physical, std::span<uint64_t> list, uint64_t* prp1, uint64_t* prp2);

FeatherDisk::Status MapStatus(uint16_t status_field);

} // namespace FeatherOS::Storage::Nvme

#endif
