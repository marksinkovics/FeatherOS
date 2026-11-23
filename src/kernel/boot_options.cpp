#include "kernel/boot_options.h"

namespace FeatherOS::BootOptions
{
namespace
{

constexpr char RootOption[] = "root=";
constexpr size_t RootOptionLength = sizeof(RootOption) - 1;
constexpr char RootPrefix[] = "root=vda";
constexpr size_t RootPrefixLength = sizeof(RootPrefix) - 1;
constexpr size_t MaximumPartition = 9999;

constexpr char UsbOption[] = "usb=";
constexpr size_t UsbOptionLength = sizeof(UsbOption) - 1;

constexpr char UsbHidOption[] = "usbhid=";
constexpr size_t UsbHidOptionLength = sizeof(UsbHidOption) - 1;
constexpr char Ps2Option[] = "ps2=";
constexpr size_t Ps2OptionLength = sizeof(Ps2Option) - 1;

constexpr char StorageOption[] = "storage=";
constexpr size_t StorageOptionLength = sizeof(StorageOption) - 1;

constinit Root selected_root = {RootRequest::Absent, 0};
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

Root ParseRootWord(const char* word, size_t length)
{
    const char* digits = word + RootPrefixLength;
    const size_t count = length - RootPrefixLength;
    if (count == 0 || digits[0] == '0')
    {
        return {RootRequest::Malformed, 0};
    }
    size_t number = 0;
    for (size_t index = 0; index < count; ++index)
    {
        if (digits[index] < '0' || digits[index] > '9')
        {
            return {RootRequest::Malformed, 0};
        }
        number = number * 10 + static_cast<size_t>(digits[index] - '0');
        if (number > MaximumPartition)
        {
            return {RootRequest::Malformed, 0};
        }
    }
    return {RootRequest::Partition, number};
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
    Root root = {RootRequest::Absent, 0};
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
            root = length >= RootPrefixLength && MatchesWord(word, RootPrefixLength, RootPrefix)
                       ? ParseRootWord(word, length)
                       : Root{RootRequest::Malformed, 0};
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
