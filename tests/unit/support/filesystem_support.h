#ifndef FEATHEROS_TESTS_FILESYSTEM_SUPPORT_H
#define FEATHEROS_TESTS_FILESYSTEM_SUPPORT_H

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The host disk and its images, which filesystem_tests.cpp owns, for the tests that run tasks on
// them. The disk's own hooks are read by every thread and set only while one thread runs.
namespace FilesystemSupport
{

using Sector = std::array<uint8_t, 512>;

struct TwoExt4Volumes
{
    uint32_t first_start;
    uint32_t first_end;
    uint32_t second_start;
    uint32_t second_end;
};

// FAT16 as vda1 and two copies of the ext4 test image as vda3 and vda4, mounted at "/a" and "/b"
// after MountBlockDevice; false without the image.
bool AttachTwoExt4Volumes(TwoExt4Volumes* volumes);
// Runs before each sector write, on the writing thread; null removes it.
void SetWriteHook(std::function<void(uint64_t sector)> hook);
std::vector<Sector> CopyRange(uint32_t start, uint32_t end);
bool NeedsRecovery(uint32_t start);
// The contents of name in the root of the volume copy as a fresh core mounts it for writing, or
// "<missing>".
std::string ReadFromCopy(const std::vector<Sector>& volume, const char* name);

} // namespace FilesystemSupport

#endif
