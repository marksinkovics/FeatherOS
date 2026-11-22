#include <catch2/catch_test_macros.hpp>

#include "kernel/usb/hid/report.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace
{

using namespace FeatherOS::Usb::Hid;
using Bytes = std::vector<uint8_t>;

std::span<const uint8_t> View(const Bytes& bytes)
{
    return {bytes.data(), bytes.size()};
}

Status Parse(const Bytes& descriptor, ReportMap* map)
{
    return ParseReportDescriptor(View(descriptor), map);
}

Bytes Join(std::initializer_list<Bytes> parts)
{
    Bytes all;
    for (const Bytes& part : parts)
    {
        all.insert(all.end(), part.begin(), part.end());
    }
    return all;
}

const Field* Find(const ReportMap& map, FieldKind kind, uint8_t report_id = 0, size_t nth = 0)
{
    for (size_t i = 0; i < map.field_count; ++i)
    {
        if (map.fields[i].kind == kind && map.fields[i].report_id == report_id && nth-- == 0)
        {
            return &map.fields[i];
        }
    }
    return nullptr;
}

const Bytes BootKeyboardDescriptor = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01,
    0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};

const Bytes BootMouseDescriptor = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1,
                                   0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
                                   0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95,
                                   0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
                                   0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95,
                                   0x02, 0x81, 0x06, 0xC0, 0xC0};

// Modifiers as eight bits, then every usage 0 to 0xDF as one bit: 29 bytes.
const Bytes NkroDescriptor = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29,
                              0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
                              0x19, 0x00, 0x29, 0xDF, 0x95, 0xE0, 0x81, 0x02, 0xC0};

// Report 1 a keyboard, 2 a consumer control, 3 a mouse with a wheel.
const Bytes ComboDescriptor = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15,
    0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
    0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0, 0x05, 0x0C, 0x09, 0x01,
    0xA1, 0x01, 0x85, 0x02, 0x19, 0x00, 0x2A, 0x3C, 0x02, 0x15, 0x00, 0x26, 0x3C, 0x02, 0x75,
    0x10, 0x95, 0x01, 0x81, 0x00, 0xC0, 0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x03, 0x09,
    0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09,
    0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0};

// Five buttons, X and Y of 12 bits (-2047 to 2047), a wheel and AC Pan of 8 bits: 6 bytes.
const Bytes WideMouseDescriptor = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02,
    0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0x05, 0x0C,
    0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0};

// Four buttons between Push and Pop, after which X and Y need the saved size and range.
const Bytes PushPopDescriptor = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0xA4, 0x75, 0x01, 0x95, 0x04,
    0x15, 0x00, 0x25, 0x01, 0x05, 0x09, 0x19, 0x04, 0x29, 0x07, 0x81, 0x02, 0xB4, 0x09, 0x30,
    0x09, 0x31, 0x81, 0x06, 0xC0, 0xC0};

} // namespace

TEST_CASE("hid T19: the boot keyboard descriptor of HID Appendix B.1", "[usb][hid][report]")
{
    ReportMap map;
    REQUIRE(Parse(BootKeyboardDescriptor, &map) == Status::Ok);
    REQUIRE(map.has_keyboard);
    REQUIRE_FALSE(map.has_mouse);
    REQUIRE_FALSE(map.uses_ids);
    REQUIRE(map.report_count == 1);
    REQUIRE(map.reports[0].input_bytes == 8);
    REQUIRE(map.reports[0].output_bytes == 1);
    REQUIRE(map.field_count == 3);

    const Field* modifiers = Find(map, FieldKind::Modifier);
    REQUIRE(modifiers != nullptr);
    REQUIRE(modifiers->bit_offset == 0);
    REQUIRE(modifiers->bit_size == 1);
    REQUIRE(modifiers->count == 8);
    REQUIRE(modifiers->usage_base == 0xE0);
    const Field* keys = Find(map, FieldKind::KeyArray);
    REQUIRE(keys != nullptr);
    REQUIRE(keys->bit_offset == 16);
    REQUIRE(keys->bit_size == 8);
    REQUIRE(keys->count == 6);
    REQUIRE(keys->logical_maximum == 0x65);
    const Field* leds = Find(map, FieldKind::Led);
    REQUIRE(leds != nullptr);
    REQUIRE(leds->bit_offset == 0);
    REQUIRE(leds->count == 5);
    REQUIRE(leds->usage_base == 1);

    const ReportMap boot = BootKeyboard();
    REQUIRE(boot.field_count == map.field_count);
    REQUIRE(boot.reports[0].input_bytes == 8);
    REQUIRE(boot.has_keyboard);
}

