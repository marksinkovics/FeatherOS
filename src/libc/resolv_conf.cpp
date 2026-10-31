#include "libc/resolv_conf.h"

#include "libc/address_text.h"

#include <string.h>

namespace FeatherOS::LibC::ResolvConf
{
namespace
{

constexpr uint16_t DefaultPort = 53;
// Above every limit, so larger numbers need not be held.
constexpr unsigned long NumberCeiling = 1000000;

struct Word
{
    const char* text;
    size_t length;
};

bool IsBlank(char character)
{
    return character == ' ' || character == '\t' || character == '\r';
}

// The next word before line_end, or a zero length when there is none.
Word NextWord(const char** cursor, const char* line_end)
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

bool Is(const Word& word, const char* keyword)
{
    return word.length == strlen(keyword) && strncmp(word.text, keyword, word.length) == 0;
}

// Decimal digits only; larger values become NumberCeiling.
bool ParseNumber(const char* text, size_t length, unsigned long* value)
{
    *value = 0;
    if (length == 0)
    {
        return false;
    }
    for (size_t index = 0; index < length; ++index)
    {
        if (text[index] < '0' || text[index] > '9')
        {
            return false;
        }
        *value = *value * 10 + static_cast<unsigned long>(text[index] - '0');
        if (*value > NumberCeiling)
        {
            *value = NumberCeiling;
        }
    }
    return true;
}

void AddNameServer(const Word& word, Config* config)
{
    if (config->name_server_count == MaximumNameServers)
    {
        return;
    }
    const char* hash = static_cast<const char*>(memchr(word.text, '#', word.length));
    const size_t address_length =
        hash == nullptr ? word.length : static_cast<size_t>(hash - word.text);
    unsigned long port = DefaultPort;
    if (hash != nullptr && (!ParseNumber(hash + 1, word.length - address_length - 1, &port) ||
                            port == 0 || port > 0xFFFF))
    {
        return;
    }
    NameServer& server = config->name_servers[config->name_server_count];
    server.address_length = ParseAddressText(word.text, address_length, server.address);
    if (server.address_length == 0)
    {
        return;
    }
    server.port = static_cast<uint16_t>(port);
    ++config->name_server_count;
}

// "name:value" for the given name, as a number from 0 up.
bool OptionValue(const Word& word, const char* name, unsigned long* value)
{
    const size_t name_length = strlen(name);
    return word.length > name_length + 1 && strncmp(word.text, name, name_length) == 0 &&
           word.text[name_length] == ':' &&
           ParseNumber(word.text + name_length + 1, word.length - name_length - 1, value);
}

int Clamp(unsigned long value, int maximum)
{
    return value < 1                                     ? 1
           : value > static_cast<unsigned long>(maximum) ? maximum
                                                         : static_cast<int>(value);
}

void SetOption(const Word& word, Config* config)
{
    unsigned long value = 0;
    if (OptionValue(word, "timeout", &value))
    {
        config->timeout_seconds = Clamp(value, MaximumTimeoutSeconds);
    }
    else if (OptionValue(word, "attempts", &value))
    {
        config->attempts = Clamp(value, MaximumAttempts);
    }
}

} // namespace

void Parse(const char* text, size_t length, Config* config)
{
    const char* cursor = text;
    const char* end = text + length;
    while (cursor < end)
    {
        const char* line_end = cursor;
        while (line_end < end && *line_end != '\n')
        {
            ++line_end;
        }
        const char* word_cursor = cursor;
        cursor = line_end < end ? line_end + 1 : end;

        const Word keyword = NextWord(&word_cursor, line_end);
        if (Is(keyword, "nameserver"))
        {
            AddNameServer(NextWord(&word_cursor, line_end), config);
        }
        else if (Is(keyword, "options"))
        {
            for (Word word = NextWord(&word_cursor, line_end); word.length > 0;
                 word = NextWord(&word_cursor, line_end))
            {
                SetOption(word, config);
            }
        }
    }
}

} // namespace FeatherOS::LibC::ResolvConf
