#include "path.h"

namespace FeatherOS::Path
{
namespace
{

bool Append(const char* text, char (&result)[MaximumLength], uint32_t* length)
{
    while (*text != '\0')
    {
        while (*text == '/')
        {
            ++text;
        }
        uint32_t component_length = 0;
        while (text[component_length] != '\0' && text[component_length] != '/')
        {
            ++component_length;
        }
        if (component_length == 0 || (component_length == 1 && text[0] == '.'))
        {
            text += component_length;
            continue;
        }
        if (component_length == 2 && text[0] == '.' && text[1] == '.')
        {
            while (*length > 0 && result[*length - 1] != '/')
            {
                --*length;
            }
            if (*length > 0)
            {
                --*length;
            }
            text += component_length;
            continue;
        }
        if (*length + 1 + component_length >= MaximumLength)
        {
            return false;
        }
        result[(*length)++] = '/';
        for (uint32_t index = 0; index < component_length; ++index)
        {
            result[(*length)++] = text[index];
        }
        text += component_length;
    }
    return true;
}

} // namespace

bool Resolve(const char* working_directory, const char* path, char (&result)[MaximumLength])
{
    if (working_directory == nullptr || path == nullptr)
    {
        return false;
    }
    uint32_t length = 0;
    if ((path[0] != '/' && !Append(working_directory, result, &length)) ||
        !Append(path, result, &length))
    {
        return false;
    }
    if (length == 0)
    {
        result[length++] = '/';
    }
    result[length] = '\0';
    return true;
}

bool ReplaceProcSelf(char (&path)[MaximumLength], uint64_t process_id)
{
    constexpr char Prefix[] = "/proc/self";
    constexpr uint32_t PrefixLength = sizeof(Prefix) - 1;
    uint32_t matched = 0;
    while (matched < PrefixLength && path[matched] == Prefix[matched])
    {
        ++matched;
    }
    if (matched < PrefixLength || (path[PrefixLength] != '\0' && path[PrefixLength] != '/'))
    {
        return true;
    }
    char digits[20];
    uint32_t digit_count = 0;
    do
    {
        digits[digit_count++] = static_cast<char>('0' + process_id % 10);
        process_id /= 10;
    } while (process_id != 0);
    uint32_t rest_length = 0;
    while (path[PrefixLength + rest_length] != '\0')
    {
        ++rest_length;
    }
    constexpr uint32_t DirectoryLength = sizeof("/proc/") - 1;
    if (DirectoryLength + digit_count + rest_length >= MaximumLength)
    {
        return false;
    }
    // Moves the rest (such as "/status") into place, then writes the digits before it.
    const uint32_t rest_start = DirectoryLength + digit_count;
    if (rest_start > PrefixLength)
    {
        for (uint32_t index = rest_length + 1; index > 0; --index)
        {
            path[rest_start + index - 1] = path[PrefixLength + index - 1];
        }
    }
    else
    {
        for (uint32_t index = 0; index <= rest_length; ++index)
        {
            path[rest_start + index] = path[PrefixLength + index];
        }
    }
    for (uint32_t index = 0; index < digit_count; ++index)
    {
        path[DirectoryLength + index] = digits[digit_count - 1 - index];
    }
    return true;
}

} // namespace FeatherOS::Path
