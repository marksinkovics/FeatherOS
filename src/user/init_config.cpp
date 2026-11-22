#include "init_config.h"

#include <stdint.h>

namespace FeatherOS::User::Init
{
namespace
{

uint64_t SkipBlanks(const char* line, uint64_t start, uint64_t length)
{
    while (start < length && (line[start] == ' ' || line[start] == '\t'))
    {
        ++start;
    }
    return start;
}

uint64_t Length(const char* text)
{
    uint64_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

bool Equal(const char* left, const char* right)
{
    uint64_t index = 0;
    while (left[index] != '\0' && left[index] == right[index])
    {
        ++index;
    }
    return left[index] == right[index];
}

bool NextLine(const char* text, uint64_t length, uint64_t* start, uint64_t* end)
{
    if (*start >= length)
    {
        return false;
    }
    *end = *start;
    while (*end < length && text[*end] != '\n')
    {
        ++*end;
    }
    return true;
}

bool IsConsole(const Service& service)
{
    return service.policy == Policy::Console;
}

bool SameService(const Service& left, const Service& right)
{
    if (IsConsole(left) != IsConsole(right))
    {
        return false;
    }
    if (IsConsole(left))
    {
        return Equal(left.terminal, right.terminal);
    }
    char left_name[MaximumCommandLineLength];
    char right_name[MaximumCommandLineLength];
    ProgramName(left, left_name, sizeof(left_name));
    ProgramName(right, right_name, sizeof(right_name));
    return Equal(left_name, right_name);
}

bool HoldsTerminal(const Configuration& config, const char* terminal, bool built_in)
{
    for (uint32_t index = 0; index < config.service_count; ++index)
    {
        const Service& held = config.services[index];
        if (IsConsole(held) && held.built_in == built_in && Equal(held.terminal, terminal))
        {
            return true;
        }
    }
    return false;
}

void RemoveService(Configuration* config, uint32_t index)
{
    for (; index + 1 < config->service_count; ++index)
    {
        config->services[index] = config->services[index + 1];
    }
    --config->service_count;
}

bool Disables(const Service& service, const char* name)
{
    if (name[0] == '/')
    {
        return IsConsole(service) && Equal(service.terminal, name);
    }
    char program[MaximumCommandLineLength];
    ProgramName(service, program, sizeof(program));
    return Equal(program, name);
}

} // namespace

bool Matches(const char* text, uint64_t length, const char* word)
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

LineKind ParseLine(const char* line, uint64_t length, Service* service)
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
    uint64_t command_start = SkipBlanks(line, word_end, length);
    service->terminal[0] = '\0';
    if (service->policy == Policy::Console)
    {
        uint64_t terminal_end = command_start;
        while (terminal_end < length && line[terminal_end] != ' ' && line[terminal_end] != '\t')
        {
            ++terminal_end;
        }
        const uint64_t terminal_length = terminal_end - command_start;
        if (terminal_length == 0 || line[command_start] != '/' ||
            terminal_length >= MaximumTerminalPathLength)
        {
            return LineKind::Invalid;
        }
        for (uint64_t index = 0; index < terminal_length; ++index)
        {
            service->terminal[index] = line[command_start + index];
        }
        service->terminal[terminal_length] = '\0';
        command_start = SkipBlanks(line, terminal_end, length);
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
    service->built_in = false;
    return variable ? LineKind::Variable : LineKind::Service;
}

bool AppendVariable(char* block, uint64_t capacity, uint64_t* used, uint64_t* count,
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

bool SetVariable(char* block, uint64_t capacity, uint64_t* used, uint64_t* count, const char* entry)
{
    uint64_t name_length = 0;
    while (entry[name_length] != '\0' && entry[name_length] != '=')
    {
        ++name_length;
    }
    for (uint64_t offset = 0; block[offset] != '\0';)
    {
        const uint64_t size = Length(block + offset) + 1;
        bool same = true;
        for (uint64_t index = 0; same && index <= name_length; ++index)
        {
            same = block[offset + index] == entry[index];
        }
        if (same)
        {
            if (*used - size + Length(entry) + 2 > capacity)
            {
                return false;
            }
            for (uint64_t index = offset; index + size <= *used; ++index)
            {
                block[index] = block[index + size];
            }
            *used -= size;
            block[*used] = '\0';
            --*count;
            break;
        }
        offset += size;
    }
    return AppendVariable(block, capacity, used, count, entry);
}

void ProgramName(const Service& service, char* name, uint64_t capacity)
{
    uint64_t end = 0;
    while (service.command[end] != '\0' && service.command[end] != ' ' &&
           service.command[end] != '\t')
    {
        ++end;
    }
    uint64_t start = end;
    while (start != 0 && service.command[start - 1] != '/')
    {
        --start;
    }
    uint64_t length = end - start;
    if (length >= capacity)
    {
        length = capacity - 1;
    }
    for (uint64_t index = 0; index < length; ++index)
    {
        name[index] = service.command[start + index];
    }
    name[length] = '\0';
}

void AddBuiltIn(Configuration* config, const char* text, uint64_t length, const Reporter& reporter)
{
    uint64_t line_number = 0;
    uint64_t end = 0;
    for (uint64_t start = 0; NextLine(text, length, &start, &end); start = end + 1)
    {
        ++line_number;
        Service service;
        const LineKind kind = ParseLine(text + start, end - start, &service);
        bool added = kind == LineKind::Empty;
        if (kind == LineKind::Variable)
        {
            added =
                SetVariable(config->environment, sizeof(config->environment),
                            &config->environment_used, &config->environment_count, service.command);
        }
        else if (kind == LineKind::Service && config->service_count != MaximumServices &&
                 !(IsConsole(service) && HoldsTerminal(*config, service.terminal, true)))
        {
            service.built_in = true;
            config->services[config->service_count++] = service;
            added = true;
        }
        if (!added && reporter.ignored_line != nullptr)
        {
            reporter.ignored_line(reporter.context, line_number);
        }
    }
}

uint32_t DisableBuiltIns(Configuration* config, const char* text, uint64_t length,
                         const Reporter& reporter)
{
    uint32_t removed = 0;
    uint64_t end = 0;
    for (uint64_t start = 0; NextLine(text, length, &start, &end); start = end + 1)
    {
        uint64_t first = start;
        uint64_t last = end;
        while (first < last && (text[first] == ' ' || text[first] == '\t'))
        {
            ++first;
        }
        while (last > first &&
               (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r'))
        {
            --last;
        }
        if (first == last || text[first] == '#' || last - first >= MaximumCommandLineLength)
        {
            continue;
        }
        char name[MaximumCommandLineLength];
        for (uint64_t index = first; index < last; ++index)
        {
            name[index - first] = text[index];
        }
        name[last - first] = '\0';
        for (uint32_t index = 0; index < config->service_count;)
        {
            if (config->services[index].built_in && Disables(config->services[index], name))
            {
                if (reporter.disabled != nullptr)
                {
                    reporter.disabled(reporter.context, config->services[index]);
                }
                RemoveService(config, index);
                ++removed;
            }
            else
            {
                ++index;
            }
        }
    }
    return removed;
}

bool EnsureConsole(Configuration* config, const Reporter& reporter)
{
    for (uint32_t index = 0; index < config->service_count; ++index)
    {
        if (IsConsole(config->services[index]))
        {
            return false;
        }
    }
    constexpr char Fallback[] = "console /dev/tty1 shell\n";
    const uint32_t before = config->service_count;
    AddBuiltIn(config, Fallback, sizeof(Fallback) - 1, reporter);
    return config->service_count != before;
}

FileResult AddServiceFile(Configuration* config, const char* text, uint64_t length,
                          const Reporter& reporter)
{
    Configuration file{};
    FileResult result = FileResult::Merged;
    uint64_t line_number = 0;
    uint64_t end = 0;
    for (uint64_t start = 0; NextLine(text, length, &start, &end); start = end + 1)
    {
        ++line_number;
        Service service;
        const LineKind kind = ParseLine(text + start, end - start, &service);
        bool accepted = kind == LineKind::Empty;
        if (kind == LineKind::Variable)
        {
            accepted =
                SetVariable(file.environment, sizeof(file.environment), &file.environment_used,
                            &file.environment_count, service.command);
        }
        else if (kind == LineKind::Service && file.service_count != MaximumServices &&
                 !(IsConsole(service) && HoldsTerminal(file, service.terminal, false)))
        {
            file.services[file.service_count++] = service;
            accepted = true;
        }
        if (!accepted)
        {
            result = FileResult::Invalid;
            if (reporter.ignored_line != nullptr)
            {
                reporter.ignored_line(reporter.context, line_number);
            }
        }
    }
    if (result != FileResult::Merged)
    {
        return result;
    }

    char environment[MaximumEnvironmentSize];
    for (uint64_t index = 0; index < sizeof(environment); ++index)
    {
        environment[index] = config->environment[index];
    }
    uint64_t used = config->environment_used;
    uint64_t count = config->environment_count;
    for (uint64_t offset = 0; file.environment[offset] != '\0';
         offset += Length(file.environment + offset) + 1)
    {
        if (!SetVariable(environment, sizeof(environment), &used, &count,
                         file.environment + offset))
        {
            return FileResult::NoRoom;
        }
    }

    uint32_t replaced = 0;
    for (uint32_t index = 0; index < config->service_count; ++index)
    {
        bool replaces = false;
        for (uint32_t other = 0; other < file.service_count; ++other)
        {
            replaces = replaces || SameService(config->services[index], file.services[other]);
        }
        replaced += config->services[index].built_in && replaces ? 1 : 0;
    }
    for (uint32_t index = 0; index < file.service_count; ++index)
    {
        if (IsConsole(file.services[index]) &&
            HoldsTerminal(*config, file.services[index].terminal, false))
        {
            return FileResult::TerminalTaken;
        }
    }
    if (config->service_count - replaced + file.service_count > MaximumServices)
    {
        return FileResult::NoRoom;
    }

    for (uint32_t index = 0; index < config->service_count;)
    {
        bool replaces = false;
        for (uint32_t other = 0; other < file.service_count; ++other)
        {
            replaces = replaces || SameService(config->services[index], file.services[other]);
        }
        if (config->services[index].built_in && replaces)
        {
            RemoveService(config, index);
        }
        else
        {
            ++index;
        }
    }
    for (uint32_t index = 0; index < file.service_count; ++index)
    {
        config->services[config->service_count++] = file.services[index];
    }
    for (uint64_t index = 0; index <= used; ++index)
    {
        config->environment[index] = environment[index];
    }
    config->environment_used = used;
    config->environment_count = count;
    return FileResult::Merged;
}

bool AllowRestart(Service* service, uint64_t now)
{
    if (service->restarts == 0 || now - service->window_start >= RestartWindowMilliseconds)
    {
        service->window_start = now;
        service->restarts = 0;
    }
    return ++service->restarts <= MaximumRestarts;
}

} // namespace FeatherOS::User::Init
