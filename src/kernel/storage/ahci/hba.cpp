#include "hba.h"

#include "kernel/klogger.h"
#include "kernel/storage/ahci/register_wait.h"
#include "kernel/string/format.h"

#include <stdint.h>

namespace FeatherOS::Storage::Ahci
{
namespace
{

constexpr uint32_t Gone32 = 0xFFFFFFFF;
constexpr uint64_t FourGiB = 1ULL << 32;

constinit KLogger logger;

template <typename... Args>
void Say(bool warning, uint16_t index, const char* format, const Args&... args)
{
    char text[144];
    char line[192];
    if (!String::FormatTo(text, sizeof(text), format, args...) ||
        !String::FormatTo(line, sizeof(line), "Storage: ahci{}: {}", index, text))
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

void CopyText(char* out, size_t capacity, const char* text)
{
    size_t i = 0;
    for (; i + 1 < capacity && text[i] != '\0'; i++)
    {
        out[i] = text[i];
    }
    out[i] = '\0';
}

bool HasCap2(const Version& version)
{
    return version.major > 1 || (version.major == 1 && version.minor >= 0x0200);
}

} // namespace

Hba* Hba::Create(ControllerHardware& hardware, Environment& environment, uint16_t index,
                 WritePolicy policy, const char* location)
{
    return new Hba(hardware, environment, index, policy, location);
}

Hba::Hba(ControllerHardware& hardware, Environment& environment, uint16_t index, WritePolicy policy,
         const char* location)
    : hardware_(hardware), environment_(environment), index_(index), policy_(policy)
{
    CopyText(location_, sizeof(location_), location);
}

Hba::~Hba() = default;

uint32_t Hba::DiskCount() const
{
    uint32_t count = 0;
    for (const Port& port : ports_)
    {
        count += port.Active() ? 1 : 0;
    }
    return count;
}

Port* Hba::DiskPort(uint32_t number)
{
    if (number >= Reg::MaxPorts || !ports_[number].Active())
    {
        return nullptr;
    }
    return &ports_[number];
}

void Hba::OnInterrupt(void* context)
{
    auto* hba = static_cast<Hba*>(context);
    const uint32_t pending = hba->hardware_.Read32(Reg::Is);
    for (uint32_t port = 0; port < Reg::MaxPorts; port++)
    {
        if (((pending >> port) & 1) != 0)
        {
            hba->environment_.Wake(&hba->ports_[port]);
        }
    }
}

void Hba::Handoff()
{
    hardware_.Write32(Reg::Bohc, hardware_.Read32(Reg::Bohc) | BohcBit::OsOwned);
    if (!WaitForRegister(hardware_, environment_, this, Reg::Bohc, BohcBit::BiosOwned, 0,
                         HandoffWaitMs))
    {
        Say(true, index_, "firmware did not release the controller, continuing");
        return;
    }
    if ((hardware_.Read32(Reg::Bohc) & BohcBit::BiosBusy) != 0 &&
        !WaitForRegister(hardware_, environment_, this, Reg::Bohc, BohcBit::BiosBusy, 0,
                         HandoffBusyWaitMs))
    {
        Say(true, index_, "firmware stayed busy, continuing");
    }
}

void Hba::LogPolicy() const
{
    switch (policy_)
    {
    case WritePolicy::Allowed:
        Say(false, index_, "QEMU controller, writes allowed");
        break;
    case WritePolicy::ReadOnlyHardware:
        Say(false, index_, "real hardware, read-only (boot with storage=rw to write)");
        break;
    case WritePolicy::ReadOnlyOption:
        Say(false, index_, "storage=ro, read-only");
        break;
    case WritePolicy::AllowedByOption:
        Say(true, index_, "storage=rw, writes allowed on real hardware");
        break;
    }
}

void Hba::LogController() const
{
    const char* interrupts = interrupts_ == InterruptKind::MsiX  ? "MSI-X"
                             : interrupts_ == InterruptKind::Msi ? "MSI"
                                                                 : "polling";
    Say(false, index_, "{} {} ports, {} command slots, {}, {}", location_, caps_.port_count,
        caps_.command_slots, interrupts, caps_.addresses_64bit ? "64-bit DMA" : "32-bit DMA");
}

bool Hba::Start()
{
    const uint32_t cap = hardware_.Read32(Reg::Cap);
    if (cap == Gone32)
    {
        Say(true, index_, "controller is gone");
        return false;
    }
    caps_ = DecodeCap(cap);
    const Version version = DecodeVersion(hardware_.Read32(Reg::Vs));
    const uint32_t cap2 = HasCap2(version) ? hardware_.Read32(Reg::Cap2) : 0;
    const uint32_t ports = hardware_.Read32(Reg::Pi);
    for (uint32_t port = 0; port < caps_.port_count; port++)
    {
        implemented_ |= PortImplemented(ports, port) ? 1U << port : 0;
    }
    if (implemented_ == 0)
    {
        Say(true, index_, "no ports implemented");
        return false;
    }
    const uint64_t below = caps_.addresses_64bit ? UINT64_MAX : FourGiB;

    if (HasHandoff(cap2))
    {
        Handoff();
    }
    hardware_.Write32(Reg::Ghc, hardware_.Read32(Reg::Ghc) | GhcBit::AhciEnable);
    for (uint32_t number = 0; number < Reg::MaxPorts; number++)
    {
        if (!PortImplemented(implemented_, number))
        {
            continue;
        }
        const PortConfig config{&hardware_, &environment_,           index_,   number, policy_,
                                below,      caps_.staggered_spin_up, location_};
        ports_[number].Initialize(config);
        if (!ports_[number].Quiesce())
        {
            Say(true, index_, "port {} did not stop before the reset", number);
        }
    }

    hardware_.Write32(Reg::Ghc, GhcBit::AhciEnable | GhcBit::Reset);
    if (!WaitForRegister(hardware_, environment_, this, Reg::Ghc, GhcBit::Reset, 0, HbaResetMs))
    {
        Say(true, index_, "controller reset did not complete, not supported");
        return false;
    }
    hardware_.Write32(Reg::Ghc, GhcBit::AhciEnable);

    interrupts_ = hardware_.SetUpInterrupts(OnInterrupt, this);
    LogController();

    uint32_t candidates = 0;
    for (uint32_t number = 0; number < Reg::MaxPorts; number++)
    {
        if (PortImplemented(implemented_, number) && ports_[number].Probe())
        {
            candidates++;
            if (interrupts_ != InterruptKind::Polling)
            {
                ports_[number].EnableInterrupts();
            }
        }
    }
    if (candidates != 0 && interrupts_ != InterruptKind::Polling)
    {
        hardware_.Write32(Reg::Ghc, GhcBit::AhciEnable | GhcBit::InterruptEnable);
    }

    LogPolicy();
    uint32_t accepted = 0;
    for (uint32_t number = 0; number < Reg::MaxPorts; number++)
    {
        Port& port = ports_[number];
        if (PortImplemented(implemented_, number) && port.Candidate() && port.Start())
        {
            accepted++;
        }
    }
    started_.store(true, std::memory_order_release);
    for (Port& port : ports_)
    {
        if (port.Active())
        {
            port.Publish();
        }
    }
    if (accepted == 0)
    {
        Say(false, index_, "no disks");
    }
    return true;
}

void Hba::Shutdown(Power::Action action)
{
    if (!started_.load(std::memory_order_acquire))
    {
        return;
    }
    for (Port& port : ports_)
    {
        port.Shutdown(action);
    }
}

} // namespace FeatherOS::Storage::Ahci
