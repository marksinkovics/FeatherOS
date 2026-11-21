#include "pci.h"

namespace FeatherOS::Pci
{
namespace
{

constexpr uint16_t HeaderTypeRegister = 0x0C;
constexpr uint16_t ClassRegister = 0x08;
constexpr uint16_t BusNumbersRegister = 0x18;
constexpr uint16_t CapabilityPointerRegister = 0x34;
constexpr uint16_t BarBase = 0x10;
constexpr uint8_t HeaderMultifunction = 0x80;
constexpr uint8_t HeaderTypeMask = 0x7F;
constexpr uint8_t HeaderTypeDevice = 0;
constexpr uint8_t HeaderTypeBridge = 1;
constexpr uint32_t StatusCapabilityList = 1U << 20;
constexpr uint16_t FirstCapabilityOffset = 0x40;
constexpr uint32_t MaximumBridgeDepth = 16;
constexpr uint32_t MaximumBridgeSteps = 8;

struct Walk
{
    ConfigSpace* config;
    Visitor visit;
    void* context;
    uint64_t visited_buses[4];
};

uint8_t ByteOf(uint32_t value, uint32_t byte)
{
    return static_cast<uint8_t>(value >> (byte * 8));
}

bool ReadFunction(ConfigSpace& config, Address address, Function* function)
{
    const uint32_t identity = config.Read32(address, 0);
    const auto vendor = static_cast<uint16_t>(identity);
    if (vendor == 0xFFFF)
    {
        return false;
    }
    const uint32_t class_word = config.Read32(address, ClassRegister);
    const uint32_t header = config.Read32(address, HeaderTypeRegister);
    const uint32_t interrupt = config.Read32(address, InterruptRegister);
    function->address = address;
    function->vendor = vendor;
    function->device = static_cast<uint16_t>(identity >> 16);
    function->class_code = ByteOf(class_word, 3);
    function->subclass = ByteOf(class_word, 2);
    function->prog_if = ByteOf(class_word, 1);
    function->header_type = ByteOf(header, 2);
    function->interrupt_pin = ByteOf(interrupt, 1);
    return true;
}

uint32_t BarCount(const Function& function)
{
    switch (function.header_type & HeaderTypeMask)
    {
    case HeaderTypeDevice:
        return 6;
    case HeaderTypeBridge:
        return 2;
    default:
        return 0;
    }
}

bool IsMemory64(uint32_t raw)
{
    return (raw & 1U) == 0 && ((raw >> 1) & 3U) == 2U;
}

uint16_t BarOffset(uint32_t index)
{
    return static_cast<uint16_t>(BarBase + index * 4U);
}

bool VisitBus(Walk& walk, uint32_t bus, uint32_t depth)
{
    if ((walk.visited_buses[bus / 64] >> (bus % 64) & 1U) != 0)
    {
        return true;
    }
    walk.visited_buses[bus / 64] |= 1ULL << (bus % 64);

    for (uint32_t device = 0; device < 32; ++device)
    {
        const Address first = {static_cast<uint8_t>(bus), static_cast<uint8_t>(device), 0};
        uint32_t functions = 1;
        Function probe;
        if (!ReadFunction(*walk.config, first, &probe))
        {
            continue;
        }
        if ((probe.header_type & HeaderMultifunction) != 0)
        {
            functions = 8;
        }
        for (uint32_t index = 0; index < functions; ++index)
        {
            const Address address = {static_cast<uint8_t>(bus), static_cast<uint8_t>(device),
                                     static_cast<uint8_t>(index)};
            Function function;
            if (!ReadFunction(*walk.config, address, &function))
            {
                continue;
            }
            if (!walk.visit(function, walk.context))
            {
                return false;
            }
            if ((function.header_type & HeaderTypeMask) != HeaderTypeBridge)
            {
                continue;
            }
            const uint32_t numbers = walk.config->Read32(address, BusNumbersRegister);
            const uint32_t secondary = ByteOf(numbers, 1);
            if (secondary > bus && depth < MaximumBridgeDepth &&
                !VisitBus(walk, secondary, depth + 1))
            {
                return false;
            }
        }
    }
    return true;
}

bool FindBridgeTo(ConfigSpace& config, uint8_t bus, Function* bridge)
{
    for (uint32_t parent = 0; parent < bus; ++parent)
    {
        for (uint32_t device = 0; device < 32; ++device)
        {
            for (uint32_t index = 0; index < 8; ++index)
            {
                const Address address = {static_cast<uint8_t>(parent), static_cast<uint8_t>(device),
                                         static_cast<uint8_t>(index)};
                Function function;
                if (!ReadFunction(config, address, &function))
                {
                    if (index == 0)
                    {
                        break;
                    }
                    continue;
                }
                if ((function.header_type & HeaderTypeMask) == HeaderTypeBridge &&
                    ByteOf(config.Read32(address, BusNumbersRegister), 1) == bus)
                {
                    *bridge = function;
                    return true;
                }
                if (index == 0 && (function.header_type & HeaderMultifunction) == 0)
                {
                    break;
                }
            }
        }
    }
    return false;
}

} // namespace

void Enumerate(ConfigSpace& config, Visitor visit, void* context)
{
    Walk walk = {&config, visit, context, {0, 0, 0, 0}};
    VisitBus(walk, 0, 0);
}

bool ReadBar(ConfigSpace& config, const Function& function, uint32_t index, Bar* bar)
{
    const uint32_t count = BarCount(function);
    if (index >= count)
    {
        return false;
    }
    for (uint32_t slot = 0; slot < index;)
    {
        slot += IsMemory64(config.Read32(function.address, BarOffset(slot))) ? 2U : 1U;
        if (slot > index)
        {
            return false;
        }
    }

    const uint16_t low_offset = BarOffset(index);
    const uint32_t low = config.Read32(function.address, low_offset);
    const bool is_io = (low & 1U) != 0;
    const bool is_64bit = IsMemory64(low);
    if (is_64bit && index + 1 >= count)
    {
        return false;
    }
    const uint16_t high_offset = BarOffset(index + 1);
    const uint32_t high = is_64bit ? config.Read32(function.address, high_offset) : 0;

    const uint32_t command = config.Read32(function.address, CommandRegister);
    // The upper half is the status register, whose set bits clear when written back.
    const uint32_t decoding = CommandIoSpace | CommandMemorySpace;
    config.Write32(function.address, CommandRegister, (command & 0xFFFFU) & ~decoding);
    config.Write32(function.address, low_offset, 0xFFFFFFFFU);
    if (is_64bit)
    {
        config.Write32(function.address, high_offset, 0xFFFFFFFFU);
    }
    const uint32_t sized_low = config.Read32(function.address, low_offset);
    const uint32_t sized_high = is_64bit ? config.Read32(function.address, high_offset) : 0;
    config.Write32(function.address, low_offset, low);
    if (is_64bit)
    {
        config.Write32(function.address, high_offset, high);
    }
    config.Write32(function.address, CommandRegister, command & 0xFFFFU);

    uint64_t stuck = is_io ? (sized_low & ~3U) : (sized_low & ~0xFU);
    if (is_64bit)
    {
        stuck |= static_cast<uint64_t>(sized_high) << 32;
    }
    bar->address = is_io ? (low & ~3U) : (low & ~0xFU);
    if (is_64bit)
    {
        bar->address |= static_cast<uint64_t>(high) << 32;
    }
    if (stuck == 0)
    {
        bar->size = 0;
    }
    else
    {
        if (is_io && (stuck & 0xFFFF0000U) == 0)
        {
            stuck |= 0xFFFF0000U;
        }
        if (!is_64bit)
        {
            stuck |= 0xFFFFFFFF00000000ULL;
        }
        bar->size = ~stuck + 1;
    }
    bar->is_io = is_io;
    bar->is_64bit = is_64bit;
    bar->prefetchable = !is_io && (low & 8U) != 0;
    return true;
}

bool AssignBar(ConfigSpace& config, const Function& function, uint32_t index, const Bar& bar,
               uint64_t address)
{
    if (index >= BarCount(function) || (bar.is_64bit && index + 1 >= BarCount(function)) ||
        (!bar.is_64bit && address > 0xFFFFFFFFULL))
    {
        return false;
    }
    const uint32_t command = config.Read32(function.address, CommandRegister);
    config.Write32(function.address, CommandRegister,
                   (command & 0xFFFFU) & ~(CommandIoSpace | CommandMemorySpace));
    const uint32_t flags =
        bar.is_io ? 1U : ((bar.is_64bit ? 4U : 0U) | (bar.prefetchable ? 8U : 0U));
    config.Write32(function.address, BarOffset(index),
                   (static_cast<uint32_t>(address) & (bar.is_io ? ~3U : ~0xFU)) | flags);
    if (bar.is_64bit)
    {
        config.Write32(function.address, BarOffset(index + 1),
                       static_cast<uint32_t>(address >> 32));
    }
    config.Write32(function.address, CommandRegister, command & 0xFFFFU);
    return true;
}

uint16_t FindCapability(ConfigSpace& config, const Function& function, uint8_t id)
{
    const uint32_t type = function.header_type & HeaderTypeMask;
    if (type != HeaderTypeDevice && type != HeaderTypeBridge)
    {
        return 0;
    }
    if ((config.Read32(function.address, CommandRegister) & StatusCapabilityList) == 0)
    {
        return 0;
    }
    uint16_t offset = ByteOf(config.Read32(function.address, CapabilityPointerRegister), 0) & 0xFCU;
    for (uint32_t step = 0; step < MaximumCapabilitySteps && offset >= FirstCapabilityOffset;
         ++step)
    {
        const uint32_t header = config.Read32(function.address, offset);
        if (ByteOf(header, 0) == id)
        {
            return offset;
        }
        offset = ByteOf(header, 1) & 0xFCU;
    }
    return 0;
}

void UpdateCommand(ConfigSpace& config, const Function& function, uint16_t set, uint16_t clear)
{
    const uint32_t command = config.Read32(function.address, CommandRegister) & 0xFFFFU;
    config.Write32(function.address, CommandRegister,
                   (command | set) & ~static_cast<uint32_t>(clear));
}

void EnableDevice(ConfigSpace& config, const Function& function, bool disable_intx)
{
    UpdateCommand(
        config, function,
        CommandMemorySpace | CommandBusMaster | (disable_intx ? CommandInterruptDisable : 0U), 0);
}

bool SwizzleToRoot(ConfigSpace& config, const Function& function, Address* root, uint8_t* pin)
{
    if (function.interrupt_pin < 1 || function.interrupt_pin > 4)
    {
        return false;
    }
    Function current = function;
    uint8_t current_pin = function.interrupt_pin;
    for (uint32_t step = 0; current.address.bus != 0; ++step)
    {
        Function bridge;
        if (step == MaximumBridgeSteps || !FindBridgeTo(config, current.address.bus, &bridge))
        {
            return false;
        }
        current_pin = static_cast<uint8_t>((current_pin - 1 + current.address.device) % 4 + 1);
        current = bridge;
    }
    *root = current.address;
    *pin = current_pin;
    return true;
}

bool Describe(const Function& function, char* buffer, size_t capacity)
{
    constexpr char Digits[] = "0123456789abcdef";
    // "PCI: 00:04.0 1b36:000d class 0c0330"
    constexpr size_t Length = 35;
    if (capacity <= Length)
    {
        return false;
    }
    char* out = buffer;
    auto put = [&out](char c) { *out++ = c; };
    auto hex = [&](uint32_t value, uint32_t digits)
    {
        for (uint32_t shift = digits * 4; shift > 0; shift -= 4)
        {
            put(Digits[(value >> (shift - 4)) & 0xFU]);
        }
    };
    for (const char* text = "PCI: "; *text != '\0'; ++text)
    {
        put(*text);
    }
    hex(function.address.bus, 2);
    put(':');
    hex(function.address.device, 2);
    put('.');
    hex(function.address.function, 1);
    put(' ');
    hex(function.vendor, 4);
    put(':');
    hex(function.device, 4);
    for (const char* text = " class "; *text != '\0'; ++text)
    {
        put(*text);
    }
    hex(function.class_code, 2);
    hex(function.subclass, 2);
    hex(function.prog_if, 2);
    put('\0');
    return true;
}

} // namespace FeatherOS::Pci
