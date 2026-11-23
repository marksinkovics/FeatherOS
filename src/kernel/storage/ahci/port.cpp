#include "port.h"

#include "kernel/klogger.h"
#include "kernel/storage/ahci/register_wait.h"
#include "kernel/string/format.h"

#include <algorithm>
#include <expected>
#include <optional>
#include <span>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Storage::Ahci
{
namespace
{

constexpr uint32_t Gone32 = 0xFFFFFFFF;
constexpr uint32_t IdentifyBytes = Ata::IdentifyBytes;
constexpr uint32_t ErrorMask = IsBit::TaskFileError | IsBit::HostBusFatal | IsBit::HostBusData |
                               IsBit::InterfaceFatal | IsBit::Overflow | IsBit::PhyRdyChange;
constexpr uint32_t InterruptMask = IsBit::DeviceToHost | IsBit::PioSetup | ErrorMask;

constinit KLogger logger;

template <typename... Args>
void SayTo(bool warning, const char* prefix, const char* format, const Args&... args)
{
    char text[144];
    char line[224];
    if (!String::FormatTo(text, sizeof(text), format, args...) ||
        !String::FormatTo(line, sizeof(line), "Storage: {}: {}", prefix, text))
    {
        return;
    }
    if (warning)
    {
        logger.Warning(line);
    }
    else
    {
        logger.Info(line);
    }
}

uint8_t* Bytes(const Memory::Dma::Buffer& buffer)
{
    return const_cast<uint8_t*>(buffer.address);
}

void Store(volatile uint8_t* destination, const void* source, size_t length)
{
    const auto* bytes = static_cast<const uint8_t*>(source);
    for (size_t i = 0; i < length; i++)
    {
        destination[i] = bytes[i];
    }
}

uint32_t Load32(const volatile uint8_t* source)
{
    return static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8) |
           (static_cast<uint32_t>(source[2]) << 16) | (static_cast<uint32_t>(source[3]) << 24);
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

void Hex(uint32_t value, char (&out)[3])
{
    constexpr char Digits[] = "0123456789abcdef";
    out[0] = Digits[(value >> 4) & 0xF];
    out[1] = Digits[value & 0xF];
    out[2] = '\0';
}

} // namespace

Port::~Port()
{
    Sync::MutexGuard guard(io_lock_);
    if (page_.size == 0)
    {
        return;
    }
    if (!StopEngine(true))
    {
        pinned_ = true;
    }
    ReleaseBuffers();
}

void Port::Initialize(const PortConfig& config)
{
    hardware_ = config.hardware;
    environment_ = config.environment;
    controller_index_ = config.controller_index;
    number_ = config.number;
    policy_ = config.policy;
    dma_below_ = config.dma_below;
    staggered_ = config.staggered_spin_up;
    CopyText(location_, sizeof(location_), config.location);
}

uint32_t Port::Number() const
{
    return number_;
}

bool Port::Candidate() const
{
    return candidate_.load(std::memory_order_acquire);
}

bool Port::Active() const
{
    return started_.load(std::memory_order_acquire);
}

bool Port::Dead() const
{
    return dead_.load(std::memory_order_acquire);
}

uint64_t Port::write_commands() const
{
    return write_commands_.load(std::memory_order_relaxed);
}

uint64_t Port::flush_commands() const
{
    return flush_commands_.load(std::memory_order_relaxed);
}

uint64_t Port::standby_commands() const
{
    return standby_commands_.load(std::memory_order_relaxed);
}

bool Port::write_cache() const
{
    return identity_.write_cache_enabled;
}

uint32_t Port::Read(uint32_t offset) const
{
    return hardware_->Read32(PortRegister(number_, offset));
}

void Port::Write(uint32_t offset, uint32_t value) const
{
    hardware_->Write32(PortRegister(number_, offset), value);
}

bool Port::WaitFor(uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms) const
{
    return WaitForRegister(*hardware_, *environment_, this, PortRegister(number_, offset), mask,
                           value, timeout_ms);
}

void Port::Name(char (&out)[48]) const
{
    if (const Disk* disk = disk_.load(std::memory_order_acquire))
    {
        CopyText(out, sizeof(out), disk->Name());
    }
    else if (!String::FormatTo(out, sizeof(out), "AHCI {} port {}", location_, number_))
    {
        out[0] = '\0';
    }
}

bool Port::Quiesce()
{
    Sync::MutexGuard guard(io_lock_);
    return StopEngine(true);
}

bool Port::StopEngine(bool receive_fis)
{
    const uint32_t command = Read(PortReg::Cmd);
    if ((command & CmdBit::Start) != 0)
    {
        Write(PortReg::Cmd, command & ~CmdBit::Start);
    }
    if (!WaitFor(PortReg::Cmd, CmdBit::CommandListRunning, 0, EngineStopMs))
    {
        return false;
    }
    if (!receive_fis)
    {
        return true;
    }
    const uint32_t current = Read(PortReg::Cmd);
    if ((current & CmdBit::FisReceiveEnable) != 0)
    {
        Write(PortReg::Cmd, current & ~CmdBit::FisReceiveEnable);
    }
    return WaitFor(PortReg::Cmd, CmdBit::FisReceiveRunning, 0, EngineStopMs);
}

void Port::StartEngine()
{
    Write(PortReg::Cmd, Read(PortReg::Cmd) | CmdBit::FisReceiveEnable);
    Write(PortReg::Cmd, Read(PortReg::Cmd) | CmdBit::Start);
}

void Port::ReleaseBuffers()
{
    if (pinned_)
    {
        return;
    }
    for (Memory::Dma::Buffer* buffer : {&page_, &bounce_})
    {
        if (buffer->size != 0)
        {
            hardware_->FreeDma(*buffer);
            *buffer = {};
        }
    }
}

void Port::Retire()
{
    // Once CR and FR are 0 the HBA does no more DMA, so the buffers are ours again (I3).
    if (!StopEngine(true))
    {
        pinned_ = true;
    }
    ReleaseBuffers();
}

bool Port::Probe()
{
    Sync::MutexGuard guard(io_lock_);
    if (!ProbeLocked())
    {
        Retire();
        return false;
    }
    candidate_.store(true, std::memory_order_release);
    return true;
}

bool Port::ProbeLocked()
{
    return Prepare() && Link() && Detect();
}

bool Port::Prepare()
{
    char name[48];
    Name(name);
    if (!hardware_->AllocateDma(CommandPageBytes, dma_below_, &page_))
    {
        SayTo(true, name, "out of memory for the command list");
        return false;
    }
    // The registers below may only change while ST, CR, FRE and FR are all 0 (I6).
    if (!StopEngine(true))
    {
        pinned_ = true;
        SayTo(true, name, "port did not stop");
        return false;
    }
    const uint64_t list = page_.physical + CommandListOffset;
    const uint64_t fis = page_.physical + ReceivedFisOffset;
    Write(PortReg::Clb, static_cast<uint32_t>(list));
    Write(PortReg::Clbu, static_cast<uint32_t>(list >> 32));
    Write(PortReg::Fb, static_cast<uint32_t>(fis));
    Write(PortReg::Fbu, static_cast<uint32_t>(fis >> 32));
    Write(PortReg::Serr, Gone32);
    Write(PortReg::Is, Gone32);
    hardware_->Write32(Reg::Is, 1U << number_);
    Write(PortReg::Cmd, Read(PortReg::Cmd) | CmdBit::FisReceiveEnable);
    return true;
}

bool Port::Comreset()
{
    const uint32_t control = Read(PortReg::Sctl) & ~SctlField::DetMask;
    Write(PortReg::Sctl, control | SctlField::DetReset);
    environment_->Sleep(ResetHoldMs);
    Write(PortReg::Sctl, control);
    return WaitFor(PortReg::Ssts, SstsField::DetMask, SstsField::DetPresent, LinkWaitMs);
}

bool Port::Link()
{
    uint32_t budget = PresenceWaitMs;
    if (staggered_)
    {
        Write(PortReg::Cmd, Read(PortReg::Cmd) | CmdBit::SpinUp);
        spun_up_ = true;
        budget = LinkWaitMs;
    }
    const uint64_t deadline = environment_->NowMilliseconds() + budget;
    for (;;)
    {
        const uint32_t det = Read(PortReg::Ssts) & SstsField::DetMask;
        if (det == SstsField::DetPresent)
        {
            return true;
        }
        if (det == SstsField::DetNoPhy)
        {
            return Comreset();
        }
        if (environment_->NowMilliseconds() >= deadline)
        {
            return false;
        }
        environment_->Sleep(WaitStepMs);
    }
}

bool Port::Detect()
{
    char name[48];
    Name(name);
    if (!WaitFor(PortReg::Tfd, TfdBit::Busy | TfdBit::DataRequest, 0, ReadyWaitMs))
    {
        SayTo(true, name, "device not ready");
        return false;
    }
    const DeviceKind kind = ClassifySignature(Read(PortReg::Sig));
    if (kind != DeviceKind::Ata)
    {
        SayTo(true, name, "{} not supported", DeviceKindText(kind));
        return false;
    }
    // Link-up leaves PRCS and SERR bits behind; they must not look like a later unplug.
    Write(PortReg::Serr, Gone32);
    Write(PortReg::Is, Gone32);
    hardware_->Write32(Reg::Is, 1U << number_);
    return true;
}

void Port::EnableInterrupts()
{
    Sync::MutexGuard guard(io_lock_);
    Write(PortReg::Ie, InterruptMask);
}

bool Port::Start()
{
    Sync::MutexGuard guard(io_lock_);
    if (!StartLocked())
    {
        Retire();
        return false;
    }
    return true;
}

bool Port::StartLocked()
{
    char name[48];
    Name(name);
    if (!hardware_->AllocateDma(BounceBytes, dma_below_, &bounce_))
    {
        SayTo(true, name, "out of memory for the transfer buffer");
        return false;
    }
    StartEngine();
    const Command identify{BuildNoData(AtaCommand::IdentifyDevice), "IDENTIFY DEVICE",
                           IdentifyBytes, false, IoTimeoutMs};
    if (!Issue(identify))
    {
        return false;
    }
    uint16_t words[Ata::IdentifyWords];
    for (size_t i = 0; i < Ata::IdentifyWords; i++)
    {
        words[i] =
            static_cast<uint16_t>(bounce_.address[2 * i] | (bounce_.address[2 * i + 1] << 8));
    }
    const Ata::AtaIdentity identity =
        Ata::ParseIdentify(std::span<const uint16_t, Ata::IdentifyWords>(words));
    if (identity.refusal != Ata::Refusal::None)
    {
        SayTo(true, name, "{}", Ata::RefusalText(identity.refusal));
        return false;
    }
    return Accept(identity);
}

bool Port::Accept(const Ata::AtaIdentity& identity)
{
    identity_ = identity;
    const bool policy_read_only = !WritesAllowed(policy_);
    info_ = {};
    info_.kind = DiskKind::Ahci;
    info_.sector_size = identity.sector_size;
    info_.sector_count = identity.sector_count;
    info_.max_transfer_bytes = BounceBytes;
    info_.removable = false;
    info_.read_only = policy_read_only;
    info_.read_only_reason = policy_read_only ? ReadOnlyReason::Policy : ReadOnlyReason::None;
    info_.controller_index = controller_index_;
    CopyText(info_.model, sizeof(info_.model), identity.model);
    CopyText(info_.location, sizeof(info_.location), location_);
    started_.store(true, std::memory_order_release);
    return true;
}

void Port::Publish()
{
    Disk* disk = Register(*this);
    char name[48];
    if (disk == nullptr)
    {
        Name(name);
        SayTo(true, name, "disk registry full");
        return;
    }
    disk_.store(disk, std::memory_order_release);
    Name(name);
    SayTo(false, name, "AHCI {} port {}, \"{}\" \"{}\", {} KiB, {}-byte sectors, {}, {}", location_,
          number_, identity_.model, identity_.serial, info_.sector_count * info_.sector_size / 1024,
          info_.sector_size, identity_.write_cache_enabled ? "write cache" : "no write cache",
          info_.read_only ? "ro" : "rw");
    if (Dead())
    {
        Remove(*disk);
    }
    else
    {
        Scan(*disk);
    }
}

DiskInfo Port::Info()
{
    return info_;
}

bool Port::Gone()
{
    return Dead();
}

FeatherDisk::Status Port::Read(uint64_t sector, uint32_t count, uint8_t* buffer)
{
    if (sector > info_.sector_count || count > info_.sector_count - sector)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    return Transfer(false, sector, count, buffer, nullptr);
}

FeatherDisk::Status Port::Write(uint64_t sector, uint32_t count, const uint8_t* buffer)
{
    if (sector > info_.sector_count || count > info_.sector_count - sector)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    return Transfer(true, sector, count, nullptr, buffer);
}

FeatherDisk::Status Port::Stop()
{
    return std::unexpected(FeatherDisk::Error::Unsupported);
}

void Port::Release() {}

FeatherDisk::Status Port::Transfer(bool write, uint64_t sector, uint32_t count, uint8_t* in,
                                   const uint8_t* out)
{
    if (write && info_.read_only)
    {
        return std::unexpected(FeatherDisk::Error::ReadOnly);
    }
    if (count == 0)
    {
        return {};
    }
    Sync::MutexGuard guard(io_lock_);
    if (Dead() || shutting_down_.load(std::memory_order_acquire))
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    const uint32_t sector_size = info_.sector_size;
    const uint32_t per_command = std::min(BounceBytes / sector_size, MaxSectorsPerCommand);
    uint32_t done = 0;
    while (done < count)
    {
        const uint32_t sectors = std::min(count - done, per_command);
        const uint32_t bytes = sectors * sector_size;
        const size_t offset = static_cast<size_t>(done) * sector_size;
        const std::optional<H2dFis> fis = BuildTransfer(write, sector + done, sectors);
        if (!fis)
        {
            return std::unexpected(FeatherDisk::Error::InvalidArgument);
        }
        if (write)
        {
            std::copy_n(out + offset, bytes, Bytes(bounce_));
            write_commands_.fetch_add(1, std::memory_order_relaxed);
        }
        const Command command{*fis, write ? "WRITE DMA EXT" : "READ DMA EXT", bytes, write,
                              IoTimeoutMs};
        if (const FeatherDisk::Status status = Issue(command); !status)
        {
            return status;
        }
        if (!write)
        {
            std::copy_n(Bytes(bounce_), bytes, in + offset);
        }
        done += sectors;
    }
    return {};
}

FeatherDisk::Status Port::Flush()
{
    if (info_.read_only || !identity_.write_cache_enabled)
    {
        return {};
    }
    Sync::MutexGuard guard(io_lock_);
    if (Dead() || shutting_down_.load(std::memory_order_acquire))
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    const bool extended = identity_.flush_cache_ext_supported;
    const uint8_t opcode = extended ? AtaCommand::FlushCacheExt : AtaCommand::FlushCache;
    flush_commands_.fetch_add(1, std::memory_order_relaxed);
    return Issue({BuildNoData(opcode), extended ? "FLUSH CACHE EXT" : "FLUSH CACHE", 0, false,
                  FlushTimeoutMs});
}

bool Port::ReadyCommand(void* context)
{
    auto* completion = static_cast<Completion*>(context);
    const Port& port = *completion->port;
    completion->ci = port.Read(PortReg::Ci);
    completion->is = port.Read(PortReg::Is);
    return completion->ci == Gone32 || (completion->ci & 1) == 0 ||
           (completion->is & ErrorMask) != 0;
}

bool Port::AwaitCompletion(uint64_t deadline_ms, Completion* completion)
{
    for (;;)
    {
        const uint64_t now = environment_->NowMilliseconds();
        const uint64_t until = std::min<uint64_t>(now + WaitStepMs, deadline_ms);
        if (environment_->WaitUntil(this, until, ReadyCommand, completion))
        {
            return true;
        }
        if (environment_->NowMilliseconds() >= deadline_ms)
        {
            return false;
        }
    }
}

FeatherDisk::Status Port::Issue(const Command& command)
{
    volatile uint8_t* table = page_.address + CommandTableOffset;
    const uint64_t table_physical = page_.physical + CommandTableOffset;
    const bool has_data = command.bytes != 0;
    const bool read = has_data && !command.write;
    const std::optional<CommandHeader> header =
        BuildCommandHeader(command.write, has_data ? 1 : 0, table_physical);
    std::optional<Prd> prd;
    if (has_data)
    {
        prd = BuildPrd(bounce_.physical, command.bytes, false);
    }
    if (!header || (has_data && !prd))
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }

    Store(table, &command.fis, sizeof(command.fis));
    if (prd)
    {
        Store(table + PrdtOffset, &*prd, sizeof(*prd));
    }
    Store(page_.address + CommandListOffset, &*header, sizeof(*header));

    if ((Read(PortReg::Is) & ErrorMask) != 0)
    {
        return Fail(command.name, "port reports an error");
    }
    Memory::Dma::ProducerBarrier();
    Write(PortReg::Ci, 1);

    Completion completion{this, 0, 0};
    if (!AwaitCompletion(environment_->NowMilliseconds() + command.timeout_ms, &completion))
    {
        return Fail(command.name, "timed out");
    }
    if (completion.ci == Gone32)
    {
        return Fail(command.name, "HBA is gone");
    }
    if ((completion.is & ErrorMask) != 0)
    {
        return Fail(command.name, "failed");
    }
    Memory::Dma::ConsumerBarrier();
    Write(PortReg::Is, completion.is);
    hardware_->Write32(Reg::Is, 1U << number_);
    if ((Read(PortReg::Tfd) & TfdBit::Error) != 0)
    {
        return Fail(command.name, "failed");
    }
    if (read &&
        Load32(page_.address + CommandListOffset + offsetof(CommandHeader, prdbc)) != command.bytes)
    {
        char name[48];
        Name(name);
        SayTo(true, name, "{} moved {} of {} bytes", command.name,
              Load32(page_.address + CommandListOffset + offsetof(CommandHeader, prdbc)),
              command.bytes);
        return std::unexpected(FeatherDisk::Error::Io);
    }
    return {};
}

