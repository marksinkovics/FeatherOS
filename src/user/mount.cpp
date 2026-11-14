#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

constexpr char Usage[] = "usage: mount [-t <type> <source> <target>]";
constexpr char Help[] = "usage: mount [-t <type> <source> <target>]\n"
                        "Without arguments, lists the mounts from /proc/mounts. With -t, mounts\n"
                        "the filesystem of that type named source at target, such as\n"
                        "mount -t tmpfs tmp /tmp.";

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
    if (argc != 5 || strcmp(argv[1], "-t") != 0)
    {
        fprintf(stderr, "%s\n", Usage);
        return 1;
    }
    const char* type = argv[2];
    const char* source = argv[3];
    const char* target = argv[4];
    if (mount(source, target, type, 0, nullptr) == 0)
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
