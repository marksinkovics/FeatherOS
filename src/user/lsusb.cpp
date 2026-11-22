#include "command_line.h"

#include <stdio.h>
#include <string.h>

constexpr char Help[] =
    "usage: lsusb\n"
    "Lists the USB devices: path, device number, speed, vendor:product, the\n"
    "class/subclass/protocol of the first interface and the driver bound to it.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 1)
    {
        puts("usage: lsusb");
        return 1;
    }
    FILE* file = fopen("/proc/usb", "r");
    if (file == nullptr)
    {
        puts("lsusb: cannot open /proc/usb");
        return 1;
    }
    for (int character; (character = fgetc(file)) != EOF;)
    {
        putchar(character);
    }
    fclose(file);
    return 0;
}
