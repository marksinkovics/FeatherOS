#include "kernel/boot_options.h"

namespace FeatherOS::BootOptions
{
namespace
{

constexpr char RootOption[] = "root=";
constexpr size_t RootOptionLength = sizeof(RootOption) - 1;
constexpr size_t MaximumPartition = 9999;

constexpr char UsbOption[] = "usb=";
constexpr size_t UsbOptionLength = sizeof(UsbOption) - 1;

constexpr char UsbHidOption[] = "usbhid=";
constexpr size_t UsbHidOptionLength = sizeof(UsbHidOption) - 1;
constexpr char Ps2Option[] = "ps2=";
constexpr size_t Ps2OptionLength = sizeof(Ps2Option) - 1;

constexpr char StorageOption[] = "storage=";
constexpr size_t StorageOptionLength = sizeof(StorageOption) - 1;

constinit Root selected_root = {RootRequest::Absent, {}, 0};
constinit UsbMode selected_usb = UsbMode::Default;
constinit UsbHidMode selected_usb_hid = UsbHidMode::Default;
constinit Ps2Mode selected_ps2 = Ps2Mode::Default;
constinit StorageMode selected_storage = StorageMode::Auto;

bool LastValue(const char* command_line, const char* option, size_t option_length,
               const char** value, size_t* value_length)
{
    bool found = false;
    if (command_line == nullptr)
    {
        return false;
    }
    const char* cursor = command_line;
    const char* word = nullptr;
    size_t length = 0;
    while (NextWord(&cursor, &word, &length))
    {
        if (length >= option_length && MatchesWord(word, option_length, option))
        {
            *value = word + option_length;
            *value_length = length - option_length;
            found = true;
        }
    }
    return found;
}

bool IsDigit(char character)
{
    return character >= '0' && character <= '9';
}

bool IsLetter(char character)
{
    return character >= 'a' && character <= 'z';
}

// Reads a decimal number with no leading zero, "0" alone excepted when zero_allowed.
bool ParseNumber(const char* text, size_t length, size_t* position, size_t limit, size_t* value,
                 bool zero_allowed)
{
    const size_t first = *position;
    size_t number = 0;
    while (*position < length && IsDigit(text[*position]))
    {
        number = number * 10 + static_cast<size_t>(text[*position] - '0');
        if (number > limit)
        {
            return false;
        }
        ++*position;
    }
    const size_t count = *position - first;
    if (count == 0 || (text[first] == '0' && (count > 1 || !zero_allowed)))
    {
        return false;
    }
    *value = number;
    return true;
}

// The disk name at the start of text; sets *position past it, and past the "p" of an NVMe name.
bool ParseDiskName(const char* text, size_t length, size_t* position, char* disk)
{
    size_t name_length = 0;
    if (length >= 3 && (text[0] == 'v' || text[0] == 's') && text[1] == 'd' && IsLetter(text[2]))
    {
        name_length = 3;
        *position = 3;
    }
    else if (length >= 4 && MatchesWord(text, 4, "nvme"))
    {
        size_t cursor = 4;
        size_t number = 0;
        if (!ParseNumber(text, length, &cursor, MaximumPartition, &number, true) ||
            cursor >= length || text[cursor] != 'n')
        {
            return false;
        }
        ++cursor;
        if (!ParseNumber(text, length, &cursor, MaximumPartition, &number, false) ||
            cursor >= length || text[cursor] != 'p')
        {
            return false;
        }
        name_length = cursor;
        *position = cursor + 1;
    }
    else
    {
        return false;
    }
    // The longest name, nvme9999n9999, is 13 characters.
    for (size_t index = 0; index < name_length; ++index)
    {
        disk[index] = text[index];
    }
    disk[name_length] = '\0';
    return true;
}

Root ParseRootWord(const char* word, size_t length)
{
    const char* text = word + RootOptionLength;
    const size_t text_length = length - RootOptionLength;
    Root root = {RootRequest::Malformed, {}, 0};
    char disk[MaxRootDisk] = {};
    size_t position = 0;
    size_t partition = 0;
    if (!ParseDiskName(text, text_length, &position, disk) ||
        !ParseNumber(text, text_length, &position, MaximumPartition, &partition, false) ||
        position != text_length)
    {
        return root;
    }
    root.request = RootRequest::Partition;
    for (size_t index = 0; index < MaxRootDisk; ++index)
    {
        root.disk[index] = disk[index];
    }
    root.partition = partition;
    return root;
}

} // namespace

bool NextWord(const char** cursor, const char** word, size_t* length)
{
    const char* position = *cursor;
    while (*position == ' ')
    {
        ++position;
    }
    if (*position == '\0')
    {
        *cursor = position;
        return false;
    }
    size_t size = 0;
    while (position[size] != '\0' && position[size] != ' ')
    {
        ++size;
    }
    *word = position;
    *length = size;
    *cursor = position + size;
    return true;
}

bool MatchesWord(const char* text, size_t length, const char* word)
{
    size_t index = 0;
    for (; index < length; ++index)
    {
        if (word[index] == '\0' || word[index] != text[index])
        {
            return false;
        }
    }
    return word[index] == '\0';
}

Root ParseRoot(const char* command_line)
{
    Root root = {RootRequest::Absent, {}, 0};
    if (command_line == nullptr)
    {
        return root;
    }
    const char* cursor = command_line;
    const char* word = nullptr;
    size_t length = 0;
    while (NextWord(&cursor, &word, &length))
    {
        if (length >= RootOptionLength && MatchesWord(word, RootOptionLength, RootOption))
        {
            root = ParseRootWord(word, length);
        }
    }
    return root;
}

void SelectRoot(Root root)
{
    selected_root = root;
}

Root SelectedRoot()
{
    return selected_root;
}

UsbMode ParseUsb(const char* command_line)
{
    UsbMode mode = UsbMode::Default;
    if (command_line == nullptr)
    {
        return mode;
    }
    const char* cursor = command_line;
    const char* word = nullptr;
    size_t length = 0;
    while (NextWord(&cursor, &word, &length))
    {
        if (length < UsbOptionLength || !MatchesWord(word, UsbOptionLength, UsbOption))
        {
            continue;
        }
        const char* value = word + UsbOptionLength;
        const size_t value_length = length - UsbOptionLength;
        if (MatchesWord(value, value_length, "off"))
        {
            mode = UsbMode::Off;
        }
        else if (MatchesWord(value, value_length, "poll"))
        {
            mode = UsbMode::Poll;
        }
        else
        {
            mode = UsbMode::Default;
        }
    }
    return mode;
}

void SelectUsb(UsbMode mode)
{
    selected_usb = mode;
}

UsbMode SelectedUsb()
{
    return selected_usb;
}

UsbHidMode ParseUsbHid(const char* command_line)
{
    const char* value = nullptr;
    size_t length = 0;
    if (LastValue(command_line, UsbHidOption, UsbHidOptionLength, &value, &length) &&
        MatchesWord(value, length, "report"))
    {
        return UsbHidMode::Report;
    }
    return UsbHidMode::Default;
}

void SelectUsbHid(UsbHidMode mode)
{
    selected_usb_hid = mode;
}

UsbHidMode SelectedUsbHid()
{
    return selected_usb_hid;
}

Ps2Mode ParsePs2(const char* command_line)
{
    const char* value = nullptr;
    size_t length = 0;
    if (LastValue(command_line, Ps2Option, Ps2OptionLength, &value, &length) &&
        MatchesWord(value, length, "off"))
    {
        return Ps2Mode::Off;
    }
    return Ps2Mode::Default;
}

void SelectPs2(Ps2Mode mode)
{
    selected_ps2 = mode;
}

Ps2Mode SelectedPs2()
{
    return selected_ps2;
}

StorageMode ParseStorage(const char* command_line)
{
    const char* value = nullptr;
    size_t length = 0;
    if (LastValue(command_line, StorageOption, StorageOptionLength, &value, &length))
    {
        if (MatchesWord(value, length, "ro"))
        {
            return StorageMode::ReadOnly;
        }
        if (MatchesWord(value, length, "rw"))
        {
            return StorageMode::ReadWrite;
        }
    }
    return StorageMode::Auto;
}

void SelectStorage(StorageMode mode)
{
    selected_storage = mode;
}

StorageMode SelectedStorage()
{
    return selected_storage;
}

} // namespace FeatherOS::BootOptions
