#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

constexpr char Help[] = "usage: cp <source> <target>\n"
                        "Copies a file. A target that is a directory gets a file of the\n"
                        "source's name; an existing file is replaced.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 3)
    {
        puts("usage: cp <source> <target>");
        return 1;
    }
    const char* source = argv[1];
    char target[2 * PATH_MAX];
    struct stat status;
    const char* slash = strrchr(source, '/');
    const bool into_directory = stat(argv[2], &status) == 0 && S_ISDIR(status.st_mode);
    const int length = into_directory ? snprintf(target, sizeof(target), "%s/%s", argv[2],
                                                 slash == nullptr ? source : slash + 1)
                                      : snprintf(target, sizeof(target), "%s", argv[2]);
    if (length >= PATH_MAX)
    {
        printf("cp: %s: %s\n", argv[2], strerror(ENAMETOOLONG));
        return 1;
    }
    const int input = open(source, O_RDONLY);
    if (input < 0)
    {
        printf("cp: %s: %s\n", source, strerror(errno));
        return 1;
    }
    const int output = open(target, O_WRONLY | O_CREAT | O_TRUNC);
    if (output < 0)
    {
        printf("cp: %s: %s\n", target, strerror(errno));
        close(input);
        return 1;
    }
    static char buffer[4096];
    int status_code = 0;
    for (;;)
    {
        const ssize_t count = read(input, buffer, sizeof(buffer));
        if (count == 0)
        {
            break;
        }
        if (count < 0 || write(output, buffer, static_cast<size_t>(count)) != count)
        {
            printf("cp: %s: %s\n", count < 0 ? source : target, strerror(errno));
            status_code = 1;
            break;
        }
    }
    close(input);
    if (close(output) != 0)
    {
        status_code = 1;
    }
    return status_code;
}
