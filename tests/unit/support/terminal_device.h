#ifndef FEATHEROS_TESTS_TERMINAL_DEVICE_H
#define FEATHEROS_TESTS_TERMINAL_DEVICE_H

#include "kernel/terminal/terminal.h"

#include <stdint.h>

#include <functional>
#include <string>

// A terminal device that records what the terminal draws. An erased character shows as "<BS>".
class TestTerminalDevice
{
public:
    TestTerminalDevice();

    const FeatherOS::Terminals::Device* Device() const
    {
        return &device;
    }

    std::string output;
    bool cursor_visible = false;
    int resets = 0;
    int fresh_rows = 0;
    uint64_t output_count = 0;
    std::function<void()> on_poll;

private:
    FeatherOS::Terminals::Device device;
};

#endif
