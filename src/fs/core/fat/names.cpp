// SPDX-License-Identifier: MIT OR Apache-2.0
#include "fat/names.h"

namespace FeatherDisk::Fat
{
namespace
{

constexpr std::uint8_t LastLongEntry = 0x40;
constexpr std::uint8_t SequenceMask = 0x1F;
constexpr std::uint8_t LowercaseBase = 0x08;
constexpr std::uint8_t LowercaseExtension = 0x10;
// Byte offsets of the 13 UCS-2 characters in a long-name entry.
constexpr std::array<std::uint8_t, CharactersPerLongEntry> CharacterOffsets = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
constexpr std::uint16_t NameEnd = 0x0000;
constexpr std::uint16_t NamePadding = 0xFFFF;

bool Contains(std::string_view set, char character)
{
    return set.find(character) != std::string_view::npos;
}

bool IsLower(char character)
{
    return character >= 'a' && character <= 'z';
}

bool IsUpper(char character)
{
    return character >= 'A' && character <= 'Z';
}

char ToUpper(char character)
{
    return IsLower(character) ? static_cast<char>(character - 'a' + 'A') : character;
}

char ToLower(char character)
{
    return IsUpper(character) ? static_cast<char>(character - 'A' + 'a') : character;
}

bool IsShortNameCharacter(char character)
{
    return IsLower(character) || IsUpper(character) || (character >= '0' && character <= '9') ||
           Contains("!#$%&'()-@^_`{}~", character);
}

// Fills part with the upper-cased characters of text, or fails if one is not allowed or the part
// mixes cases.
bool ShortPart(std::string_view text, std::uint8_t* part, std::uint32_t capacity, bool& lower_case)
{
    bool has_lower = false;
    bool has_upper = false;
    if (text.size() > capacity)
    {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (!IsShortNameCharacter(text[index]))
        {
            return false;
        }
        has_lower = has_lower || IsLower(text[index]);
        has_upper = has_upper || IsUpper(text[index]);
        part[index] = static_cast<std::uint8_t>(ToUpper(text[index]));
    }
    lower_case = has_lower;
    return !(has_lower && has_upper);
}

std::uint32_t BasisPart(std::string_view text, std::uint8_t* part, std::uint32_t capacity)
{
    std::uint32_t written = 0;
    for (std::size_t index = 0; index < text.size() && written < capacity; ++index)
    {
        const char character = text[index];
        if (character == ' ' || character == '.')
        {
            continue;
        }
        part[written++] =
            static_cast<std::uint8_t>(IsShortNameCharacter(character) ? ToUpper(character) : '_');
    }
    return written;
}

} // namespace

std::string_view Name::View() const
{
    return {characters.data(), length};
}

bool IsValidName(std::string_view name)
{
    if (name.empty() || name.size() > MaximumNameLength || name.back() == ' ' || name.back() == '.')
    {
        return false;
    }
    for (const char character : name)
    {
        if (character < 0x20 || character > 0x7E || Contains("\\/:*?\"<>|", character))
        {
            return false;
        }
    }
    return true;
}

bool ToShortName(std::string_view name, ShortName& short_name, std::uint8_t& case_flags)
{
    short_name.fill(' ');
    const std::size_t length = name.size();
    std::size_t dot = length;
    for (std::size_t index = 0; index < length; ++index)
    {
        if (name[index] == '.')
        {
            if (dot != length || index == 0)
            {
                return false;
            }
            dot = index;
        }
    }
    bool lower_base = false;
    bool lower_extension = false;
    const std::string_view extension = dot == length ? std::string_view{} : name.substr(dot + 1);
    if (dot == 0 || !ShortPart(name.substr(0, dot), short_name.data(), 8, lower_base) ||
        !ShortPart(extension, short_name.data() + 8, 3, lower_extension))
    {
        return false;
    }
    case_flags = static_cast<std::uint8_t>((lower_base ? LowercaseBase : 0) |
                                           (lower_extension ? LowercaseExtension : 0));
    return true;
}

void MakeBasis(std::string_view name, ShortName& short_name)
{
    short_name.fill(' ');
    const std::size_t length = name.size();
    std::size_t dot = length;
    for (std::size_t index = length; index > 1; --index)
    {
        if (name[index - 1] == '.')
        {
            dot = index - 1;
            break;
        }
    }
    if (BasisPart(name.substr(0, dot), short_name.data(), 8) == 0)
    {
        short_name[0] = '_';
    }
    if (dot < length)
    {
        BasisPart(name.substr(dot + 1), short_name.data() + 8, 3);
    }
}

void ApplyTail(ShortName& short_name, std::uint32_t number)
{
    std::array<char, 11> tail{};
    std::uint32_t tail_length = 0;
    do
    {
        tail[tail_length++] = static_cast<char>('0' + number % 10);
        number /= 10;
    } while (number != 0 && tail_length < 7);
    tail[tail_length++] = '~';
    std::uint32_t base_length = 0;
    while (base_length < 8 && short_name[base_length] != ' ')
    {
        ++base_length;
    }
    std::uint32_t position = base_length + tail_length > 8 ? 8 - tail_length : base_length;
    while (tail_length > 0)
    {
        short_name[position++] = static_cast<std::uint8_t>(tail[--tail_length]);
    }
    for (; position < 8; ++position)
    {
        short_name[position] = ' ';
    }
}

std::uint8_t Checksum(const std::uint8_t* short_name)
{
    std::uint8_t sum = 0;
    for (std::uint32_t index = 0; index < 11; ++index)
    {
        sum = static_cast<std::uint8_t>(((sum & 1U) << 7) + (sum >> 1) + short_name[index]);
    }
    return sum;
}

std::uint32_t LongEntryCount(std::string_view name)
{
    return static_cast<std::uint32_t>((name.size() + CharactersPerLongEntry - 1) /
                                      CharactersPerLongEntry);
}

void EncodeLongEntry(std::string_view name, std::uint32_t sequence, std::uint32_t total,
                     std::uint8_t checksum, std::uint8_t* entry)
{
    for (std::uint32_t index = 0; index < EntrySize; ++index)
    {
        entry[index] = 0;
    }
    entry[0] = static_cast<std::uint8_t>(sequence | (sequence == total ? LastLongEntry : 0));
    entry[11] = LongNameAttribute;
    entry[13] = checksum;
    const std::size_t length = name.size();
    for (std::uint32_t index = 0; index < CharactersPerLongEntry; ++index)
    {
        const std::size_t position = (sequence - 1) * CharactersPerLongEntry + index;
        const std::uint16_t code = position < length    ? static_cast<std::uint8_t>(name[position])
                                   : position == length ? NameEnd
                                                        : NamePadding;
        entry[CharacterOffsets[index]] = static_cast<std::uint8_t>(code);
        entry[CharacterOffsets[index] + 1] = static_cast<std::uint8_t>(code >> 8);
    }
}

void ShortNameText(const std::uint8_t* entry, Name& name)
{
    std::uint32_t length = 0;
    for (std::uint32_t offset = 0; offset < 8 && entry[offset] != ' '; ++offset)
    {
        const char character = static_cast<char>(entry[offset]);
        name.characters[length++] =
            (entry[12] & LowercaseBase) != 0 ? ToLower(character) : character;
    }
    if (entry[8] != ' ')
    {
        name.characters[length++] = '.';
        for (std::uint32_t offset = 8; offset < 11 && entry[offset] != ' '; ++offset)
        {
            const char character = static_cast<char>(entry[offset]);
            name.characters[length++] =
                (entry[12] & LowercaseExtension) != 0 ? ToLower(character) : character;
        }
    }
    name.length = length;
}

bool SameName(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (ToUpper(left[index]) != ToUpper(right[index]))
        {
            return false;
        }
    }
    return true;
}

