#include "command_line.h"
#include "init_config.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using namespace FeatherOS::User;
using namespace FeatherOS::User::Init;

namespace
{

constexpr char BuiltInConfigPath[] = "/system/init.conf";
constexpr char ServiceDirectory[] = "/etc/init.d";
constexpr char DisabledPath[] = "/etc/init.disabled";
constexpr char RetiredConfigPath[] = "/etc/init.conf";
constexpr char FallbackConfig[] = "console /dev/tty1 shell\n";
constexpr uint64_t MaximumConfigSize = 2048;
constexpr uint32_t MaximumServiceFiles = 16;
constexpr uint64_t ServiceFileNameCapacity = 64;
constexpr uint64_t PollMilliseconds = 200;

const char* current_path = BuiltInConfigPath;
char config_text[MaximumConfigSize];
Configuration configuration;
// Every service starts with the variables from the env lines. The entries point into the
// configuration's environment block.
char* environment_entries[MaximumEnvironmentVariables + 1];

void Report(const char* message, const Service& service)
{
    printf("init: %s%s\n", message, service.command);
}

enum class ReadResult : uint8_t
{
    Done,
    Missing,
    TooLarge,
};

ReadResult ReadConfig(const char* path, uint64_t* length_out)
{
    const int descriptor = open(path, O_RDONLY);
    if (descriptor < 0)
    {
        return ReadResult::Missing;
    }
    uint64_t length = 0;
    for (ssize_t count = 1; count > 0 && length < sizeof(config_text) - 1;
         length += static_cast<uint64_t>(count))
    {
        count = read(descriptor, config_text + length, sizeof(config_text) - 1 - length);
        if (count < 0)
        {
            count = 0;
        }
    }
    char extra = 0;
    const bool too_large = read(descriptor, &extra, 1) > 0;
    close(descriptor);
    *length_out = length;
    return too_large ? ReadResult::TooLarge : ReadResult::Done;
}

void IgnoreLine(void* /*context*/, uint64_t line_number)
{
    printf("init: %s: ignoring line %llu\n", current_path,
           static_cast<unsigned long long>(line_number));
}

void ReportDisabled(void* /*context*/, const Service& service)
{
    Report("/etc/init.disabled: not starting ", service);
}

const Reporter reporter{IgnoreLine, ReportDisabled, nullptr};

void ListEnvironment()
{
    uint64_t count = 0;
    char* const environment = configuration.environment;
    for (uint64_t offset = 0; environment[offset] != '\0';)
    {
        environment_entries[count++] = environment + offset;
        while (environment[offset++] != '\0')
        {
        }
    }
    environment_entries[count] = nullptr;
}

bool LoadBuiltIn()
{
    current_path = BuiltInConfigPath;
    uint64_t length = 0;
    const ReadResult result = ReadConfig(BuiltInConfigPath, &length);
    if (result == ReadResult::TooLarge)
    {
        printf("init: %s: larger than %llu bytes\n", BuiltInConfigPath,
               static_cast<unsigned long long>(MaximumConfigSize - 1));
        return false;
    }
    AddBuiltIn(&configuration, config_text, length, reporter);
    return configuration.service_count != 0;
}

void LoadDisabled()
{
    current_path = DisabledPath;
    uint64_t length = 0;
    const ReadResult result = ReadConfig(DisabledPath, &length);
    if (result == ReadResult::TooLarge)
    {
        printf("init: %s: larger than %llu bytes; ignored\n", DisabledPath,
               static_cast<unsigned long long>(MaximumConfigSize - 1));
    }
    else if (result == ReadResult::Done)
    {
        DisableBuiltIns(&configuration, config_text, length, reporter);
    }
}

void LoadServiceFile(const char* name)
{
    char path[sizeof(ServiceDirectory) + ServiceFileNameCapacity];
    snprintf(path, sizeof(path), "%s/%s", ServiceDirectory, name);
    current_path = path;
    uint64_t length = 0;
    const ReadResult read_result = ReadConfig(path, &length);
    if (read_result == ReadResult::Missing)
    {
        printf("init: %s: cannot read it\n", path);
        return;
    }
    if (read_result == ReadResult::TooLarge)
    {
        printf("init: %s: larger than %llu bytes; skipped\n", path,
               static_cast<unsigned long long>(MaximumConfigSize - 1));
        return;
    }
    switch (AddServiceFile(&configuration, config_text, length, reporter))
    {
    case FileResult::Merged:
        break;
    case FileResult::Invalid:
        printf("init: %s: does not parse; skipped\n", path);
        break;
    case FileResult::TerminalTaken:
        printf("init: %s: a terminal is already held by another service; skipped\n", path);
        break;
    case FileResult::NoRoom:
        printf("init: %s: too many services or variables; skipped\n", path);
        break;
    }
}

void LoadServiceFiles()
{
    DIR* directory = opendir(ServiceDirectory);
    if (directory == nullptr)
    {
        return;
    }
    char names[MaximumServiceFiles][ServiceFileNameCapacity];
    uint32_t count = 0;
    while (const dirent* entry = readdir(directory))
    {
        if (entry->d_name[0] == '.' || entry->d_type == DT_DIR)
        {
            continue;
        }
        if (strlen(entry->d_name) >= ServiceFileNameCapacity || count == MaximumServiceFiles)
        {
            printf("init: %s/%s: too many files or a name too long; skipped\n", ServiceDirectory,
                   entry->d_name);
            continue;
        }
        uint32_t position = count++;
        for (; position != 0 && strcmp(names[position - 1], entry->d_name) > 0; --position)
        {
            strcpy(names[position], names[position - 1]);
        }
        strcpy(names[position], entry->d_name);
    }
    closedir(directory);
    for (uint32_t index = 0; index < count; ++index)
    {
        LoadServiceFile(names[index]);
    }
}

bool HasVariable(const char* name)
{
    const size_t length = strlen(name);
    const char* const environment = configuration.environment;
    for (uint64_t offset = 0; environment[offset] != '\0';)
    {
        if (strncmp(environment + offset, name, length) == 0 && environment[offset + length] == '=')
        {
            return true;
        }
        offset += strlen(environment + offset) + 1;
    }
    return false;
}

// As login does: the home directory when there is one, else the root. A HOME line in the
// configuration wins.
void SetHome()
{
    if (HasVariable("HOME"))
    {
        return;
    }
    struct stat status{};
    const bool has_home = stat("/home", &status) == 0 && S_ISDIR(status.st_mode);
    AppendVariable(configuration.environment, sizeof(configuration.environment),
                   &configuration.environment_used, &configuration.environment_count,
                   has_home ? "HOME=/home" : "HOME=/");
}

// A console service leads its own session on its terminal, so Ctrl+C stops at it; respawned
// services run in the background, without a terminal.
void Start(Service* service)
{
    char command[MaximumCommandLineLength];
    memcpy(command, service->command, sizeof(command));
    char* words[MaximumArgumentCount + 1];
    size_t count = 0;
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(
        &attributes, static_cast<short>(service->policy == Policy::Console ? POSIX_SPAWN_SETSID
                                        : service->policy == Policy::Respawn
                                            ? POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_BACKGROUND
                                            : 0));
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    if (service->policy == Policy::Console)
    {
        posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO, service->terminal, O_RDWR, 0);
        posix_spawn_file_actions_adddup2(&file_actions, STDIN_FILENO, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&file_actions, STDIN_FILENO, STDERR_FILENO);
    }
    pid_t process_id = 0;
    const bool started =
        SplitCommandLine(command, words, MaximumArgumentCount + 1, &count) == SplitResult::Done &&
        count != 0 &&
        posix_spawnp(&process_id, words[0], &file_actions, &attributes, words,
                     environment_entries) == 0;
    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attributes);
    if (!started)
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
                        "The first process: starts the services of the built-in\n"
                        "/system/init.conf and of each file in /etc/init.d/ (in name order, one\n"
                        "replacing the built-in service that runs the same program), minus the\n"
                        "built-in programs named in /etc/init.disabled, with their env\n"
                        "variables (HOME is /home when it exists, else /, unless a file sets\n"
                        "it), and restarts those that exit. The kernel runs it at boot.";

int main(int argc, char** argv)
{
    if (HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (!LoadBuiltIn())
    {
        printf("init: cannot use %s; starting the shell\n", BuiltInConfigPath);
        AddBuiltIn(&configuration, FallbackConfig, sizeof(FallbackConfig) - 1, reporter);
    }
    LoadDisabled();
    LoadServiceFiles();
    struct stat retired{};
    if (stat(RetiredConfigPath, &retired) == 0)
    {
        printf("init: %s is not read any more; put services in %s/\n", RetiredConfigPath,
               ServiceDirectory);
    }
    SetHome();
    ListEnvironment();

    // One-shot services run in order and keep the console while they do.
    for (uint32_t index = 0; index < configuration.service_count; ++index)
    {
        Service& service = configuration.services[index];
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
    for (uint32_t index = 0; index < configuration.service_count; ++index)
    {
        if (configuration.services[index].policy != Policy::Once)
        {
            Start(&configuration.services[index]);
        }
    }

    for (;;)
    {
        usleep(PollMilliseconds * 1000);
        for (uint32_t index = 0; index < configuration.service_count; ++index)
        {
            Service& service = configuration.services[index];
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
