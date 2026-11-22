#ifndef FEATHEROS_TESTS_SUPPORT_FONT_BUILDER_H
#define FEATHEROS_TESTS_SUPPORT_FONT_BUILDER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace FeatherOS::TestSupport
{

struct TestGlyph
{
    uint32_t code_point = 0;
    int16_t left = 0;
    int16_t top = 0;
    uint16_t advance = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    // width * height values of 0 to 15, row by row.
    std::vector<uint8_t> coverage;
    uint16_t spare = 0;
};

struct TestFont
{
    std::string name = "test 16";
    std::string notice = "Made for the tests. SIL Open Font License 1.1.";
    std::string text_suffix;
    bool monospace = false;
    uint16_t em = 13;
    uint16_t line_height = 16;
    int16_t ascent = 13;
    int16_t descent = 3;
    uint16_t cell_width = 0;
    std::vector<TestGlyph> glyphs;
};

namespace FontOffset
{
constexpr size_t Magic = 0;
constexpr size_t Version = 4;
constexpr size_t HeaderSize = 6;
constexpr size_t FileSize = 8;
constexpr size_t Flags = 12;
constexpr size_t Em = 16;
constexpr size_t LineHeight = 18;
constexpr size_t Ascent = 20;
constexpr size_t Descent = 22;
constexpr size_t CellWidth = 24;
constexpr size_t CoverageBits = 26;
constexpr size_t Reserved = 27;
constexpr size_t GlyphCount = 28;
constexpr size_t TableOffset = 32;
constexpr size_t TableSize = 36;
constexpr size_t BitmapsOffset = 40;
constexpr size_t BitmapsSize = 44;
constexpr size_t TextOffset = 48;
constexpr size_t TextSize = 52;
constexpr size_t Checksum = 56;
constexpr size_t RecordSize = 20;
} // namespace FontOffset

uint8_t SampleCoverage(uint32_t code_point, int x, int y);

// Record 0 (a hollow box), U+0020-007E, U+00A0 (with hand-written coverage), U+00E9, U+0391,
// U+2500 and U+10FFFF.
TestFont SampleFont();

std::vector<uint8_t> BuildFont(const TestFont& font);

void Seal(std::vector<uint8_t>& bytes);

uint16_t Get16(const std::vector<uint8_t>& bytes, size_t offset);
uint32_t Get32(const std::vector<uint8_t>& bytes, size_t offset);
void Put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value);
void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value);

size_t RecordOffset(const std::vector<uint8_t>& bytes, size_t index);
// The offset in the file of the first byte of record `index`'s bitmap.
size_t BitmapOffset(const std::vector<uint8_t>& bytes, size_t index);

} // namespace FeatherOS::TestSupport

#endif
