#include "command_line.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

constexpr char Help[] = "usage: ls [-l] [directory]\n"
                        "Lists a directory (the current one by default) with file sizes in\n"
                        "bytes; subdirectories end in /. With -l, each line has the type and\n"
                        "permissions, the link count, owner, group, size and modification time\n"
                        "(UTC) before the name. /system holds the read-only boot files.";

namespace
{

char TypeCharacter(mode_t mode)
{
    switch (mode & S_IFMT)
    {
    case S_IFREG:
        return '-';
    case S_IFDIR:
        return 'd';
    case S_IFLNK:
        return 'l';
    case S_IFCHR:
        return 'c';
    case S_IFBLK:
        return 'b';
    case S_IFIFO:
        return 'p';
    case S_IFSOCK:
        return 's';
    default:
        return '?';
    }
}

mode_t TypeOfEntry(unsigned char type)
{
    switch (type)
    {
    case DT_REG:
        return S_IFREG;
    case DT_DIR:
        return S_IFDIR;
    case DT_LNK:
        return S_IFLNK;
    case DT_CHR:
        return S_IFCHR;
    case DT_BLK:
        return S_IFBLK;
    case DT_FIFO:
        return S_IFIFO;
    case DT_SOCK:
        return S_IFSOCK;
    default:
        return 0;
    }
}

// As ls -l writes them: set-user-ID, set-group-ID and sticky bits take the execute places, in
// lower case when the execute bit is also set.
void ModeText(mode_t mode, char (&text)[11])
{
    const char* const letters = "rwxrwxrwx";
    text[0] = TypeCharacter(mode);
    for (int bit = 0; bit < 9; ++bit)
    {
        text[1 + bit] = (mode & (0400U >> bit)) != 0 ? letters[bit] : '-';
    }
    const struct
    {
        mode_t flag;
        int place;
        char letter;
    } specials[] = {{S_ISUID, 3, 's'}, {S_ISGID, 6, 's'}, {S_ISVTX, 9, 't'}};
    for (const auto& special : specials)
    {
        if ((mode & special.flag) != 0)
        {
            text[special.place] = text[special.place] == '-'
                                      ? static_cast<char>(special.letter - 32)
                                      : special.letter;
        }
    }
    text[10] = '\0';
}

void PrintLong(const dirent& entry, const struct stat* status)
{
    if (status == nullptr)
    {
        char mode[11];
        ModeText(TypeOfEntry(entry.d_type), mode);
        printf("%c????????? %3s %5s %5s %10s %16s %s\n", mode[0], "?", "?", "?", "?", "?",
               entry.d_name);
        return;
    }
    char mode[11];
    ModeText(status->st_mode, mode);
    char modified[24] = "?";
    tm time = {};
    if (gmtime_r(&status->st_mtime, &time) != nullptr)
    {
        strftime(modified, sizeof(modified), "%Y-%m-%d %H:%M", &time);
    }
    printf("%s %3lu %5u %5u %10lld %16s %s\n", mode, static_cast<unsigned long>(status->st_nlink),
           status->st_uid, status->st_gid, static_cast<long long>(status->st_size), modified,
           entry.d_name);
}

void PrintShort(const dirent& entry, bool is_directory, long long size)
{
    constexpr int NameColumnWidth = 24;
    const int length = static_cast<int>(strlen(entry.d_name)) + (is_directory ? 1 : 0);
    printf("%s%s%*s", entry.d_name, is_directory ? "/" : "",
           length < NameColumnWidth ? NameColumnWidth - length : 1, "");
    if (is_directory)
    {
        puts("<dir>");
        return;
    }
    printf("%lld\n", size);
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    const bool long_format = argc > 1 && strcmp(argv[1], "-l") == 0;
    const int first_operand = long_format ? 2 : 1;
    if (argc > first_operand + 1)
    {
        puts("usage: ls [-l] [directory]");
        return 1;
    }
    const char* name = argc > first_operand ? argv[first_operand] : ".";
    DIR* directory = opendir(name);
    // Entries are looked up from inside the directory, so their paths stay short.
    if (directory == nullptr || chdir(name) != 0)
    {
        printf("ls: %s: no such directory\n", name);
        return 1;
    }
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
        struct stat status;
        // The listing already says what is a directory, so only -l needs them stat'ed.
        const bool have_status =
            (long_format || !is_directory) && stat(entry->d_name, &status) == 0;
        const long long size = have_status && !is_directory ? status.st_size : 0;
        if (long_format)
        {
            PrintLong(*entry, have_status ? &status : nullptr);
        }
        else
        {
            PrintShort(*entry, is_directory, size);
        }
        if (is_directory)
        {
            ++directories;
            continue;
        }
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
