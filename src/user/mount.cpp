#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

constexpr char Usage[] = "usage: mount [-r] [-o <options>] [-t <type> <source> <target>]\n"
                         "       mount -o remount,ro|remount,rw <target>";
constexpr char Help[] = "usage: mount [-r] [-o <options>] [-t <type> <source> <target>]\n"
                        "       mount -o remount,ro|remount,rw <target>\n"
                        "Without arguments, lists the mounts from /proc/mounts. With -t, mounts\n"
                        "the filesystem of that type named source at target, such as\n"
                        "mount -t tmpfs tmp /tmp, read-write unless -r or -o ro asks for\n"
                        "read-only. -o takes a comma list of ro, rw and remount (the last of ro\n"
                        "and rw wins); remount changes the mode of the mount at target and needs\n"
                        "ro or rw, such as mount -o remount,ro /.";

int ListMounts()
{
    const int descriptor = open("/proc/mounts", O_RDONLY);
    if (descriptor < 0)
    {
        fprintf(stderr, "mount: /proc/mounts: %s\n", strerror(errno));
        return 1;
    }
    char buffer[256];
    ssize_t count;
    while ((count = read(descriptor, buffer, sizeof(buffer))) > 0)
    {
        fwrite(buffer, 1, static_cast<size_t>(count), stdout);
    }
    close(descriptor);
    return count < 0 ? 1 : 0;
}

bool ParseOptions(const char* list, unsigned long* flags, bool* remount, bool* mode_given)
{
    char word[16];
    for (const char* cursor = list;;)
    {
        size_t length = 0;
        while (cursor[length] != ',' && cursor[length] != '\0')
        {
            length += 1;
        }
        if (length == 0 || length >= sizeof(word))
        {
            return false;
        }
        memcpy(word, cursor, length);
        word[length] = '\0';
        if (strcmp(word, "ro") == 0 || strcmp(word, "rw") == 0)
        {
            *flags = (*flags & ~static_cast<unsigned long>(MS_RDONLY)) |
                     (word[1] == 'o' ? MS_RDONLY : 0UL);
            *mode_given = true;
        }
        else if (strcmp(word, "remount") == 0)
        {
            *remount = true;
        }
        else
        {
            return false;
        }
        if (cursor[length] == '\0')
        {
            return true;
        }
        cursor += length + 1;
    }
}

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc == 1)
    {
        return ListMounts();
    }
    unsigned long flags = 0;
    bool remount = false;
    bool mode_given = false;
    int first = 1;
    for (bool options = true; options && first < argc;)
    {
        if (strcmp(argv[first], "-r") == 0)
        {
            flags |= MS_RDONLY;
            mode_given = true;
            first += 1;
        }
        else if (strcmp(argv[first], "-o") == 0 && first + 1 < argc &&
                 ParseOptions(argv[first + 1], &flags, &remount, &mode_given))
        {
            first += 2;
        }
        else
        {
            options = false;
        }
    }
    if (remount)
    {
        if (!mode_given || argc - first != 1)
        {
            fprintf(stderr, "%s\n", Usage);
            return 1;
        }
        const char* target = argv[first];
        if (mount(nullptr, target, nullptr, flags | MS_REMOUNT, nullptr) == 0)
        {
            return 0;
        }
        fprintf(stderr, "mount: %s: %s\n", target, strerror(errno));
        return 1;
    }
    if (argc - first != 4 || strcmp(argv[first], "-t") != 0)
    {
        fprintf(stderr, "%s\n", Usage);
        return 1;
    }
    const char* type = argv[first + 1];
    const char* source = argv[first + 2];
    const char* target = argv[first + 3];
    if (mount(source, target, type, flags, nullptr) == 0)
    {
        return 0;
    }
    if (errno == ENODEV)
    {
        fprintf(stderr, "mount: %s: unknown filesystem type\n", type);
    }
    else
    {
        fprintf(stderr, "mount: %s on %s: %s\n", source, target, strerror(errno));
    }
    return 1;
}
