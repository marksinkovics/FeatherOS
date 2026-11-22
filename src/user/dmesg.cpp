#include "command_line.h"

#include <stdio.h>

constexpr char Help[] = "usage: dmesg\n"
                        "Prints the kernel log, as far back as its 64 KiB ring reaches: the lines\n"
                        "the kernel wrote with a KERN/ prefix, oldest first. Page it with\n"
                        "dmesg | head -n 60 and dmesg | tail -n 60.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 1)
    {
        puts("usage: dmesg");
        return 1;
    }
    FILE* file = fopen("/proc/kmsg", "r");
    if (file == nullptr)
    {
        puts("dmesg: cannot open /proc/kmsg");
        return 1;
    }
    for (int character; (character = fgetc(file)) != EOF;)
    {
        if (putchar(character) == EOF)
        {
            break;
        }
    }
    const bool failed = ferror(file) != 0;
    fclose(file);
    if (failed)
    {
        puts("dmesg: read failed");
        return 1;
    }
    return 0;
}
