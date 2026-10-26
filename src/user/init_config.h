#ifndef FEATHEROS_USER_INIT_CONFIG_H
#define FEATHEROS_USER_INIT_CONFIG_H

#include "process.h"

#include <stdint.h>

// /system/init.conf: one service per line as "<policy> <command line>", or "env NAME=value" for a
// variable in every service's environment. '#' starts a comment line.
namespace FeatherOS::User::Init
{

enum class Policy : uint8_t
{
    // Runs to completion before the next line starts.
    Once,
    // Runs in the background and restarts when it exits.
    Respawn,
    // Holds the console as a session leader and restarts when it exits; at most one.
    Console,
};

constexpr uint32_t MaximumServices = 8;
// A service that exits this often within the window is disabled instead of restarted.
constexpr uint32_t MaximumRestarts = 5;
constexpr uint64_t RestartWindowMilliseconds = 10000;

struct Service
{
    Policy policy;
    char command[MaximumCommandLineLength];
    uint64_t process_id;
    uint64_t window_start;
    uint32_t restarts;
    bool running;
    bool disabled;
};

enum class LineKind : uint8_t
{
    Empty,
    Service,
    // An env line; service->command holds its "NAME=value".
    Variable,
    Invalid,
};

bool Matches(const char* text, uint64_t length, const char* word);

// Parses line[0, length), which holds no newline.
LineKind ParseLine(const char* line, uint64_t length, Service* service);

// Appends entry to an environment block of capacity bytes holding used bytes before its final
// terminator. False if it does not fit.
bool AppendVariable(char* block, uint64_t capacity, uint64_t* used, uint64_t* count,
                    const char* entry);

// Counts a restart at now (milliseconds since boot); false once the service exits too often.
bool AllowRestart(Service* service, uint64_t now);

} // namespace FeatherOS::User::Init

#endif
