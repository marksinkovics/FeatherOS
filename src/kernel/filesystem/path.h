#ifndef FEATHEROS_FILESYSTEM_PATH_H
#define FEATHEROS_FILESYSTEM_PATH_H

#include <stdint.h>

namespace FeatherOS::Path
{

// Including the terminator; matches User::MaximumPathLength.
constexpr uint32_t MaximumLength = 64;

// Joins a relative path to working_directory, or takes an absolute one as it is, then collapses
// ".", "..", and repeated '/' lexically ("/home/.." is "/"). Fails if the result does not fit.
bool Resolve(const char* working_directory, const char* path, char (&result)[MaximumLength]);

// Rewrites an absolute "/proc/self" path to name the process, as "/proc/7/status". Paths
// elsewhere are left alone. Fails if the result does not fit.
bool ReplaceProcSelf(char (&path)[MaximumLength], uint64_t process_id);

} // namespace FeatherOS::Path

#endif
