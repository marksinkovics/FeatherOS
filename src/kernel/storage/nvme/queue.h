#ifndef FEATHEROS_STORAGE_NVME_QUEUE_H
#define FEATHEROS_STORAGE_NVME_QUEUE_H

#include "kernel/memory/dma.h"
#include "kernel/storage/controller_hardware.h"
#include "kernel/storage/nvme/command.h"

#include <stdint.h>

namespace FeatherOS::Storage::Nvme
{

constexpr uint16_t NoCid = 0xFFFF;

struct StaleCompletions
{
    uint32_t count;
    uint16_t last_cid;
};

// One submission and completion queue pair. The owner serializes every call, with one lock for
// both the submit and the reap side, because the SQ head comes back in completions.
class QueuePair
{
public:
    void Initialize(ControllerHardware& hardware, uint16_t qid, uint16_t entries,
                    uint32_t doorbell_stride, const Memory::Dma::Buffer& sq,
                    const Memory::Dma::Buffer& cq);
    // Zeroes both rings and starts over; the device must not be using them.
    void Reset();
    // Stamps a new CID into the command and rings the tail doorbell; NoCid when the ring is full.
    uint16_t Submit(Sqe sqe);
    // Consumes every posted completion. One with wanted_cid goes to out and makes the result
    // true; every other one is counted in stale and dropped.
    bool Reap(uint16_t wanted_cid, Cqe* out, StaleCompletions* stale);
    uint16_t Entries() const;

private:
    uint16_t NextCid();

    ControllerHardware* hardware_ = nullptr;
    uint16_t qid_ = 0;
    uint16_t entries_ = 0;
    uint32_t stride_ = 0;
    Memory::Dma::Buffer sq_{};
    Memory::Dma::Buffer cq_{};
    uint16_t tail_ = 0;
    uint16_t sq_head_ = 0;
    uint16_t head_ = 0;
    uint16_t next_cid_ = 0;
    bool phase_ = true;
};

} // namespace FeatherOS::Storage::Nvme

#endif
