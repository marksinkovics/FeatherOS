#include "fake_ahci.h"

#include "kernel/storage/ahci/fis.h"
#include "kernel/storage/ahci/registers.h"

#include <algorithm>
#include <cstring>

namespace FeatherOS::Storage::Testing
{
namespace A = FeatherOS::Storage::Ahci;

namespace
{

constexpr uint32_t Gone32 = 0xFFFFFFFF;
constexpr uint64_t FourGiB = 1ULL << 32;
constexpr uint64_t PageSize = 4096;
constexpr uint32_t PrdtOffset = 0x80;
constexpr uint32_t MaxPrds = 8;
constexpr uint32_t Status = 0x51;
constexpr uint32_t ErrorAbort = 0x04;
constexpr uint32_t ErrorUncorrectable = 0x40;
constexpr uint32_t ErrorIdNotFound = 0x10;
constexpr uint32_t EngineMask = A::CmdBit::Start | A::CmdBit::FisReceiveEnable;
constexpr uint32_t LinkDiagnostic = 1U << 16;

uint32_t Load32(const volatile uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void Store32(volatile uint8_t* p, uint32_t value)
{
    for (uint32_t i = 0; i < 4; i++)
    {
        p[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void PutText(std::array<uint16_t, 256>& words, size_t first, size_t chars, const std::string& text)
{
    for (size_t i = 0; i < chars; i++)
    {
        const char c = i < text.size() ? text[i] : ' ';
        uint16_t& word = words[first + i / 2];
        if (i % 2 == 0)
        {
            word = static_cast<uint16_t>((word & 0x00FF) | (static_cast<uint8_t>(c) << 8));
        }
        else
        {
            word = static_cast<uint16_t>((word & 0xFF00) | static_cast<uint8_t>(c));
        }
    }
}

} // namespace

// Every public entry point holds the lock; interrupts are raised after the outermost one lets go,
// as a device raises them without holding anything the handler may need.
class FakeAhci::Scope
{
public:
    explicit Scope(FakeAhci& fake) : fake_(fake)
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
    FakeAhci& fake_;
};

FakeAhci::FakeAhci(FeatherOS::Testing::PhysicalMemory& memory) : memory_(memory) {}

ModelDisk& FakeAhci::AddPort(uint32_t number)
{
    return AddPort(number, PortOptions{});
}

ModelDisk& FakeAhci::AddPort(uint32_t number, const PortOptions& config)
{
    PortState& port = ports_.at(number);
    port.present = true;
    port.config = config;
    port.disk = std::make_unique<ModelDisk>(config.sector_size, config.sectors,
                                            config.write_cache_enabled);
    return *port.disk;
}

ModelDisk& FakeAhci::PortDisk(uint32_t number)
{
    return *ports_.at(number).disk;
}

void FakeAhci::Fault(const std::string& what)
{
    faults_.push_back(what);
}

uint32_t FakeAhci::Implemented() const
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < PortCount; i++)
    {
        mask |= ports_[i].present ? 1U << i : 0;
    }
    return mask;
}

bool FakeAhci::Running(const PortState& port) const
{
    return (port.cmd & EngineMask) != 0 || port.cr || port.fr;
}

volatile uint8_t* FakeAhci::Host(uint64_t physical, uint64_t length) const
{
    if (volatile uint8_t* host = memory_.Translate(physical, length))
    {
        return host;
    }
    return low_.Translate(physical, length);
}

void FakeAhci::Initialize()
{
    if (initialized_)
    {
        return;
    }
    initialized_ = true;
    bios_owned_ = options.firmware_owns;
    for (uint32_t i = 0; i < PortCount; i++)
    {
        PortState& port = ports_[i];
        if (!port.present)
        {
            continue;
        }
        ResetPort(i);
        if (port.config.engine_running)
        {
            const Memory::Dma::Buffer page = memory_.Allocate(PageSize);
            port.clb = page.physical;
            port.fb = page.physical + 0x400;
            port.cmd = EngineMask;
            port.cr = true;
            port.fr = true;
        }
    }
}

void FakeAhci::ResetPort(uint32_t number)
{
    PortState& port = ports_[number];
    port.clb = 0;
    port.fb = 0;
    port.is = 0;
    port.ie = 0;
    port.cmd = 0;
    port.serr = 0;
    port.sctl = 0;
    port.sact = 0;
    port.ci = 0;
    port.cr = false;
    port.fr = false;
    port.cr_stopping = false;
    port.fr_stopping = false;
    port.tfd_status = 0x50;
    port.tfd_error = 0;
    port.hanging = false;
    port.link = Link::Down;
    if (!options.sss)
    {
        BeginLink(number);
    }
}

void FakeAhci::ResetHba()
{
    ghc_ = 0;
    resets_++;
    for (uint32_t i = 0; i < PortCount; i++)
    {
        if (ports_[i].present)
        {
            ResetPort(i);
        }
    }
}

void FakeAhci::BeginLink(uint32_t number)
{
    PortState& port = ports_[number];
    if (port.config.device == Device::None)
    {
        port.link = Link::Down;
        return;
    }
    port.link = Link::Pending;
    port.link_reads = port.config.link_delay_reads;
    port.busy_left = port.config.busy_reads;
}

uint32_t FakeAhci::Read32(uint32_t offset)
{
    const Scope scope(*this);
    Initialize();
    switch (offset)
    {
    case A::Reg::Cap:
    {
        uint32_t highest = 0;
        for (uint32_t i = 0; i < PortCount; i++)
        {
            highest = ports_[i].present ? i : highest;
        }
        return highest | ((options.slots - 1) << 8) | (options.sss ? A::CapBit::Sss : 0) |
               A::CapBit::Sncq | (options.s64a ? A::CapBit::S64a : 0);
    }
    case A::Reg::Ghc:
        return ReadGhc();
    case A::Reg::Is:
        return ReadIs();
    case A::Reg::Pi:
        return Implemented();
    case A::Reg::Vs:
        return options.version;
    case A::Reg::Cap2:
        return options.boh ? A::Cap2Bit::Boh : 0;
    case A::Reg::Bohc:
        return ReadBohc();
    default:
        break;
    }
    if (offset >= A::Reg::PortBase)
    {
        const uint32_t number = (offset - A::Reg::PortBase) / A::Reg::PortStride;
        if (number < PortCount && ports_[number].present)
        {
            return ReadPort(number, (offset - A::Reg::PortBase) % A::Reg::PortStride);
        }
    }
    return 0;
}

uint64_t FakeAhci::Read64(uint32_t offset)
{
    const Scope scope(*this);
    const uint64_t low = Read32(offset);
    const uint64_t high = Read32(offset + 4);
    return low | (high << 32);
}

void FakeAhci::Write64(uint32_t offset, uint64_t value)
{
    const Scope scope(*this);
    Write32(offset, static_cast<uint32_t>(value));
    Write32(offset + 4, static_cast<uint32_t>(value >> 32));
}

uint32_t FakeAhci::ReadGhc()
{
    if (hr_pending_)
    {
        if (options.hr_never_clears)
        {
            return ghc_ | A::GhcBit::Reset;
        }
        if (hr_reads_ > 0)
        {
            hr_reads_--;
            return ghc_ | A::GhcBit::Reset;
        }
        hr_pending_ = false;
        ResetHba();
    }
    return ghc_;
}

uint32_t FakeAhci::ReadBohc()
{
    if (os_owned_ && bios_owned_ && !options.bios_never_releases)
    {
        if (bios_reads_ > 0)
        {
            bios_reads_--;
        }
        else
        {
            bios_owned_ = false;
            bios_busy_ = options.bios_stays_busy || options.bios_busy_reads > 0;
            busy_reads_ = options.bios_busy_reads;
        }
    }
    else if (bios_busy_ && !options.bios_stays_busy)
    {
        if (busy_reads_ > 0)
        {
            busy_reads_--;
        }
        else
        {
            bios_busy_ = false;
        }
    }
    return (bios_owned_ ? A::BohcBit::BiosOwned : 0) | (os_owned_ ? A::BohcBit::OsOwned : 0) |
           (bios_busy_ ? A::BohcBit::BiosBusy : 0);
}

uint32_t FakeAhci::ReadIs() const
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < PortCount; i++)
    {
        value |= ports_[i].present && (ports_[i].is & ports_[i].ie) != 0 ? 1U << i : 0;
    }
    return value;
}

uint32_t FakeAhci::ReadPort(uint32_t number, uint32_t offset)
{
    PortState& port = ports_[number];
    switch (offset)
    {
    case A::PortReg::Clb:
        return static_cast<uint32_t>(port.clb);
    case A::PortReg::Clbu:
        return static_cast<uint32_t>(port.clb >> 32);
    case A::PortReg::Fb:
        return static_cast<uint32_t>(port.fb);
    case A::PortReg::Fbu:
        return static_cast<uint32_t>(port.fb >> 32);
    case A::PortReg::Is:
        return port.is;
    case A::PortReg::Ie:
        return port.ie;
    case A::PortReg::Cmd:
        return ReadCmd(port);
    case A::PortReg::Tfd:
        return ReadTfd(port);
    case A::PortReg::Sig:
        return ReadSignature(port);
    case A::PortReg::Ssts:
        return ReadSsts(number, port);
    case A::PortReg::Sctl:
        return port.sctl;
    case A::PortReg::Serr:
        return port.serr;
    case A::PortReg::Sact:
        return port.sact;
    case A::PortReg::Ci:
        return port.ci;
    default:
        return 0;
    }
}

uint32_t FakeAhci::ReadCmd(PortState& port)
{
    if (port.cr_stopping && !options.engine_never_stops)
    {
        if (port.cr_reads > 0)
        {
            port.cr_reads--;
        }
        else
        {
            port.cr = false;
            port.cr_stopping = false;
        }
    }
    if (port.fr_stopping && !options.engine_never_stops)
    {
        if (port.fr_reads > 0)
        {
            port.fr_reads--;
        }
        else
        {
            port.fr = false;
            port.fr_stopping = false;
        }
    }
    return (port.cmd & (EngineMask | A::CmdBit::SpinUp)) |
           (port.cr ? A::CmdBit::CommandListRunning : 0) |
           (port.fr ? A::CmdBit::FisReceiveRunning : 0);
}

uint32_t FakeAhci::ReadSsts(uint32_t number, PortState& port)
{
    if (port.link == Link::Pending)
    {
        if (port.link_reads > 0)
        {
            port.link_reads--;
        }
        else if (port.config.det1_until_comreset)
        {
            port.link = Link::NoComm;
        }
        else
        {
            port.link = Link::Comm;
            port.is |= A::IsBit::PhyRdyChange;
            port.serr |= LinkDiagnostic;
            log_.push_back({Event::LinkUp, number, 0, 0});
        }
    }
    switch (port.link)
    {
    case Link::Comm:
        return 0x113;
    case Link::NoComm:
        return A::SstsField::DetNoPhy;
    default:
        return 0;
    }
}

uint32_t FakeAhci::ReadTfd(PortState& port)
{
    if (port.link != Link::Comm)
    {
        return 0x7F;
    }
    if (port.config.busy_forever)
    {
        return A::TfdBit::Busy;
    }
    if (port.busy_left > 0)
    {
        port.busy_left--;
        return A::TfdBit::Busy;
    }
    return port.tfd_status | (port.tfd_error << 8);
}

uint32_t FakeAhci::ReadSignature(const PortState& port) const
{
    if (port.link != Link::Comm)
    {
        return Gone32;
    }
    switch (port.config.device)
    {
    case Device::Ata:
        return 0x00000101;
    case Device::Atapi:
        return 0xEB140101;
    case Device::PortMultiplier:
        return 0x96690101;
    case Device::Semb:
        return 0xC33C0101;
    default:
        return Gone32;
    }
}

void FakeAhci::Write32(uint32_t offset, uint32_t value)
{
    const Scope scope(*this);
    Initialize();
    switch (offset)
    {
    case A::Reg::Ghc:
        WriteGhc(value);
        return;
    case A::Reg::Is:
        return;
    case A::Reg::Bohc:
        if ((value & A::BohcBit::OsOwned) != 0 && !os_owned_)
        {
            os_owned_ = true;
            bios_reads_ = options.bios_release_reads;
            log_.push_back({Event::HandoffRequested, 0, 0, 0});
        }
        return;
    default:
        break;
    }
    if (offset >= A::Reg::PortBase)
    {
        const uint32_t number = (offset - A::Reg::PortBase) / A::Reg::PortStride;
        if (number < PortCount && ports_[number].present)
        {
            WritePort(number, (offset - A::Reg::PortBase) % A::Reg::PortStride, value);
        }
    }
}

void FakeAhci::WriteGhc(uint32_t value)
{
    if (hr_pending_)
    {
        return;
    }
    if ((value & A::GhcBit::Reset) != 0)
    {
        for (const PortState& port : ports_)
        {
            if (port.present && Running(port))
            {
                Fault("HBA reset while a port is running");
            }
        }
        hr_pending_ = true;
        hr_reads_ = options.hr_delay_reads;
        log_.push_back({Event::HbaReset, 0, 0, 0});
        return;
    }
    ghc_ = value & (A::GhcBit::AhciEnable | A::GhcBit::InterruptEnable);
}

void FakeAhci::WritePort(uint32_t number, uint32_t offset, uint32_t value)
{
    PortState& port = ports_[number];
    if ((ghc_ & A::GhcBit::AhciEnable) == 0)
    {
        Fault("port register written while GHC.AE is clear");
    }
    switch (offset)
    {
    case A::PortReg::Clb:
    case A::PortReg::Clbu:
    case A::PortReg::Fb:
    case A::PortReg::Fbu:
        if (Running(port))
        {
            Fault("PxCLB or PxFB written while ST, CR, FRE or FR is set");
        }
        WriteAddress(port, offset, value);
        break;
    case A::PortReg::Is:
        port.is &= ~value;
        break;
    case A::PortReg::Ie:
        port.ie = value;
        break;
    case A::PortReg::Cmd:
        WriteCmd(number, port, value);
        break;
    case A::PortReg::Sctl:
        WriteSctl(number, port, value);
        break;
    case A::PortReg::Serr:
        port.serr &= ~value;
        break;
    case A::PortReg::Sact:
        port.sact = value;
        break;
    case A::PortReg::Ci:
        WriteCi(number, port, value);
        break;
    default:
        break;
    }
}

void FakeAhci::WriteAddress(PortState& port, uint32_t offset, uint32_t value)
{
    switch (offset)
    {
    case A::PortReg::Clb:
        if ((value & 0x3FF) != 0)
        {
            Fault("PxCLB is not 1 KiB aligned");
        }
        port.clb = (port.clb & ~0xFFFFFFFFULL) | value;
        break;
    case A::PortReg::Fb:
        if ((value & 0xFF) != 0)
        {
            Fault("PxFB is not 256-byte aligned");
        }
        port.fb = (port.fb & ~0xFFFFFFFFULL) | value;
        break;
    case A::PortReg::Clbu:
        if (!options.s64a && value != 0)
        {
            Fault("PxCLBU written on an HBA without S64A");
        }
        port.clb = (port.clb & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    default:
        if (!options.s64a && value != 0)
        {
            Fault("PxFBU written on an HBA without S64A");
        }
        port.fb = (port.fb & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    }
}

void FakeAhci::WriteCmd(uint32_t number, PortState& port, uint32_t value)
{
    const uint32_t old = port.cmd;
    const bool start = (value & A::CmdBit::Start) != 0;
    const bool receive = (value & A::CmdBit::FisReceiveEnable) != 0;
    if (start && (old & A::CmdBit::Start) == 0)
    {
        if (!receive)
        {
            Fault("ST set while FRE is clear");
        }
        port.cr = true;
        port.cr_stopping = false;
        port.tfd_status = 0x50;
        port.tfd_error = 0;
        log_.push_back({Event::EngineStarted, number, 0, 0});
    }
    if (!start && (old & A::CmdBit::Start) != 0)
    {
        port.cr_stopping = true;
        port.cr_reads = options.engine_stop_delay_reads;
        port.ci = 0;
        port.hanging = false;
        port.tfd_status = 0x50;
        port.tfd_error = 0;
        log_.push_back({Event::EngineStopped, number, 0, 0});
    }
    if (receive && (old & A::CmdBit::FisReceiveEnable) == 0)
    {
        port.fr = true;
        port.fr_stopping = false;
    }
    if (!receive && (old & A::CmdBit::FisReceiveEnable) != 0)
    {
        if (port.cr || start)
        {
            Fault("FRE cleared while the command list is running");
        }
        port.fr_stopping = true;
        port.fr_reads = options.engine_stop_delay_reads;
    }
    if ((value & A::CmdBit::SpinUp) != 0 && (old & A::CmdBit::SpinUp) == 0)
    {
        log_.push_back({Event::SpinUp, number, 0, 0});
        if (options.sss)
        {
            BeginLink(number);
        }
    }
    port.cmd = value & (EngineMask | A::CmdBit::SpinUp);
}

void FakeAhci::WriteSctl(uint32_t number, PortState& port, uint32_t value)
{
    const uint32_t before = port.sctl & A::SctlField::DetMask;
    const uint32_t after = value & A::SctlField::DetMask;
    port.sctl = value;
    if (after == A::SctlField::DetReset && before != A::SctlField::DetReset)
    {
        log_.push_back({Event::Comreset, number, 0, 0});
    }
    if (before == A::SctlField::DetReset && after == 0 && port.link != Link::Down &&
        port.link != Link::Pending)
    {
        if (port.link == Link::NoComm)
        {
            port.link = Link::Comm;
            port.is |= A::IsBit::PhyRdyChange;
            port.serr |= LinkDiagnostic;
            log_.push_back({Event::LinkUp, number, 0, 0});
        }
        port.busy_left = port.config.busy_reads;
    }
}

void FakeAhci::WriteCi(uint32_t number, PortState& port, uint32_t value)
{
    if ((port.cmd & A::CmdBit::Start) == 0 || !port.cr)
    {
        Fault("PxCI written while the engine is not running");
        return;
    }
    if ((value & ~1U) != 0)
    {
        Fault("a command slot other than 0 was issued");
    }
    port.ci |= value;
    if (options.defer_processing)
    {
        pending_commands_ = true;
        return;
    }
    Execute(number, port);
}

void FakeAhci::Pump()
{
    const Scope scope(*this);
    if (!pending_commands_)
    {
        return;
    }
    pending_commands_ = false;
    for (uint32_t i = 0; i < PortCount; i++)
    {
        if (ports_[i].present)
        {
            Execute(i, ports_[i]);
        }
    }
}

void FakeAhci::Inject(uint32_t port, Failure failure, uint32_t count)
{
    const Scope scope(*this);
    ports_.at(port).failure = failure;
    ports_.at(port).failures = count;
}

void FakeAhci::RaiseIfEnabled(const PortState& port)
{
    if ((ghc_ & A::GhcBit::InterruptEnable) != 0 && (port.is & port.ie) != 0 &&
        options.interrupts != InterruptKind::Polling && !options.drop_interrupts)
    {
        interrupts_++;
        interrupts_due_++;
    }
}

void FakeAhci::Error(uint32_t, PortState& port, uint32_t status, uint32_t error)
{
    port.tfd_status = status;
    port.tfd_error = error;
    port.is |= A::IsBit::TaskFileError | A::IsBit::DeviceToHost;
    RaiseIfEnabled(port);
}

void FakeAhci::Complete(PortState& port, uint64_t prdbc_physical, uint32_t bytes, bool pio)
{
    if (volatile uint8_t* prdbc = Host(prdbc_physical, 4))
    {
        Store32(prdbc, bytes);
    }
    port.ci &= ~1U;
    port.is |= A::IsBit::DeviceToHost | (pio ? A::IsBit::PioSetup : 0);
    RaiseIfEnabled(port);
}

bool FakeAhci::GatherPrds(PortState& port, volatile uint8_t* table, uint32_t count,
                          uint32_t needed, std::vector<Segment>* segments)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const volatile uint8_t* entry = table + PrdtOffset + i * A::PrdBytes;
        const uint64_t address = Load32(entry) | (static_cast<uint64_t>(Load32(entry + 4)) << 32);
        const uint32_t dbc = Load32(entry + 12);
        const uint64_t bytes = (dbc & 0x3FFFFF) + 1ULL;
        if ((address & 1) != 0)
        {
            Fault("PRD address is odd");
        }
        if ((dbc & 1) == 0)
        {
            Fault("PRD byte count is not even (DBC bit 0 clear)");
        }
        if (!options.s64a && (address >= FourGiB || address + bytes > FourGiB))
        {
            Fault("PRD address above 4 GiB on an HBA without S64A");
        }
        volatile uint8_t* host = Host(address, bytes);
        if (host == nullptr)
        {
            Fault("PRD outside every allocated buffer");
            return false;
        }
        port.dma_bases.push_back(address);
        const uint64_t take = std::min<uint64_t>(bytes, needed - std::min<uint64_t>(total, needed));
        if (take != 0)
        {
            segments->push_back({host, static_cast<uint32_t>(take)});
        }
        total += bytes;
    }
    if (total < needed)
    {
        Fault("PRDT shorter than the transfer");
        return false;
    }
    return true;
}

std::array<uint16_t, 256> FakeAhci::IdentifyWords(const PortState& port) const
{
    const PortOptions& c = port.config;
    std::array<uint16_t, 256> words{};
    PutText(words, 10, 20, c.serial);
    PutText(words, 23, 8, "1.0");
    PutText(words, 27, 40, c.model);
    words[82] = static_cast<uint16_t>(0x4000 | (c.write_cache_supported ? 1U << 5 : 0));
    words[83] = static_cast<uint16_t>(0x4000 | (c.lba48 ? 1U << 10 : 0) |
                                      (c.flush_ext ? 1U << 13 : 0) | (c.flush ? 1U << 12 : 0));
    words[85] = static_cast<uint16_t>(0x4000 | (c.write_cache_enabled ? 1U << 5 : 0));
    words[86] = static_cast<uint16_t>((c.lba48 ? 1U << 10 : 0) | (c.flush_ext ? 1U << 13 : 0) |
                                      (c.flush ? 1U << 12 : 0));
    for (uint32_t i = 0; i < 4; i++)
    {
        words[100 + i] = static_cast<uint16_t>(c.sectors >> (16 * i));
    }
    words[106] = 0x4000;
    if (c.sector_size == 4096)
    {
        words[106] = 0x5000;
        words[117] = 2048;
    }
    words[255] = 0x00A5;
    uint8_t sum = 0;
    for (const uint16_t word : words)
    {
        sum = static_cast<uint8_t>(sum + (word & 0xFF) + (word >> 8));
    }
    words[255] = static_cast<uint16_t>(0x00A5 | (static_cast<uint8_t>(0 - sum) << 8));
    return words;
}

void FakeAhci::Execute(uint32_t number, PortState& port)
{
    if ((port.ci & 1) == 0 || port.hanging || !port.cr)
    {
        return;
    }
    volatile uint8_t* header = Host(port.clb, 32);
    if (header == nullptr)
    {
        Fault("command list is not in allocated memory");
        port.hanging = true;
        return;
    }
    const uint32_t flags = Load32(header);
    const uint64_t table_address =
        Load32(header + 8) | (static_cast<uint64_t>(Load32(header + 12)) << 32);
    if (flags == 0 && table_address == 0)
    {
        Fault("PxCI written before the command header");
        port.hanging = true;
        return;
    }
    const uint32_t prdtl = flags >> 16;
    if ((flags & 0x1F) != A::CommandFisLengthDwords)
    {
        Fault("command FIS length is not 5 dwords");
    }
    if ((table_address & (A::CommandTableAlignment - 1)) != 0)
    {
        Fault("command table is not 128-byte aligned");
    }
    if (!options.s64a && table_address + PrdtOffset + prdtl * A::PrdBytes > FourGiB)
    {
        Fault("command table above 4 GiB on an HBA without S64A");
    }
    if (prdtl > MaxPrds)
    {
        Fault("more PRD entries than the command table holds");
        port.hanging = true;
        return;
    }
    volatile uint8_t* table = Host(table_address, PrdtOffset + prdtl * A::PrdBytes);
    if (table == nullptr)
    {
        Fault("command table is not in allocated memory");
        port.hanging = true;
        return;
    }
    if (table[0] != A::FisTypeH2d)
    {
        Fault("PxCI written before the command FIS");
        port.hanging = true;
        return;
    }
    if ((table[1] & A::FisCommandBit) == 0)
    {
        Fault("command FIS without the C bit");
    }

    if (port.failures > 0 && port.failure == Failure::Hang)
    {
        port.failures--;
        port.hanging = true;
        return;
    }
    if (port.failures > 0 && port.failure == Failure::TaskFile)
    {
        port.failures--;
        Error(number, port, Status, ErrorUncorrectable);
        return;
    }

    const uint8_t command = table[2];
    const uint64_t lba = static_cast<uint64_t>(table[4]) | (static_cast<uint64_t>(table[5]) << 8) |
                         (static_cast<uint64_t>(table[6]) << 16) |
                         (static_cast<uint64_t>(table[8]) << 24) |
                         (static_cast<uint64_t>(table[9]) << 32) |
                         (static_cast<uint64_t>(table[10]) << 40);
    uint32_t sectors = static_cast<uint32_t>(table[12]) | (static_cast<uint32_t>(table[13]) << 8);
    sectors = sectors == 0 ? 65536 : sectors;
    const uint64_t prdbc_physical = port.clb + 4;
    const uint32_t ss = port.config.sector_size;
    std::vector<Segment> segments;

    switch (command)
    {
    case A::AtaCommand::IdentifyDevice:
    {
        log_.push_back({Event::Identify, number, 0, 0});
        if (!GatherPrds(port, table, prdtl, 512, &segments))
        {
            port.hanging = true;
            return;
        }
        const std::array<uint16_t, 256> words = IdentifyWords(port);
        size_t index = 0;
        for (const Segment& segment : segments)
        {
            for (uint32_t i = 0; i < segment.length; i++, index++)
            {
                segment.host[i] =
                    static_cast<uint8_t>(words[index / 2] >> (8 * (index % 2)));
            }
        }
        Complete(port, prdbc_physical, 512, true);
        return;
    }
    case A::AtaCommand::ReadDmaExt:
    case A::AtaCommand::WriteDmaExt:
    {
        const bool write = command == A::AtaCommand::WriteDmaExt;
        log_.push_back({write ? Event::Write : Event::Read, number, lba, sectors});
        if ((table[7] & A::DeviceLba) == 0)
        {
            Fault("DMA EXT command without the LBA bit in the device register");
        }
        const uint32_t bytes = sectors * ss;
        const bool hit = GatherPrds(port, table, prdtl, bytes, &segments);
        if (!hit)
        {
            port.hanging = true;
            return;
        }
        if (prdtl == 0)
        {
            Fault("a data command without a PRD");
        }
        std::vector<uint8_t> data(bytes);
        if (write)
        {
            size_t index = 0;
            for (const Segment& segment : segments)
            {
                for (uint32_t i = 0; i < segment.length; i++)
                {
                    data[index++] = segment.host[i];
                }
            }
            if (!port.disk->Write(lba, data))
            {
                Error(number, port, Status, ErrorIdNotFound);
                return;
            }
        }
        else
        {
            if (!port.disk->Read(lba, data))
            {
                Error(number, port, Status, ErrorIdNotFound);
                return;
            }
            size_t index = 0;
            for (const Segment& segment : segments)
            {
                for (uint32_t i = 0; i < segment.length; i++)
                {
                    segment.host[i] = data[index++];
                }
            }
        }
        const bool shorten =
            !write && port.failures > 0 && port.failure == Failure::ShortRead;
        if (shorten)
        {
            port.failures--;
        }
        Complete(port, prdbc_physical, shorten ? bytes / 2 : bytes,
                 false);
        return;
    }
    case A::AtaCommand::FlushCacheExt:
    case A::AtaCommand::FlushCache:
    {
        const bool extended = command == A::AtaCommand::FlushCacheExt;
        log_.push_back({extended ? Event::FlushExt : Event::Flush, number, 0, 0});
        if (extended ? !port.config.flush_ext : !port.config.flush)
        {
            Error(number, port, Status, ErrorAbort);
            return;
        }
        port.disk->Flush();
        Complete(port, prdbc_physical, 0, false);
        return;
    }
    case A::AtaCommand::StandbyImmediate:
        log_.push_back({Event::Standby, number, 0, 0});
        Complete(port, prdbc_physical, 0, false);
        return;
    default:
        Error(number, port, Status, ErrorAbort);
        return;
    }
}

uint32_t FakeAhci::ReadConfig32(uint16_t offset)
{
    if (offset == 0x2C)
    {
        return (static_cast<uint32_t>(options.subsystem) << 16) | options.subsystem_vendor;
    }
    return 0;
}

bool FakeAhci::AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer)
{
    const Scope scope(*this);
    if (allocations_ >= options.allocations_before_failure)
    {
        return false;
    }
    allocations_++;
    limits_.push_back(below);
    const Memory::Dma::Buffer allocated =
        below <= FourGiB ? low_.Allocate(bytes) : memory_.Allocate(bytes);
    if (allocated.physical + bytes > below)
    {
        Fault("buffer above the physical limit asked for");
        return false;
    }
    if (!options.s64a && allocated.physical + bytes > FourGiB)
    {
        Fault("buffer above 4 GiB on an HBA without S64A");
    }
    live_.push_back(allocated);
    *buffer = allocated;
    return true;
}

