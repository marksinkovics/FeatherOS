#include "fake_nvme.h"

#include "kernel/storage/nvme/registers.h"

#include <algorithm>
#include <cstring>

namespace FeatherOS::Storage::Testing
{

namespace
{

namespace N = Nvme;

constexpr uint32_t PageSize = 4096;
constexpr uint32_t Poison = 0xFFFFFFFF;
constexpr uint32_t Gone32 = 0xFFFFFFFF;

constexpr uint8_t TypeGeneric = 0;
constexpr uint8_t TypeCommand = 1;
constexpr uint8_t CodeInvalidOpcode = 0x01;
constexpr uint8_t CodeInvalidField = 0x02;
constexpr uint8_t CodeInternalError = 0x06;
constexpr uint8_t CodeAborted = 0x07;
constexpr uint8_t CodeAbortLimit = 0x03;
constexpr uint8_t CodeInvalidNamespace = 0x0B;
constexpr uint8_t CodeWriteProtected = 0x20;
constexpr uint8_t CodeLbaRange = 0x80;
constexpr uint8_t CodeInvalidQueue = 0x01;
constexpr uint8_t CodeQueueSize = 0x02;
constexpr uint8_t CodeInvalidVector = 0x08;

void Put(volatile uint8_t* page, uint32_t offset, uint64_t value, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++)
    {
        page[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void PutText(volatile uint8_t* page, uint32_t offset, const char* text, uint32_t bytes)
{
    const size_t length = std::strlen(text);
    for (uint32_t i = 0; i < bytes; i++)
    {
        page[offset + i] = i < length ? static_cast<uint8_t>(text[i]) : ' ';
    }
}

uint32_t Log2(uint32_t value)
{
    uint32_t shift = 0;
    while ((1U << shift) < value)
    {
        shift++;
    }
    return shift;
}

} // namespace

// Every public entry point holds the lock; interrupts are raised after the outermost one lets go,
// as a device raises them without holding anything the handler may need.
class FakeNvme::Scope
{
public:
    explicit Scope(FakeNvme& fake) : fake_(fake)
    {
        fake_.lock_.lock();
        ++fake_.depth_;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    ~Scope()
    {
        uint32_t due = 0;
        void (*handler)(void*) = fake_.handler_;
        void* context = fake_.handler_context_;
        if (--fake_.depth_ == 0)
        {
            due = fake_.interrupts_due_;
            fake_.interrupts_due_ = 0;
        }
        fake_.lock_.unlock();
        for (; due != 0 && handler != nullptr; --due)
        {
            handler(context);
        }
    }

private:
    FakeNvme& fake_;
};

FakeNvme::FakeNvme(FeatherOS::Testing::PhysicalMemory& memory) : memory_(memory) {}

ModelDisk& FakeNvme::AddNamespace(uint32_t nsid, uint32_t sector_size, uint64_t sectors)
{
    return AddNamespace(nsid, sector_size, sectors, NamespaceFlags{});
}

ModelDisk& FakeNvme::AddNamespace(uint32_t nsid, uint32_t sector_size, uint64_t sectors,
                                  const NamespaceFlags& flags)
{
    Namespace& ns = namespaces_[nsid];
    ns.disk = std::make_unique<ModelDisk>(sector_size, sectors, options.volatile_cache);
    ns.flags = flags;
    ns.sector_size = sector_size;
    return *ns.disk;
}

ModelDisk& FakeNvme::NamespaceDisk(uint32_t nsid)
{
    return *namespaces_.at(nsid).disk;
}

FakeNvme::Namespace* FakeNvme::Find(uint32_t nsid)
{
    const auto it = namespaces_.find(nsid);
    return it == namespaces_.end() ? nullptr : &it->second;
}

uint32_t FakeNvme::NamespaceMax() const
{
    uint32_t highest = 0;
    for (const auto& entry : namespaces_)
    {
        highest = std::max(highest, entry.first);
    }
    return std::max(options.namespace_count, highest);
}

uint32_t FakeNvme::Stride() const
{
    return 4U << options.doorbell_stride_log2;
}

void FakeNvme::Fault(const std::string& what)
{
    faults_.push_back(what);
}

void FakeNvme::Initialize()
{
    if (initialized_)
    {
        return;
    }
    initialized_ = true;
    if (options.firmware_enabled)
    {
        const Memory::Dma::Buffer sq = memory_.Allocate(PageSize);
        const Memory::Dma::Buffer cq = memory_.Allocate(PageSize);
        aqa_ = (7U << 16) | 7U;
        asq_ = sq.physical;
        acq_ = cq.physical;
        cc_ = N::ControllerCc(N::CcBit::Enable);
        ready_ = true;
        Enable();
    }
}

uint32_t FakeNvme::Read32(uint32_t offset)
{
    const Scope scope(*this);
    Initialize();
    if (gone_)
    {
        return Gone32;
    }
    const uint32_t cap_low = options.mqes | (options.timeout_units << 24);
    const uint32_t cap_high = options.doorbell_stride_log2 | (options.nvm_command_set ? 1U << 5 : 0) |
                              (options.min_page_log2 << 16) | (options.min_page_log2 << 20);
    switch (offset)
    {
    case N::Reg::Cap:
        return cap_low;
    case N::Reg::Cap + 4:
        return cap_high;
    case N::Reg::Vs:
        return options.version;
    case N::Reg::Cc:
        return cc_;
    case N::Reg::Csts:
        return ReadCsts();
    case N::Reg::Aqa:
        return aqa_;
    case N::Reg::Asq:
        return static_cast<uint32_t>(asq_);
    case N::Reg::Asq + 4:
        return static_cast<uint32_t>(asq_ >> 32);
    case N::Reg::Acq:
        return static_cast<uint32_t>(acq_);
    case N::Reg::Acq + 4:
        return static_cast<uint32_t>(acq_ >> 32);
    default:
        return 0;
    }
}

uint64_t FakeNvme::Read64(uint32_t offset)
{
    const Scope scope(*this);
    const uint64_t low = Read32(offset);
    const uint64_t high = Read32(offset + 4);
    return low | (high << 32);
}

void FakeNvme::Write64(uint32_t offset, uint64_t value)
{
    const Scope scope(*this);
    Write32(offset, static_cast<uint32_t>(value));
    Write32(offset + 4, static_cast<uint32_t>(value >> 32));
}

uint32_t FakeNvme::ReadCsts()
{
    if (transition_target_ >= 0)
    {
        if (transition_reads_ > 0)
        {
            transition_reads_--;
        }
        else
        {
            ready_ = transition_target_ != 0;
            transition_target_ = -1;
            if (!ready_)
            {
                Stop();
            }
        }
    }
    if (shutdown_pending_ && !options.never_shutdown)
    {
        if (shutdown_reads_ > 0)
        {
            shutdown_reads_--;
        }
        else
        {
            shutdown_complete_ = true;
            shutdown_pending_ = false;
        }
    }
    return (ready_ ? N::CstsBit::Ready : 0) | (fatal_ ? N::CstsBit::FatalStatus : 0) |
           (shutdown_complete_ ? N::CstsBit::ShutdownComplete : 0);
}

void FakeNvme::Write32(uint32_t offset, uint32_t value)
{
    const Scope scope(*this);
    Initialize();
    if (offset >= N::Reg::DoorbellBase)
    {
        WriteDoorbell(offset, value);
    }
    else if (offset == N::Reg::Cc)
    {
        WriteCc(value);
    }
    else
    {
        WriteRegister(offset, value);
    }
}

void FakeNvme::WriteRegister(uint32_t offset, uint32_t value)
{
    const bool admin = offset == N::Reg::Aqa || offset == N::Reg::Asq ||
                       offset == N::Reg::Asq + 4 || offset == N::Reg::Acq ||
                       offset == N::Reg::Acq + 4;
    if (!admin)
    {
        return;
    }
    if ((cc_ & N::CcBit::Enable) != 0 || ready_)
    {
        Fault("admin queue registers written while the controller is enabled");
        return;
    }
    switch (offset)
    {
    case N::Reg::Aqa:
        aqa_ = value;
        events_.push_back(Event::AdminProgrammed);
        break;
    case N::Reg::Asq:
        asq_ = (asq_ & ~0xFFFFFFFFULL) | value;
        break;
    case N::Reg::Asq + 4:
        asq_ = (asq_ & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    case N::Reg::Acq:
        acq_ = (acq_ & ~0xFFFFFFFFULL) | value;
        break;
    default:
        acq_ = (acq_ & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    }
}

void FakeNvme::WriteCc(uint32_t value)
{
    const uint32_t config = ~(N::CcBit::Enable | N::CcBit::ShutdownMask);
    const bool was = (cc_ & N::CcBit::Enable) != 0;
    const bool now = (value & N::CcBit::Enable) != 0;
    if (was && now && ((value ^ cc_) & config) != 0)
    {
        Fault("CC configuration changed while the controller is enabled");
        return;
    }
    if (!was && now)
    {
        if (((value >> 7) & 0xF) != 0 || ((value >> 4) & 7) != 0 || ((value >> 11) & 7) != 0 ||
            ((value >> 16) & 0xF) != N::SqEntryLog2 || ((value >> 20) & 0xF) != N::CqEntryLog2)
        {
            Fault("CC enabled with an unexpected page size, command set or entry size");
        }
        if (aqa_ == 0 || asq_ == 0 || acq_ == 0)
        {
            Fault("enabled without admin queue registers");
        }
    }
    cc_ = value;
    if (!was && now)
    {
        Enable();
    }
    else if (was && !now)
    {
        Disable();
    }
    if ((value & N::CcBit::ShutdownMask) == N::CcBit::ShutdownNormal)
    {
        events_.push_back(Event::ShutdownRequested);
        shutdown_pending_ = true;
        shutdown_reads_ = options.shutdown_delay_reads;
    }
}

void FakeNvme::Enable()
{
    if (transition_target_ == 0)
    {
        Fault("controller enabled before it finished stopping");
    }
    ResetQueues();
    sq_[0].exists = true;
    sq_[0].base = asq_;
    sq_[0].entries = (aqa_ & 0xFFF) + 1;
    cq_[0].exists = true;
    cq_[0].base = acq_;
    cq_[0].entries = ((aqa_ >> 16) & 0xFFF) + 1;
    cq_[0].interrupts = true;
    events_.push_back(Event::Enabled);
    if (options.fatal_on_enable)
    {
        fatal_ = true;
        return;
    }
    if (options.never_ready)
    {
        return;
    }
    transition_target_ = 1;
    transition_reads_ = options.ready_delay_reads;
    if (transition_reads_ == 0)
    {
        ready_ = true;
        transition_target_ = -1;
    }
}

void FakeNvme::Disable()
{
    ResetQueues();
    events_.push_back(Event::Disabled);
    fatal_ = false;
    shutdown_complete_ = false;
    shutdown_pending_ = false;
    if (options.never_disable)
    {
        return;
    }
    transition_target_ = 0;
    transition_reads_ = options.ready_delay_reads;
    if (transition_reads_ == 0)
    {
        ready_ = false;
        transition_target_ = -1;
        Stop();
    }
}

void FakeNvme::ResetQueues()
{
    sq_[0] = {};
    sq_[1] = {};
    cq_[0] = {};
    cq_[1] = {};
    queues_created_ = false;
}

void FakeNvme::WriteDoorbell(uint32_t offset, uint32_t value)
{
    const uint32_t relative = offset - N::Reg::DoorbellBase;
    if (relative % Stride() != 0)
    {
        Fault("doorbell not on the doorbell stride");
        return;
    }
    const uint32_t index = relative / Stride();
    const uint32_t qid = index / 2;
    const bool completion = (index & 1) != 0;
    if (qid > 1 || (cc_ & N::CcBit::Enable) == 0)
    {
        Fault("doorbell of a queue that does not exist");
        return;
    }
    if (completion)
    {
        CompletionQueue& cq = cq_[qid];
        if (!cq.exists || value >= cq.entries)
        {
            Fault("completion queue head doorbell out of range");
            return;
        }
        cq.head = value;
        return;
    }
    SubmissionQueue& sq = sq_[qid];
    if (!sq.exists || value >= sq.entries)
    {
        Fault("submission queue tail doorbell out of range");
        return;
    }
    sq.tail = value;
    pending_doorbell_ = true;
    if (!options.defer_processing)
    {
        Process();
    }
}

void FakeNvme::Pump()
{
    const Scope scope(*this);
    if (pending_doorbell_)
    {
        Process();
    }
}

void FakeNvme::Process()
{
    pending_doorbell_ = false;
    Drain(0);
    Drain(1);
}

void FakeNvme::Drain(uint32_t qid)
{
    SubmissionQueue& sq = sq_[qid];
    while (sq.exists && sq.head != sq.tail)
    {
        volatile uint8_t* slot = memory_.Translate(sq.base + static_cast<uint64_t>(sq.head) * 64, 64);
        if (slot == nullptr)
        {
            Fault("submission queue is not in memory");
            sq.head = sq.tail;
            return;
        }
        N::Sqe sqe{};
        auto* words = reinterpret_cast<volatile const uint32_t*>(slot);
        auto* target = reinterpret_cast<uint32_t*>(&sqe);
        bool empty = true;
        for (uint32_t w = 0; w < 16; w++)
        {
            target[w] = words[w];
            empty = empty && target[w] == 0;
        }
        if (sqe.cdw0 == Poison || empty)
        {
            Fault("doorbell rung before the command was written");
            sq.head = sq.tail;
            return;
        }
        for (uint32_t i = 0; i < 64; i++)
        {
            slot[i] = 0xFF;
        }
        sq.head = (sq.head + 1) % sq.entries;
        const uint16_t cid = static_cast<uint16_t>(sqe.cdw0 >> 16);
        if (qid == 1)
        {
            io_commands_++;
            if (stall_ > 0)
            {
                stall_--;
                HoldCommand(sqe, cid);
                continue;
            }
        }
        else if (stall_admin_ > 0)
        {
            stall_admin_--;
            continue;
        }
        Post(qid, cid, qid == 0 ? Admin(sqe) : Io(sqe));
    }
}

FakeNvme::Outcome FakeNvme::Admin(const N::Sqe& sqe)
{
    switch (sqe.cdw0 & 0xFF)
    {
    case N::AdminOpcode::Identify:
        return Identify(sqe);
    case N::AdminOpcode::CreateIoCq:
        return CreateCq(sqe);
    case N::AdminOpcode::CreateIoSq:
        return CreateSq(sqe);
    case N::AdminOpcode::SetFeatures:
        if ((sqe.cdw10 & 0xFF) != N::NumberOfQueuesFeature)
        {
            return {0, TypeGeneric, CodeInvalidField};
        }
        if (options.fail_set_queue_count)
        {
            return {0, TypeGeneric, CodeInternalError};
        }
        return {0, TypeGeneric, 0};
    case N::AdminOpcode::Abort:
    {
        events_.push_back(Event::Abort);
        if (options.fail_abort)
        {
            return {0, TypeCommand, CodeAbortLimit};
        }
        const uint16_t target = static_cast<uint16_t>(sqe.cdw10 >> 16);
        if ((sqe.cdw10 & 0xFFFF) == 1 && held_.active && held_.cid == target &&
            held_.mode == Hold::UntilAborted)
        {
            held_.active = false;
            Post(1, target, {0, TypeGeneric, CodeAborted});
            return {};
        }
        return {1, TypeGeneric, 0};
    }
    default:
        return {0, TypeGeneric, CodeInvalidOpcode};
    }
}

FakeNvme::Outcome FakeNvme::Identify(const N::Sqe& sqe)
{
    if ((sqe.prp1 & (PageSize - 1)) != 0)
    {
        Fault("identify buffer is not page aligned");
        return {0, TypeGeneric, CodeInvalidField};
    }
    volatile uint8_t* page = memory_.Translate(sqe.prp1, PageSize);
    if (page == nullptr)
    {
        Fault("identify buffer is not in memory");
        return {0, TypeGeneric, CodeInvalidField};
    }
    for (uint32_t i = 0; i < PageSize; i++)
    {
        page[i] = 0;
    }
    const uint32_t cns = sqe.cdw10 & 0xFF;
    if (cns == N::IdentifyCns::Controller)
    {
        Put(page, 0, 0x1B36, 2);
        Put(page, 2, 0x1AF4, 2);
        PutText(page, 4, "FAKE0001", 20);
        PutText(page, 24, "Fake NVMe Controller", 40);
        PutText(page, 64, "1.0", 8);
        Put(page, 77, options.mdts, 1);
        Put(page, 80, options.version, 4);
        Put(page, 88, options.rtd3_entry_latency_us, 4);
        Put(page, 512, 0x66, 1);
        Put(page, 513, 0x44, 1);
        Put(page, 516, NamespaceMax(), 4);
        Put(page, 525, options.volatile_cache ? 1 : 0, 1);
        return {};
    }
    if (cns == N::IdentifyCns::Namespace)
    {
        const uint32_t nsid = sqe.nsid;
        if (nsid == 0 || nsid > NamespaceMax())
        {
            return {0, TypeGeneric, CodeInvalidNamespace};
        }
        Namespace* ns = Find(nsid);
        if (ns == nullptr || ns->flags.inactive)
        {
            return {};
        }
        Put(page, 0, ns->disk->Sectors(), 8);
        Put(page, 8, ns->disk->Sectors(), 8);
        Put(page, 16, ns->disk->Sectors(), 8);
        Put(page, 25, 0, 1);
        Put(page, 26, 0, 1);
        Put(page, 29, ns->flags.protection, 1);
        Put(page, 99, ns->flags.write_protected ? 1 : 0, 1);
        Put(page, 128, ns->flags.metadata_bytes | (Log2(ns->sector_size) << 16), 4);
        return {};
    }
    if (cns == N::IdentifyCns::ActiveNamespaces)
    {
        if (!N::HasActiveNamespaceList(N::DecodeVersion(options.version)))
        {
            Fault("active namespace list asked of a controller older than 1.1");
            return {0, TypeGeneric, CodeInvalidField};
        }
        uint32_t slot = 0;
        for (const auto& entry : namespaces_)
        {
            if (!entry.second.flags.inactive && entry.first > sqe.nsid)
            {
                Put(page, 4 * slot++, entry.first, 4);
            }
        }
        return {};
    }
    return {0, TypeGeneric, CodeInvalidField};
}

FakeNvme::Outcome FakeNvme::CreateCq(const N::Sqe& sqe)
{
    const uint32_t qid = sqe.cdw10 & 0xFFFF;
    const uint32_t entries = (sqe.cdw10 >> 16) + 1;
    const bool contiguous = (sqe.cdw11 & 1) != 0;
    const bool interrupts = (sqe.cdw11 & 2) != 0;
    const uint32_t vector = sqe.cdw11 >> 16;
    if (!contiguous)
    {
        Fault("completion queue created without physically contiguous memory");
    }
    if (qid != 1 || cq_[1].exists)
    {
        return {0, TypeCommand, CodeInvalidQueue};
    }
    if (entries < 2 || entries > options.mqes + 1)
    {
        return {0, TypeCommand, CodeQueueSize};
    }
    if (vector != 0)
    {
        return {0, TypeCommand, CodeInvalidVector};
    }
    if (interrupts != (options.interrupts != InterruptKind::Polling))
    {
        Fault(interrupts ? "interrupts enabled on the I/O completion queue in polling mode"
                         : "interrupts not enabled on the I/O completion queue");
    }
    if ((sqe.prp1 & (PageSize - 1)) != 0 || memory_.Translate(sqe.prp1, entries * 16ULL) == nullptr)
    {
        Fault("completion queue memory is not page aligned or not in memory");
        return {0, TypeGeneric, CodeInvalidField};
    }
    cq_[1].exists = true;
    cq_[1].base = sqe.prp1;
    cq_[1].entries = entries;
    cq_[1].interrupts = interrupts;
    queues_created_ = true;
    return {};
}

FakeNvme::Outcome FakeNvme::CreateSq(const N::Sqe& sqe)
{
    const uint32_t qid = sqe.cdw10 & 0xFFFF;
    const uint32_t entries = (sqe.cdw10 >> 16) + 1;
    const bool contiguous = (sqe.cdw11 & 1) != 0;
    const uint32_t cqid = sqe.cdw11 >> 16;
    if (!contiguous)
    {
        Fault("submission queue created without physically contiguous memory");
    }
    if (qid != 1 || sq_[1].exists)
    {
        return {0, TypeCommand, CodeInvalidQueue};
    }
    if (cqid > 1 || !cq_[cqid].exists)
    {
        return {0, TypeCommand, 0x00};
    }
    if (entries < 2 || entries > options.mqes + 1)
    {
        return {0, TypeCommand, CodeQueueSize};
    }
    if ((sqe.prp1 & (PageSize - 1)) != 0 || memory_.Translate(sqe.prp1, entries * 64ULL) == nullptr)
    {
        Fault("submission queue memory is not page aligned or not in memory");
        return {0, TypeGeneric, CodeInvalidField};
    }
    sq_[1].exists = true;
    sq_[1].base = sqe.prp1;
    sq_[1].entries = entries;
    sq_[1].cqid = cqid;
    return {};
}

FakeNvme::Outcome FakeNvme::Io(const N::Sqe& sqe)
{
    Namespace* ns = Find(sqe.nsid);
    const uint8_t opcode = sqe.cdw0 & 0xFF;
    if (opcode != N::NvmOpcode::Flush && opcode != N::NvmOpcode::Write &&
        opcode != N::NvmOpcode::Read)
    {
        return {0, TypeGeneric, CodeInvalidOpcode};
    }
    if (ns == nullptr || ns->flags.inactive)
    {
        return {0, TypeGeneric, CodeInvalidNamespace};
    }
    if (opcode == N::NvmOpcode::Flush)
    {
        events_.push_back(Event::Flush);
        io_log_.push_back({Event::Flush, sqe.nsid, 0, 0});
        ns->disk->Flush();
        return {};
    }
    return Transfer(sqe, opcode == N::NvmOpcode::Write);
}

bool FakeNvme::Gather(uint64_t prp1, uint64_t prp2, uint32_t bytes, std::vector<Segment>* segments)
{
    const uint32_t offset = static_cast<uint32_t>(prp1 & (PageSize - 1));
    if (prp1 == 0 || (offset & 3) != 0)
    {
        Fault("PRP1 is empty or not dword aligned");
        return false;
    }
    const uint32_t pages = (offset + bytes + PageSize - 1) / PageSize;
    std::vector<uint64_t> addresses;
    addresses.push_back(prp1);
    if (pages == 1)
    {
        if (prp2 != 0)
        {
            Fault("PRP2 set for a transfer of one page");
            return false;
        }
    }
    else if (pages == 2)
    {
        if (prp2 == 0 || (prp2 & (PageSize - 1)) != 0)
        {
            Fault("PRP2 of a two page transfer is empty or not page aligned");
            return false;
        }
        addresses.push_back(prp2);
    }
    else
    {
        if (prp2 == 0 || (prp2 & (PageSize - 1)) != 0 || pages - 1 > N::PrpEntriesPerPage)
        {
            Fault("PRP list pointer is empty, not page aligned or the list would chain");
            return false;
        }
        volatile uint8_t* list = memory_.Translate(prp2, 8ULL * (pages - 1));
        if (list == nullptr)
        {
            Fault("PRP list is not in memory");
            return false;
        }
        for (uint32_t i = 0; i + 1 < pages; i++)
        {
            uint64_t entry = 0;
            for (uint32_t b = 0; b < 8; b++)
            {
                entry |= static_cast<uint64_t>(list[8 * i + b]) << (8 * b);
            }
            if (entry == 0 || (entry & (PageSize - 1)) != 0)
            {
                Fault("PRP list entry is empty or not page aligned");
                return false;
            }
            addresses.push_back(entry);
        }
    }
    uint32_t remaining = bytes;
    for (uint32_t i = 0; i < pages; i++)
    {
        const uint32_t start = i == 0 ? offset : 0;
        const uint32_t length = std::min(remaining, PageSize - start);
        volatile uint8_t* host = memory_.Translate(addresses[i], PageSize);
        if (host == nullptr)
        {
            Fault("transfer page is not in memory");
            return false;
        }
        segments->push_back({host + start, length});
        remaining -= length;
    }
    return true;
}

FakeNvme::Outcome FakeNvme::Transfer(const N::Sqe& sqe, bool write)
{
    Namespace& ns = *Find(sqe.nsid);
    if (((sqe.cdw0 >> 14) & 3) != 0)
    {
        Fault("SGL used for a transfer");
    }
    if ((sqe.cdw12 >> 16) != 0)
    {
        Fault("FUA, PRINFO or limited retry set");
    }
    const uint64_t lba = sqe.cdw10 | (static_cast<uint64_t>(sqe.cdw11) << 32);
    const uint32_t blocks = (sqe.cdw12 & 0xFFFF) + 1;
    const uint64_t bytes = static_cast<uint64_t>(blocks) * ns.sector_size;
    events_.push_back(write ? Event::Write : Event::Read);
    io_log_.push_back({write ? Event::Write : Event::Read, sqe.nsid, lba, blocks});
    if (write && ns.flags.write_protected)
    {
        return {0, TypeGeneric, CodeWriteProtected};
    }
    if (options.mdts != 0 && bytes > (static_cast<uint64_t>(PageSize) << options.mdts))
    {
        Fault("transfer larger than MDTS");
        return {0, TypeGeneric, CodeInvalidField};
    }
    if (lba >= ns.disk->Sectors() || blocks > ns.disk->Sectors() - lba)
    {
        return {0, TypeGeneric, CodeLbaRange};
    }
    std::vector<Segment> segments;
    if (!Gather(sqe.prp1, sqe.prp2, static_cast<uint32_t>(bytes), &segments))
    {
        return {0, TypeGeneric, CodeInvalidField};
    }
    std::vector<uint8_t> data(bytes);
    if (write)
    {
        size_t at = 0;
        for (const Segment& segment : segments)
        {
            for (uint32_t i = 0; i < segment.length; i++)
            {
                data[at++] = segment.host[i];
            }
        }
        ns.disk->Write(lba, data);
        return {};
    }
    ns.disk->Read(lba, data);
    size_t at = 0;
    for (const Segment& segment : segments)
    {
        for (uint32_t i = 0; i < segment.length; i++)
        {
            segment.host[i] = data[at++];
        }
    }
    return {};
}

void FakeNvme::Post(uint32_t qid, uint16_t cid, const Outcome& outcome)
{
    const uint32_t cqid = sq_[qid].cqid;
    const uint16_t head = static_cast<uint16_t>(sq_[qid].head);
    if (qid == 1)
    {
        for (const uint16_t stale : stale_)
        {
            PostRaw(cqid, head, 1, stale, 0, TypeGeneric, CodeInternalError);
        }
        stale_.clear();
    }
    PostRaw(cqid, head, static_cast<uint16_t>(qid), cid, outcome.result, outcome.type,
            outcome.code);
}

void FakeNvme::PostRaw(uint32_t cqid, uint16_t sq_head, uint16_t sqid, uint16_t cid,
                       uint32_t result, uint8_t type, uint8_t code)
{
    CompletionQueue& cq = cq_[cqid];
    if (!cq.exists)
    {
        Fault("completion for a queue that does not exist");
        return;
    }
    if ((cq.tail + 1) % cq.entries == cq.head)
    {
        Fault("completion queue overrun");
        return;
    }
    volatile uint8_t* raw = memory_.Translate(cq.base + static_cast<uint64_t>(cq.tail) * 16, 16);
    if (raw == nullptr)
    {
        Fault("completion queue is not in memory");
        return;
    }
    auto* slot = reinterpret_cast<volatile uint32_t*>(raw);
    slot[0] = result;
    slot[1] = 0;
    slot[2] = sq_head | (static_cast<uint32_t>(sqid) << 16);
    const uint32_t status = (cq.phase ? 1U : 0U) | (static_cast<uint32_t>(code) << 1) |
                            (static_cast<uint32_t>(type) << 9);
    __atomic_store_n(&slot[3], cid | (status << 16), __ATOMIC_RELEASE);
    cq.tail = (cq.tail + 1) % cq.entries;
    if (cq.tail == 0)
    {
        cq.phase = !cq.phase;
    }
    if (handler_ != nullptr && !options.drop_interrupts &&
        options.interrupts != InterruptKind::Polling && (cqid == 0 || cq.interrupts))
    {
        interrupts_++;
        interrupts_due_++;
    }
}

uint32_t FakeNvme::ReadConfig32(uint16_t offset)
{
    if (offset == 0x2C)
    {
        return (static_cast<uint32_t>(options.subsystem) << 16) | options.subsystem_vendor;
    }
    return 0;
}

bool FakeNvme::AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer)
{
    const Scope scope(*this);
    if (allocations_ >= options.allocations_before_failure)
    {
        return false;
    }
    allocations_++;
    const Memory::Dma::Buffer allocated = memory_.Allocate(bytes);
    if (allocated.physical + bytes > below)
    {
        Fault("buffer above the physical limit asked for");
        return false;
    }
    live_.push_back(allocated);
    *buffer = allocated;
    return true;
}

void FakeNvme::FreeDma(const Memory::Dma::Buffer& buffer)
{
    const Scope scope(*this);
    const auto it = std::find_if(live_.begin(), live_.end(), [&](const Memory::Dma::Buffer& b)
                                 { return b.physical == buffer.physical; });
    if (it == live_.end())
    {
        Fault("freed a buffer that is not allocated");
        return;
    }
    if ((cc_ & N::CcBit::Enable) != 0 || ready_)
    {
        Fault("buffer freed while the controller may still use it");
    }
    live_.erase(it);
}

InterruptKind FakeNvme::SetUpInterrupts(void (*handler)(void* context), void* context)
{
    const Scope scope(*this);
    handler_ = handler;
    handler_context_ = context;
    return options.interrupts;
}

void FakeNvme::StallIo(uint32_t count)
{
    HoldIo(Hold::Never, count);
}

void FakeNvme::HoldIo(Hold mode, uint32_t count, Effect effect)
{
    const Scope scope(*this);
    stall_ = count;
    hold_mode_ = mode;
    hold_effect_ = effect;
}

void FakeNvme::StallAdmin(uint32_t count)
{
    const Scope scope(*this);
    stall_admin_ = count;
}

void FakeNvme::HoldCommand(const N::Sqe& sqe, uint16_t cid)
{
    held_ = {sqe, cid, hold_mode_, true};
    if (hold_effect_ == Effect::Fatal)
    {
        fatal_ = true;
    }
    else if (hold_effect_ == Effect::Unplug)
    {
        gone_ = true;
    }
}

void FakeNvme::Stop()
{
    if (held_.active)
    {
        if (held_.mode == Hold::LateDma)
        {
            events_.push_back(Event::LateDma);
            Io(held_.sqe);
        }
        held_.active = false;
    }
    events_.push_back(Event::Stopped);
}

bool FakeNvme::HoldActive() const
{
    return held_.active;
}

void FakeNvme::InjectStale(uint16_t cid)
{
    const Scope scope(*this);
    stale_.push_back(cid);
}

void FakeNvme::Unplug()
{
    const Scope scope(*this);
    gone_ = true;
}

uint32_t FakeNvme::Count(Event event) const
{
    return static_cast<uint32_t>(std::count(events_.begin(), events_.end(), event));
}

const std::vector<FakeNvme::Event>& FakeNvme::Events() const
{
    return events_;
}

const std::vector<FakeNvme::IoRecord>& FakeNvme::IoLog() const
{
    return io_log_;
}

const std::vector<std::string>& FakeNvme::Faults() const
{
    return faults_;
}

uint32_t FakeNvme::Interrupts() const
{
    return interrupts_;
}

uint32_t FakeNvme::LiveBuffers() const
{
    return static_cast<uint32_t>(live_.size());
}

bool FakeNvme::Enabled() const
{
    return (cc_ & N::CcBit::Enable) != 0;
}

uint32_t FakeNvme::IoQueueEntries() const
{
    return sq_[1].entries;
}

uint32_t FakeNvme::IoCommands() const
{
    return io_commands_;
}

} // namespace FeatherOS::Storage::Testing