TEST_CASE("hid T19: the boot mouse descriptor of HID Appendix B.2", "[usb][hid][report]")
{
    ReportMap map;
    REQUIRE(Parse(BootMouseDescriptor, &map) == Status::Ok);
    REQUIRE(map.has_mouse);
    REQUIRE_FALSE(map.has_keyboard);
    REQUIRE(map.reports[0].input_bytes == 3);
    const Field* buttons = Find(map, FieldKind::Button);
    REQUIRE(buttons != nullptr);
    REQUIRE(buttons->count == 3);
    REQUIRE(buttons->bit_offset == 0);
    const Field* x = Find(map, FieldKind::X);
    const Field* y = Find(map, FieldKind::Y);
    REQUIRE(x != nullptr);
    REQUIRE(y != nullptr);
    REQUIRE(x->bit_offset == 8);
    REQUIRE(y->bit_offset == 16);
    REQUIRE(x->bit_size == 8);
    REQUIRE(x->is_signed);
    REQUIRE(x->logical_minimum == -127);
    REQUIRE(x->logical_maximum == 127);
    REQUIRE(BootMouse().has_mouse);
}

TEST_CASE("hid T19: an NKRO bitmap", "[usb][hid][report]")
{
    ReportMap map;
    REQUIRE(Parse(NkroDescriptor, &map) == Status::Ok);
    REQUIRE(map.has_keyboard);
    REQUIRE(map.reports[0].input_bytes == 29);
    const Field* bitmap = Find(map, FieldKind::KeyBitmap);
    REQUIRE(bitmap != nullptr);
    REQUIRE(bitmap->count == 224);
    REQUIRE(bitmap->bit_offset == 8);
    REQUIRE(bitmap->usage_base == 0);
    REQUIRE(Find(map, FieldKind::Modifier) != nullptr);
}

TEST_CASE("hid T19: a keyboard, a consumer control and a mouse with report IDs",
          "[usb][hid][report]")
{
    ReportMap map;
    REQUIRE(Parse(ComboDescriptor, &map) == Status::Ok);
    REQUIRE(map.uses_ids);
    REQUIRE(map.has_keyboard);
    REQUIRE(map.has_mouse);
    REQUIRE(map.report_count == 3);
    REQUIRE(map.reports[0].id == 1);
    REQUIRE(map.reports[0].input_bytes == 8);
    REQUIRE(map.reports[1].id == 2);
    REQUIRE(map.reports[1].input_bytes == 3);
    REQUIRE(map.reports[2].id == 3);
    REQUIRE(map.reports[2].input_bytes == 5);
    // The consumer report has no fields we read.
    for (size_t i = 0; i < map.field_count; ++i)
    {
        REQUIRE(map.fields[i].report_id != 2);
    }
    REQUIRE(Find(map, FieldKind::KeyArray, 1) != nullptr);
    REQUIRE(Find(map, FieldKind::Button, 3) != nullptr);
    const Field* wheel = Find(map, FieldKind::Wheel, 3);
    REQUIRE(wheel != nullptr);
    REQUIRE(wheel->bit_offset == 24);
}

TEST_CASE("hid T19: X and Y of 12 bits, a wheel and AC Pan", "[usb][hid][report]")
{
    ReportMap map;
    REQUIRE(Parse(WideMouseDescriptor, &map) == Status::Ok);
    REQUIRE(map.reports[0].input_bytes == 6);
    const Field* x = Find(map, FieldKind::X);
    const Field* y = Find(map, FieldKind::Y);
    REQUIRE(x != nullptr);
    REQUIRE(y != nullptr);
    REQUIRE(x->bit_size == 12);
    REQUIRE(x->bit_offset == 8);
    REQUIRE(y->bit_offset == 20);
    REQUIRE(x->logical_minimum == -2047);
    REQUIRE(x->logical_maximum == 2047);
    REQUIRE(Find(map, FieldKind::Wheel)->bit_offset == 32);
    REQUIRE(Find(map, FieldKind::Pan)->bit_offset == 40);
}

