#include "libc/hosts_file.h"

#include "libc/address_text.h"

#include <string.h>
#include <strings.h>

namespace FeatherOS::LibC::Hosts
{
namespace
{

bool IsBlank(char character)
{
    return character == ' ' || character == '\t' || character == '\r';
}

// The next word before the end of the line or a comment, or a zero length when there is none.
Name NextWord(const char** cursor, const char* line_end)
{
    const char* start = *cursor;
    while (start < line_end && IsBlank(*start))
    {
        ++start;
    }
    const char* stop = start;
    while (stop < line_end && !IsBlank(*stop))
    {
        ++stop;
    }
    *cursor = stop;
    return {start, static_cast<size_t>(stop - start)};
}

} // namespace

bool NextLine(const char** cursor, const char* end, Line* line)
{
    while (*cursor < end)
    {
        const char* line_end = *cursor;
        while (line_end < end && *line_end != '\n' && *line_end != '#')
        {
            ++line_end;
        }
        const char* next = line_end;
        while (next < end && *next != '\n')
        {
            ++next;
        }
        const char* word_cursor = *cursor;
        *cursor = next < end ? next + 1 : end;

        const Name address = NextWord(&word_cursor, line_end);
        line->address_length = ParseAddressText(address.text, address.length, line->address);
        if (line->address_length == 0)
        {
            continue;
        }
        line->name_count = 0;
        for (Name word = NextWord(&word_cursor, line_end);
             word.length > 0 && line->name_count < MaximumNames;
             word = NextWord(&word_cursor, line_end))
        {
            line->names[line->name_count++] = word;
        }
        if (line->name_count > 0)
        {
            return true;
        }
    }
    return false;
}

bool Names(const Line& line, const char* name)
{
    size_t length = strlen(name);
    if (length > 0 && name[length - 1] == '.')
    {
        --length;
    }
    for (size_t index = 0; index < line.name_count; ++index)
    {
        if (line.names[index].length == length &&
            strncasecmp(line.names[index].text, name, length) == 0)
        {
            return true;
        }
    }
    return false;
}

} // namespace FeatherOS::LibC::Hosts
