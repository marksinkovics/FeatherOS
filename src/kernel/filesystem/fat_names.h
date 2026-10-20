#ifndef FEATHEROS_FILESYSTEM_FAT_NAMES_H
#define FEATHEROS_FILESYSTEM_FAT_NAMES_H

#include <stdint.h>

// FAT file names: 8.3 short names, and the VFAT long names stored in extra directory entries
// before a file's short entry. Long names here are ASCII only; other characters read as '?'.
namespace FeatherOS::FatNames
{

// Long names are limited to what fits in a path.
constexpr uint32_t MaximumNameLength = 63;
constexpr uint32_t CharactersPerLongEntry = 13;
constexpr uint8_t LongNameAttribute = 0x0F;
constexpr uint32_t EntrySize = 32;

// Printable ASCII without \ / : * ? " < > |, not ending in a space or dot.
bool IsValidName(const char* name);
// Succeeds when name fits 8.3 with one case in each part, writing the upper-case short name and
// the byte-12 flags that record lower case (as Windows NT and Linux do).
bool ToShortName(const char* name, uint8_t (&short_name)[11], uint8_t* case_flags);
// The short name for a long name before numbering: upper-cased, with unsupported characters
// replaced by '_'.
void MakeBasis(const char* name, uint8_t (&short_name)[11]);
// Turns a basis into "BASIS~N", shortening it to fit eight characters.
void ApplyTail(uint8_t (&short_name)[11], uint32_t number);
uint8_t Checksum(const uint8_t* short_name);
uint32_t LongEntryCount(const char* name);
// Entry sequence (1 for the first 13 characters) of a name needing total entries.
void EncodeLongEntry(const char* name, uint32_t sequence, uint32_t total, uint8_t checksum,
                     uint8_t* entry);
// "NAME.EXT" from a short entry, lower-cased where its flags say so.
void ShortNameText(const uint8_t* entry, char (&name)[MaximumNameLength + 1]);
// ASCII case-insensitive.
bool SameName(const char* left, const char* right);

// Assembles a long name from the entries before a short entry, which come last part first.
class LongNameReader
{
public:
    void Reset();
    void Feed(const uint8_t* entry, uint32_t slot);
    // True when a complete long name with the matching checksum precedes short_entry.
    bool Finish(const uint8_t* short_entry, char (&name)[MaximumNameLength + 1],
                uint32_t* first_slot);

private:
    char text[MaximumNameLength + 1];
    uint32_t length = 0;
    uint32_t next_sequence = 0;
    uint32_t first_slot = 0;
    uint8_t checksum = 0;
    bool valid = false;
};

} // namespace FeatherOS::FatNames

#endif
