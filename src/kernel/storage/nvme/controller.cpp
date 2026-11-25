#include "controller.h"

#include "kernel/klogger.h"
#include "kernel/string/format.h"

#include <algorithm>
#include <expected>
#include <optional>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Storage::Nvme
{
namespace
{

constexpr uint32_t MinimumTimeoutMs = 500;
constexpr uint32_t ShutdownMinimumMs = 5000;
constexpr uint32_t ShutdownCapMs = 60000;
constexpr uint32_t Gone32 = 0xFFFFFFFF;
constexpr uint32_t PageSize = PageBytes;

constinit KLogger logger;

enum class Level : uint8_t
{
    Info,
    Warning,
};

template <typename... Args>
void Say(Level level, uint16_t index, const char* format, const Args&... args)
{
    char text[144];
    char line[192];
    if (!String::FormatTo(text, sizeof(text), format, args...) ||
        !String::FormatTo(line, sizeof(line), "Storage: nvme{}: {}", index, text))
    {
        return;
    }
    if (level == Level::Warning)
    {
        logger.Warning(line);
    }
    else
    {
        logger.Info(line);
    }
}

template <typename... Args> void SayDisk(const char* name, const char* format, const Args&... args)
{
    char text[144];
    char line[192];
    if (String::FormatTo(text, sizeof(text), format, args...) &&
        String::FormatTo(line, sizeof(line), "Storage: {}: {}", name, text))
    {
        logger.Info(line);
    }
}

uint8_t* Bytes(const Memory::Dma::Buffer& buffer)
{
    return const_cast<uint8_t*>(buffer.address);
}

void CopyText(char* out, size_t capacity, const char* text)
{
    size_t i = 0;
    for (; i + 1 < capacity && text[i] != '\0'; i++)
    {
        out[i] = text[i];
    }
    out[i] = '\0';
}

} // namespace

Controller* Controller::Create(ControllerHardware& hardware, Environment& environment,
                               uint16_t index, WritePolicy policy, const char* location)
{
    return new Controller(hardware, environment, index, policy, location);
}

Controller::Controller(ControllerHardware& hardware, Environment& environment, uint16_t index,
                       WritePolicy policy, const char* location)
    : hardware_(hardware), environment_(environment), index_(index), policy_(policy)
{
    CopyText(location_, sizeof(location_), location);
}

Controller::~Controller()
{
    if (started_.load(std::memory_order_acquire) && !Dead())
    {
        buffers_pinned_ = !Disable(false);
    }
    ReleaseBuffers();
}

bool Controller::Dead() const
{
    return dead_.load(std::memory_order_acquire);
}

uint64_t Controller::write_commands() const
{
    return write_commands_.load(std::memory_order_relaxed);
}

uint64_t Controller::flush_commands() const
{
    return flush_commands_.load(std::memory_order_relaxed);
}

uint32_t Controller::StaleCount() const
{
    return stale_completions_.load(std::memory_order_relaxed);
}

uint32_t Controller::AbortCount() const
{
    return aborts_.load(std::memory_order_relaxed);
}

uint32_t Controller::ResetCount() const
{
    return resets_.load(std::memory_order_relaxed);
}

uint32_t Controller::NamespaceCount() const
{
    return namespace_count_;
}

Namespace& Controller::NamespaceAt(uint32_t index)
{
    return namespaces_[index];
}

void Controller::OnInterrupt(void* context)
{
    auto* controller = static_cast<Controller*>(context);
    controller->environment_.Wake(controller);
}

bool Controller::ReadyRegister(void* context)
{
    auto* wait = static_cast<RegisterWait*>(context);
    const uint32_t csts = wait->controller->hardware_.Read32(Reg::Csts);
    if (csts == Gone32)
    {
        wait->fatal = true;
        return true;
    }
    if (wait->check_fatal && (csts & CstsBit::FatalStatus) != 0)
    {
        wait->fatal = true;
        return true;
    }
    return (csts & wait->mask) == wait->value;
}

Controller::Wait Controller::WaitRegister(uint32_t mask, uint32_t value, bool check_fatal,
                                          uint32_t timeout_ms)
{
    RegisterWait wait{this, mask, value, check_fatal, false};
    const uint64_t deadline = environment_.NowMilliseconds() + timeout_ms;
    for (;;)
    {
        const uint64_t now = environment_.NowMilliseconds();
        const uint64_t until = std::min<uint64_t>(now + WaitStepMs, deadline);
        if (environment_.WaitUntil(this, until, ReadyRegister, &wait))
        {
            return wait.fatal ? Wait::Fatal : Wait::Reached;
        }
        if (environment_.NowMilliseconds() >= deadline)
        {
            return Wait::Timeout;
        }
    }
}

Controller::Wait Controller::WaitReady(bool want, uint32_t timeout_ms)
{
    return WaitRegister(CstsBit::Ready, want ? CstsBit::Ready : 0, want, timeout_ms);
}

bool Controller::Disable(bool wait_for_ready)
{
    const uint32_t timeout = std::max(caps_.timeout_ms, MinimumTimeoutMs);
    const uint32_t cc = hardware_.Read32(Reg::Cc);
    if ((cc & CcBit::Enable) != 0)
    {
        if (wait_for_ready && (hardware_.Read32(Reg::Csts) & CstsBit::Ready) == 0)
        {
            WaitReady(true, timeout);
        }
        hardware_.Write32(Reg::Cc, cc & ~CcBit::Enable);
    }
    return WaitReady(false, timeout) == Wait::Reached;
}

bool Controller::AllocateBuffers()
{
    Memory::Dma::Buffer* const buffers[] = {&admin_sq_, &admin_cq_, &identify_, &io_sq_, &io_cq_};
    for (Memory::Dma::Buffer* buffer : buffers)
    {
        if (!hardware_.AllocateDma(PageSize, UINT64_MAX, buffer))
        {
            return false;
        }
    }
    return true;
}

bool Controller::AllocateTransferBuffers()
{
    return hardware_.AllocateDma(PageSize, UINT64_MAX, &prp_list_) &&
           hardware_.AllocateDma(bounce_bytes_, UINT64_MAX, &bounce_);
}

void Controller::ReleaseBuffers()
{
    if (buffers_pinned_)
    {
        return;
    }
    Memory::Dma::Buffer* const buffers[] = {&admin_sq_, &admin_cq_, &identify_, &io_sq_,
                                            &io_cq_,    &prp_list_, &bounce_};
    for (Memory::Dma::Buffer* buffer : buffers)
    {
        if (buffer->size != 0)
        {
            hardware_.FreeDma(*buffer);
            *buffer = {};
        }
    }
}

bool Controller::Refuse(const char* reason)
{
    Say(Level::Warning, index_, "{}", reason);
    buffers_pinned_ = !Disable(false);
    ReleaseBuffers();
    return false;
}

bool Controller::Start()
{
    if (!BringUp())
    {
        return false;
    }
    started_.store(true, std::memory_order_release);
    Publish();
    return true;
}

bool Controller::BringUp()
{
    Sync::MutexGuard guard(admin_lock_);
    return BringUpLocked();
}

bool Controller::BringUpLocked()
{
    const uint64_t cap = hardware_.Read64(Reg::Cap);
    if (cap == UINT64_MAX || hardware_.Read32(Reg::Csts) == Gone32)
    {
        Say(Level::Warning, index_, "controller is gone");
        return false;
    }
    caps_ = DecodeCap(cap);
    version_ = hardware_.Read32(Reg::Vs);
    if (!caps_.nvm_command_set || !Supports4KiBPages(caps_))
    {
        Say(Level::Warning, index_, "no NVM command set or 4 KiB pages, not supported");
        return false;
    }

    if (!Disable(true))
    {
        buffers_pinned_ = true;
        Say(Level::Warning, index_, "controller did not disable");
        return false;
    }
    if (!AllocateBuffers())
    {
        return Refuse("out of memory for the queues");
    }

    const uint32_t admin_entries = std::min(caps_.max_queue_entries, MaxAdminQueueEntries);
    {
        Sync::SpinLockGuard guard(completion_lock_);
        admin_queue_.Initialize(hardware_, 0, static_cast<uint16_t>(admin_entries),
                                caps_.doorbell_stride, admin_sq_, admin_cq_);
    }
    hardware_.Write32(Reg::Aqa, AdminQueueAttributes(admin_entries, admin_entries));
    hardware_.Write64(Reg::Asq, admin_sq_.physical);
    hardware_.Write64(Reg::Acq, admin_cq_.physical);

    interrupts_ = hardware_.SetUpInterrupts(OnInterrupt, this);
    Memory::Dma::ProducerBarrier();
    hardware_.Write32(Reg::Cc, ControllerCc(CcBit::Enable));
    const uint32_t timeout = std::max(caps_.timeout_ms, MinimumTimeoutMs);
    const Wait ready = WaitReady(true, timeout);
    if (ready != Wait::Reached)
    {
        return Refuse(ready == Wait::Fatal ? "controller reported a fatal status"
                                           : "controller did not become ready");
    }

    return IdentifyController() && CreateQueues() && DiscoverNamespaces();
}

bool Controller::IdentifyController()
{
    Cqe cqe{};
    if (!Admin(Identify(0, 0, IdentifyCns::Controller, identify_.physical), &cqe))
    {
        return Refuse("identify controller failed");
    }
    for (uint32_t i = 0; i < IdentifyBytes; i++)
    {
        identify_copy_[i] = identify_.address[i];
    }
    data_ = ParseController(std::span<const uint8_t, IdentifyBytes>(identify_copy_));
    if (!SupportsEntrySizes(data_))
    {
        return Refuse("queue entry sizes of 64 and 16 bytes are not supported");
    }
    bounce_bytes_ = BounceBufferBytes(data_);
    if (!AllocateTransferBuffers())
    {
        return Refuse("out of memory for the transfer buffer");
    }
    const uint32_t pages = bounce_bytes_ / PageSize;
    if (pages >= 3)
    {
        uint64_t addresses[BounceBytes / PageSize] = {};
        uint64_t list[BounceBytes / PageSize] = {};
        for (uint32_t i = 0; i < pages; i++)
        {
            addresses[i] = bounce_.physical + static_cast<uint64_t>(i) * PageSize;
        }
        uint64_t prp1 = 0;
        uint64_t prp2 = 0;
        if (!BuildPrps(std::span<const uint64_t>(addresses, pages), 0, bounce_bytes_,
                       prp_list_.physical, std::span<uint64_t>(list, pages), &prp1, &prp2))
        {
            return Refuse("cannot build the transfer list");
        }
        auto* slots = reinterpret_cast<volatile uint64_t*>(prp_list_.address);
        for (uint32_t i = 0; i + 1 < pages; i++)
        {
            slots[i] = list[i];
        }
    }
    return true;
}

bool Controller::CreateQueues()
{
    Cqe cqe{};
    if (!Admin(SetQueueCount(0, 0, 0), &cqe))
    {
        return Refuse("set number of queues failed");
    }
    io_depth_ = std::min(caps_.max_queue_entries, MaxIoQueueEntries);
    {
        Sync::SpinLockGuard guard(completion_lock_);
        io_queue_.Initialize(hardware_, 1, static_cast<uint16_t>(io_depth_), caps_.doorbell_stride,
                             io_sq_, io_cq_);
    }
    const bool interrupts = interrupts_ != InterruptKind::Polling;
    if (!Admin(CreateIoCq(0, 1, static_cast<uint16_t>(io_depth_), io_cq_.physical, 0, interrupts),
               &cqe))
    {
        return Refuse("create I/O completion queue failed");
    }
    if (!Admin(CreateIoSq(0, 1, static_cast<uint16_t>(io_depth_), io_sq_.physical, 1), &cqe))
    {
        return Refuse("create I/O submission queue failed");
    }
    return true;
}

bool Controller::AddNamespace(uint32_t nsid, bool quiet)
{
    Cqe cqe{};
    if (!Admin(Identify(0, nsid, IdentifyCns::Namespace, identify_.physical), &cqe))
    {
        if (!quiet)
        {
            Say(Level::Warning, index_, "namespace {}: identify failed", nsid);
        }
        return true;
    }
    for (uint32_t i = 0; i < IdentifyBytes; i++)
    {
        identify_copy_[i] = identify_.address[i];
    }
    const NamespaceData ns =
        ParseNamespace(std::span<const uint8_t, IdentifyBytes>(identify_copy_));
    if (ns.refusal != Refusal::None)
    {
        if (!(quiet && ns.refusal == Refusal::Inactive))
        {
            Say(Level::Warning, index_, "namespace {}: {}", nsid, RefusalText(ns.refusal));
        }
        return true;
    }
    if (namespace_count_ >= MaxNamespaces)
    {
        Say(Level::Warning, index_, "namespace {}: at most {} namespaces", nsid, MaxNamespaces);
        return true;
    }

    DiskInfo info{};
    info.kind = DiskKind::Nvme;
    info.sector_size = ns.sector_size;
    info.sector_count = ns.sector_count;
    info.max_transfer_bytes = bounce_bytes_;
    info.removable = false;
    const bool policy_read_only = !WritesAllowed(policy_);
    info.read_only = policy_read_only || ns.write_protected;
    info.read_only_reason = policy_read_only     ? ReadOnlyReason::Policy
                            : ns.write_protected ? ReadOnlyReason::Medium
                                                 : ReadOnlyReason::None;
    info.controller_index = index_;
    info.namespace_id = nsid;
    CopyText(info.model, sizeof(info.model), data_.model);
    CopyText(info.location, sizeof(info.location), location_);
    namespaces_[namespace_count_++].Initialize(*this, info);
    return true;
}

bool Controller::DiscoverNamespaces()
{
    if (data_.namespace_count == 0)
    {
        return Refuse("no namespaces");
    }
    uint32_t count = 0;
    const bool listed = HasActiveNamespaceList(DecodeVersion(version_));
    if (listed)
    {
        Cqe cqe{};
        if (!Admin(Identify(0, 0, IdentifyCns::ActiveNamespaces, identify_.physical), &cqe))
        {
            return Refuse("identify active namespaces failed");
        }
        for (uint32_t i = 0; i < IdentifyBytes; i++)
        {
            identify_copy_[i] = identify_.address[i];
        }
        count = ParseActiveList(std::span<const uint8_t, IdentifyBytes>(identify_copy_),
                                std::span<uint32_t>(active_list_, MaxProbedNamespaces));
    }
    else
    {
        count = std::min(data_.namespace_count, MaxNamespaces);
        for (uint32_t i = 0; i < count; i++)
        {
            active_list_[i] = i + 1;
        }
    }
    for (uint32_t i = 0; i < count; i++)
    {
        AddNamespace(active_list_[i], !listed);
    }
    if (namespace_count_ == 0)
    {
        return Refuse("no namespaces");
    }
    return true;
}

void Controller::LogPolicy()
{
    switch (policy_)
    {
    case WritePolicy::Allowed:
        Say(Level::Info, index_, "QEMU controller, writes allowed");
        break;
    case WritePolicy::ReadOnlyHardware:
        Say(Level::Info, index_, "real hardware, read-only (boot with storage=rw to write)");
        break;
    case WritePolicy::ReadOnlyOption:
        Say(Level::Info, index_, "storage=ro, read-only");
        break;
    case WritePolicy::AllowedByOption:
        Say(Level::Warning, index_, "storage=rw, writes allowed on real hardware");
        break;
    }
}

void Controller::LogController()
{
    const char* interrupts = interrupts_ == InterruptKind::MsiX  ? "MSI-X"
                             : interrupts_ == InterruptKind::Msi ? "MSI"
                                                                 : "polling";
    Say(Level::Info, index_, "{} \"{}\" fw {}, {}, 1 I/O queue of {}, max transfer {} KiB, {}",
        location_, data_.model, data_.firmware, interrupts, io_depth_, bounce_bytes_ / 1024,
        data_.volatile_write_cache ? "volatile write cache" : "no volatile write cache");
}

void Controller::Publish()
{
    LogPolicy();
    LogController();
    Disk* registered[MaxNamespaces] = {};
    for (uint32_t i = 0; i < namespace_count_; i++)
    {
        Namespace& ns = namespaces_[i];
        Disk* disk = Register(ns);
        if (disk == nullptr)
        {
            Say(Level::Warning, index_, "namespace {}: disk registry full", ns.Id());
            continue;
        }
        const DiskInfo info = ns.Info();
        char name[MaxDiskName];
        CopyText(name, sizeof(name), disk->Name());
        SayDisk(name, "{} KiB, {}-byte sectors, {}", info.sector_count * info.sector_size / 1024,
                info.sector_size, info.read_only ? "ro" : "rw");
        disks_[i].store(disk, std::memory_order_release);
        registered[i] = disk;
    }
    for (Disk* disk : registered)
    {
        if (disk == nullptr)
        {
            continue;
        }
        if (Dead())
        {
            Remove(*disk);
        }
        else
        {
            Scan(*disk);
        }
    }
}

FeatherDisk::Status Controller::Admin(const Sqe& sqe, Cqe* completion)
{
    Cqe cqe{};
    if (Execute(Which::Admin, sqe, &cqe, AdminTimeoutMs) != Outcome::Done)
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    if (completion != nullptr)
    {
        *completion = cqe;
    }
    if (CompletionStatus(cqe) != 0)
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    return {};
}

bool Controller::ReadyCommand(void* context)
{
    auto* waiter = static_cast<Waiter*>(context);
    return waiter->controller->ReapPending(waiter->which);
}

bool Controller::ReapPending(Which which)
{
    Sync::SpinLockGuard guard(completion_lock_);
    QueuePair& queue = which == Which::Admin ? admin_queue_ : io_queue_;
    Pending& pending = which == Which::Admin ? admin_pending_ : io_pending_;
    Cqe cqe{};
    StaleCompletions stale{};
    if (queue.Reap(pending.active ? pending.cid : NoCid, &cqe, &stale))
    {
        pending.cqe = cqe;
        pending.done = true;
    }
    if (stale.count != 0)
    {
        stale_completions_.fetch_add(stale.count, std::memory_order_relaxed);
        last_stale_cid_ = stale.last_cid;
    }
    return pending.done;
}

void Controller::LogStale()
{
    if (stale_completions_.load(std::memory_order_relaxed) == 0 ||
        stale_logged_.exchange(true, std::memory_order_relaxed))
    {
        return;
    }
    uint16_t cid;
    {
        Sync::SpinLockGuard guard(completion_lock_);
        cid = last_stale_cid_;
    }
    Say(Level::Warning, index_, "stale completion cid {}", cid);
}

bool Controller::AwaitCompletion(Which which, uint64_t deadline_ms)
{
    Waiter waiter{this, which};
    for (;;)
    {
        const uint64_t now = environment_.NowMilliseconds();
        const uint64_t until = std::min<uint64_t>(now + WaitStepMs, deadline_ms);
        if (environment_.WaitUntil(this, until, ReadyCommand, &waiter))
        {
            return true;
        }
        if (environment_.NowMilliseconds() >= deadline_ms)
        {
            return false;
        }
    }
}

Controller::Outcome Controller::Execute(Which which, const Sqe& sqe, Cqe* completion,
                                        uint32_t budget_ms, uint16_t* cid_out)
{
    {
        Sync::SpinLockGuard guard(completion_lock_);
        QueuePair& queue = which == Which::Admin ? admin_queue_ : io_queue_;
        Pending& pending = which == Which::Admin ? admin_pending_ : io_pending_;
        const uint16_t cid = queue.Submit(sqe);
        if (cid == NoCid)
        {
            return Outcome::Full;
        }
        pending = {cid, true, false, {}};
        if (cid_out != nullptr)
        {
            *cid_out = cid;
        }
    }
    const bool finished = AwaitCompletion(which, environment_.NowMilliseconds() + budget_ms);
    {
        Sync::SpinLockGuard guard(completion_lock_);
        Pending& pending = which == Which::Admin ? admin_pending_ : io_pending_;
        if (finished)
        {
            *completion = pending.cqe;
        }
        pending.active = false;
    }
    if (!finished)
    {
        return Outcome::TimedOut;
    }
    LogStale();
    return Outcome::Done;
}

FeatherDisk::Status Controller::IoCommand(const Sqe& sqe)
{
    Cqe cqe{};
    uint16_t cid = NoCid;
    const Outcome outcome = Execute(Which::Io, sqe, &cqe, IoTimeoutMs, &cid);
    if (outcome == Outcome::TimedOut)
    {
        Recover(cid);
        return std::unexpected(FeatherDisk::Error::Io);
    }
    if (outcome != Outcome::Done)
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    const FeatherDisk::Status status = MapStatus(CompletionStatus(cqe));
    if (!status && status.error() == FeatherDisk::Error::Io)
    {
        Say(Level::Warning, index_, "command failed, status {}", CompletionStatus(cqe));
    }
    return status;
}

uint64_t Controller::Prp2For(uint32_t bytes) const
{
    const uint32_t pages = (bytes + PageSize - 1) / PageSize;
    if (pages <= 1)
    {
        return 0;
    }
    return pages == 2 ? bounce_.physical + PageSize : prp_list_.physical;
}

FeatherDisk::Status Controller::Io(const Namespace& ns, bool write, uint64_t sector, uint32_t count,
                                   uint8_t* in, const uint8_t* out)
{
    if (write && ns.ReadOnly())
    {
        return std::unexpected(FeatherDisk::Error::ReadOnly);
    }
    if (count == 0)
    {
        return {};
    }
    Sync::MutexGuard guard(io_lock_);
    if (Dead() || ns.Vanished() || shutting_down_.load(std::memory_order_acquire))
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    const uint32_t sector_size = ns.SectorSize();
    const uint32_t per_command = std::min(bounce_bytes_ / sector_size, MaxBlocksPerCommand);
    uint32_t done = 0;
    while (done < count)
    {
        const uint32_t blocks = std::min(count - done, per_command);
        const uint32_t bytes = blocks * sector_size;
        const size_t offset = static_cast<size_t>(done) * sector_size;
        if (write)
        {
            std::copy_n(out + offset, bytes, Bytes(bounce_));
            write_commands_.fetch_add(1, std::memory_order_relaxed);
        }
        const std::optional<Sqe> sqe =
            ReadWrite(write, 0, ns.Id(), sector + done, blocks, bounce_.physical, Prp2For(bytes));
        if (!sqe)
        {
            return std::unexpected(FeatherDisk::Error::InvalidArgument);
        }
        if (const FeatherDisk::Status status = IoCommand(*sqe); !status)
        {
            return status;
        }
        if (!write)
        {
            std::copy_n(Bytes(bounce_), bytes, in + offset);
        }
        done += blocks;
    }
    return {};
}

FeatherDisk::Status Controller::FlushNamespace(const Namespace& ns)
{
    if (ns.ReadOnly() || !data_.volatile_write_cache)
    {
        return {};
    }
    Sync::MutexGuard guard(io_lock_);
    if (Dead() || ns.Vanished() || shutting_down_.load(std::memory_order_acquire))
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    flush_commands_.fetch_add(1, std::memory_order_relaxed);
    return IoCommand(Flush(0, ns.Id()));
}

void Controller::Recover(uint16_t cid)
{
    const uint32_t csts = hardware_.Read32(Reg::Csts);
    if (csts == Gone32)
    {
        Say(Level::Warning, index_, "controller is gone");
        MakeDead(false);
        return;
    }
    if ((csts & CstsBit::FatalStatus) != 0)
    {
        Say(Level::Warning, index_, "controller reports a fatal status");
        ResetOrDie();
        return;
    }
    if (AbortCommand(cid))
    {
        Say(Level::Warning, index_, "command timed out; aborted");
        return;
    }
    ResetOrDie();
}

bool Controller::AbortCommand(uint16_t cid)
{
    aborts_.fetch_add(1, std::memory_order_relaxed);
    {
        Sync::MutexGuard admin_guard(admin_lock_);
        if (!Admin(Abort(0, cid, 1), nullptr))
        {
            return false;
        }
    }
    {
        Sync::SpinLockGuard guard(completion_lock_);
        io_pending_ = {cid, true, false, {}};
    }
    const bool finished = AwaitCompletion(Which::Io, environment_.NowMilliseconds() + AbortWaitMs);
    Sync::SpinLockGuard guard(completion_lock_);
    io_pending_.active = false;
    return finished;
}

void Controller::ResetOrDie()
{
    const uint64_t now = environment_.NowMilliseconds();
    if (resets_.load(std::memory_order_relaxed) != 0 && now - last_reset_ms_ < ResetWindowMs)
    {
        Say(Level::Warning, index_, "second reset within {} s", ResetWindowMs / 1000);
        MakeDead(true);
        return;
    }
    last_reset_ms_ = now;
    resets_.fetch_add(1, std::memory_order_relaxed);
    if (!Reset())
    {
        MakeDead(!buffers_pinned_);
        return;
    }
    Say(Level::Warning, index_, "command timed out; controller reset");
}

bool Controller::Reset()
{
    // Once RDY is 0 the device does no more DMA: the bounce buffer and the rings are ours (I3).
    if (!Disable(false))
    {
        buffers_pinned_ = true;
        return false;
    }
    Sync::MutexGuard admin_guard(admin_lock_);
    return ResetLocked();
}

bool Controller::ResetLocked()
{
    const uint32_t admin_entries = std::min(caps_.max_queue_entries, MaxAdminQueueEntries);
    {
        Sync::SpinLockGuard guard(completion_lock_);
        admin_queue_.Reset();
        io_queue_.Reset();
        admin_pending_ = {};
        io_pending_ = {};
    }
    hardware_.Write32(Reg::Aqa, AdminQueueAttributes(admin_entries, admin_entries));
    hardware_.Write64(Reg::Asq, admin_sq_.physical);
    hardware_.Write64(Reg::Acq, admin_cq_.physical);
    Memory::Dma::ProducerBarrier();
    hardware_.Write32(Reg::Cc, ControllerCc(CcBit::Enable));
    if (WaitReady(true, std::max(caps_.timeout_ms, MinimumTimeoutMs)) != Wait::Reached)
    {
        return false;
    }

    Cqe cqe{};
    if (!Admin(Identify(0, 0, IdentifyCns::Controller, identify_.physical), &cqe))
    {
        return false;
    }
    for (uint32_t i = 0; i < IdentifyBytes; i++)
    {
        identify_copy_[i] = identify_.address[i];
    }
    const ControllerData data =
        ParseController(std::span<const uint8_t, IdentifyBytes>(identify_copy_));
    if (!SupportsEntrySizes(data) || BounceBufferBytes(data) != bounce_bytes_)
    {
        return false;
    }

    if (!Admin(SetQueueCount(0, 0, 0), &cqe))
    {
        return false;
    }
    {
        Sync::SpinLockGuard guard(completion_lock_);
        io_queue_.Initialize(hardware_, 1, static_cast<uint16_t>(io_depth_), caps_.doorbell_stride,
                             io_sq_, io_cq_);
    }
    const bool interrupts = interrupts_ != InterruptKind::Polling;
    return Admin(CreateIoCq(0, 1, static_cast<uint16_t>(io_depth_), io_cq_.physical, 0, interrupts),
                 &cqe) &&
           Admin(CreateIoSq(0, 1, static_cast<uint16_t>(io_depth_), io_sq_.physical, 1), &cqe) &&
           Revalidate();
}

bool Controller::Revalidate()
{
    for (uint32_t i = 0; i < namespace_count_; i++)
    {
        Namespace& ns = namespaces_[i];
        if (ns.Vanished())
        {
            continue;
        }
        Cqe cqe{};
        if (!Admin(Identify(0, ns.Id(), IdentifyCns::Namespace, identify_.physical), &cqe))
        {
            return false;
        }
        for (uint32_t b = 0; b < IdentifyBytes; b++)
        {
            identify_copy_[b] = identify_.address[b];
        }
        const NamespaceData now =
            ParseNamespace(std::span<const uint8_t, IdentifyBytes>(identify_copy_));
        if (now.refusal == Refusal::None && now.sector_size == ns.SectorSize() &&
            now.sector_count == ns.SectorCount())
        {
            continue;
        }
        ns.MarkGone();
        Say(Level::Warning, index_, "namespace {} changed during the reset; its disk is gone",
            ns.Id());
        if (Disk* disk = disks_[i].load(std::memory_order_acquire))
        {
            Remove(*disk);
        }
    }
    return true;
}

void Controller::MakeDead(bool disable)
{
    if (dead_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    // After this the controller does no more DMA, so the buffers stay safe to reuse (I3).
    if (disable && !Disable(false))
    {
        buffers_pinned_ = true;
    }
    Say(Level::Warning, index_, "controller failed; its disks are gone");
    for (std::atomic<Disk*>& entry : disks_)
    {
        if (Disk* disk = entry.load(std::memory_order_acquire))
        {
            Remove(*disk);
        }
    }
}

void Controller::Shutdown()
{
    Sync::MutexGuard guard(io_lock_);
    if (!started_.load(std::memory_order_acquire) || Dead() ||
        shutting_down_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    const uint32_t cc = hardware_.Read32(Reg::Cc);
    hardware_.Write32(Reg::Cc, (cc & ~CcBit::ShutdownMask) | CcBit::ShutdownNormal);
    const uint32_t latency_ms = (data_.rtd3_entry_latency_us + 999) / 1000;
    const uint32_t timeout = std::min(std::max(ShutdownMinimumMs, latency_ms), ShutdownCapMs);
    const Wait wait =
        WaitRegister(CstsBit::ShutdownMask, CstsBit::ShutdownComplete, false, timeout);
    if (wait == Wait::Reached)
    {
        Say(Level::Info, index_, "shutdown complete");
    }
    else
    {
        Say(Level::Warning, index_, "shutdown did not complete in {} ms", timeout);
    }
}

} // namespace FeatherOS::Storage::Nvme
