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

} // namespace FeatherOS::Path
