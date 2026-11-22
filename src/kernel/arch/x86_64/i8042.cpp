#include "i8042.h"

namespace FeatherOS::Arch::X86_64::I8042
{
namespace
{

constexpr uint8_t ReadConfigCommand = 0x20;
constexpr uint8_t WriteConfigCommand = 0x60;

bool WaitForOutput(Ports& ports)
{
    for (uint32_t attempt = 0; attempt < PollLimit; ++attempt)
    {
        if ((ports.Read8(CommandPort) & StatusOutputFull) != 0)
        {
            return true;
        }
    }
    return false;
}

} // namespace

bool WaitForInputEmpty(Ports& ports)
{
    for (uint32_t attempt = 0; attempt < PollLimit; ++attempt)
    {
        if ((ports.Read8(CommandPort) & StatusInputFull) == 0)
        {
            return true;
        }
    }
    return false;
}

bool Flush(Ports& ports, uint32_t* flushed)
{
    for (uint32_t count = 0; count <= FlushLimit; ++count)
    {
        if ((ports.Read8(CommandPort) & StatusOutputFull) == 0)
        {
            *flushed = count;
            return true;
        }
        if (count < FlushLimit)
        {
            (void)ports.Read8(DataPort);
        }
    }
    *flushed = FlushLimit;
    return false;
}

Setup Configure(Ports& ports)
{
    Setup setup = {Status::TimedOut, 0, 0, 0};
    if (ports.Read8(CommandPort) == StatusNoController)
    {
        setup.status = Status::NoController;
        return setup;
    }
    if (!Flush(ports, &setup.flushed) || !WaitForInputEmpty(ports))
    {
        return setup;
    }
    ports.Write8(CommandPort, ReadConfigCommand);
    if (!WaitForOutput(ports))
    {
        return setup;
    }
    setup.original = ports.Read8(DataPort);
    const auto wanted =
        static_cast<uint8_t>((setup.original | ConfigFirstPortInterrupt | ConfigTranslation) &
                             ~ConfigFirstPortClockDisabled);
    if (!WaitForInputEmpty(ports))
    {
        return setup;
    }
    ports.Write8(CommandPort, WriteConfigCommand);
    if (!WaitForInputEmpty(ports))
    {
        return setup;
    }
    ports.Write8(DataPort, wanted);
    setup.config = wanted;
    setup.status = Status::Ready;
    return setup;
}

} // namespace FeatherOS::Arch::X86_64::I8042
