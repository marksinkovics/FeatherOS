#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

namespace FeatherOS
{
struct Color
{
    static const uint32_t Black = 0x000000;
    static const uint32_t Red = 0xFF0000;
    static const uint32_t Green = 0x00FF00;
    static const uint32_t Yellow = 0xFFFF00;
    static const uint32_t Blue = 0x0000FF;
    static const uint32_t White = 0xFFFFFF;

    static const uint32_t FGColor = White;
    static const uint32_t BGColor = Black;
};
}; // namespace FeatherOS

#endif