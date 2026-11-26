#ifndef FEATHEROS_STORAGE_NVME_CONTROLLER_H
#define FEATHEROS_STORAGE_NVME_CONTROLLER_H

#include "kernel/memory/dma.h"
#include "kernel/storage/controller_hardware.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/environment.h"
#include "kernel/storage/nvme/command.h"
#include "kernel/storage/nvme/identify.h"
#include "kernel/storage/nvme/namespace.h"
#include "kernel/storage/nvme/queue.h"
#include "kernel/storage/nvme/registers.h"
#include "kernel/storage/write_policy.h"
#include "kernel/sync/mutex.h"
#include "kernel/sync/spinlock.h"
#include "user/thread_safety.h"

#include <atomic>
#include <stdint.h>

namespace FeatherOS::Storage::Nvme
{

constexpr uint32_t IoTimeoutMs = 30000;
constexpr uint32_t AdminTimeoutMs = 10000;
constexpr uint32_t WaitStepMs = 10;
constexpr uint32_t MaxIoQueueEntries = 32;
constexpr uint32_t MaxAdminQueueEntries = 32;
constexpr uint32_t MaxProbedNamespaces = 32;
constexpr uint32_t AbortWaitMs = 1000;
constexpr uint32_t ResetWindowMs = 60000;

// One command in flight per controller (I9): every transfer goes through the bounce buffer
// under io_lock_. Lock order: io_lock_, then admin_lock_, then completion_lock_.
class Controller
{
public:
    static Controller* Create(ControllerHardware& hardware, Environment& environment,
                              uint16_t index, WritePolicy policy, const char* location);
    ~Controller();

    // The worker's job: false when the controller was refused.
    bool Start();
    FeatherDisk::Status Io(const Namespace& ns, bool write, uint64_t sector, uint32_t count,
                           uint8_t* in, const uint8_t* out);
    FeatherDisk::Status FlushNamespace(const Namespace& ns);
    void Shutdown();
    bool Dead() const;
    static void OnInterrupt(void* context);

    uint32_t StaleCount() const;
    uint32_t AbortCount() const;
    uint32_t ResetCount() const;
    uint64_t write_commands() const;
    uint64_t flush_commands() const;
    uint32_t NamespaceCount() const;
    Namespace& NamespaceAt(uint32_t index);
    // The registered disk of namespace index; null once it was removed or never registered.
    Disk* DiskAt(uint32_t index) const;

private:
    enum class Which : uint8_t
    {
        Admin,
        Io,
    };

    enum class Outcome : uint8_t
    {
        Done,
        TimedOut,
        Full,
    };

    enum class Wait : uint8_t
    {
        Reached,
        Timeout,
        Fatal,
    };

    struct Pending
    {
        uint16_t cid;
        bool active;
        bool done;
        Cqe cqe;
    };

    struct Waiter
    {
        Controller* controller;
        Which which;
    };

    struct RegisterWait
    {
        Controller* controller;
        uint32_t mask;
        uint32_t value;
        bool check_fatal;
        bool fatal;
    };

    Controller(ControllerHardware& hardware, Environment& environment, uint16_t index,
               WritePolicy policy, const char* location);

    bool BringUp();
    bool BringUpLocked() REQUIRES(admin_lock_);
    bool Refuse(const char* reason) REQUIRES(admin_lock_);
    bool Disable(bool wait_for_ready);
    Wait WaitRegister(uint32_t mask, uint32_t value, bool check_fatal, uint32_t timeout_ms);
    Wait WaitReady(bool want, uint32_t timeout_ms);
    bool AllocateBuffers();
    bool AllocateTransferBuffers();
    void ReleaseBuffers();
    bool IdentifyController() REQUIRES(admin_lock_);
    bool CreateQueues() REQUIRES(admin_lock_);
    bool DiscoverNamespaces() REQUIRES(admin_lock_);
    bool AddNamespace(uint32_t nsid, bool quiet) REQUIRES(admin_lock_);
    void LogPolicy();
    void LogController();
    void Publish();

    FeatherDisk::Status Admin(const Sqe& sqe, Cqe* completion) REQUIRES(admin_lock_);
    FeatherDisk::Status IoCommand(const Sqe& sqe) REQUIRES(io_lock_);
    Outcome Execute(Which which, const Sqe& sqe, Cqe* completion, uint32_t budget_ms,
                    uint16_t* cid = nullptr);
    bool AwaitCompletion(Which which, uint64_t deadline_ms);
    bool ReapPending(Which which);
    void LogStale();
    uint64_t Prp2For(uint32_t bytes) const;
    // The ladder for a command that timed out: CSTS, Abort, controller reset, dead.
    void Recover(uint16_t cid) REQUIRES(io_lock_);
    bool AbortCommand(uint16_t cid) REQUIRES(io_lock_);
    void ResetOrDie() REQUIRES(io_lock_);
    bool Reset() REQUIRES(io_lock_);
    bool ResetLocked() REQUIRES(io_lock_, admin_lock_);
    bool Revalidate() REQUIRES(io_lock_, admin_lock_);
    void MakeDead(bool disable) REQUIRES(io_lock_);
    static bool ReadyCommand(void* context);
    static bool ReadyRegister(void* context);

    ControllerHardware& hardware_;
    Environment& environment_;
    const uint16_t index_;
    const WritePolicy policy_;
    char location_[MaxDiskLocation] = {};

    Sync::Mutex io_lock_;
    Sync::Mutex admin_lock_;
    Sync::SpinLock completion_lock_;
    QueuePair admin_queue_ GUARDED_BY(completion_lock_);
    QueuePair io_queue_ GUARDED_BY(completion_lock_);
    Pending admin_pending_ GUARDED_BY(completion_lock_) = {};
    Pending io_pending_ GUARDED_BY(completion_lock_) = {};
    uint16_t last_stale_cid_ GUARDED_BY(completion_lock_) = 0;
    uint64_t last_reset_ms_ GUARDED_BY(io_lock_) = 0;

    Capabilities caps_{};
    uint32_t version_ = 0;
    ControllerData data_{};
    InterruptKind interrupts_ = InterruptKind::Polling;
    uint32_t io_depth_ = 0;
    uint32_t bounce_bytes_ = 0;
    bool buffers_pinned_ = false;
    Memory::Dma::Buffer admin_sq_{};
    Memory::Dma::Buffer admin_cq_{};
    Memory::Dma::Buffer io_sq_{};
    Memory::Dma::Buffer io_cq_{};
    Memory::Dma::Buffer identify_{};
    Memory::Dma::Buffer prp_list_{};
    Memory::Dma::Buffer bounce_{};
    uint8_t identify_copy_[IdentifyBytes] = {};
    uint32_t active_list_[MaxProbedNamespaces] = {};

    Namespace namespaces_[MaxNamespaces];
    uint32_t namespace_count_ = 0;
    std::atomic<Disk*> disks_[MaxNamespaces] = {};

    std::atomic<bool> started_ = false;
    std::atomic<bool> dead_ = false;
    std::atomic<bool> shutting_down_ = false;
    std::atomic<uint32_t> stale_completions_ = 0;
    std::atomic<uint32_t> aborts_ = 0;
    std::atomic<uint32_t> resets_ = 0;
    std::atomic<uint64_t> write_commands_ = 0;
    std::atomic<uint64_t> flush_commands_ = 0;
    std::atomic<bool> stale_logged_ = false;
};

} // namespace FeatherOS::Storage::Nvme

#endif
