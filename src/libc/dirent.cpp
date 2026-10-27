#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The path is made absolute when the directory is opened, so a later chdir does not change
// which directory the stream reads.
struct FeatherDirectory
{
    char path[FeatherOS::User::MaximumPathLength];
    uint64_t index;
    struct dirent entry;
};

DIR* opendir(const char* path)
{
    struct stat status;
    if (stat(path, &status) != 0)
    {
        return nullptr;
    }
    if (!S_ISDIR(status.st_mode))
    {
        errno = ENOTDIR;
        return nullptr;
    }
    auto* directory = static_cast<DIR*>(calloc(1, sizeof(DIR)));
    if (directory == nullptr)
    {
        return nullptr;
    }
    const size_t length = strlen(path);
    if (path[0] == '/')
    {
        if (length >= sizeof(directory->path))
        {
            free(directory);
            errno = ENAMETOOLONG;
            return nullptr;
        }
        memcpy(directory->path, path, length + 1);
        return directory;
    }
    if (getcwd(directory->path, sizeof(directory->path)) == nullptr)
    {
        free(directory);
        return nullptr;
    }
    const size_t prefix = strlen(directory->path);
    const bool needs_separator = directory->path[prefix - 1] != '/';
    if (prefix + (needs_separator ? 1 : 0) + length >= sizeof(directory->path))
    {
        free(directory);
        errno = ENAMETOOLONG;
        return nullptr;
    }
    if (needs_separator)
    {
        directory->path[prefix] = '/';
    }
    memcpy(directory->path + prefix + (needs_separator ? 1 : 0), path, length + 1);
    return directory;
}

struct dirent* readdir(DIR* directory)
{
    FeatherOS::User::DirectoryEntry entry;
    const uint64_t result =
        FeatherOS::User::ReadDirectory(directory->path, directory->index, &entry);
    // Past the last entry is the normal end of the listing, which leaves errno alone.
    if (result == FeatherOS::User::SyscallErrorNotFound)
    {
        return nullptr;
    }
    if (FeatherOS::LibC::Check<int>(result) != 0)
    {
        return nullptr;
    }
    ++directory->index;
    directory->entry.d_ino = directory->index;
    directory->entry.d_type = entry.directory ? DT_DIR : DT_REG;
    memcpy(directory->entry.d_name, entry.name, sizeof(directory->entry.d_name));
    directory->entry.d_name[sizeof(directory->entry.d_name) - 1] = '\0';
    return &directory->entry;
}

void rewinddir(DIR* directory)
{
    directory->index = 0;
}

int closedir(DIR* directory)
{
    free(directory);
    return 0;
}