TEST_CASE("hid T19: Push and Pop restore the global items", "[usb][hid][report]")
{
    ReportMap map;
    REQUIRE(Parse(PushPopDescriptor, &map) == Status::Ok);
    const Field* x = Find(map, FieldKind::X);
    const Field* y = Find(map, FieldKind::Y);
    REQUIRE(x != nullptr);
    REQUIRE(y != nullptr);
    REQUIRE(x->bit_size == 8);
    REQUIRE(x->logical_minimum == -127);
    REQUIRE(x->bit_offset == 12);
    REQUIRE(y->bit_offset == 20);
    const Field* button = Find(map, FieldKind::Button, 0, 1);
    REQUIRE(button != nullptr);
    REQUIRE(button->usage_base == 4);
    REQUIRE(button->count == 4);
    REQUIRE(button->bit_offset == 8);

    // Five Pushes are one too many, and a Pop with nothing pushed is an error.
    Bytes pushes = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0xA4, 0xA4, 0xA4, 0xA4, 0xA4, 0xC0};
    REQUIRE(Parse(pushes, &map) == Status::TooLarge);
    Bytes four = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0xA4, 0xA4, 0xA4, 0xA4, 0xB4, 0xB4,
                  0xB4, 0xB4, 0xC0};
    REQUIRE(Parse(four, &map) == Status::Ok);
    Bytes pop = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0xB4, 0xC0};
    REQUIRE(Parse(pop, &map) == Status::Malformed);
}

TEST_CASE("hid T19: malformed descriptors are refused and leave an empty map",
          "[usb][hid][report]")
{
    ReportMap map;
    REQUIRE(Parse({}, &map) == Status::Malformed);

    Bytes long_item = BootKeyboardDescriptor;
    long_item.insert(long_item.begin() + 4, {0xFE, 0x02, 0x10, 0xAA, 0xBB});
    REQUIRE(Parse(long_item, &map) == Status::Malformed);
    REQUIRE(map.field_count == 0);
    REQUIRE(map.report_count == 0);

    Bytes extra_end = BootKeyboardDescriptor;
    extra_end.push_back(0xC0);
    REQUIRE(Parse(extra_end, &map) == Status::Malformed);

    Bytes missing_end = BootKeyboardDescriptor;
    missing_end.pop_back();
    REQUIRE(Parse(missing_end, &map) == Status::Malformed);

    Bytes truncated_item = BootKeyboardDescriptor;
    truncated_item.back() = 0x05;
    REQUIRE(Parse(truncated_item, &map) == Status::Malformed);
    Bytes cut = {0x05, 0x01, 0x26, 0xFF};
    REQUIRE(Parse(cut, &map) == Status::Malformed);

    Bytes zero_id = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x00, 0xC0};
    REQUIRE(Parse(zero_id, &map) == Status::Malformed);

    // Items without a report ID, then one with: the earlier ones would have no ID in their reports.
    Bytes mixed = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
                   0x85, 0x01, 0x81, 0x01, 0xC0};
    REQUIRE(Parse(mixed, &map) == Status::Malformed);

    Bytes descending_range = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0x10,
                              0x29, 0x05, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0xC0};
    REQUIRE(Parse(descending_range, &map) == Status::Malformed);
}

TEST_CASE("hid T19: limits on IDs, field and report sizes", "[usb][hid][report]")
{
    ReportMap map;
    Bytes nine_ids = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x75, 0x08, 0x95, 0x01};
    for (uint8_t id = 1; id <= 9; ++id)
    {
        nine_ids.insert(nine_ids.end(), {0x85, id, 0x81, 0x01});
    }
    nine_ids.push_back(0xC0);
    REQUIRE(Parse(nine_ids, &map) == Status::TooLarge);
    nine_ids.erase(nine_ids.end() - 5, nine_ids.end() - 1);
    REQUIRE(Parse(nine_ids, &map) == Status::Ok);
    REQUIRE(map.report_count == 8);

    Bytes wide_field = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x75, 0x21, 0x95, 0x01, 0x81, 0x01, 0xC0};
    REQUIRE(Parse(wide_field, &map) == Status::TooLarge);
    wide_field[7] = 0x20;
    REQUIRE(Parse(wide_field, &map) == Status::Ok);

    Bytes big_report = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x75, 0x08, 0x95, 0x41, 0x81, 0x01, 0xC0};
    REQUIRE(Parse(big_report, &map) == Status::TooLarge);
    big_report[9] = 0x40;
    REQUIRE(Parse(big_report, &map) == Status::Ok);
    REQUIRE(map.reports[0].input_bytes == 64);
    // With a report ID the byte for it counts too.
    Bytes with_id = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x75, 0x08,
                     0x95, 0x40, 0x81, 0x01, 0xC0};
    REQUIRE(Parse(with_id, &map) == Status::TooLarge);

    Bytes too_long(MaxDescriptorBytes + 1, 0x00);
    REQUIRE(Parse(too_long, &map) == Status::TooLarge);
}

