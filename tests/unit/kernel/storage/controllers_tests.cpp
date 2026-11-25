#include <catch2/catch_test_macros.hpp>

#include "kernel/storage/disk.h"
#include "kernel/storage/probe_wait.h"
#include "kernel/storage/worker.h"
#include "storage_rig.h"

#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

// A controller's bring-up is posted to the storage worker before the root is chosen, and the boot
// task waits for every one of them, up to a deadline.
namespace
{

namespace Storage = FeatherOS::Storage;
namespace ProbeWait = FeatherOS::Storage::ProbeWait;
using Storage::Testing::PumpEnvironment;

constexpr uint64_t Timeout = 60'000;

class FakeNvmeDisk final : public Storage::DiskDriver
{
public:
    Storage::DiskInfo Info() override
    {
        Storage::DiskInfo info = {};
        info.kind = Storage::DiskKind::Nvme;
        info.sector_size = 512;
        info.sector_count = 8;
        info.max_transfer_bytes = 4096;
        info.controller_index = 0;
        info.namespace_id = 1;
        return info;
    }

    bool Gone() override
    {
        return false;
    }

    FeatherDisk::Status Read(uint64_t, uint32_t, uint8_t*) override
    {
        return {};
    }

    FeatherDisk::Status Write(uint64_t, uint32_t, const uint8_t*) override
    {
        return {};
    }

    FeatherDisk::Status Flush() override
    {
        return {};
    }

    FeatherDisk::Status Stop() override
    {
        return std::unexpected(FeatherDisk::Error::Unsupported);
    }

    void Release() override {}
};

std::vector<std::string> scanned;

void OnScan(Storage::Disk& disk)
{
    scanned.emplace_back(disk.Name());
}

// What a controller's job does: register its disks, ask for their scan, then end.
struct Probe
{
    PumpEnvironment* environment;
    Storage::DiskRegistry* registry;
    FakeNvmeDisk* disk;
    Storage::Disk* registered;
};

void ProbeJob(void* context)
{
    auto* probe = static_cast<Probe*>(context);
    if (probe->disk != nullptr)
    {
        probe->registered = probe->registry->Register(*probe->disk);
        probe->registry->Scan(*probe->registered);
    }
    ProbeWait::End(*probe->environment, probe);
}

void RunWorker()
{
    while (FeatherOS::Storage::Worker::RunNext())
    {
    }
}

struct Fixture
{
    Fixture()
    {
        ProbeWait::Reset();
        RunWorker();
        registry.Initialize();
        registry.SetScanHandler(OnScan);
        scanned.clear();
    }

