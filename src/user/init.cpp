#include "command_line.h"
#include "init_config.h"

#include <fcntl.h>
#include <string.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using namespace FeatherOS::User;
using namespace FeatherOS::User::Init;

namespace
{

constexpr char ConfigPath[] = "/system/init.conf";
constexpr char FallbackConfig[] = "console shell\n";
constexpr uint64_t MaximumConfigSize = 2048;
constexpr uint64_t PollMilliseconds = 200;

char config[MaximumConfigSize];
Service services[MaximumServices];
uint32_t service_count = 0;
// Every service starts with the variables from the config's env lines. The block keeps them in
// the kernel's environment format, which also enforces its limits; the entries point into it.
char environment[MaximumEnvironmentSize];
uint64_t environment_used = 0;
uint64_t environment_count = 0;
char* environment_entries[MaximumEnvironmentVariables + 1];

void Report(const char* message, const Service& service)
{
    printf("init: %s%s\n", message, service.command);
}

uint64_t ReadConfig()
{
    const int descriptor = open(ConfigPath, O_RDONLY);
    if (descriptor < 0)
    {
        return 0;
    }
    uint64_t length = 0;
    for (ssize_t count = 1; count > 0 && length < sizeof(config) - 1;
         length += static_cast<uint64_t>(count))
    {
        count = read(descriptor, config + length, sizeof(config) - 1 - length);
        if (count < 0)
        {
            count = 0;
        }
    }
    close(descriptor);
    return length;
}

void IgnoreLine(uint64_t line_number)
{
    printf("init: ignoring line %llu of %s\n", static_cast<unsigned long long>(line_number),
           ConfigPath);
}

void ListEnvironment()
{
    uint64_t count = 0;
    for (uint64_t offset = 0; environment[offset] != '\0';)
    {
        environment_entries[count++] = environment + offset;
        while (environment[offset++] != '\0')
        {
        }
    }
    environment_entries[count] = nullptr;
}

void ParseConfig(const char* text, uint64_t length)
{
    bool has_console = false;
    uint64_t line_number = 1;
    for (uint64_t start = 0; start < length; ++line_number)
    {
        uint64_t end = start;
        while (end < length && text[end] != '\n')
        {
            ++end;
        }
        Service service;
        const LineKind kind = ParseLine(text + start, end - start, &service);
        const bool second_console =
            kind == LineKind::Service && service.policy == Policy::Console && has_console;
        if (kind == LineKind::Variable)
        {
            if (!AppendVariable(environment, sizeof(environment), &environment_used,
                                &environment_count, service.command))
            {
                IgnoreLine(line_number);
            }
        }
        else if (kind == LineKind::Invalid || second_console ||
                 (kind == LineKind::Service && service_count == MaximumServices))
        {
            IgnoreLine(line_number);
        }
        else if (kind == LineKind::Service)
        {
            has_console = has_console || service.policy == Policy::Console;
            services[service_count++] = service;
        }
        start = end + 1;
    }
}

// The console service leads its own session, so Ctrl+C stops at it; respawned services run in
// the background, without the console.
void Start(Service* service)
{
    char command[MaximumCommandLineLength];
    memcpy(command, service->command, sizeof(command));
    char* words[MaximumArgumentCount + 1];
    size_t count = 0;
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, service->policy == Policy::Console ? POSIX_SPAWN_SETSID
                                          : service->policy == Policy::Respawn
                                              ? POSIX_SPAWN_SETPGROUP
                                              : 0);
    pid_t process_id = 0;
    if (SplitCommandLine(command, words, MaximumArgumentCount + 1, &count) != SplitResult::Done ||
        count == 0 ||
        posix_spawnp(&process_id, words[0], nullptr, &attributes, words, environment_entries) != 0)
    {
        Report("cannot start ", *service);
        service->disabled = true;
        return;
    }
    service->process_id = static_cast<uint64_t>(process_id);
    service->running = true;
}

void ReportExit(const Service& service, int status)
{
    if (WIFSIGNALED(status))
    {
        printf("init: %s ended by signal %d\n", service.command, WTERMSIG(status));
        return;
    }
    printf("init: %s exited with status %d\n", service.command, WEXITSTATUS(status));
}

uint64_t MillisecondsSinceBoot()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

} // namespace

constexpr char Help[] = "usage: init\n"
                        "The first process: starts the services in /system/init.conf, with\n"
                        "its env variables, and restarts those that exit. The kernel runs it\n"
                        "at boot.";

int main(int argc, char** argv)
{
    if (HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    const uint64_t length = ReadConfig();
    if (length == 0)
    {
        printf("init: cannot read %s; starting the shell\n", ConfigPath);
        ParseConfig(FallbackConfig, sizeof(FallbackConfig) - 1);
    }
    else
    {
        ParseConfig(config, length);
    }
    ListEnvironment();

    // One-shot services run in order and keep the console while they do.
    for (uint32_t index = 0; index < service_count; ++index)
    {
        Service& service = services[index];
        if (service.policy != Policy::Once)
        {
            continue;
        }
        Start(&service);
        const auto process_id = static_cast<pid_t>(service.process_id);
        int status = 0;
        if (service.running && waitpid(process_id, &status, 0) == process_id && status != 0)
        {
            ReportExit(service, status);
        }
        service.running = false;
    }
    for (uint32_t index = 0; index < service_count; ++index)
    {
        if (services[index].policy != Policy::Once)
        {
            Start(&services[index]);
        }
    }

    for (;;)
    {
        usleep(PollMilliseconds * 1000);
        for (uint32_t index = 0; index < service_count; ++index)
        {
            Service& service = services[index];
            int status = 0;
            if (!service.running ||
                waitpid(static_cast<pid_t>(service.process_id), &status, WNOHANG) == 0)
            {
                continue;
            }
            service.running = false;
            ReportExit(service, status);
            if (AllowRestart(&service, MillisecondsSinceBoot()))
            {
                Report("restarting ", service);
                Start(&service);
            }
            else
            {
                Report("restarted too often; disabled ", service);
                service.disabled = true;
            }
        }
    }
}
