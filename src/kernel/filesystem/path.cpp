#include "path.h"

namespace FeatherOS::Path
{
namespace
{

bool Copy(const char* text, char (&result)[MaximumLength], uint32_t* length)
{
    for (; *text != '\0'; ++text)
    {
        if (*length + 1 >= MaximumLength)
        {
            return false;
        }
        result[(*length)++] = *text;
    }
    return true;
}

} // namespace

bool Join(const char* working_directory, const char* path, char (&result)[MaximumLength])
{
    if (working_directory == nullptr || path == nullptr)
    {
        return false;
    }
    uint32_t length = 0;
    if (path[0] == '/')
    {
        if (!Copy(path, result, &length))
        {
            return false;
        }
        result[length] = '\0';
        return true;
    }
    if (!Copy(working_directory, result, &length))
    {
        return false;
    }
    while (length > 1 && result[length - 1] == '/')
    {
        --length;
    }
    for (;;)
    {
        while (*path == '/')
        {
            ++path;
        }
        if (path[0] == '.' && (path[1] == '/' || path[1] == '\0'))
        {
            ++path;
            continue;
        }
        if (path[0] != '.' || path[1] != '.' || (path[2] != '/' && path[2] != '\0'))
        {
            break;
        }
        path += 2;
        while (length > 1 && result[length - 1] != '/')
        {
            --length;
        }
        if (length > 1)
        {
            --length;
        }
    }
    if (*path != '\0')
    {
        if (length > 1)
        {
            if (length + 1 >= MaximumLength)
            {
                return false;
            }
            result[length++] = '/';
        }
        if (!Copy(path, result, &length))
        {
            return false;
        }
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
