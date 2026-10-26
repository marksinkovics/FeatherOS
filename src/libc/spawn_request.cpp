#include "spawn_request.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

namespace SpawnRequestDetail
{

bool IsNameCharacter(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
}

LineWriter::LineWriter(char (&target)[User::MaximumCommandLineLength]) : line(target) {}

bool LineWriter::Put(char character)
{
    if (length + 1 >= sizeof(line))
    {
        return false;
    }
    line[length++] = character;
    line[length] = '\0';
    return true;
}

} // namespace SpawnRequestDetail

} // namespace FeatherOS::LibC

namespace FeatherOS::LibC
{

SpawnRequestResult BuildCommandLine(const char* path, char* const arguments[],
                                    char (&line)[User::MaximumCommandLineLength])
{
    using namespace SpawnRequestDetail;
    line[0] = '\0';
    const char* name = path;
    size_t prefix = 0;
    while (SystemPrefix[prefix] != '\0' && path[prefix] == SystemPrefix[prefix])
    {
        ++prefix;
    }
    if (SystemPrefix[prefix] == '\0')
    {
        name = path + prefix;
    }
    if (*name == '\0')
    {
        return SpawnRequestResult::NotFound;
    }
    LineWriter writer(line);
    for (const char* character = name; *character != '\0'; ++character)
    {
        if (!IsNameCharacter(*character))
        {
            return SpawnRequestResult::NotFound;
        }
        if (!writer.Put(*character))
        {
            return SpawnRequestResult::TooBig;
        }
    }
    uint64_t count = 1;
    for (size_t index = 1; arguments != nullptr && arguments[0] != nullptr && arguments[index];
         ++index)
    {
        const char* argument = arguments[index];
        bool quoted = *argument == '\0';
        for (const char* character = argument; *character != '\0'; ++character)
        {
            if (*character == '"')
            {
                return SpawnRequestResult::Invalid;
            }
            quoted = quoted || *character == ' ';
        }
        if (++count > User::MaximumArgumentCount || !writer.Put(' ') ||
            (quoted && !writer.Put('"')))
        {
            return SpawnRequestResult::TooBig;
        }
        for (const char* character = argument; *character != '\0'; ++character)
        {
            if (!writer.Put(*character))
            {
                return SpawnRequestResult::TooBig;
            }
        }
        if (quoted && !writer.Put('"'))
        {
            return SpawnRequestResult::TooBig;
        }
    }
    return SpawnRequestResult::Done;
}

SpawnRequestResult BuildEnvironmentBlock(char* const variables[],
                                         char (&block)[User::MaximumEnvironmentSize])
{
    size_t used = 0;
    uint64_t count = 0;
    for (size_t index = 0; variables != nullptr && variables[index] != nullptr; ++index)
    {
        const char* variable = variables[index];
        size_t length = 0;
        bool named = false;
        for (; variable[length] != '\0'; ++length)
        {
            named = named || variable[length] == '=';
        }
        named = named && variable[0] != '=';
        if (!named)
        {
            return SpawnRequestResult::Invalid;
        }
        if (++count > User::MaximumEnvironmentVariables || used + length + 2 > sizeof(block))
        {
            return SpawnRequestResult::TooBig;
        }
        for (size_t offset = 0; offset <= length; ++offset)
        {
            block[used + offset] = variable[offset];
        }
        used += length + 1;
    }
    block[used] = '\0';
    return SpawnRequestResult::Done;
}

} // namespace FeatherOS::LibC
