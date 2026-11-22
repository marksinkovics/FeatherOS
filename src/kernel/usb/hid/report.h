#ifndef FEATHEROS_USB_HID_REPORT_H
#define FEATHEROS_USB_HID_REPORT_H

#include <span>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Usb::Hid
{

constexpr size_t MaxDescriptorBytes = 1024;
constexpr size_t MaxReportIds = 8;
constexpr size_t MaxReportBytes = 64;
constexpr size_t MaxFields = 32;
constexpr size_t MaxFieldBits = 32;
constexpr size_t MaxPushDepth = 4;
constexpr size_t MaxKeys = 32;
constexpr size_t MaxLedUsages = 5;

enum class Status : uint8_t
{
    Ok,
    Malformed,
    TooLarge,
};

enum class FieldKind : uint8_t
{
    Modifier,
    KeyArray,
    KeyBitmap,
    Led,
    Button,
    X,
    Y,
    Wheel,
    Pan,
};

// A run of count elements of bit_size bits each, starting bit_offset bits after the report ID
// byte (or the report's start without IDs). Element i of a bitmap-like field has the usage
// usage_base + i; an array field's values are usages minus logical_minimum from usage_base.
struct Field
{
    FieldKind kind;
    bool is_signed;
    uint8_t report_id;
    uint8_t bit_size;
    uint16_t bit_offset;
    uint16_t count;
    uint16_t usage_base;
    int32_t logical_minimum;
    int32_t logical_maximum;
};

// Report sizes in bytes, the ID byte included; 0 where the report has no such items.
struct ReportInfo
{
    uint8_t id;
    uint8_t input_bytes;
    uint8_t output_bytes;
};

struct ReportMap
{
    bool uses_ids = false;
    bool has_keyboard = false;
    bool has_mouse = false;
    uint8_t report_count = 0;
    uint8_t field_count = 0;
    ReportInfo reports[MaxReportIds] = {};
    Field fields[MaxFields] = {};
};

struct KeySet
{
    uint8_t modifiers = 0;
    uint8_t count = 0;
    // Every slot of an array field holds the rollover usage: the report says nothing.
    bool rollover = false;
    uint16_t keys[MaxKeys] = {};
};

struct MouseReport
{
    uint32_t buttons = 0;
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t wheel = 0;
    int32_t pan = 0;
};

Status ParseReportDescriptor(std::span<const uint8_t> descriptor, ReportMap* map);

// False if the report is not one of the map's keyboard reports: an unknown ID, shorter than the
// map says, or without keyboard fields. Fields never read past the report.
bool ExtractKeyboard(const ReportMap& map, std::span<const uint8_t> report, KeySet* keys);
bool ExtractMouse(const ReportMap& map, std::span<const uint8_t> report, MouseReport* mouse);

// The Output report holding the LED fields, for the lock state in the bit layout of
// Input::LedState (bit 0 Num Lock, bit 1 Caps Lock, bits 2-4 the other LEDs, always off).
bool BuildLedReport(const ReportMap& map, uint8_t led_state, uint8_t* report, size_t capacity,
                    size_t* length);

ReportMap BootKeyboard();
ReportMap BootMouse();

} // namespace FeatherOS::Usb::Hid

#endif
