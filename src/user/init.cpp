#include "init_config.h"
#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;
using namespace FeatherOS::User::Init;

namespace
{

constexpr char ConfigPath[] = "/init.conf";
constexpr char FallbackConfig[] = "console shell\n";
constexpr uint64_t MaximumConfigSize = 2048;
constexpr uint64_t PollMilliseconds = 200;

char config[MaximumConfigSize];
Service services[MaximumServices];
uint32_t service_count = 0;

void Report(const char* message, const Service& service)
{
    WriteText("init: ");
    WriteText(message);
    WriteLine(service.command);
}

uint64_t ReadConfig()
{
    const uint64_t descriptor = OpenFile(ConfigPath, OpenRead);
    if (IsSyscallError(descriptor))
    {
        return 0;
    }
    uint64_t length = 0;
    for (uint64_t count = 1; count > 0 && length < sizeof(config) - 1; length += count)
    {
        count = ReadFile(descriptor, config + length, sizeof(config) - 1 - length);
        if (IsSyscallError(count))
        {
            count = 0;
        }
    }
    CloseFile(descriptor);
    return length;
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
        if (kind == LineKind::Invalid || second_console || service_count == MaximumServices)
        {
            if (kind != LineKind::Empty)
            {
                WriteText("init: ignoring line ");
                WriteNumber(line_number);
                WriteText(" of ");
                WriteLine(ConfigPath);
            }
        }
        else if (kind == LineKind::Service)
        {
            has_console = has_console || service.policy == Policy::Console;
            services[service_count++] = service;
        }
        start = end + 1;
    }
}

void Start(Service* service)
{
    const uint64_t flags = service->policy == Policy::Console   ? SpawnSessionLeader
                           : service->policy == Policy::Respawn ? SpawnBackground
                                                                : 0;
    const uint64_t process_id = Spawn(service->command, flags);
    if (IsSyscallError(process_id))
    {
        Report("cannot start ", *service);
        service->disabled = true;
        return;
    }
    service->process_id = process_id;
    service->running = true;
}

void ReportExit(const Service& service, uint64_t status)
{
    WriteText("init: ");
    WriteText(service.command);
    WriteText(" exited with status ");
    WriteNumber(status);
    WriteCharacter('\n');
}

} // namespace

constexpr char Help[] = "usage: init\n"
                        "The first process: starts the services in /init.conf and restarts\n"
                        "those that exit. The kernel runs it at boot.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    const uint64_t length = ReadConfig();
    if (length == 0)
    {
        WriteText("init: cannot read ");
        WriteText(ConfigPath);
        WriteLine("; starting the shell");
        ParseConfig(FallbackConfig, sizeof(FallbackConfig) - 1);
    }
    else
    {
        ParseConfig(config, length);
    }

    // One-shot services run in order and keep the console while they do.
    for (uint32_t index = 0; index < service_count; ++index)
    {
        Service& service = services[index];
        if (service.policy != Policy::Once)
        {
            continue;
        }
        Start(&service);
        uint64_t status = 0;
        if (service.running && !IsSyscallError(Wait(service.process_id, &status)) && status != 0)
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
        Sleep(PollMilliseconds);
        for (uint32_t index = 0; index < service_count; ++index)
        {
            Service& service = services[index];
            uint64_t status = 0;
            if (!service.running ||
                Wait(service.process_id, &status, WaitNoHang) == SyscallErrorWouldBlock)
            {
                continue;
            }
            service.running = false;
            ReportExit(service, status);
            if (AllowRestart(&service, ReadUptime()))
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
