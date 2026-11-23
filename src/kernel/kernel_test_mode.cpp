#include "kernel/kernel_test_mode.h"

#include "kernel/boot_options.h"

#include <stddef.h>

namespace FeatherOS::KernelTest
{
namespace
{

struct ModeName
{
    const char* name;
    Mode mode;
};

constexpr ModeName mode_names[] = {
    {"panic", Mode::Panic},
    {"exception", Mode::Exception},
    {"interrupt", Mode::Interrupt},
    {"memory", Mode::Memory},
    {"timer", Mode::Timer},
    {"scheduler", Mode::Scheduler},
    {"user-preemption", Mode::UserPreemption},
    {"smp", Mode::Smp},
    {"storage", Mode::Storage},
    {"network", Mode::Network},
    {"input", Mode::Input},
    {"pci", Mode::Pci},
    {"usb-storage", Mode::UsbStorage},
    {"nvme", Mode::Nvme},
    {"storage-policy", Mode::StoragePolicy},
};

constinit Mode selected_mode = Mode::None;

} // namespace

bool ParseCommandLine(const char* command_line, Mode* mode)
{
    static constexpr char prefix[] = "test=";
    constexpr size_t prefix_length = sizeof(prefix) - 1;
    *mode = Mode::None;
    if (command_line == nullptr)
    {
        return true;
    }
    const char* cursor = command_line;
    const char* option = nullptr;
    size_t length = 0;
    while (BootOptions::NextWord(&cursor, &option, &length))
    {
        if (length >= prefix_length && BootOptions::MatchesWord(option, prefix_length, prefix))
        {
            const char* name = option + prefix_length;
            const size_t name_length = length - prefix_length;
            bool known = false;
            for (const ModeName& entry : mode_names)
            {
                if (BootOptions::MatchesWord(name, name_length, entry.name))
                {
                    *mode = entry.mode;
                    known = true;
                }
            }
            if (!known)
            {
                return false;
            }
        }
    }
    return true;
}

void SelectMode(Mode mode)
{
    selected_mode = mode;
}

Mode SelectedMode()
{
    return selected_mode;
}

bool Selected(Mode mode)
{
    return selected_mode == mode;
}

} // namespace FeatherOS::KernelTest
