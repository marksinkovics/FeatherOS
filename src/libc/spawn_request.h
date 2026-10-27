#ifndef FEATHEROS_LIBC_SPAWN_REQUEST_H
#define FEATHEROS_LIBC_SPAWN_REQUEST_H

#include "user/process.h"
#include "user/syscall_abi.h"

#include <stddef.h>
#include <stdint.h>

// posix_spawn takes a path and an argv array, but the kernel's Spawn takes one command line
// whose first word is the program: a name in /system, or a path. These build what Spawn takes.
namespace FeatherOS::LibC
{

enum class SpawnRequestResult : uint8_t
{
    Done,
    // ENOENT: the path is empty.
    NotFound,
    // E2BIG: too long, or too many arguments or variables.
    TooBig,
    // EINVAL: an argument holds a double quote, or a variable has no '='.
    Invalid,
};

namespace SpawnRequestDetail
{

constexpr char SystemPrefix[] = "/system/";

class LineWriter
{
public:
    explicit LineWriter(char (&target)[User::MaximumCommandLineLength]);
    bool Put(char character);

private:
    char (&line)[User::MaximumCommandLineLength];
    size_t length = 0;
};

} // namespace SpawnRequestDetail

// argv[0] is not passed on: the kernel always makes the program its first argument. A path in
// /system is sent as the bare name, which leaves more of the line for arguments. Words with
// spaces, or empty ones, are put in double quotes, which the kernel removes.
SpawnRequestResult BuildCommandLine(const char* path, char* const arguments[],
                                    char (&line)[User::MaximumCommandLineLength]);

// Writes the next "<directory>/<file>" from search, a PATH list, and moves search past it. Empty
// directories and candidates too long for a path are skipped. False once the list is done.
bool NextSearchCandidate(const char** search, const char* file,
                         char (&candidate)[User::MaximumPathLength]);

// Packs "NAME=value" strings into Spawn's block: each with its terminator, then an empty string.
SpawnRequestResult BuildEnvironmentBlock(char* const variables[],
                                         char (&block)[User::MaximumEnvironmentSize]);

} // namespace FeatherOS::LibC

#endif
