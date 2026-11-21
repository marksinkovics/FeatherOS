#ifndef FEATHEROS_BOOT_OPTIONS_H
#define FEATHEROS_BOOT_OPTIONS_H

#include <stddef.h>
#include <stdint.h>

// Options on the Limine kernel command line, separated by spaces.
namespace FeatherOS::BootOptions
{

// Moves *cursor past the next word, which starts at *word; false at the end.
bool NextWord(const char** cursor, const char** word, size_t* length);
bool MatchesWord(const char* text, size_t length, const char* word);

enum class RootRequest : uint8_t
{
    Absent,
    Partition,
    // A root= that is not vda followed by a partition number from 1.
    Malformed,
};

struct Root
{
    RootRequest request;
    // The partition's number in vdaN, from 1; 0 unless request is Partition.
    size_t partition;
};

// The last root= option wins.
Root ParseRoot(const char* command_line);
// Set once on the boot CPU before any other CPU starts.
void SelectRoot(Root root);
Root SelectedRoot();

enum class UsbMode : uint8_t
{
    Default,
    Off,
    Poll,
};

// usb=off leaves USB alone, usb=poll takes controller events from the timer instead of an
// interrupt. The last usb= wins and a value that is neither counts as absent.
UsbMode ParseUsb(const char* command_line);
void SelectUsb(UsbMode mode);
UsbMode SelectedUsb();

} // namespace FeatherOS::BootOptions

#endif
