#include "kernel/arch/power.h"

#include <stddef.h>

namespace FeatherOS::Arch::Power
{
namespace
{

constexpr uint32_t FdtMagic = 0xD00DFEED;
constexpr uint32_t FdtBeginNode = 1;
constexpr uint32_t FdtEndNode = 2;
constexpr uint32_t FdtProperty = 3;
constexpr uint32_t FdtNoOp = 4;
constexpr uint32_t FdtEnd = 9;
constexpr uint64_t PsciSystemOff = 0x84000008;
constexpr uint64_t PsciSystemReset = 0x84000009;

enum class Conduit : uint8_t
{
    None,
    Hypervisor,
    SecureMonitor,
};

Conduit conduit = Conduit::None;

uint32_t ReadBigEndian32(const uint8_t* bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

bool SameText(const char* text, const char* expected)
{
    for (; *expected != '\0'; ++text, ++expected)
    {
        if (*text != *expected)
        {
            return false;
        }
    }
    return *text == '\0';
}

// "compatible" is a list of strings; any "arm,psci..." entry names the PSCI node.
bool ListsPsci(const char* value, uint32_t length)
{
    constexpr char Prefix[] = "arm,psci";
    for (uint32_t start = 0; start < length;)
    {
        uint32_t index = 0;
        while (index < sizeof(Prefix) - 1 && start + index < length &&
               value[start + index] == Prefix[index])
        {
            ++index;
        }
        if (index == sizeof(Prefix) - 1)
        {
            return true;
        }
        while (start < length && value[start] != '\0')
        {
            ++start;
        }
        ++start;
    }
    return false;
}

// Walks the flattened device tree for the PSCI node's "method" property: "hvc" or "smc".
Conduit FindConduit(const void* device_tree)
{
    const auto* bytes = static_cast<const uint8_t*>(device_tree);
    if (bytes == nullptr || ReadBigEndian32(bytes) != FdtMagic)
    {
        return Conduit::None;
    }
    const uint32_t total_size = ReadBigEndian32(bytes + 4);
    const uint8_t* structure = bytes + ReadBigEndian32(bytes + 8);
    const auto* strings = reinterpret_cast<const char*>(bytes + ReadBigEndian32(bytes + 12));
    const uint8_t* end = bytes + total_size;
    bool node_is_psci = false;
    Conduit node_method = Conduit::None;
    for (const uint8_t* token = structure; token + 4 <= end;)
    {
        const uint32_t kind = ReadBigEndian32(token);
        token += 4;
        if (kind == FdtBeginNode)
        {
            node_is_psci = false;
            node_method = Conduit::None;
            while (token < end && *token != '\0')
            {
                ++token;
            }
            token = bytes + ((token + 1 - bytes + 3) & ~static_cast<size_t>(3));
        }
        else if (kind == FdtProperty && token + 8 <= end)
        {
            const uint32_t length = ReadBigEndian32(token);
            const char* name = strings + ReadBigEndian32(token + 4);
            const auto* value = reinterpret_cast<const char*>(token + 8);
            if (SameText(name, "compatible"))
            {
                node_is_psci = ListsPsci(value, length);
            }
            else if (SameText(name, "method"))
            {
                node_method = SameText(value, "smc")   ? Conduit::SecureMonitor
                              : SameText(value, "hvc") ? Conduit::Hypervisor
                                                       : Conduit::None;
            }
            if (node_is_psci && node_method != Conduit::None)
            {
                return node_method;
            }
            token += 8 + ((length + 3) & ~3U);
        }
        else if (kind == FdtEnd)
        {
            break;
        }
        else if (kind != FdtEndNode && kind != FdtNoOp)
        {
            break;
        }
    }
    return Conduit::None;
}

void Call(uint64_t function)
{
    register uint64_t argument asm("x0") = function;
    if (conduit == Conduit::SecureMonitor)
    {
        asm volatile("smc #0" : "+r"(argument) : : "memory");
    }
    else if (conduit == Conduit::Hypervisor)
    {
        asm volatile("hvc #0" : "+r"(argument) : : "memory");
    }
}

} // namespace

void Initialize(uint64_t, const void* device_tree)
{
    conduit = FindConduit(device_tree);
}

void PowerOff()
{
    Call(PsciSystemOff);
}

void Reset()
{
    Call(PsciSystemReset);
}

} // namespace FeatherOS::Arch::Power
