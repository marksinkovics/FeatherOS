#ifndef FEATHEROS_USER_INIT_CONFIG_H
#define FEATHEROS_USER_INIT_CONFIG_H

#include "process.h"

#include <stdint.h>

// The built-in /system/init.conf and each file of /etc/init.d/: one service per line as "<policy>
// <command line>", "console <terminal> <command line>", or "env NAME=value" for a variable in every
// service's environment. '#' starts a comment line. /etc/init.disabled names built-in services not
// to start.
namespace FeatherOS::User::Init
{

enum class Policy : uint8_t
{
    // Runs to completion before the next line starts.
    Once,
    // Runs in the background and restarts when it exits.
    Respawn,
    // Holds its terminal as a session leader and restarts when it exits; one per terminal.
    Console,
};

constexpr uint32_t MaximumServices = 16;
constexpr uint64_t MaximumTerminalPathLength = 32;
// A service that exits this often within the window is disabled instead of restarted.
constexpr uint32_t MaximumRestarts = 5;
constexpr uint64_t RestartWindowMilliseconds = 10000;

struct Service
{
    Policy policy;
    char command[MaximumCommandLineLength];
    // A console service's terminal, such as /dev/tty1; empty for the others.
    char terminal[MaximumTerminalPathLength];
    uint64_t process_id;
    uint64_t window_start;
    uint32_t restarts;
    bool running;
    bool disabled;
    // Comes from /system/init.conf, so /etc/init.disabled and a same-named /etc/init.d service can
    // remove it.
    bool built_in;
};

struct Configuration
{
    Service services[MaximumServices];
    uint32_t service_count;
    // The kernel's environment format, which also enforces its limits.
    char environment[MaximumEnvironmentSize];
    uint64_t environment_used;
    uint64_t environment_count;
};

struct Reporter
{
    void (*ignored_line)(void* context, uint64_t line);
    void (*disabled)(void* context, const Service& service);
    void* context;
};

enum class FileResult : uint8_t
{
    Merged,
    // A line did not parse; the file contributes nothing.
    Invalid,
    // A console service names a terminal another /etc/init.d service holds.
    TerminalTaken,
    // More services or variables than fit.
    NoRoom,
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

// Replaces NAME's earlier value if there is one; false, leaving the block as it was, if it does
// not fit.
bool SetVariable(char* block, uint64_t capacity, uint64_t* used, uint64_t* count,
                 const char* entry);

// The program a service runs: the first word of its command without its directory.
void ProgramName(const Service& service, char* name, uint64_t capacity);

// Adds the built-in services and variables of text one line at a time; a line that does not parse,
// repeats a console's terminal or does not fit is reported and skipped.
void AddBuiltIn(Configuration* config, const char* text, uint64_t length, const Reporter& reporter);

// Removes each built-in service named by a line of text (a program name, or a terminal such as
// /dev/tty2 for a console), reporting it. Returns how many were removed.
uint32_t DisableBuiltIns(Configuration* config, const char* text, uint64_t length,
                         const Reporter& reporter);

// Adds the services and variables of one /etc/init.d file in order, as a whole or not at all. A
// service replaces the built-in ones with the same program (a console: the same terminal); the
// variables replace earlier ones of the same name.
FileResult AddServiceFile(Configuration* config, const char* text, uint64_t length,
                          const Reporter& reporter);

// Counts a restart at now (milliseconds since boot); false once the service exits too often.
bool AllowRestart(Service* service, uint64_t now);

} // namespace FeatherOS::User::Init

#endif
