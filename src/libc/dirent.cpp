#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static_assert(DT_DIR == FeatherOS::User::ModeDirectory >> FeatherOS::User::EntryTypeShift &&
              DT_REG == FeatherOS::User::ModeRegular >> FeatherOS::User::EntryTypeShift &&
              DT_LNK == FeatherOS::User::ModeSymbolicLink >> FeatherOS::User::EntryTypeShift &&
              DT_CHR == FeatherOS::User::ModeCharacterDevice >> FeatherOS::User::EntryTypeShift &&
              DT_BLK == FeatherOS::User::ModeBlockDevice >> FeatherOS::User::EntryTypeShift &&
              DT_FIFO == FeatherOS::User::ModeFifo >> FeatherOS::User::EntryTypeShift &&
              DT_SOCK == FeatherOS::User::ModeSocket >> FeatherOS::User::EntryTypeShift);

// The path is resolved to its physical form when the directory is opened, so a later chdir, or a
// symbolic link changed on the way, does not change which directory the stream reads.
struct FeatherDirectory
{
    char path[PATH_MAX];
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
    if (realpath(path, directory->path) == nullptr)
    {
        free(directory);
        return nullptr;
    }
    return directory;
}

struct dirent* readdir(DIR* directory)
{
    FeatherOS::User::DirectoryEntry entry{};
    const uint64_t result =
        FeatherOS::User::ReadDirectory(directory->path, directory->index, &entry);
    static_assert(sizeof(directory->entry.d_name) == sizeof(entry.name) &&
                  PATH_MAX == FeatherOS::User::MaximumPathLength &&
                  NAME_MAX == FeatherOS::User::MaximumNameLength);
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
    directory->entry.d_ino = static_cast<ino_t>(entry.node);
    directory->entry.d_type = static_cast<unsigned char>(entry.type);
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
