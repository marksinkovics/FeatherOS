#ifndef FEATHEROS_WRITEBACK_H
#define FEATHEROS_WRITEBACK_H

#include <stdint.h>

namespace FeatherOS::Writeback
{

constexpr uint32_t IntervalSeconds = 5;

// Starts the kernel task that syncs writable ext2, ext3 and ext4 mounts every IntervalSeconds.
// Call it once, after the timer runs and after every kernel test that uses the VFS without the
// file system lock.
void Start();

} // namespace FeatherOS::Writeback

#endif
