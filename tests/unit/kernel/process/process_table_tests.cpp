#include <catch2/catch_test_macros.hpp>

#include "kernel/filesystem/procfs.h"
#include "user/process_table.h"

#include <cstring>
#include <string>

namespace
{

bool OneProcess(uint32_t index, uint64_t* process_id)
{
    *process_id = 7;
    return index == 0;
}

bool DescribeSeven(uint64_t process_id, FeatherOS::Procfs::ProcessStatus* status)
{
    if (process_id != 7)
    {
        return false;
    }
    std::strcpy(status->name, "primes");
    status->state = 'R';
    status->process_id = 7;
    status->parent_id = 3;
    status->group = 7;
    status->pages = 41;
    status->page_limit = 1024;
    status->open_files = 3;
    status->cpu_milliseconds = 61230;
    status->last_cpu = 2;
    status->start_milliseconds = 4000;
    return true;
}

void NoMemory(FeatherOS::Procfs::MemoryStatus* status)
{
    *status = {};
}

void TwoCpus(FeatherOS::Procfs::SystemStatus* status)
{
    status->uptime_milliseconds = 90000;
    status->cpu_count = 2;
    status->busy_milliseconds[0] = 45000;
    status->busy_milliseconds[1] = 9000;
}

std::string Generate(const char* path)
{
    FeatherOS::Procfs::SetSource({OneProcess, DescribeSeven, NoMemory, TwoCpus});
    char text[FeatherOS::Procfs::MaximumFileSize];
    size_t length = 0;
    REQUIRE(FeatherOS::Procfs::Generate(path, text, &length));
    return std::string(text, length);
}

} // namespace

TEST_CASE("ps reads back what /proc/<pid>/status says")
{
    const std::string text = Generate("7/status");
    FeatherOS::User::ProcessInfo info = {};
    REQUIRE(FeatherOS::User::ParseProcessStatus(text.c_str(), &info));
    REQUIRE(std::string(info.name) == "primes");
    REQUIRE(info.state == 'R');
    REQUIRE(info.pid == 7);
    REQUIRE(info.parent == 3);
    REQUIRE(info.group == 7);
    REQUIRE(info.pages == 41);
    REQUIRE(info.cpu_milliseconds == 61230);
    REQUIRE(info.last_cpu == 2);
    REQUIRE(info.start_milliseconds == 4000);
}

TEST_CASE("a status missing a field is refused")
{
    FeatherOS::User::ProcessInfo info = {};
    REQUIRE_FALSE(FeatherOS::User::ParseProcessStatus("Name:\tx\nState:\tR (running)\nPid:\t1\n",
                                                      &info));
    REQUIRE_FALSE(FeatherOS::User::ParseProcessStatus("", &info));
    // A key must match whole, so "Pidx" is not "Pid".
    REQUIRE(FeatherOS::User::ProcessTableDetail::FindValue("Pidx:\t1\nPid:\t2\n", "Pid") !=
            nullptr);
    unsigned long long value = 0;
    REQUIRE(FeatherOS::User::ProcessTableDetail::ReadNumber("Pidx:\t1\nPid:\t2\n", "Pid", &value));
    REQUIRE(value == 2);
    REQUIRE_FALSE(FeatherOS::User::ProcessTableDetail::ReadNumber("Pid:\t2x\n", "Pid", &value));
}

TEST_CASE("top reads back what /proc/stat says")
{
    const std::string text = Generate("stat");
    FeatherOS::User::SystemInfo info = {};
    REQUIRE(FeatherOS::User::ParseSystemStat(text.c_str(), &info));
    REQUIRE(info.uptime_milliseconds == 90000);
    REQUIRE(info.cpus == 2);
    REQUIRE(info.busy_milliseconds[0] == 45000);
    REQUIRE(info.busy_milliseconds[1] == 9000);
    REQUIRE_FALSE(FeatherOS::User::ParseSystemStat("UptimeMs:\t1\nCpus:\t2\nCpu0BusyMs:\t1\n",
                                                   &info));
    REQUIRE_FALSE(FeatherOS::User::ParseSystemStat("UptimeMs:\t1\nCpus:\t0\n", &info));
}

TEST_CASE("CPU time prints as minutes, seconds and hundredths, and usage in tenths of a percent")
{
    char text[24];
    FeatherOS::User::FormatCpuTime(61230, text, sizeof(text));
    REQUIRE(std::string(text) == "1:01.23");
    FeatherOS::User::FormatCpuTime(0, text, sizeof(text));
    REQUIRE(std::string(text) == "0:00.00");
    FeatherOS::User::FormatCpuTime(3600000, text, sizeof(text));
    REQUIRE(std::string(text) == "60:00.00");
    REQUIRE(FeatherOS::User::CpuPermille(500, 1000) == 500);
    REQUIRE(FeatherOS::User::CpuPermille(1000, 1000) == 1000);
    REQUIRE(FeatherOS::User::CpuPermille(1, 3) == 333);
    REQUIRE(FeatherOS::User::CpuPermille(5, 0) == 0);
}
