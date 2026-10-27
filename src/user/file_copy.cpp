#include "file_copy.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace FeatherOS::User
{

bool ResolveTarget(const char* program, const char* source, const char* target, char* path,
                   size_t capacity)
{
    struct stat status;
    const char* slash = strrchr(source, '/');
    const bool into_directory = stat(target, &status) == 0 && S_ISDIR(status.st_mode);
    const int length = into_directory ? snprintf(path, capacity, "%s/%s", target,
                                                 slash == nullptr ? source : slash + 1)
                                      : snprintf(path, capacity, "%s", target);
    if (length < 0 || length >= PATH_MAX || static_cast<size_t>(length) >= capacity)
    {
        printf("%s: %s: %s\n", program, target, strerror(ENAMETOOLONG));
        return false;
    }
    return true;
}

bool CopyFile(const char* program, const char* source, const char* target)
{
    const int input = open(source, O_RDONLY);
    if (input < 0)
    {
        printf("%s: %s: %s\n", program, source, strerror(errno));
        return false;
    }
    const int output = open(target, O_WRONLY | O_CREAT | O_TRUNC);
    if (output < 0)
    {
        printf("%s: %s: %s\n", program, target, strerror(errno));
        close(input);
        return false;
    }
    static char buffer[4096];
    bool copied = true;
    for (;;)
    {
        const ssize_t count = read(input, buffer, sizeof(buffer));
        if (count == 0)
        {
            break;
        }
        if (count < 0 || write(output, buffer, static_cast<size_t>(count)) != count)
        {
            printf("%s: %s: %s\n", program, count < 0 ? source : target, strerror(errno));
            copied = false;
            break;
        }
    }
    close(input);
    return close(output) == 0 && copied;
}

} // namespace FeatherOS::User
