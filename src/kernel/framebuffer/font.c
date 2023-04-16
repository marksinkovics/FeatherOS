
#include "kernel/framebuffer/font.h"

uint8_t font16_8[] =
{
    0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x34, 0x34, 0x34, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x6C, 0x2C, 0x7F, 0x24, 0x24, 0x34, 0x7F, 0x12, 0x12, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x08, 0x3C, 0x06, 0x02, 0x06, 0x1C, 0x30, 0x60, 0x60, 0x3E, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x46, 0x2D, 0x3D, 0x16, 0x08, 0x7C, 0x54, 0x56, 0x72, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1C, 0x16, 0x12, 0x1E, 0x0C, 0x3A, 0x33, 0x32, 0x7E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x30, 0x18, 0x08, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0C, 0x18, 0x30, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x08, 0x18, 0x10, 0x30, 0x30, 0x30, 0x30, 0x10, 0x10, 0x08, 0x0C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x2A, 0x3E, 0x1C, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x7E, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x10, 0x0C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x20, 0x20, 0x30, 0x10, 0x10, 0x18, 0x08, 0x08, 0x0C, 0x04, 0x06, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1C, 0x26, 0x62, 0x6A, 0x6A, 0x62, 0x62, 0x26, 0x1C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x18, 0x1C, 0x12, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1C, 0x32, 0x20, 0x20, 0x10, 0x08, 0x04, 0x06, 0x7E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1E, 0x20, 0x20, 0x1C, 0x20, 0x60, 0x60, 0x20, 0x1E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x30, 0x38, 0x28, 0x24, 0x26, 0x22, 0x7E, 0x20, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3C, 0x04, 0x04, 0x0C, 0x38, 0x60, 0x60, 0x20, 0x1E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x38, 0x0C, 0x06, 0x3E, 0x22, 0x62, 0x62, 0x26, 0x3C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7E, 0x60, 0x30, 0x10, 0x18, 0x08, 0x08, 0x08, 0x0C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3C, 0x26, 0x62, 0x26, 0x3C, 0x62, 0x62, 0x66, 0x3C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1C, 0x26, 0x62, 0x66, 0x7C, 0x60, 0x20, 0x38, 0x0E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x08, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x0E, 0x06, 0x1C, 0x60, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0E, 0x78, 0x60, 0x1C, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1C, 0x20, 0x20, 0x10, 0x08, 0x08, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x38, 0x64, 0x42, 0x72, 0x4A, 0x4A, 0x4A, 0x4A, 0x76, 0x0C, 0x38, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x18, 0x1C, 0x34, 0x34, 0x26, 0x22, 0x7E, 0x43, 0x43, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1E, 0x22, 0x22, 0x22, 0x1E, 0x22, 0x62, 0x22, 0x3E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x38, 0x04, 0x02, 0x02, 0x02, 0x02, 0x02, 0x06, 0x78, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1E, 0x32, 0x62, 0x62, 0x42, 0x42, 0x62, 0x32, 0x1E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7E, 0x06, 0x06, 0x06, 0x3E, 0x06, 0x06, 0x06, 0x7E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7E, 0x06, 0x06, 0x06, 0x3E, 0x06, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x38, 0x04, 0x02, 0x02, 0x02, 0x42, 0x42, 0x46, 0x78, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x3E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3C, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x1E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x62, 0x32, 0x12, 0x0E, 0x0E, 0x1A, 0x32, 0x62, 0x42, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x7E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x26, 0x66, 0x76, 0x5E, 0x5A, 0x4A, 0x43, 0x43, 0x43, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x42, 0x46, 0x6E, 0x6A, 0x5A, 0x72, 0x72, 0x62, 0x62, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3C, 0x26, 0x62, 0x43, 0x43, 0x43, 0x62, 0x26, 0x3C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3E, 0x62, 0x62, 0x62, 0x3E, 0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1C, 0x26, 0x62, 0x43, 0x43, 0x43, 0x42, 0x62, 0x3E, 0x18, 0x38, 0x60,
    0x00, 0x00, 0x00, 0x00, 0x1E, 0x22, 0x22, 0x22, 0x1E, 0x12, 0x22, 0x22, 0x62, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3C, 0x06, 0x02, 0x06, 0x18, 0x20, 0x60, 0x60, 0x3E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x62, 0x26, 0x3C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x43, 0x43, 0x62, 0x22, 0x26, 0x24, 0x14, 0x1C, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x43, 0x43, 0x43, 0x4A, 0x5A, 0x5E, 0x76, 0x66, 0x26, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x62, 0x26, 0x34, 0x1C, 0x18, 0x1C, 0x24, 0x22, 0x62, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x43, 0x62, 0x26, 0x34, 0x1C, 0x18, 0x08, 0x08, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7E, 0x20, 0x30, 0x10, 0x08, 0x0C, 0x04, 0x06, 0x7E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x3C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x3C, 0x00,
    0x00, 0x06, 0x06, 0x04, 0x0C, 0x08, 0x08, 0x18, 0x10, 0x10, 0x30, 0x20, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x1C, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x18, 0x1C, 0x34, 0x26, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0x00,
    0x00, 0x00, 0x00, 0x08, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x20, 0x20, 0x7C, 0x62, 0x62, 0x3C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x3E, 0x26, 0x62, 0x42, 0x62, 0x22, 0x1E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x06, 0x02, 0x02, 0x02, 0x06, 0x7C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x20, 0x20, 0x20, 0x3C, 0x26, 0x22, 0x22, 0x22, 0x26, 0x3C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x26, 0x62, 0x7E, 0x02, 0x06, 0x3C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x78, 0x08, 0x0C, 0x7E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x26, 0x62, 0x62, 0x22, 0x26, 0x3C, 0x20, 0x1E, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x1E, 0x22, 0x22, 0x22, 0x22, 0x22, 0x62, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00, 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x70, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x1E, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x22, 0x12, 0x0E, 0x0E, 0x1A, 0x32, 0x62, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x70, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3E, 0x4A, 0x4A, 0x4A, 0x42, 0x42, 0x42, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1E, 0x22, 0x22, 0x22, 0x62, 0x62, 0x62, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x26, 0x62, 0x42, 0x62, 0x26, 0x1C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1E, 0x22, 0x62, 0x42, 0x62, 0x26, 0x3E, 0x02, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x26, 0x62, 0x62, 0x22, 0x26, 0x3C, 0x20, 0x20, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x06, 0x06, 0x1C, 0x20, 0x20, 0x3E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x7E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x78, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x22, 0x62, 0x62, 0x22, 0x26, 0x3C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x62, 0x26, 0x24, 0x14, 0x1C, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0x43, 0x5B, 0x5A, 0x76, 0x36, 0x26, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x34, 0x1C, 0x18, 0x3C, 0x26, 0x62, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x66, 0x24, 0x24, 0x3C, 0x18, 0x18, 0x08, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3E, 0x30, 0x10, 0x08, 0x0C, 0x06, 0x3E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x38, 0x08, 0x08, 0x08, 0x08, 0x06, 0x08, 0x08, 0x08, 0x08, 0x08, 0x38, 0x00,
    0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x0E, 0x18, 0x18, 0x18, 0x10, 0x30, 0x10, 0x18, 0x18, 0x18, 0x18, 0x0E, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x7A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};