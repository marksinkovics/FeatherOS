#include "spawn_request.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

namespace SpawnRequestDetail
{

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

namespace
{

SpawnRequestResult PutWord(LineWriter* writer, const char* word)
{
    bool quoted = *word == '\0';
    for (const char* character = word; *character != '\0'; ++character)
    {
        if (*character == '"')
        {
            return SpawnRequestResult::Invalid;
        }
        quoted = quoted || *character == ' ';
    }
    if (quoted && !writer->Put('"'))
    {
        return SpawnRequestResult::TooBig;
    }
    for (const char* character = word; *character != '\0'; ++character)
    {
        if (!writer->Put(*character))
        {
            return SpawnRequestResult::TooBig;
        }
    }
    return quoted && !writer->Put('"') ? SpawnRequestResult::TooBig : SpawnRequestResult::Done;
}

} // namespace

} // namespace SpawnRequestDetail

} // namespace FeatherOS::LibC

namespace FeatherOS::LibC
{

SpawnRequestResult BuildCommandLine(const char* path, char* const arguments[],
                                    char (&line)[User::MaximumCommandLineLength])
{
    using namespace SpawnRequestDetail;
    line[0] = '\0';
    if (*path == '\0')
    {
        return SpawnRequestResult::NotFound;
    }
    const char* program = path;
    size_t prefix = 0;
    while (SystemPrefix[prefix] != '\0' && path[prefix] == SystemPrefix[prefix])
    {
        ++prefix;
    }
    bool named = SystemPrefix[prefix] == '\0' && path[prefix] != '\0';
    for (size_t index = prefix; named && path[index] != '\0'; ++index)
    {
        named = path[index] != '/';
    }
    if (named)
    {
        program = path + prefix;
    }
    LineWriter writer(line);
    SpawnRequestResult result = PutWord(&writer, program);
    uint64_t count = 1;
    for (size_t index = 1; result == SpawnRequestResult::Done && arguments != nullptr &&
                           arguments[0] != nullptr && arguments[index] != nullptr;
         ++index)
    {
        if (++count > User::MaximumArgumentCount || !writer.Put(' '))
        {
            return SpawnRequestResult::TooBig;
        }
        result = PutWord(&writer, arguments[index]);
    }
    return result;
}

bool NextSearchCandidate(const char** search, const char* file,
                         char (&candidate)[User::MaximumPathLength])
{
    while (**search != '\0')
    {
        const char* directory = *search;
        size_t length = 0;
        while (directory[length] != '\0' && directory[length] != ':')
        {
            ++length;
        }
        *search = directory[length] == ':' ? directory + length + 1 : directory + length;
        const size_t separator = length > 0 && directory[length - 1] != '/' ? 1 : 0;
        size_t file_length = 0;
        while (file[file_length] != '\0')
        {
            ++file_length;
        }
        if (length == 0 || length + separator + file_length >= sizeof(candidate))
        {
            continue;
        }
        for (size_t index = 0; index < length; ++index)
        {
            candidate[index] = directory[index];
        }
        candidate[length] = '/';
        for (size_t index = 0; index <= file_length; ++index)
        {
            candidate[length + separator + index] = file[index];
        }
        return true;
    }
    return false;
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
