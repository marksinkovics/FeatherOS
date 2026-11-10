#include "kernel/kernel_test_mode.h"

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
};

constinit Mode selected_mode = Mode::None;

bool MatchesWord(const char* text, size_t length, const char* word)
{
    size_t index = 0;
    for (; index < length; ++index)
    {
        if (word[index] == '\0' || word[index] != text[index])
        {
            return false;
        }
    }
    return word[index] == '\0';
}

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
    const char* option = command_line;
    while (*option != '\0')
    {
        while (*option == ' ')
        {
            ++option;
        }
        size_t length = 0;
        while (option[length] != '\0' && option[length] != ' ')
        {
            ++length;
        }
        if (length >= prefix_length && MatchesWord(option, prefix_length, prefix))
        {
            const char* name = option + prefix_length;
            const size_t name_length = length - prefix_length;
            bool known = false;
            for (const ModeName& entry : mode_names)
            {
                if (MatchesWord(name, name_length, entry.name))
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
        option += length;
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
