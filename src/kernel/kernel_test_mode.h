#ifndef FEATHEROS_KERNEL_TEST_MODE_H
#define FEATHEROS_KERNEL_TEST_MODE_H

#include <stdint.h>

namespace FeatherOS::KernelTest
{

// The smoke test's hook, chosen with test=<name> on the kernel command line.
enum class Mode : uint8_t
{
    None,
    Panic,
    Exception,
    Interrupt,
    Memory,
    Timer,
    Scheduler,
    UserPreemption,
    Smp,
    Storage,
    Network,
    Input,
};

// False for a test= option that names no mode; options are separated by spaces.
bool ParseCommandLine(const char* command_line, Mode* mode);
// Set once on the boot CPU before any other CPU starts.
void SelectMode(Mode mode);
Mode SelectedMode();
bool Selected(Mode mode);

} // namespace FeatherOS::KernelTest

#endif
