#ifndef FEATHEROS_LIBC_SPAWN_REQUEST_H
#define FEATHEROS_LIBC_SPAWN_REQUEST_H

#include "user/process.h"
#include "user/syscall_abi.h"

#include <stddef.h>
#include <stdint.h>

// posix_spawn takes a path and an argv array, but the kernel's Spawn takes one command line
// whose first word names a program in /system. These build what Spawn takes.
namespace FeatherOS::LibC
{

enum class SpawnRequestResult : uint8_t
{
    Done,
    // ENOENT: the path does not name a program in /system.
    NotFound,
    // E2BIG: too long, or too many arguments or variables.
    TooBig,
    // EINVAL: an argument holds a double quote, or a variable has no '='.
    Invalid,
};

namespace SpawnRequestDetail
{

constexpr char SystemPrefix[] = "/system/";

bool IsNameCharacter(char character);

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

// argv[0] is not passed on: the kernel always makes the program's name its first argument.
// Arguments with spaces, or empty ones, are put in double quotes, which the kernel removes.
SpawnRequestResult BuildCommandLine(const char* path, char* const arguments[],
                                    char (&line)[User::MaximumCommandLineLength]);

// Packs "NAME=value" strings into Spawn's block: each with its terminator, then an empty string.
SpawnRequestResult BuildEnvironmentBlock(char* const variables[],
                                         char (&block)[User::MaximumEnvironmentSize]);

} // namespace FeatherOS::LibC

#endif
