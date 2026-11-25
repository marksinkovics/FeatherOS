#ifndef FEATHEROS_TESTS_STORAGE_FAKE_NVME_H
#define FEATHEROS_TESTS_STORAGE_FAKE_NVME_H

#include "model_disk.h"

#include "kernel/memory/dma.h"
#include "kernel/storage/controller_hardware.h"
#include "kernel/storage/nvme/command.h"
#include "kernel/support/physical_memory.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace FeatherOS::Storage::Testing
{

// An NVMe controller at register level: it executes the admin and I/O commands it finds in the
// rings, posts completions with the phase bit and keeps a list of faults, the ways a driver can
// break the specification that real hardware would answer with corruption or a hang.
class FakeNvme final : public ControllerHardware
{
public:
    struct Options
    {
        uint32_t mqes = 255;
        uint32_t timeout_units = 2;
        uint32_t doorbell_stride_log2 = 0;
        uint32_t min_page_log2 = 0;
        bool nvm_command_set = true;
        uint32_t version = 0x00010400;
        uint32_t mdts = 5;
        bool volatile_cache = true;
        uint32_t namespace_count = 0;
        uint32_t rtd3_entry_latency_us = 0;
        uint16_t subsystem_vendor = 0x1AF4;
        uint16_t subsystem = 0x1100;
        InterruptKind interrupts = InterruptKind::MsiX;
        // Left running by firmware, with an admin queue of its own.
        bool firmware_enabled = false;
        // CSTS reads before RDY follows CC.EN.
        uint32_t ready_delay_reads = 0;
        bool never_ready = false;
        bool never_disable = false;
        bool fatal_on_enable = false;
        bool defer_processing = false;
        bool drop_interrupts = false;
        bool never_shutdown = false;
        uint32_t shutdown_delay_reads = 0;
        bool fail_set_queue_count = false;
        uint32_t allocations_before_failure = ~0U;
        // Abort completes with an error status instead of acting.
        bool fail_abort = false;
    };

    // What happens to a command the controller fetched and holds.
    enum class Hold : uint8_t
    {
        // Never completes; dropped when the controller stops.
        Never,
        // Completes with "aborted" when an Abort names it.
        UntilAborted,
        // Never completes, but its DMA lands at the moment the controller stops (RDY goes 0).
        LateDma,
    };

    enum class Effect : uint8_t
    {
        None,
        // CSTS.CFS is set while the command is held.
        Fatal,
        // CSTS reads all ones from then on.
        Unplug,
    };

    struct NamespaceFlags
    {
        uint16_t metadata_bytes = 0;
        uint8_t protection = 0;
        bool write_protected = false;
        bool inactive = false;
    };

    enum class Event : uint8_t
    {
        Enabled,
        Disabled,
        AdminProgrammed,
        Read,
        Write,
        Flush,
        ShutdownRequested,
        Abort,
        LateDma,
        Stopped,
    };

    struct IoRecord
    {
        Event kind;
        uint32_t nsid;
        uint64_t lba;
        uint32_t blocks;
    };

    explicit FakeNvme(FeatherOS::Testing::PhysicalMemory& memory);

    ModelDisk& AddNamespace(uint32_t nsid, uint32_t sector_size, uint64_t sectors,
                            const NamespaceFlags& flags);
    ModelDisk& AddNamespace(uint32_t nsid, uint32_t sector_size, uint64_t sectors);
    ModelDisk& NamespaceDisk(uint32_t nsid);

    uint32_t Read32(uint32_t offset) override;
    void Write32(uint32_t offset, uint32_t value) override;
    uint64_t Read64(uint32_t offset) override;
    void Write64(uint32_t offset, uint64_t value) override;
    uint32_t ReadConfig32(uint16_t offset) override;
    bool AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer) override;
    void FreeDma(const Memory::Dma::Buffer& buffer) override;
    InterruptKind SetUpInterrupts(void (*handler)(void* context), void* context) override;

    // The environment's pump calls this: with deferred processing, doorbells are applied here.
    void Pump();
    // The next count I/O commands are fetched and never completed.
    void StallIo(uint32_t count);
    void HoldIo(Hold mode, uint32_t count = 1, Effect effect = Effect::None);
    // The next count admin commands are fetched and never completed.
    void StallAdmin(uint32_t count);
    // A completion with this CID and an internal-error status comes before the next I/O one.
    void InjectStale(uint16_t cid);
    void Unplug();

    uint32_t Count(Event event) const;
    bool HoldActive() const;
    const std::vector<Event>& Events() const;
    const std::vector<IoRecord>& IoLog() const;
    const std::vector<std::string>& Faults() const;
    uint32_t Interrupts() const;
    uint32_t LiveBuffers() const;
    bool Enabled() const;
    uint32_t IoQueueEntries() const;
    uint32_t IoCommands() const;

    Options options;

private:
    struct SubmissionQueue
    {
        bool exists = false;
        uint64_t base = 0;
        uint32_t entries = 0;
        uint32_t head = 0;
        uint32_t tail = 0;
        uint32_t cqid = 0;
    };

    struct CompletionQueue
    {
        bool exists = false;
        uint64_t base = 0;
        uint32_t entries = 0;
        uint32_t tail = 0;
        uint32_t head = 0;
        bool phase = true;
        bool interrupts = false;
    };

    struct Namespace
    {
        std::unique_ptr<ModelDisk> disk;
        NamespaceFlags flags;
        uint32_t sector_size = 512;
    };

    struct Segment
    {
        volatile uint8_t* host;
        uint32_t length;
    };

    struct Outcome
    {
        uint32_t result = 0;
        uint8_t type = 0;
        uint8_t code = 0;
    };

    class Scope;
    friend class Scope;

    struct HeldCommand
    {
        Nvme::Sqe sqe{};
        uint16_t cid = 0;
        Hold mode = Hold::Never;
        bool active = false;
    };

    void Fault(const std::string& what);
    void Stop();
    void HoldCommand(const Nvme::Sqe& sqe, uint16_t cid);
    uint32_t ReadCsts();
    void WriteCc(uint32_t value);
    void WriteRegister(uint32_t offset, uint32_t value);
    void WriteDoorbell(uint32_t offset, uint32_t value);
    void Enable();
    void Disable();
    void ResetQueues();
    void Process();
    void Drain(uint32_t qid);
    Outcome Admin(const Nvme::Sqe& sqe);
    Outcome Io(const Nvme::Sqe& sqe);
    Outcome Identify(const Nvme::Sqe& sqe);
    Outcome CreateCq(const Nvme::Sqe& sqe);
    Outcome CreateSq(const Nvme::Sqe& sqe);
    Outcome Transfer(const Nvme::Sqe& sqe, bool write);
    bool Gather(uint64_t prp1, uint64_t prp2, uint32_t bytes, std::vector<Segment>* segments);
    void Initialize();
    void Post(uint32_t qid, uint16_t cid, const Outcome& outcome);
    void PostRaw(uint32_t cqid, uint16_t sq_head, uint16_t sqid, uint16_t cid, uint32_t result,
                 uint8_t type, uint8_t code);
    uint32_t NamespaceMax() const;
    Namespace* Find(uint32_t nsid);
    uint32_t Stride() const;

    FeatherOS::Testing::PhysicalMemory& memory_;
    std::map<uint32_t, Namespace> namespaces_;
    std::vector<Event> events_;
    std::vector<IoRecord> io_log_;
    std::vector<std::string> faults_;
    std::vector<Memory::Dma::Buffer> live_;
    std::vector<uint16_t> stale_;
    uint32_t allocations_ = 0;
    uint32_t io_commands_ = 0;
    uint32_t stall_ = 0;
    uint32_t stall_admin_ = 0;
    Hold hold_mode_ = Hold::Never;
    Effect hold_effect_ = Effect::None;
    HeldCommand held_;
    std::recursive_mutex lock_;
    uint32_t depth_ = 0;
    uint32_t interrupts_due_ = 0;

    bool initialized_ = false;
    uint32_t cc_ = 0;
    uint32_t aqa_ = 0;
    uint64_t asq_ = 0;
    uint64_t acq_ = 0;
    bool ready_ = false;
    bool fatal_ = false;
    bool gone_ = false;
    bool shutdown_complete_ = false;
    bool pending_doorbell_ = false;
    bool queues_created_ = false;
    uint32_t transition_reads_ = 0;
    int transition_target_ = -1;
    uint32_t shutdown_reads_ = 0;
    bool shutdown_pending_ = false;
    SubmissionQueue sq_[2];
    CompletionQueue cq_[2];
    void (*handler_)(void*) = nullptr;
    void* handler_context_ = nullptr;
    uint32_t interrupts_ = 0;
};

} // namespace FeatherOS::Storage::Testing

#endif
