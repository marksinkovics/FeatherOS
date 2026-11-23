#include "write_policy.h"

namespace FeatherOS::Storage
{

WritePolicy DecideWrites(BootOptions::StorageMode mode, uint16_t subsystem_vendor,
                         uint16_t subsystem)
{
    if (mode == BootOptions::StorageMode::ReadOnly)
    {
        return WritePolicy::ReadOnlyOption;
    }
    const bool qemu = subsystem_vendor == QemuSubsystemVendor && subsystem == QemuSubsystem;
    if (qemu)
    {
        return WritePolicy::Allowed;
    }
    return mode == BootOptions::StorageMode::ReadWrite ? WritePolicy::AllowedByOption
                                                       : WritePolicy::ReadOnlyHardware;
}

} // namespace FeatherOS::Storage
