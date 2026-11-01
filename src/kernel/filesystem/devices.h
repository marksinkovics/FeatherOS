#ifndef FEATHEROS_FILESYSTEM_DEVICES_H
#define FEATHEROS_FILESYSTEM_DEVICES_H

#include "kernel/filesystem/fat16.h"

#include <stdint.h>

namespace FeatherOS::Terminals
{
class Terminal;
} // namespace FeatherOS::Terminals

// The character devices mounted at /dev. Every device accepts and discards writes, and has no
// size, so seeking does nothing.
namespace FeatherOS::Devices
{

enum class Device : uint8_t
{
    // Reads end at once.
    Null,
    // Reads return zero bytes.
    Zero,
    // Reads return random bytes, from RDRAND or RNDR when the CPU has it.
    Random,
    // "tty": Files opens the caller's controlling terminal in its place.
    ControllingTerminal,
    // "ptmx": Files creates a pseudo-terminal pair and opens its master.
    PseudoTerminalMaster,
    // "pts/N": Files opens the slave of pair N, listed while the pair is in use.
    PseudoTerminalSlave,
    // "ttyN": the terminal of virtual console N, from 1, listed once SetConsoles named it.
    Console,
};

void SetConsoles(Terminals::Terminal* const* terminals, uint32_t count);
// path is relative to the mount, such as "null" or "pts/0". node receives the device and, for a
// slave or a console, its number, as the VFS keeps them.
bool Find(const char* path, uint32_t* node);
Device DeviceOf(uint32_t node);
uint32_t PairOf(uint32_t node);
Terminals::Terminal* ConsoleOf(uint32_t node);
// "" and "pts".
bool IsDirectory(const char* path);
void Read(Device device, void* buffer, uint64_t length, uint64_t* bytes_read);
bool ReadDirectory(const char* path, uint32_t index, FAT16::DirectoryEntry* entry);

} // namespace FeatherOS::Devices

#endif
