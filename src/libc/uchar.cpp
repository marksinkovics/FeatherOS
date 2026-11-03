#include "libc/unicode_conversion.h"

#include <errno.h>
#include <string.h>
#include <uchar.h>

namespace
{

using FeatherOS::LibC::ConversionInvalid;
using FeatherOS::LibC::ConversionState;

static_assert(sizeof(mbstate_t) == sizeof(ConversionState));

// Each function keeps its own state for callers that pass none, as C requires.
template <typename Convert> size_t WithState(mbstate_t* given, mbstate_t& own, Convert convert)
{
    mbstate_t* state = given != nullptr ? given : &own;
    ConversionState converted = {};
    memcpy(&converted, state, sizeof(converted));
    const size_t result = convert(converted);
    memcpy(state, &converted, sizeof(converted));
    if (result == ConversionInvalid)
    {
        errno = EILSEQ;
    }
    return result;
}

mbstate_t mbrtoc16_state;
mbstate_t c16rtomb_state;
mbstate_t mbrtoc32_state;
mbstate_t c32rtomb_state;

} // namespace

size_t mbrtoc16(char16_t* unit, const char* bytes, size_t count, mbstate_t* state)
{
    if (bytes == nullptr)
    {
        return mbrtoc16(nullptr, "", 1, state);
    }
    return WithState(state, mbrtoc16_state,
                     [&](ConversionState& converted)
                     {
                         uint16_t decoded = 0;
                         const size_t result =
                             FeatherOS::LibC::DecodeUtf8ToUtf16(converted, bytes, count, &decoded);
                         if (unit != nullptr)
                         {
                             *unit = decoded;
                         }
                         return result;
                     });
}

size_t c16rtomb(char* bytes, char16_t unit, mbstate_t* state)
{
    char ignored[4];
    if (bytes == nullptr)
    {
        bytes = ignored;
        unit = 0;
    }
    return WithState(state, c16rtomb_state, [&](ConversionState& converted)
                     { return FeatherOS::LibC::EncodeUtf16AsUtf8(converted, unit, bytes); });
}

size_t mbrtoc32(char32_t* character, const char* bytes, size_t count, mbstate_t* state)
{
    if (bytes == nullptr)
    {
        return mbrtoc32(nullptr, "", 1, state);
    }
    return WithState(state, mbrtoc32_state,
                     [&](ConversionState& converted)
                     {
                         uint32_t decoded = 0;
                         const size_t result =
                             FeatherOS::LibC::DecodeUtf8(converted, bytes, count, &decoded);
                         if (character != nullptr)
                         {
                             *character = decoded;
                         }
                         return result;
                     });
}

size_t c32rtomb(char* bytes, char32_t character, mbstate_t* state)
{
    char ignored[4];
    if (bytes == nullptr)
    {
        bytes = ignored;
        character = 0;
    }
    return WithState(state, c32rtomb_state,
                     [&](ConversionState& converted)
                     {
                         converted = {};
                         return FeatherOS::LibC::EncodeUtf8(character, bytes);
                     });
}
