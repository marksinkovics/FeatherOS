#ifndef FEATHEROS_FILESYSTEM_PATH_H
#define FEATHEROS_FILESYSTEM_PATH_H

#include "user/syscall_abi.h"

#include <stdint.h>

namespace FeatherOS::Path
{

// Including the terminator.
constexpr uint32_t MaximumLength = User::MaximumPathLength;

// Takes an absolute path as it is, or joins a relative one to working_directory, which must be
// physical (absolute, without links, ".", ".." or repeated '/', as the VFS walk leaves it). The
// relative path's leading "." and ".." components apply to working_directory, whose textual parent
// is its real one; the rest stays as given, since only the walk can tell where a ".." after a
// symbolic link leads. Fails if the result does not fit.
bool Join(const char* working_directory, const char* path, char (&result)[MaximumLength]);

// Rewrites an absolute "/proc/self" path to name the process, as "/proc/7/status". Paths
// elsewhere are left alone. Fails if the result does not fit.
bool ReplaceProcSelf(char (&path)[MaximumLength], uint64_t process_id);

} // namespace FeatherOS::Path

#endif
