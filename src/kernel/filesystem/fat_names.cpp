#include "fat_names.h"

namespace FeatherOS::FatNames
{
namespace
{

constexpr uint8_t LastLongEntry = 0x40;
constexpr uint8_t SequenceMask = 0x1F;
constexpr uint8_t LowercaseBase = 0x08;
constexpr uint8_t LowercaseExtension = 0x10;
// Byte offsets of the 13 UCS-2 characters in a long-name entry.
constexpr uint8_t CharacterOffsets[CharactersPerLongEntry] = {1,  3,  5,  7,  9,  14, 16,
                                                              18, 20, 22, 24, 28, 30};
constexpr uint16_t NameEnd = 0x0000;
constexpr uint16_t NamePadding = 0xFFFF;

uint32_t Length(const char* text)
{
    uint32_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

bool Contains(const char* set, char character)
{
    for (; *set != '\0'; ++set)
    {
        if (*set == character)
        {
            return true;
        }
    }
    return false;
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

// Fills part with the upper-cased characters of text[0, length), or fails if one is not allowed
// or the part mixes cases.
bool ShortPart(const char* text, uint32_t length, uint8_t* part, uint32_t capacity,
               bool* lower_case)
{
    bool has_lower = false;
    bool has_upper = false;
    if (length > capacity)
    {
        return false;
    }
    for (uint32_t index = 0; index < length; ++index)
    {
        if (!IsShortNameCharacter(text[index]))
        {
            return false;
        }
        has_lower = has_lower || IsLower(text[index]);
        has_upper = has_upper || IsUpper(text[index]);
        part[index] = static_cast<uint8_t>(ToUpper(text[index]));
    }
    *lower_case = has_lower;
    return !(has_lower && has_upper);
}

uint32_t BasisPart(const char* text, uint32_t length, uint8_t* part, uint32_t capacity)
{
    uint32_t written = 0;
    for (uint32_t index = 0; index < length && written < capacity; ++index)
    {
        const char character = text[index];
        if (character == ' ' || character == '.')
        {
            continue;
        }
        part[written++] =
            static_cast<uint8_t>(IsShortNameCharacter(character) ? ToUpper(character) : '_');
    }
    return written;
}

} // namespace

bool IsValidName(const char* name)
{
    const uint32_t length = Length(name);
    if (length == 0 || length > MaximumNameLength || name[length - 1] == ' ' ||
        name[length - 1] == '.')
    {
        return false;
    }
    for (uint32_t index = 0; index < length; ++index)
    {
        if (name[index] < 0x20 || name[index] > 0x7E || Contains("\\/:*?\"<>|", name[index]))
        {
            return false;
        }
    }
    return true;
}

bool ToShortName(const char* name, uint8_t (&short_name)[11], uint8_t* case_flags)
{
    for (uint8_t& byte : short_name)
    {
        byte = ' ';
    }
    const uint32_t length = Length(name);
    uint32_t dot = length;
    for (uint32_t index = 0; index < length; ++index)
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
    const uint32_t extension_length = dot == length ? 0 : length - dot - 1;
    if (dot == 0 || !ShortPart(name, dot, short_name, 8, &lower_base) ||
        !ShortPart(name + dot + (dot == length ? 0 : 1), extension_length, short_name + 8, 3,
                   &lower_extension))
    {
        return false;
    }
    *case_flags = (lower_base ? LowercaseBase : 0) | (lower_extension ? LowercaseExtension : 0);
    return true;
}

void MakeBasis(const char* name, uint8_t (&short_name)[11])
{
    for (uint8_t& byte : short_name)
    {
        byte = ' ';
    }
    const uint32_t length = Length(name);
    uint32_t dot = length;
    for (uint32_t index = length; index > 1; --index)
    {
        if (name[index - 1] == '.')
        {
            dot = index - 1;
            break;
        }
    }
    if (BasisPart(name, dot, short_name, 8) == 0)
    {
        short_name[0] = '_';
    }
    if (dot < length)
    {
        BasisPart(name + dot + 1, length - dot - 1, short_name + 8, 3);
    }
}

void ApplyTail(uint8_t (&short_name)[11], uint32_t number)
{
    char tail[11];
    uint32_t tail_length = 0;
    do
    {
        tail[tail_length++] = static_cast<char>('0' + number % 10);
        number /= 10;
    } while (number != 0 && tail_length < 7);
    tail[tail_length++] = '~';
    uint32_t base_length = 0;
    while (base_length < 8 && short_name[base_length] != ' ')
    {
        ++base_length;
    }
    uint32_t position = base_length + tail_length > 8 ? 8 - tail_length : base_length;
    while (tail_length > 0)
    {
        short_name[position++] = static_cast<uint8_t>(tail[--tail_length]);
    }
    for (; position < 8; ++position)
    {
        short_name[position] = ' ';
    }
}

uint8_t Checksum(const uint8_t* short_name)
{
    uint8_t sum = 0;
    for (uint32_t index = 0; index < 11; ++index)
    {
        sum = static_cast<uint8_t>(((sum & 1U) << 7) + (sum >> 1) + short_name[index]);
    }
    return sum;
}

uint32_t LongEntryCount(const char* name)
{
    return (Length(name) + CharactersPerLongEntry - 1) / CharactersPerLongEntry;
}

void EncodeLongEntry(const char* name, uint32_t sequence, uint32_t total, uint8_t checksum,
                     uint8_t* entry)
{
    for (uint32_t index = 0; index < EntrySize; ++index)
    {
        entry[index] = 0;
    }
    entry[0] = static_cast<uint8_t>(sequence | (sequence == total ? LastLongEntry : 0));
    entry[11] = LongNameAttribute;
    entry[13] = checksum;
    const uint32_t length = Length(name);
    for (uint32_t index = 0; index < CharactersPerLongEntry; ++index)
    {
        const uint32_t position = (sequence - 1) * CharactersPerLongEntry + index;
        const uint16_t code = position < length    ? static_cast<uint8_t>(name[position])
                              : position == length ? NameEnd
                                                   : NamePadding;
        entry[CharacterOffsets[index]] = static_cast<uint8_t>(code);
        entry[CharacterOffsets[index] + 1] = static_cast<uint8_t>(code >> 8);
    }
}

void ShortNameText(const uint8_t* entry, char (&name)[MaximumNameLength + 1])
{
    uint32_t length = 0;
    for (uint32_t offset = 0; offset < 8 && entry[offset] != ' '; ++offset)
    {
        const char character = static_cast<char>(entry[offset]);
        name[length++] = (entry[12] & LowercaseBase) != 0 ? ToLower(character) : character;
    }
    if (entry[8] != ' ')
    {
        name[length++] = '.';
        for (uint32_t offset = 8; offset < 11 && entry[offset] != ' '; ++offset)
        {
            const char character = static_cast<char>(entry[offset]);
            name[length++] = (entry[12] & LowercaseExtension) != 0 ? ToLower(character) : character;
        }
    }
    name[length] = '\0';
}

bool SameName(const char* left, const char* right)
{
    for (; *left != '\0' && ToUpper(*left) == ToUpper(*right); ++left, ++right)
    {
    }
    return *left == *right;
}

void LongNameReader::Reset()
{
    valid = false;
    next_sequence = 0;
}

void LongNameReader::Feed(const uint8_t* entry, uint32_t slot)
{
    const uint32_t sequence = entry[0] & SequenceMask;
    if ((entry[0] & LastLongEntry) != 0)
    {
        // Names longer than MaximumNameLength cannot be named in a path, so they read as their
        // short names.
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
    for (uint32_t index = 0; index < CharactersPerLongEntry; ++index)
    {
        const uint32_t position = (sequence - 1) * CharactersPerLongEntry + index;
        const uint16_t code = static_cast<uint16_t>(entry[CharacterOffsets[index]] |
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

bool LongNameReader::Finish(const uint8_t* short_entry, char (&name)[MaximumNameLength + 1],
                            uint32_t* name_first_slot)
{
    if (!valid || next_sequence != 0 || length == 0 || Checksum(short_entry) != checksum)
    {
        return false;
    }
    for (uint32_t index = 0; index < length; ++index)
    {
        name[index] = text[index];
    }
    name[length] = '\0';
    *name_first_slot = first_slot;
    return true;
}

} // namespace FeatherOS::FatNames
