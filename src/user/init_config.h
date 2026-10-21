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

inline bool Matches(const char* text, uint64_t length, const char* word)
{
    uint64_t index = 0;
    for (; index < length && word[index] != '\0'; ++index)
    {
        if (text[index] != word[index])
        {
            return false;
        }
    }
    return index == length && word[index] == '\0';
}

// Parses line[0, length), which holds no newline.
inline LineKind ParseLine(const char* line, uint64_t length, Service* service)
{
    uint64_t start = 0;
    while (start < length && (line[start] == ' ' || line[start] == '\t' || line[start] == '\r'))
    {
        ++start;
    }
    while (length > start &&
           (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r'))
    {
        --length;
    }
    if (start == length || line[start] == '#')
    {
        return LineKind::Empty;
    }
    uint64_t word_end = start;
    while (word_end < length && line[word_end] != ' ' && line[word_end] != '\t')
    {
        ++word_end;
    }
    const char* word = line + start;
    const uint64_t word_length = word_end - start;
    const bool variable = Matches(word, word_length, "env");
    if (variable)
    {
        service->policy = Policy::Once;
    }
    else if (Matches(word, word_length, "once"))
    {
        service->policy = Policy::Once;
    }
    else if (Matches(word, word_length, "respawn"))
    {
        service->policy = Policy::Respawn;
    }
    else if (Matches(word, word_length, "console"))
    {
        service->policy = Policy::Console;
    }
    else
    {
        return LineKind::Invalid;
    }
    uint64_t command_start = word_end;
    while (command_start < length && (line[command_start] == ' ' || line[command_start] == '\t'))
    {
        ++command_start;
    }
    const uint64_t command_length = length - command_start;
    if (command_length == 0 || command_length >= MaximumCommandLineLength)
    {
        return LineKind::Invalid;
    }
    if (variable)
    {
        // The name must be non-empty and the line a single NAME=value word.
        uint64_t separator = command_start;
        while (separator < length && line[separator] != '=' && line[separator] != ' ' &&
               line[separator] != '\t')
        {
            ++separator;
        }
        if (separator == command_start || separator == length || line[separator] != '=')
        {
            return LineKind::Invalid;
        }
        for (uint64_t index = separator; index < length; ++index)
        {
            if (line[index] == ' ' || line[index] == '\t')
            {
                return LineKind::Invalid;
            }
        }
    }
    for (uint64_t index = 0; index < command_length; ++index)
    {
        service->command[index] = line[command_start + index];
    }
    service->command[command_length] = '\0';
    service->process_id = 0;
    service->window_start = 0;
    service->restarts = 0;
    service->running = false;
    service->disabled = false;
    return variable ? LineKind::Variable : LineKind::Service;
}

// Appends entry to an environment block of capacity bytes holding used bytes before its final
// terminator. False if it does not fit.
inline bool AppendVariable(char* block, uint64_t capacity, uint64_t* used, uint64_t* count,
                           const char* entry)
{
    uint64_t length = 0;
    while (entry[length] != '\0')
    {
        ++length;
    }
    if (*count == MaximumEnvironmentVariables || *used + length + 2 > capacity)
    {
        return false;
    }
    for (uint64_t index = 0; index <= length; ++index)
    {
        block[*used + index] = entry[index];
    }
    *used += length + 1;
    block[*used] = '\0';
    ++*count;
    return true;
}

// Counts a restart at now (milliseconds since boot); false once the service exits too often.
inline bool AllowRestart(Service* service, uint64_t now)
{
    if (service->restarts == 0 || now - service->window_start >= RestartWindowMilliseconds)
    {
        service->window_start = now;
        service->restarts = 0;
    }
    return ++service->restarts <= MaximumRestarts;
}

} // namespace FeatherOS::User::Init

#endif