    ~Fixture()
    {
        registry.SetScanHandler(nullptr);
        RunWorker();
        ProbeWait::Reset();
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    Storage::DiskRegistry registry;
};

} // namespace

TEST_CASE("T35: waiting with no controller pending returns at once")
{
    Fixture fixture;
    PumpEnvironment environment;
    const uint64_t start = environment.NowMilliseconds();
    char late[64] = "unchanged";

    CHECK(ProbeWait::Wait(environment, start + Timeout, late, sizeof(late)));
    CHECK(environment.NowMilliseconds() == start);
    CHECK(std::strcmp(late, "unchanged") == 0);
}

TEST_CASE("T35: the wait returns when the pending count reaches zero, not before")
{
    Fixture fixture;
    PumpEnvironment environment([] { FeatherOS::Storage::Worker::RunNext(); });
    FakeNvmeDisk first_disk;
    Probe first{&environment, &fixture.registry, &first_disk, nullptr};
    Probe second{&environment, &fixture.registry, nullptr, nullptr};
    fixture.registry.DeferScans(true);

    REQUIRE(ProbeWait::Begin(&first, "nvme0"));
    REQUIRE(ProbeWait::Begin(&second, "ahci0"));
    CHECK(ProbeWait::Pending() == 2);
    REQUIRE(Storage::Worker::Post(ProbeJob, &first));
    REQUIRE(Storage::Worker::Post(ProbeJob, &second));

    const uint64_t start = environment.NowMilliseconds();
    char late[64] = {};
    CHECK(ProbeWait::Wait(environment, start + Timeout, late, sizeof(late)));
    CHECK(ProbeWait::Pending() == 0);
    CHECK(environment.NowMilliseconds() < start + Timeout);
    CHECK(environment.Wakes() == 2);
    // The disk is registered when the wait returns, so the root choice can see it.
    REQUIRE(first.registered != nullptr);
    CHECK(fixture.registry.Acquire("nvme0n1") == first.registered);
    first.registered->Release();

    // Registered during the deferral: not scanned until it ends.
    CHECK(scanned.empty());
    fixture.registry.DeferScans(false);
    RunWorker();
    CHECK(scanned == std::vector<std::string>{"nvme0n1"});

    fixture.registry.Remove(*first.registered);
}

TEST_CASE("T35: a probe that is not done at the deadline is named, and registers late")
{
    Fixture fixture;
    // Nothing runs the worker while the boot task waits: the controller hangs.
    PumpEnvironment environment;
    FakeNvmeDisk late_disk;
    Probe stuck{&environment, &fixture.registry, &late_disk, nullptr};
    Probe quick{&environment, &fixture.registry, nullptr, nullptr};
    fixture.registry.DeferScans(true);

    REQUIRE(ProbeWait::Begin(&stuck, "nvme0"));
    REQUIRE(ProbeWait::Begin(&quick, "ahci0"));
    ProbeJob(&quick);

    const uint64_t start = environment.NowMilliseconds();
    char late[64] = {};
    CHECK_FALSE(ProbeWait::Wait(environment, start + Timeout, late, sizeof(late)));
    CHECK(environment.NowMilliseconds() == start + Timeout);
    CHECK(std::string(late) == "nvme0");
    CHECK(ProbeWait::Pending() == 1);
    CHECK(fixture.registry.Acquire("nvme0n1") == nullptr);

    // The root is chosen without it, and the scans are released.
    fixture.registry.DeferScans(false);
    CHECK(scanned.empty());

    REQUIRE(Storage::Worker::Post(ProbeJob, &stuck));
    RunWorker();
    CHECK(ProbeWait::Pending() == 0);
    REQUIRE(stuck.registered != nullptr);
    CHECK(std::string(stuck.registered->Name()) == "nvme0n1");
    // Scanned at once, as an other disk: the boot disk is already chosen.
    CHECK(scanned == std::vector<std::string>{"nvme0n1"});

    fixture.registry.Remove(*stuck.registered);
}

TEST_CASE("T35: every late controller is named")
{
    Fixture fixture;
    PumpEnvironment environment;
    int first = 0;
    int second = 0;

    REQUIRE(ProbeWait::Begin(&first, "nvme0"));
    REQUIRE(ProbeWait::Begin(&second, "ahci0"));
    char late[64] = {};
    CHECK_FALSE(ProbeWait::Wait(environment, environment.NowMilliseconds() + 100, late,
                                sizeof(late)));
    CHECK(std::string(late) == "nvme0, ahci0");

    char small[8] = {};
    CHECK_FALSE(ProbeWait::Wait(environment, environment.NowMilliseconds(), small, sizeof(small)));
    CHECK(std::string(small) == "nvme0, ");

    ProbeWait::End(environment, &first);
    ProbeWait::End(environment, &second);
    // An unknown or repeated key changes nothing.
    ProbeWait::End(environment, &second);
    ProbeWait::End(environment, nullptr);
    CHECK(ProbeWait::Pending() == 0);
}

TEST_CASE("T35: the table of pending probes is bounded")
{
    Fixture fixture;
    PumpEnvironment environment;
    std::vector<int> keys(ProbeWait::MaxPending + 1);

    for (uint32_t index = 0; index < ProbeWait::MaxPending; ++index)
    {
        REQUIRE(ProbeWait::Begin(&keys[index], "nvme0"));
    }
    CHECK_FALSE(ProbeWait::Begin(&keys[ProbeWait::MaxPending], "nvme0"));
    CHECK(ProbeWait::Pending() == ProbeWait::MaxPending);
    for (uint32_t index = 0; index < ProbeWait::MaxPending; ++index)
    {
        ProbeWait::End(environment, &keys[index]);
    }
    CHECK(ProbeWait::Pending() == 0);
}

TEST_CASE("T35: no controller wait is allowed before the timer runs")
{
    CHECK_FALSE(ProbeWait::MayWait(false));
    CHECK(ProbeWait::MayWait(true));
}
