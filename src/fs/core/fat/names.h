// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_FAT_NAMES_H
#define FEATHERDISK_FAT_NAMES_H

#include <array>
#include <cstdint>
#include <string_view>

// FAT file names: 8.3 short names, and the VFAT long names stored in extra directory entries
// before a file's short entry. Long names here are ASCII only; other characters read as '?'.
namespace FeatherDisk::Fat
{

constexpr std::uint32_t MaximumNameLength = 63;
constexpr std::uint32_t CharactersPerLongEntry = 13;
constexpr std::uint8_t LongNameAttribute = 0x0F;
constexpr std::uint32_t EntrySize = 32;

// Base name then extension, space-padded, as a directory entry stores it.
using ShortName = std::array<std::uint8_t, 11>;

struct Name
{
    std::array<char, MaximumNameLength> characters;
    std::uint32_t length;

    std::string_view View() const;
};

// Printable ASCII without \ / : * ? " < > |, not ending in a space or dot.
bool IsValidName(std::string_view name);
// Succeeds when name fits 8.3 with one case in each part, writing the upper-case short name and
// the byte-12 flags that record lower case (as Windows NT and Linux do).
bool ToShortName(std::string_view name, ShortName& short_name, std::uint8_t& case_flags);
// The short name for a long name before numbering: upper-cased, with unsupported characters
// replaced by '_'.
void MakeBasis(std::string_view name, ShortName& short_name);
// Turns a basis into "BASIS~N", shortening it to fit eight characters.
void ApplyTail(ShortName& short_name, std::uint32_t number);
std::uint8_t Checksum(const std::uint8_t* short_name);
std::uint32_t LongEntryCount(std::string_view name);
// Entry sequence (1 for the first 13 characters) of a name needing total entries.
void EncodeLongEntry(std::string_view name, std::uint32_t sequence, std::uint32_t total,
                     std::uint8_t checksum, std::uint8_t* entry);
// "NAME.EXT" from a short entry, lower-cased where its flags say so.
void ShortNameText(const std::uint8_t* entry, Name& name);
// ASCII case-insensitive.
bool SameName(std::string_view left, std::string_view right);

// Assembles a long name from the entries before a short entry, which come last part first.
class LongNameReader
{
public:
    void Reset();
    void Feed(const std::uint8_t* entry, std::uint32_t slot);
    // True when a complete long name with the matching checksum precedes short_entry.
    bool Finish(const std::uint8_t* short_entry, Name& name, std::uint32_t& name_first_slot);

private:
    std::array<char, MaximumNameLength> text{};
    std::uint32_t length = 0;
    std::uint32_t next_sequence = 0;
    std::uint32_t first_slot = 0;
    std::uint8_t checksum = 0;
    bool valid = false;
};

} // namespace FeatherDisk::Fat

#endif
