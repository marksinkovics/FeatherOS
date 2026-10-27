#ifndef FEATHEROS_POWER_H
#define FEATHEROS_POWER_H

#include <stdint.h>

namespace FeatherOS::Power
{

enum class Action : uint8_t
{
    PowerOff,
    Restart,
};

// Stops every process except caller and its ancestors, makes the disk consistent, stops the
// other CPUs, and asks the firmware to power off or restart. Panics if the firmware does
// neither.
[[noreturn]] void Shutdown(Action action, uint64_t caller_process);

} // namespace FeatherOS::Power

#endif
