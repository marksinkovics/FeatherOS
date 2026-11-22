#ifndef FEATHEROS_X86_64_I8042_H
#define FEATHEROS_X86_64_I8042_H

#include <stdint.h>

namespace FeatherOS::Arch::X86_64::I8042
{

constexpr uint16_t DataPort = 0x60;
constexpr uint16_t CommandPort = 0x64;

constexpr uint8_t StatusOutputFull = 1U << 0;
constexpr uint8_t StatusInputFull = 1U << 1;
constexpr uint8_t StatusNoController = 0xFF;

constexpr uint8_t ConfigFirstPortInterrupt = 1U << 0;
constexpr uint8_t ConfigFirstPortClockDisabled = 1U << 4;
constexpr uint8_t ConfigTranslation = 1U << 6;

constexpr uint32_t FlushLimit = 64;
constexpr uint32_t PollLimit = 100000;

class Ports
{
public:
    virtual uint8_t Read8(uint16_t port) = 0;
    virtual void Write8(uint16_t port, uint8_t value) = 0;

protected:
    Ports() = default;
    ~Ports() = default;
    Ports(const Ports&) = default;
    Ports& operator=(const Ports&) = default;
};

enum class Status : uint8_t
{
    Ready,
    NoController,
    TimedOut,
};

struct Setup
{
    Status status;
    uint8_t original;
    uint8_t config;
    uint32_t flushed;
};

Setup Configure(Ports& ports);
bool Flush(Ports& ports, uint32_t* flushed);
bool WaitForInputEmpty(Ports& ports);

} // namespace FeatherOS::Arch::X86_64::I8042

#endif