TEST_CASE("hid T20: signed 12-bit fields across bytes", "[usb][hid][report]")
{
    ReportMap map;
    REQUIRE(Parse(WideMouseDescriptor, &map) == Status::Ok);
    // X = -100 (0xF9C) and Y = 300 (0x12C) share the middle byte.
    const Bytes report = {0x05, 0x9C, 0xCF, 0x12, 0xFB, 0x03};
    MouseReport mouse;
    REQUIRE(ExtractMouse(map, View(report), &mouse));
    REQUIRE(mouse.buttons == 0x05);
    REQUIRE(mouse.dx == -100);
    REQUIRE(mouse.dy == 300);
    REQUIRE(mouse.wheel == -5);
    REQUIRE(mouse.pan == 3);

    const Bytes extremes = {0x1F, 0x01, 0x08, 0x80, 0x7F, 0x81};
    REQUIRE(ExtractMouse(map, View(extremes), &mouse));
    REQUIRE(mouse.buttons == 0x1F);
    REQUIRE(mouse.dx == -2047);
    REQUIRE(mouse.dy == -2048);
    REQUIRE(mouse.wheel == 127);
    REQUIRE(mouse.pan == -127);

    const ReportMap boot = BootMouse();
    const Bytes boot_report = {0x02, 0x81, 0x7F};
    REQUIRE(ExtractMouse(boot, View(boot_report), &mouse));
    REQUIRE(mouse.buttons == 2);
    REQUIRE(mouse.dx == -127);
    REQUIRE(mouse.dy == 127);
    REQUIRE(mouse.wheel == 0);
}