void LongNameReader::Reset()
{
    valid = false;
    next_sequence = 0;
}

void LongNameReader::Feed(const std::uint8_t* entry, std::uint32_t slot)
{
    const std::uint32_t sequence = entry[0] & SequenceMask;
    if ((entry[0] & LastLongEntry) != 0)
    {
        // Names longer than MaximumNameLength cannot be looked up, so they read as their short
        // names.
        valid = sequence != 0 && (sequence - 1) * CharactersPerLongEntry < MaximumNameLength;
        checksum = entry[13];
        first_slot = slot;
        length = sequence * CharactersPerLongEntry;
    }
    else if (!valid || sequence != next_sequence || entry[13] != checksum)
    {
        valid = false;
    }
    if (!valid)
    {
        return;
    }
    for (std::uint32_t index = 0; index < CharactersPerLongEntry; ++index)
    {
        const std::uint32_t position = (sequence - 1) * CharactersPerLongEntry + index;
        const auto code = static_cast<std::uint16_t>(entry[CharacterOffsets[index]] |
                                                     (entry[CharacterOffsets[index] + 1] << 8));
        if (code == NameEnd && position < length)
        {
            length = position;
        }
        if (position >= length)
        {
            continue;
        }
        if (position >= MaximumNameLength)
        {
            valid = false;
            return;
        }
        text[position] = code >= 0x20 && code <= 0x7E ? static_cast<char>(code) : '?';
    }
    next_sequence = sequence - 1;
}

bool LongNameReader::Finish(const std::uint8_t* short_entry, Name& name,
                            std::uint32_t& name_first_slot)
{
    if (!valid || next_sequence != 0 || length == 0 || Checksum(short_entry) != checksum)
    {
        return false;
    }
    for (std::uint32_t index = 0; index < length; ++index)
    {
        name.characters[index] = text[index];
    }
    name.length = length;
    name_first_slot = first_slot;
    return true;
}

} // namespace FeatherDisk::Fat