void FakeAhci::FreeDma(const Memory::Dma::Buffer& buffer)
{
    const Scope scope(*this);
    const auto it = std::find_if(live_.begin(), live_.end(), [&](const Memory::Dma::Buffer& b)
                                 { return b.physical == buffer.physical; });
    if (it == live_.end())
    {
        Fault("freed a buffer that is not allocated");
        return;
    }
    for (const PortState& port : ports_)
    {
        if (!port.present || !Running(port))
        {
            continue;
        }
        const bool used =
            port.clb == buffer.physical ||
            std::find(port.dma_bases.begin(), port.dma_bases.end(), buffer.physical) !=
                port.dma_bases.end();
        if (used)
        {
            Fault("buffer freed while the port may still use it");
        }
    }
    live_.erase(it);
}

InterruptKind FakeAhci::SetUpInterrupts(void (*handler)(void* context), void* context)
{
    const Scope scope(*this);
    handler_ = handler;
    handler_context_ = context;
    return options.interrupts;
}

uint32_t FakeAhci::Count(Event event) const
{
    return static_cast<uint32_t>(std::count_if(log_.begin(), log_.end(),
                                               [&](const Record& r) { return r.event == event; }));
}

uint32_t FakeAhci::Count(Event event, uint32_t port) const
{
    return static_cast<uint32_t>(
        std::count_if(log_.begin(), log_.end(),
                      [&](const Record& r) { return r.event == event && r.port == port; }));
}

const std::vector<FakeAhci::Record>& FakeAhci::Log() const
{
    return log_;
}

const std::vector<std::string>& FakeAhci::Faults() const
{
    return faults_;
}

const std::vector<uint64_t>& FakeAhci::Limits() const
{
    return limits_;
}

uint32_t FakeAhci::Interrupts() const
{
    return interrupts_;
}

uint32_t FakeAhci::LiveBuffers() const
{
    return static_cast<uint32_t>(live_.size());
}

bool FakeAhci::EngineRunning(uint32_t port) const
{
    return ports_.at(port).cr || (ports_.at(port).cmd & A::CmdBit::Start) != 0;
}

bool FakeAhci::ReceivingFis(uint32_t port) const
{
    return ports_.at(port).fr || (ports_.at(port).cmd & A::CmdBit::FisReceiveEnable) != 0;
}

uint32_t FakeAhci::Resets() const
{
    return resets_;
}

} // namespace FeatherOS::Storage::Testing
