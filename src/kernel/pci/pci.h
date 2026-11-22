#ifndef FEATHEROS_PCI_PCI_H
#define FEATHEROS_PCI_PCI_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Pci
{

constexpr uint16_t CommandRegister = 0x04;
constexpr uint16_t InterruptRegister = 0x3C;
constexpr uint16_t CommandIoSpace = 1U << 0;
constexpr uint16_t CommandMemorySpace = 1U << 1;
constexpr uint16_t CommandBusMaster = 1U << 2;
constexpr uint16_t CommandInterruptDisable = 1U << 10;
constexpr uint8_t CapabilityMsi = 0x05;
constexpr uint8_t CapabilityMsix = 0x11;
constexpr uint32_t MaximumCapabilitySteps = 48;

struct Address
{
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

struct Function
{
    Address address;
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t interrupt_pin;
};

struct Bar
{
    uint64_t address;
    uint64_t size;
    bool is_io;
    bool is_64bit;
    bool prefetchable;
};

class ConfigSpace
{
public:
    virtual uint32_t Read32(Address address, uint16_t offset) = 0;
    virtual void Write32(Address address, uint16_t offset, uint32_t value) = 0;

protected:
    ConfigSpace() = default;
    ~ConfigSpace() = default;
    ConfigSpace(const ConfigSpace&) = default;
    ConfigSpace& operator=(const ConfigSpace&) = default;
};

// The visitor returns false to stop the walk.
using Visitor = bool (*)(const Function& function, void* context);

void Enumerate(ConfigSpace& config, Visitor visit, void* context);

// Index is a BAR slot; the upper half of a 64-bit BAR is not a BAR of its own and fails.
bool ReadBar(ConfigSpace& config, const Function& function, uint32_t index, Bar* bar);
bool AssignBar(ConfigSpace& config, const Function& function, uint32_t index, const Bar& bar,
               uint64_t address);

// Returns the config-space offset of the capability, or 0.
uint16_t FindCapability(ConfigSpace& config, const Function& function, uint8_t id);

void UpdateCommand(ConfigSpace& config, const Function& function, uint16_t set, uint16_t clear);
void EnableDevice(ConfigSpace& config, const Function& function, bool disable_intx);

// Follows the swizzle up the bridge chain: root is the bus-0 function whose interrupt-map entry
// applies, pin the INTx pin (1-4) as seen there.
bool SwizzleToRoot(ConfigSpace& config, const Function& function, Address* root, uint8_t* pin);

bool Describe(const Function& function, char* buffer, size_t capacity);

} // namespace FeatherOS::Pci

#endif
