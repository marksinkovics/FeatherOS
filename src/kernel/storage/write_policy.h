#ifndef FEATHEROS_STORAGE_WRITE_POLICY_H
#define FEATHEROS_STORAGE_WRITE_POLICY_H

#include "kernel/boot_options.h"

#include <stdint.h>

namespace FeatherOS::Storage
{

enum class WritePolicy : uint8_t
{
    Allowed,
    ReadOnlyHardware,
    ReadOnlyOption,
    AllowedByOption,
};

constexpr uint16_t QemuSubsystemVendor = 0x1AF4;
constexpr uint16_t QemuSubsystem = 0x1100;

WritePolicy DecideWrites(BootOptions::StorageMode mode, uint16_t subsystem_vendor,
                         uint16_t subsystem);

constexpr bool WritesAllowed(WritePolicy policy)
{
    return policy == WritePolicy::Allowed || policy == WritePolicy::AllowedByOption;
}

} // namespace FeatherOS::Storage

#endif