FeatherDisk::Status Port::Fail(const char* name, const char* what)
{
    const uint32_t tfd = Read(PortReg::Tfd);
    char status[3];
    char error[3];
    Hex(tfd & 0xFF, status);
    Hex((tfd >> 8) & 0xFF, error);
    char port[48];
    Name(port);
    SayTo(true, port, "{} {} (status {}h error {}h)", name, what, status, error);
    MakeDead();
    return std::unexpected(FeatherDisk::Error::Io);
}

void Port::MakeDead()
{
    if (dead_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    // After CR is 0 the HBA does no more DMA, so the buffers stay safe to reuse (I3).
    if (!StopEngine(false))
    {
        pinned_ = true;
    }
    char name[48];
    Name(name);
    SayTo(true, name, "port failed; its disk is gone");
    if (Disk* disk = disk_.load(std::memory_order_acquire))
    {
        Remove(*disk);
    }
}

void Port::Shutdown(Power::Action action)
{
    Sync::MutexGuard guard(io_lock_);
    if (!started_.load(std::memory_order_acquire) || Dead() ||
        shutting_down_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    if (action != Power::Action::PowerOff)
    {
        return;
    }
    standby_commands_.fetch_add(1, std::memory_order_relaxed);
    char name[48];
    Name(name);
    if (Issue({BuildNoData(AtaCommand::StandbyImmediate), "STANDBY IMMEDIATE", 0, false,
               StandbyTimeoutMs}))
    {
        SayTo(false, name, "parked");
    }
}

} // namespace FeatherOS::Storage::Ahci
