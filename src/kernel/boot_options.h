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

enum class UsbHidMode : uint8_t
{
    Default,
    Report,
};

// usbhid=report drives boot keyboards and mice through their report descriptors instead of the
// fixed boot layouts. The last usbhid= wins and a value that is not report counts as absent.
UsbHidMode ParseUsbHid(const char* command_line);
void SelectUsbHid(UsbHidMode mode);
UsbHidMode SelectedUsbHid();

enum class Ps2Mode : uint8_t
{
    Default,
    Off,
};

// ps2=off leaves the keyboard that is not on USB (PS/2 on x86-64, virtio-input on AArch64)
// uninitialized. The last ps2= wins and a value that is not off counts as absent.
Ps2Mode ParsePs2(const char* command_line);
void SelectPs2(Ps2Mode mode);
Ps2Mode SelectedPs2();

enum class StorageMode : uint8_t
{
    Auto,
    ReadOnly,
    ReadWrite,
};

// storage=ro makes every NVMe and AHCI disk read-only, storage=rw writable; without either only
// QEMU's controllers are writable. The last storage= wins and any other value counts as absent.
StorageMode ParseStorage(const char* command_line);
void SelectStorage(StorageMode mode);
StorageMode SelectedStorage();

} // namespace FeatherOS::BootOptions

#endif
