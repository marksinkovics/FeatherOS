#include "command_line.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

constexpr char Help[] = "usage: ls [directory]\n"
                        "Lists a directory (the current one by default) with file sizes in\n"
                        "bytes; subdirectories end in /. /system holds the read-only boot files.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc > 2)
    {
        puts("usage: ls [directory]");
        return 1;
    }
    const char* name = argc == 2 ? argv[1] : ".";
    DIR* directory = opendir(name);
    // Entries are looked up from inside the directory, so their paths stay short.
    if (directory == nullptr || chdir(name) != 0)
    {
        printf("ls: %s: no such directory\n", name);
        return 1;
    }
    constexpr int NameColumnWidth = 24;
    unsigned long files = 0;
    unsigned long directories = 0;
    unsigned long long bytes = 0;
    for (;;)
    {
        errno = 0;
        const dirent* entry = readdir(directory);
        if (entry == nullptr)
        {
            if (errno != 0)
            {
                printf("ls: %s: no such directory\n", name);
                return 1;
            }
            break;
        }
        const bool is_directory = entry->d_type == DT_DIR;
        const int length = static_cast<int>(strlen(entry->d_name)) + (is_directory ? 1 : 0);
        printf("%s%s%*s", entry->d_name, is_directory ? "/" : "",
               length < NameColumnWidth ? NameColumnWidth - length : 1, "");
        if (is_directory)
        {
            puts("<dir>");
            ++directories;
            continue;
        }
        struct stat status;
        const long long size = stat(entry->d_name, &status) == 0 ? status.st_size : 0;
        printf("%lld\n", size);
        ++files;
        bytes += static_cast<unsigned long long>(size);
    }
    closedir(directory);
    if (directories > 0)
    {
        printf("%lu %s, ", directories, directories == 1 ? "directory" : "directories");
    }
    printf("%lu %s, %llu bytes\n", files, files == 1 ? "file" : "files", bytes);
    return 0;
}
