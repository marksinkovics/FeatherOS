#ifndef FEATHEROS_USER_TEXT_UTF8_H
#define FEATHEROS_USER_TEXT_UTF8_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Text
{

constexpr char32_t ReplacementCharacter = 0xFFFD;

// Invalid input becomes one U+FFFD per maximal subpart, as Unicode recommends. A byte that
// breaks a sequence ends it and is then read again.
class Utf8Decoder
{
public:
    size_t Push(uint8_t byte, char32_t out[2]);
    size_t Finish(char32_t out[2]);
    bool Pending() const;

private:
    size_t Start(uint8_t byte, char32_t* out);

    char32_t m_code = 0;
    uint8_t m_remaining = 0;
    uint8_t m_lower = 0x80;
    uint8_t m_upper = 0xBF;
};

struct Utf8Unit
{
    uint8_t bytes[4];
    uint8_t length;
    // False for the bytes a display draws as one U+FFFD.
    bool valid;
};

// Groups a byte stream into the units a display draws as one cell each: a whole sequence, or the
// maximal subpart of an invalid one. Bytes below 0x80 are units of their own.
class Utf8Units
{
public:
    static constexpr size_t MaximumUnits = 2;

    // Returns how many units byte completed, in out. A byte that breaks a pending sequence ends it
    // as an invalid unit and is then read again.
    size_t Push(uint8_t byte, Utf8Unit out[MaximumUnits]);
    // Ends a pending sequence as an invalid unit.
    size_t Finish(Utf8Unit* out);
    bool Pending() const;

private:
    Utf8Decoder m_decoder;
    uint8_t m_bytes[4] = {};
    uint8_t m_length = 0;
};

// Returns 0 with *used 0 for an empty string, else the code point of the first sequence.
char32_t DecodeNext(const char* text, size_t length, size_t* used);

bool IsValidUtf8(const char* text, size_t length);

// Cells a display gives text: one per sequence, one per maximal invalid subpart.
size_t CellCount(const char* text, size_t length);

// Boundaries between the units of text, found from its start. PreviousBoundary is the start of the
// last unit that starts before position (0 for position 0), NextBoundary the end of the unit that
// holds position (length at the end), FloorBoundary position itself or, inside a unit, its start.
size_t PreviousBoundary(const char* text, size_t length, size_t position);
size_t NextBoundary(const char* text, size_t length, size_t position);
size_t FloorBoundary(const char* text, size_t length, size_t position);

} // namespace FeatherOS::Text

#endif