TEST_CASE("hid T20: reports go to their ID's fields and unknown IDs are ignored",
          "[usb][hid][report]")
{
    ReportMap map;
    REQUIRE(Parse(ComboDescriptor, &map) == Status::Ok);
    KeySet keys;
    MouseReport mouse;

    const Bytes keyboard = {0x01, 0x02, 0x00, 0x04, 0x05, 0x00, 0x00, 0x00};
    REQUIRE(ExtractKeyboard(map, View(keyboard), &keys));
    REQUIRE(keys.modifiers == 0x02);
    REQUIRE(keys.count == 2);
    REQUIRE(keys.keys[0] == 4);
    REQUIRE(keys.keys[1] == 5);
    REQUIRE_FALSE(ExtractMouse(map, View(keyboard), &mouse));

    const Bytes pointer = {0x03, 0x01, 0xFF, 0x02, 0xFF};
    REQUIRE(ExtractMouse(map, View(pointer), &mouse));
    REQUIRE(mouse.buttons == 1);
    REQUIRE(mouse.dx == -1);
    REQUIRE(mouse.dy == 2);
    REQUIRE(mouse.wheel == -1);
    REQUIRE_FALSE(ExtractKeyboard(map, View(pointer), &keys));

    const Bytes consumer = {0x02, 0xE9, 0x00};
    REQUIRE_FALSE(ExtractKeyboard(map, View(consumer), &keys));
    REQUIRE_FALSE(ExtractMouse(map, View(consumer), &mouse));
    const Bytes unknown = {0x09, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    REQUIRE_FALSE(ExtractKeyboard(map, View(unknown), &keys));
    REQUIRE_FALSE(ExtractMouse(map, View(unknown), &mouse));
    REQUIRE_FALSE(ExtractKeyboard(map, std::span<const uint8_t>(), &keys));
}

TEST_CASE("hid T20: keyboard arrays, bitmaps and rollover", "[usb][hid][report]")
{
    const ReportMap boot = BootKeyboard();
    KeySet keys;
    const Bytes pressed = {0x03, 0x00, 0x04, 0x04, 0x1E, 0x00, 0x00, 0x00};
    REQUIRE(ExtractKeyboard(boot, View(pressed), &keys));
    REQUIRE(keys.modifiers == 0x03);
    REQUIRE(keys.count == 2);
    REQUIRE(keys.keys[0] == 4);
    REQUIRE(keys.keys[1] == 0x1E);
    REQUIRE_FALSE(keys.rollover);

    const Bytes rollover = {0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    REQUIRE(ExtractKeyboard(boot, View(rollover), &keys));
    REQUIRE(keys.rollover);
    const Bytes partial = {0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00};
    REQUIRE(ExtractKeyboard(boot, View(partial), &keys));
    REQUIRE_FALSE(keys.rollover);
    REQUIRE(keys.count == 1);

    ReportMap nkro;
    REQUIRE(Parse(NkroDescriptor, &nkro) == Status::Ok);
    Bytes bitmap(29, 0);
    bitmap[0] = 0x02;
    bitmap[1 + 0] |= 1U << 4;
    bitmap[1 + 3] |= 1U << 6;
    bitmap[1 + 28 - 1] |= 1U << 7;
    REQUIRE(ExtractKeyboard(nkro, View(bitmap), &keys));
    REQUIRE(keys.modifiers == 0x02);
    REQUIRE(keys.count == 3);
    REQUIRE(keys.keys[0] == 4);
    REQUIRE(keys.keys[1] == 30);
    REQUIRE(keys.keys[2] == 223);
}

TEST_CASE("hid T20: no field is read outside its report", "[usb][hid][report]")
{
    const Bytes* descriptors[] = {&BootKeyboardDescriptor, &BootMouseDescriptor, &NkroDescriptor,
                                  &ComboDescriptor, &WideMouseDescriptor, &PushPopDescriptor};
    for (const Bytes* descriptor : descriptors)
    {
        ReportMap map;
        REQUIRE(Parse(*descriptor, &map) == Status::Ok);
        size_t longest = 0;
        for (size_t i = 0; i < map.report_count; ++i)
        {
            longest = std::max<size_t>(longest, map.reports[i].input_bytes);
        }
        for (size_t i = 0; i < map.report_count; ++i)
        {
            const ReportInfo& info = map.reports[i];
            for (size_t length = 0; length <= longest + 1; ++length)
            {
                Bytes report(length, 0xFF);
                if (map.uses_ids && length > 0)
                {
                    report[0] = info.id;
                }
                KeySet keys;
                MouseReport mouse;
                const bool keyboard = ExtractKeyboard(map, View(report), &keys);
                const bool pointer = ExtractMouse(map, View(report), &mouse);
                if (length < info.input_bytes)
                {
                    REQUIRE_FALSE(keyboard);
                    REQUIRE_FALSE(pointer);
                }
            }
        }
    }
}

TEST_CASE("hid: the LED report follows the lock state", "[usb][hid][report]")
{
    uint8_t report[8];
    size_t length = 0;
    const ReportMap boot = BootKeyboard();
    REQUIRE(BuildLedReport(boot, 0, report, sizeof(report), &length));
    REQUIRE(length == 1);
    REQUIRE(report[0] == 0);
    REQUIRE(BuildLedReport(boot, 1, report, sizeof(report), &length));
    REQUIRE(report[0] == 1);
    REQUIRE(BuildLedReport(boot, 3, report, sizeof(report), &length));
    REQUIRE(report[0] == 3);
    REQUIRE_FALSE(BuildLedReport(boot, 3, report, 0, &length));
    REQUIRE_FALSE(BuildLedReport(BootMouse(), 3, report, sizeof(report), &length));

    // With report IDs the report starts with the ID.
    ReportMap map;
    const Bytes with_id = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x07, 0x05, 0x08, 0x19,
                           0x01, 0x29, 0x03, 0x75, 0x01, 0x95, 0x03, 0x91, 0x02, 0x75, 0x05,
                           0x95, 0x01, 0x91, 0x01, 0xC0};
    REQUIRE(Parse(with_id, &map) == Status::Ok);
    REQUIRE(BuildLedReport(map, 2, report, sizeof(report), &length));
    REQUIRE(length == 2);
    REQUIRE(report[0] == 7);
    REQUIRE(report[1] == 2);
}
