#include <catch2/catch_test_macros.hpp>

#include "fake_device.h"
#include "recording_host.h"
#include "rig.h"

#include "support/scheduler_stubs.h"

#include "kernel/log_ring.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/worker.h"
#include "kernel/usb/device.h"
#include "kernel/usb/hub.h"
#include "kernel/usb/storage/drive.h"
#include "kernel/usb/storage/driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace T = FeatherOS::Usb;
namespace S = FeatherOS::Usb::Storage;
namespace Worker = FeatherOS::Storage::Worker;
using FeatherDisk::Error;
using T::Speed;
using T::Status;
using T::Testing::MakeDisk;
using T::Testing::RecordingHost;
using T::Testing::Rig;
using T::Testing::ScriptedMassStorage;

using Disk = std::shared_ptr<ScriptedMassStorage>;
using Fault = ScriptedMassStorage::Fault;
using Reply = ScriptedMassStorage::Reply;
using Stage = ScriptedMassStorage::Stage;
using Model = ScriptedMassStorage::Model;

constexpr uint8_t OpInquiry = 0x12;
constexpr uint8_t OpTestUnitReady = 0x00;
constexpr uint8_t OpRequestSense = 0x03;
constexpr uint8_t OpModeSense = 0x1A;
constexpr uint8_t OpReadCapacity10 = 0x25;
constexpr uint8_t OpServiceIn16 = 0x9E;
constexpr uint8_t OpRead10 = 0x28;

const T::Driver* const Drivers[] = {&T::HubDriver, &S::MassStorageDriver};

size_t LogLines(const std::string& needle)
{
    static char text[FeatherOS::KernelLog::Capacity];
    const size_t length = FeatherOS::KernelLog::Snapshot(text, sizeof(text));
    const std::string all(text, length);
    size_t count = 0;
    for (size_t at = all.find(needle); at != std::string::npos; at = all.find(needle, at + 1))
    {
        ++count;
    }
    return count;
}

// Runs the controller's and the manager's work by hand and moves a clock that only the waits of
// the drive move: when nothing is ready, time passes until the deadline.
class PumpEnvironment final : public S::Environment
{
public:
    explicit PumpEnvironment(Rig& rig) : rig_(rig) {}

    uint64_t NowMilliseconds() override
    {
        return now_;
    }

    bool WaitUntil(const void*, uint64_t deadline_ms, bool (*ready)(void*), void* context) override
    {
        for (;;)
        {
            if (ready(context))
            {
                return true;
            }
            if (now_ >= deadline_ms)
            {
                return false;
            }
            rig_.Settle();
            if (ready(context))
            {
                return true;
            }
            Advance(std::min<uint64_t>(deadline_ms - now_, Step));
        }
    }

    // The controller's clock moves with the drive's, as the kernel's single clock does.
    void Advance(uint64_t milliseconds)
    {
        now_ += milliseconds;
        rig_.hardware.Pause(static_cast<uint32_t>(milliseconds));
    }

    void Wake(const void*) override {}

    void Sleep(uint32_t milliseconds) override
    {
        Advance(milliseconds);
        rig_.Settle();
    }

    static constexpr uint64_t Step = 10;

private:
    Rig& rig_;
    uint64_t now_ = 1'000'000;
};

S::Drive* TestDrive()
{
    FeatherOS::Storage::Disk* disk = FeatherOS::Storage::Acquire("sda");
    if (disk == nullptr)
    {
        return nullptr;
    }
    auto* drive = static_cast<S::Drive*>(&disk->Driver());
    drive->Retain();
    disk->Release();
    return drive;
}

struct Held
{
    explicit Held(S::Drive* held = nullptr) : drive(held) {}
    ~Held()
    {
        if (drive != nullptr)
        {
            drive->Release();
        }
    }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

    S::Drive* operator->() const
    {
        return drive;
    }

    S::Drive* drive;
};

class World
{
public:
    World() : env(rig)
    {
        FeatherOS::Storage::Initialize();
        rig.drivers = Drivers;
        rig.wrap_host = [this](T::HostController& inner) -> T::HostController&
        {
            recording = std::make_unique<RecordingHost>(inner);
            return *recording;
        };
        REQUIRE(rig.Create());
        S::SetEnvironment(&env);
        while (Worker::RunNext())
        {
        }
    }

    ~World()
    {
        for (uint32_t port : plugged)
        {
            rig.fake.Disconnect(port);
        }
        rig.Settle();
        while (Worker::RunNext())
        {
        }
        S::SetEnvironment(nullptr);
    }

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    uint32_t Plug(uint32_t port, const Disk& disk)
    {
        plugged.push_back(port);
        rig.fake.Connect(port, Speed::High, true, disk);
        rig.Settle();
        return rig.manager->SlotOfPort(port);
    }

    S::Drive* DriveOf(uint32_t slot)
    {
        const T::Device* device = rig.manager->Find(slot);
        return device == nullptr ? nullptr : static_cast<S::Drive*>(device->driver_state[0]);
    }

    // Plugs a disk and runs its probe.
    S::Drive* Attach(const Disk& disk, uint32_t port = 1)
    {
        const uint32_t slot = Plug(port, disk);
        REQUIRE(slot != 0);
        S::Drive* drive = DriveOf(slot);
        REQUIRE(drive != nullptr);
        REQUIRE(Worker::RunNext());
        return drive;
    }

    std::vector<std::string> Recorded() const
    {
        return recording->Log();
    }

    std::unique_ptr<RecordingHost> recording;
    Rig rig;
    PumpEnvironment env;
    std::vector<uint32_t> plugged;
};

Disk MakeDefaultDisk(Model model = {})
{
    return MakeDisk(Speed::High, std::move(model));
}

std::vector<uint8_t> Opcodes(const Disk& disk)
{
    std::vector<uint8_t> opcodes;
    for (const auto& command : disk->Commands())
    {
        opcodes.push_back(command.opcode);
    }
    return opcodes;
}

size_t CountOpcode(const Disk& disk, uint8_t opcode)
{
    const std::vector<uint8_t> opcodes = Opcodes(disk);
    return static_cast<size_t>(std::count(opcodes.begin(), opcodes.end(), opcode));
}

} // namespace

TEST_CASE("usb-storage T13: a healthy drive is probed in order and published",
          "[usb][usbstorage]")
{
    World world;
    const Disk disk = MakeDefaultDisk();
    const size_t before = LogLines("Storage: sda: USB 1-1, \"QEMU\" \"QEMU HARDDISK\", 1024 KiB, ");
    world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    const FeatherOS::Storage::DiskInfo info = drive->Info();
    REQUIRE(info.kind == FeatherOS::Storage::DiskKind::Usb);
    REQUIRE(info.sector_size == 512);
    REQUIRE(info.sector_count == 2048);
    REQUIRE(info.max_transfer_bytes == 65536);
    REQUIRE(info.removable);
    REQUIRE_FALSE(info.read_only);
    REQUIRE(std::string(info.model) == "QEMU QEMU HARDDISK");
    REQUIRE(std::string(info.location) == "USB 1-1");
    REQUIRE(Opcodes(disk) == std::vector<uint8_t>{OpInquiry, OpTestUnitReady, OpReadCapacity10,
                                                  OpModeSense});
    REQUIRE(LogLines("Storage: sda: USB 1-1, \"QEMU\" \"QEMU HARDDISK\", 1024 KiB, 512-byte sectors, "
                     "removable") == before + 1);
}

TEST_CASE("usb-storage I14: attach sends no BOT command; the probe waits for the worker",
          "[usb][usbstorage]")
{
    World world;
    const Disk disk = MakeDefaultDisk();
    // A drive that never answers must not stall the manager's attach.
    disk->FailStage(Stage::Command, Fault::Hang, 1);
    const uint32_t slot = world.Plug(1, disk);
    REQUIRE(slot != 0);
    REQUIRE(world.DriveOf(slot) != nullptr);
    REQUIRE(disk->Commands().empty());
    REQUIRE(disk->BulkReceived(0x02).empty());
    REQUIRE(TestDrive() == nullptr);
    REQUIRE(Worker::RunNext());
    REQUIRE_FALSE(Worker::RunNext());
}

TEST_CASE("usb-storage T13: GET MAX LUN stalls, answers 0 and 3, and only LUN 0 is used",
          "[usb][usbstorage]")
{
    SECTION("a stall")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        disk->SetMaxLun(0, true);
        const size_t before = LogLines("LUNs, only LUN 0 is used");
        world.Attach(disk);
        REQUIRE(LogLines("LUNs, only LUN 0 is used") == before);
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
    }
    SECTION("three")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        disk->SetMaxLun(3);
        const size_t before = LogLines("Storage: USB 1-1: 4 LUNs, only LUN 0 is used");
        world.Attach(disk);
        REQUIRE(LogLines("Storage: USB 1-1: 4 LUNs, only LUN 0 is used") == before + 1);
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
        std::vector<uint8_t> out(512);
        REQUIRE(drive->Read(0, 1, out.data()));
        size_t cbws = 0;
        for (const auto& bytes : disk->BulkReceived(0x02))
        {
            if (bytes.size() == 31)
            {
                ++cbws;
                REQUIRE(bytes[13] == 0);
            }
        }
        REQUIRE(cbws >= 5);
    }
    SECTION("above fifteen")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        disk->SetMaxLun(200);
        const size_t before = LogLines("LUNs, only LUN 0 is used");
        world.Attach(disk);
        REQUIRE(LogLines("LUNs, only LUN 0 is used") == before);
    }
}

TEST_CASE("usb-storage T13: NOT READY is waited out within the probe budget", "[usb][usbstorage]")
{
    SECTION("three times")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        disk->NotReady(3);
        const uint64_t start = world.env.NowMilliseconds();
        world.Attach(disk);
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
        REQUIRE(CountOpcode(disk, OpTestUnitReady) == 4);
        REQUIRE(CountOpcode(disk, OpRequestSense) == 3);
        REQUIRE(world.env.NowMilliseconds() - start >= 300);
    }
    SECTION("forever")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        disk->NotReady(100000);
        const size_t before = LogLines("Storage: USB 1-1: did not become ready; ignored");
        const uint64_t start = world.env.NowMilliseconds();
        world.Attach(disk);
        REQUIRE(TestDrive() == nullptr);
        REQUIRE(LogLines("Storage: USB 1-1: did not become ready; ignored") == before + 1);
        const uint64_t spent = world.env.NowMilliseconds() - start;
        REQUIRE(spent >= 9900);
        REQUIRE(spent < 11000);
        REQUIRE(CountOpcode(disk, OpReadCapacity10) == 0);
    }
}

TEST_CASE("usb-storage T13: UNIT ATTENTION after a power-on or reset is retried",
          "[usb][usbstorage]")
{
    SECTION("power on")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        disk->UnitAttention(0x29);
        world.Attach(disk);
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
    }
    SECTION("medium may have changed, at probe")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        disk->UnitAttention(0x28);
        world.Attach(disk);
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
    }
    SECTION("more than three in a row give up")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        disk->UnitAttention(0x29, 10);
        world.Attach(disk);
        REQUIRE(TestDrive() == nullptr);
    }
}

TEST_CASE("usb-storage T13: no medium and other device types are not registered",
          "[usb][usbstorage]")
{
    SECTION("no medium")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        disk->NoMedium(true);
        const size_t before = LogLines("Storage: USB 1-1: no medium; ignored");
        world.Attach(disk);
        REQUIRE(TestDrive() == nullptr);
        REQUIRE(LogLines("Storage: USB 1-1: no medium; ignored") == before + 1);
        REQUIRE(CountOpcode(disk, OpReadCapacity10) == 0);
    }
    SECTION("a CD-ROM")
    {
        World world;
        Model model;
        model.type = 5;
        const Disk disk = MakeDefaultDisk(model);
        const size_t before = LogLines("Storage: USB 1-1: device type 05 is not a disk; ignored");
        world.Attach(disk);
        REQUIRE(TestDrive() == nullptr);
        REQUIRE(LogLines("Storage: USB 1-1: device type 05 is not a disk; ignored") == before + 1);
        REQUIRE(Opcodes(disk) == std::vector<uint8_t>{OpInquiry});
    }
}

TEST_CASE("usb-storage T13: a capacity of 0xFFFFFFFF switches to READ CAPACITY(16)",
          "[usb][usbstorage]")
{
    World world;
    Model model;
    model.blocks = 64;
    const Disk disk = MakeDefaultDisk(model);
    disk->ReportBlocks((1ULL << 32) + 100);
    world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    REQUIRE(CountOpcode(disk, OpReadCapacity10) == 1);
    REQUIRE(CountOpcode(disk, OpServiceIn16) == 1);
    REQUIRE(drive->Info().sector_count == (1ULL << 32) + 100);
}

TEST_CASE("usb-storage T13: sector sizes 512 and 4096 are accepted, 520 refused",
          "[usb][usbstorage]")
{
    for (const uint32_t size : {512U, 4096U})
    {
        World world;
        Model model;
        model.block_size = size;
        model.blocks = 64;
        const Disk disk = MakeDefaultDisk(model);
        world.Attach(disk);
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
        REQUIRE(drive->Info().sector_size == size);
        REQUIRE(drive->Info().sector_count == 64);
    }
    World world;
    Model model;
    model.block_size = 520;
    model.blocks = 64;
    const size_t before =
        LogLines("Storage: USB 1-1: unusable capacity; ignored");
    world.Attach(MakeDefaultDisk(model));
    REQUIRE(TestDrive() == nullptr);
    REQUIRE(LogLines("Storage: USB 1-1: unusable capacity; ignored") ==
            before + 1);
}

TEST_CASE("usb-storage T13: a write-protected drive is read-only", "[usb][usbstorage]")
{
    World world;
    Model model;
    model.write_protected = true;
    const Disk disk = MakeDefaultDisk(model);
    const size_t before = LogLines("512-byte sectors, removable, write-protected");
    world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    REQUIRE(drive->Info().read_only);
    REQUIRE(LogLines("512-byte sectors, removable, write-protected") == before + 1);
    std::vector<uint8_t> data(512, 0xAA);
    const auto written = drive->Write(0, 1, data.data());
    REQUIRE_FALSE(written);
    REQUIRE(written.error() == Error::ReadOnly);
    for (const auto& command : disk->Commands())
    {
        REQUIRE(command.opcode != 0x2A);
    }
}

TEST_CASE("usb-storage: transfers outside the disk are refused before any command",
          "[usb][usbstorage]")
{
    World world;
    const Disk disk = MakeDefaultDisk();
    world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    const size_t sent = disk->Commands().size();
    std::vector<uint8_t> data(1024);
    for (const auto& [sector, count] :
         {std::pair<uint64_t, uint32_t>{2048, 1}, {2047, 2}, {5000, 1}, {0, 2049}})
    {
        const auto read = drive->Read(sector, count, data.data());
        REQUIRE_FALSE(read);
        REQUIRE(read.error() == Error::InvalidArgument);
    }
    REQUIRE(disk->Commands().size() == sent);
    REQUIRE(drive->Read(2047, 1, data.data()));
    REQUIRE(drive->Read(0, 0, data.data()));
}

TEST_CASE("usb-storage T12: a data timeout runs reset recovery in the order of BOT 5.3.4",
          "[usb][usbstorage]")
{
    World world;
    const Disk disk = MakeDefaultDisk();
    world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    const uint64_t start = world.env.NowMilliseconds();
    world.recording->Clear();
    disk->FailStage(Stage::Data, Fault::Hang);
    std::vector<uint8_t> out(512);
    // The drive retries the command once recovery ended.
    REQUIRE(drive->Read(3, 1, out.data()));
    REQUIRE(world.env.NowMilliseconds() - start >= S::RecoveryTimeoutMs / 30);
    REQUIRE(world.Recorded() == std::vector<std::string>{"cancel 81", "cancel 02",
                                                         "control 21 ff 00 00", "clear-halt 81",
                                                         "clear-halt 02"});
    REQUIRE(disk->BotResets() == 1);
    // Nothing was sent between the failure and the end of recovery, and the next command works.
    const std::vector<uint8_t>& image = disk->Image();
    REQUIRE(std::equal(out.begin(), out.end(), image.begin() + 3 * 512));
    REQUIRE(LogLines("Storage: sda: reset recovery after data-in timeout") >= 1);
}

TEST_CASE("usb-storage T12: a phase error or a bad status runs reset recovery",
          "[usb][usbstorage]")
{
    for (const Reply reply : {Reply::PhaseError, Reply::BadTag, Reply::BadSignature})
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        world.Attach(disk);
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
        world.recording->Clear();
        disk->FailReply(reply);
        std::vector<uint8_t> out(512);
        REQUIRE(drive->Read(0, 1, out.data()));
        REQUIRE(world.Recorded() == std::vector<std::string>{"cancel 81", "cancel 02",
                                                             "control 21 ff 00 00",
                                                             "clear-halt 81", "clear-halt 02"});
        REQUIRE(drive->Read(0, 1, out.data()));
    }
}

TEST_CASE("usb-storage T12: a stall on the data stage is answered by the CSW, not by recovery",
          "[usb][usbstorage]")
{
    World world;
    const Disk disk = MakeDefaultDisk();
    world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    world.recording->Clear();
    disk->FailStage(Stage::Data, Fault::Stall);
    std::vector<uint8_t> out(512);
    REQUIRE(drive->Read(0, 1, out.data()));
    disk->FailStage(Stage::Status, Fault::Stall);
    REQUIRE(drive->Read(0, 1, out.data()));
    REQUIRE(disk->BotResets() == 0);
    for (const std::string& line : world.Recorded())
    {
        REQUIRE(line.rfind("control 21 ff", 0) == std::string::npos);
    }
}

namespace
{

constexpr char PortResetLine[] =
    "Storage: sda: port reset, the drive comes back as a new disk";

// A hung data stage whose BOMS reset stalls: recovery fails and ends in a port reset.
void FailRecovery(const Disk& disk)
{
    disk->FailControl(ScriptedMassStorage::Outcome::Stall, 1, 0xFF);
    disk->FailStage(Stage::Data, Fault::Hang);
}

} // namespace

TEST_CASE("usb-storage T12: a failed BOMS reset ends in a port reset and a new drive",
          "[usb][usbstorage]")
{
    World world;
    const Disk disk = MakeDefaultDisk();
    S::Drive* attached = world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive == attached);
    const uint32_t resets = world.rig.fake.PortResets(1);
    const T::Device* before_device = world.rig.manager->Find(world.rig.manager->SlotOfPort(1));
    const uint32_t generation = before_device->generation.load();
    const size_t before = LogLines(PortResetLine);
    world.recording->Clear();
    FailRecovery(disk);
    std::vector<uint8_t> out(512);
    const auto failed = drive->Read(0, 1, out.data());
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error() == Error::Io);
    REQUIRE(LogLines(PortResetLine) == before + 1);

    // The command log ends the recovery before anything else, then the device is detached.
    const std::vector<std::string> recorded = world.Recorded();
    REQUIRE(recorded.size() >= 3);
    REQUIRE(std::vector<std::string>(recorded.begin(), recorded.begin() + 3) ==
            std::vector<std::string>{"cancel 81", "cancel 02", "control 21 ff 00 00"});
    REQUIRE(drive->Gone());
    REQUIRE(TestDrive() == nullptr);
    REQUIRE(world.rig.fake.PortResets(1) == resets + 1);

    // The device came back as a new one with its own drive; the old drive stays dead.
    const uint32_t slot = world.rig.manager->SlotOfPort(1);
    REQUIRE(slot != 0);
    const T::Device* device = world.rig.manager->Find(slot);
    REQUIRE(device != nullptr);
    REQUIRE((device != before_device || device->generation.load() != generation));
    S::Drive* replacement = world.DriveOf(slot);
    REQUIRE(replacement != nullptr);
    REQUIRE(replacement != drive.drive);
    REQUIRE(disk->Count(0xA1, 0xFE) == 2);
    const size_t sent = disk->Commands().size();
    REQUIRE_FALSE(drive->Read(0, 1, out.data()));
    REQUIRE(disk->Commands().size() == sent);
    REQUIRE(Worker::RunNext());
    const Held fresh(TestDrive());
    REQUIRE(fresh.drive == replacement);
    REQUIRE(fresh->Read(3, 1, out.data()));
    REQUIRE(std::equal(out.begin(), out.end(), disk->Image().begin() + 3 * 512));
}

TEST_CASE("usb-storage T12: a device behind a hub is reset through its hub port",
          "[usb][usbstorage][hub]")
{
    World world;
    const auto hub = T::Testing::MakeHub(Speed::High, 4);
    const Disk disk = MakeDefaultDisk();
    hub->Plug(2, disk, Speed::High);
    world.plugged.push_back(1);
    world.rig.fake.Connect(1, Speed::High, true, hub);
    world.rig.Settle();
    REQUIRE(Worker::RunNext());
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    REQUIRE(std::string(drive->Info().location) == "USB 1-1.2");
    const uint32_t resets = hub->Resets(2);
    const uint32_t root_resets = world.rig.fake.PortResets(1);
    const size_t before = LogLines("Storage: sda: port reset, the drive comes back");
    FailRecovery(disk);
    std::vector<uint8_t> out(512);
    REQUIRE_FALSE(drive->Read(0, 1, out.data()));
    REQUIRE(LogLines("Storage: sda: port reset, the drive comes back") == before + 1);
    REQUIRE(drive->Gone());
    REQUIRE(hub->Resets(2) == resets + 1);
    REQUIRE(world.rig.fake.PortResets(1) == root_resets);
    REQUIRE(disk->Count(0xA1, 0xFE) == 2);
    REQUIRE(Worker::RunNext());
    const Held fresh(TestDrive());
    REQUIRE(fresh.drive != nullptr);
    REQUIRE(fresh.drive != drive.drive);
    REQUIRE(std::string(fresh->Info().location) == "USB 1-1.2");
    REQUIRE(fresh->Read(5, 1, out.data()));
    REQUIRE(std::equal(out.begin(), out.end(), disk->Image().begin() + 5 * 512));
}

TEST_CASE("usb-storage T12: a second port reset within 60 s leaves the port disabled",
          "[usb][usbstorage]")
{
    SECTION("a root port")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        world.Attach(disk);
        std::vector<uint8_t> out(512);
        {
            const Held drive(TestDrive());
            FailRecovery(disk);
            REQUIRE_FALSE(drive->Read(0, 1, out.data()));
        }
        REQUIRE(world.rig.manager->SlotOfPort(1) != 0);
        REQUIRE(Worker::RunNext());
        const uint32_t resets = world.rig.fake.PortResets(1);
        const size_t refused = LogLines("USB 1-1: needs another port reset within 60 s");
        {
            const Held drive(TestDrive());
            REQUIRE(drive.drive != nullptr);
            world.env.Advance(3000);
            FailRecovery(disk);
            REQUIRE_FALSE(drive->Read(0, 1, out.data()));
            REQUIRE(drive->Gone());
        }
        REQUIRE(LogLines("USB 1-1: needs another port reset within 60 s") == refused + 1);
        REQUIRE(world.rig.fake.PortResets(1) == resets);
        REQUIRE(world.rig.manager->SlotOfPort(1) == 0);
        REQUIRE_FALSE(Worker::RunNext());
        // Pulling the plug and plugging it in again starts over.
        world.rig.fake.Disconnect(1);
        world.rig.Settle();
        world.Attach(disk);
        const Held again(TestDrive());
        REQUIRE(again.drive != nullptr);
        REQUIRE(again->Read(0, 1, out.data()));
    }
    SECTION("after the window another reset is allowed")
    {
        World world;
        const Disk disk = MakeDefaultDisk();
        world.Attach(disk);
        std::vector<uint8_t> out(512);
        {
            const Held drive(TestDrive());
            FailRecovery(disk);
            REQUIRE_FALSE(drive->Read(0, 1, out.data()));
        }
        REQUIRE(Worker::RunNext());
        world.env.Advance(T::PortResetWindowMs + 100);
        const uint32_t resets = world.rig.fake.PortResets(1);
        {
            const Held drive(TestDrive());
            FailRecovery(disk);
            REQUIRE_FALSE(drive->Read(0, 1, out.data()));
        }
        REQUIRE(world.rig.fake.PortResets(1) == resets + 1);
        REQUIRE(Worker::RunNext());
        const Held fresh(TestDrive());
        REQUIRE(fresh.drive != nullptr);
        REQUIRE(fresh->Read(0, 1, out.data()));
    }
}

namespace
{

constexpr uint8_t OpWrite10 = 0x2A;
constexpr uint8_t OpSynchronizeCache = 0x35;

struct Fixture
{
    Fixture() : disk(MakeDefaultDisk()), drive((world.Attach(disk), TestDrive()))
    {
        REQUIRE(drive.drive != nullptr);
    }

    FeatherDisk::Status Read(uint64_t sector = 3)
    {
        return drive->Read(sector, 1, out.data());
    }

    World world;
    Disk disk;
    Held drive;
    std::vector<uint8_t> out = std::vector<uint8_t>(512);
};

} // namespace

TEST_CASE("usb-storage T14: a command that ended in a transport failure is retried twice",
          "[usb][usbstorage]")
{
    SECTION("two failures, then success")
    {
        Fixture fixture;
        fixture.disk->FailReply(Reply::ShortData, 2);
        REQUIRE(fixture.Read());
        REQUIRE(CountOpcode(fixture.disk, OpRead10) == 3);
        REQUIRE(std::equal(fixture.out.begin(), fixture.out.end(),
                           fixture.disk->Image().begin() + 3 * 512));
    }
    SECTION("three failures end in an I/O error after three attempts")
    {
        Fixture fixture;
        fixture.disk->FailReply(Reply::ShortData, 3);
        const auto failed = fixture.Read();
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error() == Error::Io);
        REQUIRE(CountOpcode(fixture.disk, OpRead10) == 3);
        REQUIRE(fixture.Read());
    }
    SECTION("a hang is recovered and then retried")
    {
        Fixture fixture;
        fixture.disk->FailStage(Stage::Data, Fault::Hang);
        REQUIRE(fixture.Read());
        REQUIRE(fixture.disk->BotResets() == 1);
        REQUIRE(CountOpcode(fixture.disk, OpRead10) == 2);
    }
    SECTION("a write is sent again with its data")
    {
        Fixture fixture;
        fixture.disk->FailReply(Reply::PhaseError);
        const std::vector<uint8_t> data(512, 0xA7);
        REQUIRE(fixture.drive->Write(7, 1, data.data()));
        REQUIRE(CountOpcode(fixture.disk, OpWrite10) == 2);
        REQUIRE(std::equal(data.begin(), data.end(), fixture.disk->Image().begin() + 7 * 512));
    }
}

TEST_CASE("usb-storage sense rules: UNIT ATTENTION 29 and ABORTED COMMAND are retried three times",
          "[usb][usbstorage]")
{
    SECTION("power on, three in a row")
    {
        Fixture fixture;
        fixture.disk->UnitAttention(0x29, 3);
        REQUIRE(fixture.Read());
        REQUIRE(CountOpcode(fixture.disk, OpRead10) == 4);
    }
    SECTION("power on, four in a row")
    {
        Fixture fixture;
        fixture.disk->UnitAttention(0x29, 4);
        const auto failed = fixture.Read();
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error() == Error::Io);
        REQUIRE(fixture.Read());
    }
    SECTION("aborted command, three in a row")
    {
        Fixture fixture;
        fixture.disk->AbortedCommand(3);
        REQUIRE(fixture.Read());
    }
    SECTION("aborted command, four in a row")
    {
        Fixture fixture;
        fixture.disk->AbortedCommand(4);
        REQUIRE_FALSE(fixture.Read());
        REQUIRE(fixture.Read());
    }
    SECTION("no recovery runs for a check condition")
    {
        Fixture fixture;
        fixture.disk->AbortedCommand(2);
        REQUIRE(fixture.Read());
        REQUIRE(fixture.disk->BotResets() == 0);
    }
}

TEST_CASE("usb-storage sense rules: NOT READY waits 100 ms and retries within 5 s",
          "[usb][usbstorage]")
{
    SECTION("becomes ready")
    {
        Fixture fixture;
        fixture.disk->NotReady(3);
        const uint64_t start = fixture.world.env.NowMilliseconds();
        REQUIRE(fixture.Read());
        REQUIRE(fixture.world.env.NowMilliseconds() - start >= 300);
    }
    SECTION("never becomes ready")
    {
        Fixture fixture;
        fixture.disk->NotReady(100000);
        const uint64_t start = fixture.world.env.NowMilliseconds();
        REQUIRE_FALSE(fixture.Read());
        const uint64_t spent = fixture.world.env.NowMilliseconds() - start;
        REQUIRE(spent >= S::WaitBudgetMs - 200);
        REQUIRE(spent <= S::WaitBudgetMs + 500);
    }
}

TEST_CASE("usb-storage sense rules: SYNCHRONIZE CACHE unsupported is success and sent once",
          "[usb][usbstorage]")
{
    Fixture fixture;
    fixture.disk->SynchronizeCacheUnsupported(true);
    REQUIRE(fixture.drive->Flush());
    REQUIRE(fixture.drive->Flush());
    REQUIRE(CountOpcode(fixture.disk, OpSynchronizeCache) == 1);
    REQUIRE(fixture.disk->BotResets() == 0);
}

TEST_CASE("usb-storage sense rules: DATA PROTECT on a write makes the drive read-only",
          "[usb][usbstorage]")
{
    Fixture fixture;
    REQUIRE_FALSE(fixture.drive->Info().read_only);
    fixture.disk->WriteProtect(true);
    const std::vector<uint8_t> data(512, 0x3C);
    const auto failed = fixture.drive->Write(9, 1, data.data());
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error() == Error::ReadOnly);
    REQUIRE(fixture.drive->Info().read_only);
    REQUIRE(CountOpcode(fixture.disk, OpWrite10) == 1);
    const auto again = fixture.drive->Write(9, 1, data.data());
    REQUIRE_FALSE(again);
    REQUIRE(again.error() == Error::ReadOnly);
    REQUIRE(CountOpcode(fixture.disk, OpWrite10) == 1);
    REQUIRE(fixture.Read());
}

TEST_CASE("usb-storage sense rules: a medium change after the probe drops the drive",
          "[usb][usbstorage]")
{
    Fixture fixture;
    const size_t before = LogLines("Storage: sda: the medium may have changed");
    fixture.disk->UnitAttention(0x28);
    const auto failed = fixture.Read();
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error() == Error::Io);
    REQUIRE(LogLines("Storage: sda: the medium may have changed") == before + 1);
    REQUIRE(TestDrive() == nullptr);
    const size_t sent = fixture.disk->Commands().size();
    REQUIRE_FALSE(fixture.Read());
    const std::vector<uint8_t> data(512, 1);
    REQUIRE_FALSE(fixture.drive->Write(0, 1, data.data()));
    REQUIRE_FALSE(fixture.drive->Flush());
    REQUIRE(fixture.disk->Commands().size() == sent);
}

TEST_CASE("usb-storage I5: a transfer that timed out is not reused until it finished",
          "[usb][usbstorage]")
{
    World world;
    const Disk disk = MakeDefaultDisk();
    world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    S::Drive::TaskPipe& pipe = drive->Transport();
    disk->FailStage(Stage::Command, Fault::Hang);
    Status status = Status::Success;
    REQUIRE(pipe.Start(FeatherOS::Usb::Storage::Bot::Stage::Command, 31, &status));
    uint32_t actual = 0;
    REQUIRE(pipe.Finish(world.env.NowMilliseconds() + 100, &actual) == Status::Timeout);
    REQUIRE_FALSE(pipe.Start(FeatherOS::Usb::Storage::Bot::Stage::Command, 31, &status));
    REQUIRE(pipe.Recover() == Status::Success);
    REQUIRE(pipe.Start(FeatherOS::Usb::Storage::Bot::Stage::Command, 31, &status));
    pipe.Finish(world.env.NowMilliseconds() + 100, &actual);
}

namespace
{

struct ModelRun
{
    uint32_t faulted = 0;
    uint32_t commands = 0;
};

uint32_t Arm(const Disk& disk, std::mt19937& random, bool write)
{
    const uint32_t kind = random() % 8;
    switch (kind)
    {
    case 0:
        disk->FailStage(Stage::Data, Fault::Stall);
        break;
    case 1:
        disk->FailReply(write ? Reply::PhaseError : Reply::ShortData);
        break;
    case 2:
        disk->FailReply(Reply::PhaseError);
        break;
    case 3:
        disk->FailReply(Reply::BadTag);
        break;
    case 4:
        disk->FailStage(Stage::Data, Fault::Hang);
        break;
    case 5:
        disk->NotReady(1);
        break;
    case 6:
        disk->FailStage(Stage::Data, Fault::TransactionError);
        break;
    default:
        disk->FailReply(Reply::BadSignature);
        break;
    }
    return kind;
}

ModelRun RunModelCheck(uint32_t block_size, uint32_t operations, uint32_t seed)
{
    World world;
    Model model;
    model.block_size = block_size;
    model.blocks = 4 * 1024 * 1024 / block_size;
    const Disk disk = MakeDefaultDisk(model);
    world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    std::vector<uint8_t> expected = disk->Image();
    std::mt19937 random(seed);
    ModelRun run;
    for (uint32_t operation = 0; operation < operations; ++operation)
    {
        const uint32_t count = 1 + random() % 256;
        const uint64_t sector = random() % (model.blocks - count + 1);
        const bool write = random() % 2 == 0;
        std::vector<uint8_t> data(static_cast<size_t>(count) * block_size);
        for (uint8_t& byte : data)
        {
            byte = static_cast<uint8_t>(random());
        }
        uint32_t fault = 99;
        if (random() % 6 == 0)
        {
            fault = Arm(disk, random, write);
        }
        const size_t commands_before = disk->Commands().size();
        INFO("operation " << operation << " fault " << fault << " write " << write << " sector "
                          << sector << " count " << count);
        if (write)
        {
            const auto result = drive->Write(sector, count, data.data());
            REQUIRE(result);
        }
        else
        {
            std::vector<uint8_t> read(data.size());
            const auto result = drive->Read(sector, count, read.data());
            REQUIRE(result);
            REQUIRE(std::equal(read.begin(), read.end(),
                               expected.begin() + static_cast<long>(sector * block_size)));
        }
        // The drive retries a faulted command at most twice, so no operation sends more than three
        // attempts of each chunk, each with a REQUEST SENSE.
        const uint32_t chunks = (count * block_size + 65535) / 65536;
        const size_t sent = disk->Commands().size() - commands_before;
        REQUIRE(sent <= static_cast<size_t>(chunks) * 3 * 2);
        run.faulted += sent > chunks ? 1 : 0;
        if (write)
        {
            std::copy(data.begin(), data.end(),
                      expected.begin() + static_cast<long>(sector * block_size));
        }
        // Three recoveries of one endpoint within a second unbind the driver.
        world.env.Advance(1100);
    }
    REQUIRE(drive->Flush());
    REQUIRE(disk->Image() == expected);
    run.commands = static_cast<uint32_t>(disk->Commands().size());
    return run;
}

} // namespace

TEST_CASE("usb-storage T14: random reads and writes with faults match the model",
          "[usb][usbstorage]")
{
    SECTION("512-byte sectors")
    {
        const ModelRun run = RunModelCheck(512, 250, 1);
        REQUIRE(run.faulted > 0);
    }
    SECTION("4096-byte sectors")
    {
        const ModelRun run = RunModelCheck(4096, 100, 2);
        REQUIRE(run.faulted > 0);
    }
}

TEST_CASE("usb-storage T14: the full 2000-operation model check", "[.slow][usb][usbstorage]")
{
    REQUIRE(RunModelCheck(512, 2000, 3).faulted > 0);
    REQUIRE(RunModelCheck(4096, 2000, 4).faulted > 0);
}

namespace
{

constexpr uint64_t UnplugBoundMs = 2000;

void CheckUnplug(Stage stage, bool write)
{
    World world;
    const Disk disk = MakeDefaultDisk();
    S::Drive* attached = world.Attach(disk);
    REQUIRE(attached != nullptr);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    disk->on_unplug = [&world] { world.rig.fake.Disconnect(1); };
    disk->FailStage(stage, Fault::Unplug);
    std::vector<uint8_t> data(512, 0x5A);
    const uint64_t start = world.env.NowMilliseconds();
    const auto result = write ? drive->Write(1, 1, data.data()) : drive->Read(1, 1, data.data());
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == Error::Io);
    // The waiter woke with NoDevice rather than at its 5 s or 20 s deadline.
    REQUIRE(world.env.NowMilliseconds() - start < UnplugBoundMs);
    world.rig.Settle();
    REQUIRE(drive->Gone());
    REQUIRE(TestDrive() == nullptr);
    const size_t sent = disk->Commands().size();
    Status status = Status::Success;
    REQUIRE_FALSE(drive->Transport().Start(S::Bot::Stage::Command, 31, &status));
    REQUIRE(status == Status::NoDevice);
    REQUIRE_FALSE(drive->Read(0, 1, data.data()));
    REQUIRE(disk->Commands().size() == sent);
}

} // namespace

TEST_CASE("usb-storage T15: an unplug at each stage wakes the waiter with NoDevice",
          "[usb][usbstorage]")
{
    SECTION("command, read")
    {
        CheckUnplug(Stage::Command, false);
    }
    SECTION("data, read")
    {
        CheckUnplug(Stage::Data, false);
    }
    SECTION("status, read")
    {
        CheckUnplug(Stage::Status, false);
    }
    SECTION("command, write")
    {
        CheckUnplug(Stage::Command, true);
    }
    SECTION("data, write")
    {
        CheckUnplug(Stage::Data, true);
    }
}

TEST_CASE("usb-storage T15: buffers are freed once and the drive goes with its last reference",
          "[usb][usbstorage]")
{
    World world;
    const Disk disk = MakeDefaultDisk();
    world.Attach(disk);
    {
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
        REQUIRE(world.rig.hardware.Outstanding() > world.rig.baseline);
        world.rig.fake.Disconnect(1);
        world.rig.Settle();
        // The binding and the registry let go; this reference alone keeps the buffers.
        REQUIRE(drive->Gone());
        REQUIRE(world.rig.hardware.Outstanding() > world.rig.baseline);
    }
    world.plugged.clear();
    REQUIRE(world.rig.hardware.Outstanding() == world.rig.baseline);
}

TEST_CASE("usb-storage T15: an unplug during recovery ends the wait", "[usb][usbstorage]")
{
    World world;
    const Disk disk = MakeDefaultDisk();
    world.Attach(disk);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    disk->on_request = [&world](const ScriptedMassStorage::Request& request)
    {
        if (request.request == 0xFF)
        {
            world.rig.fake.Disconnect(1);
        }
    };
    disk->FailStage(Stage::Data, Fault::Hang);
    std::vector<uint8_t> out(512);
    REQUIRE_FALSE(drive->Read(0, 1, out.data()));
    world.rig.Settle();
    REQUIRE(drive->Gone());
}

TEST_CASE("usb-storage I15: a replugged device never sees the old drive's commands",
          "[usb][usbstorage]")
{
    World world;
    const Disk first = MakeDefaultDisk();
    world.Attach(first);
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    world.rig.fake.Disconnect(1);
    world.rig.Settle();
    REQUIRE(drive->Gone());
    // The slot's Device object is reused by the next device.
    const Disk second = MakeDefaultDisk();
    world.Plug(2, second);
    std::vector<uint8_t> out(512);
    REQUIRE_FALSE(drive->Read(0, 1, out.data()));
    REQUIRE_FALSE(drive->Flush());
    Status status = Status::Success;
    REQUIRE_FALSE(drive->Transport().Start(S::Bot::Stage::Command, 31, &status));
    REQUIRE(second->BulkReceived(0x02).empty());
    REQUIRE(second->Commands().empty());
}

namespace
{

thread_local bool holds_gate = false;
std::shared_mutex* global_gate = nullptr;

class ThreadEnvironment final : public S::Environment
{
public:
    ThreadEnvironment(std::shared_mutex& gate, uint32_t scale)
        : gate_(gate), scale_(scale), start_(std::chrono::steady_clock::now())
    {
    }

    uint64_t NowMilliseconds() override
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        return 1'000'000 + static_cast<uint64_t>(micros) * scale_ / 1000;
    }

    bool WaitUntil(const void*, uint64_t deadline_ms, bool (*ready)(void*), void* context) override
    {
        Release();
        bool result;
        {
            std::unique_lock lock(mutex_);
            for (;;)
            {
                if (ready(context))
                {
                    result = true;
                    break;
                }
                const uint64_t now = NowMilliseconds();
                if (now >= deadline_ms)
                {
                    result = false;
                    break;
                }
                const uint64_t real_us = (deadline_ms - now) * 1000 / scale_;
                signal_.wait_for(lock, std::chrono::microseconds(std::min<uint64_t>(real_us + 50, 2000)));
            }
        }
        Acquire();
        return result;
    }

    void Wake(const void*) override
    {
        {
            const std::lock_guard lock(mutex_);
        }
        signal_.notify_all();
    }

    void Sleep(uint32_t milliseconds) override
    {
        Release();
        std::this_thread::sleep_for(std::chrono::microseconds(milliseconds * 1000ULL / scale_));
        Acquire();
    }

private:
    void Release()
    {
        if (holds_gate)
        {
            gate_.unlock_shared();
        }
    }

    void Acquire()
    {
        if (holds_gate)
        {
            gate_.lock_shared();
        }
    }

    std::shared_mutex& gate_;
    uint32_t scale_;
    std::chrono::steady_clock::time_point start_;
    std::mutex mutex_;
    std::condition_variable signal_;
};

// Threads that call into the drive hold the gate shared, except while they wait; the thread that
// stands for the event and manager tasks and the one that unplugs hold it exclusively, because
// the fake reads TRBs the way hardware does.
class GateGuard
{
public:
    explicit GateGuard(std::shared_mutex& gate) : gate_(gate)
    {
        gate_.lock_shared();
        holds_gate = true;
    }
    ~GateGuard()
    {
        holds_gate = false;
        gate_.unlock_shared();
    }
    GateGuard(const GateGuard&) = delete;
    GateGuard& operator=(const GateGuard&) = delete;

private:
    std::shared_mutex& gate_;
};

class ThreadWorld
{
public:
    ThreadWorld() : env(gate, 100)
    {
        rig.drivers = Drivers;
        rig.wrap_host = [this](T::HostController& inner) -> T::HostController&
        {
            recording = std::make_unique<RecordingHost>(inner);
            return *recording;
        };
        REQUIRE(rig.Create());
        S::SetEnvironment(&env);
        // A thread that waits for a sleeping lock must not keep the gate shared.
        TestScheduler::on_wait = []
        {
            if (holds_gate)
            {
                global_gate->unlock_shared();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            if (holds_gate)
            {
                global_gate->lock_shared();
            }
        };
        global_gate = &gate;
    }

    void Start()
    {
        rig.hardware.DeferDoorbells(true);
        rig.hardware.on_idle = [this]
        {
            rig.hardware.PumpDoorbells();
            rig.controller->ProcessEvents();
        };
        pump = std::thread(
            [this]
            {
                while (!stop.load())
                {
                    {
                        const std::unique_lock exclusive(gate);
                        rig.hardware.PumpDoorbells();
                        rig.controller->ProcessEvents();
                        if (rig.manager->HasWork())
                        {
                            rig.manager->Run();
                        }
                    }
                    std::this_thread::yield();
                }
            });
    }

    void Unplug()
    {
        const std::unique_lock exclusive(gate);
        rig.fake.Disconnect(1);
    }

    void Stop()
    {
        stop.store(true);
        if (pump.joinable())
        {
            pump.join();
        }
        rig.hardware.on_idle = [this] { rig.controller->ProcessEvents(); };
        rig.hardware.DeferDoorbells(false);
        rig.hardware.PumpDoorbells();
        rig.Settle();
    }

    ~ThreadWorld()
    {
        Stop();
        TestScheduler::on_wait = nullptr;
        while (Worker::RunNext())
        {
        }
        S::SetEnvironment(nullptr);
    }

    ThreadWorld(const ThreadWorld&) = delete;
    ThreadWorld& operator=(const ThreadWorld&) = delete;

    std::shared_mutex gate;
    std::unique_ptr<RecordingHost> recording;
    Rig rig;
    ThreadEnvironment env;
    std::atomic<bool> stop = false;
    std::thread pump;
};

// Joins the threads when they finish within the limit; otherwise they are left running and the
// test fails.
bool JoinWithin(std::vector<std::thread>& threads, std::atomic<int>& finished,
                std::chrono::seconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (finished.load() < static_cast<int>(threads.size()))
    {
        if (std::chrono::steady_clock::now() > end)
        {
            for (std::thread& thread : threads)
            {
                thread.detach();
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    return true;
}

} // namespace

TEST_CASE("usb-storage T22: two volumes of one drive and an unplug", "[usb][usbstorage][concurrency]")
{
    for (int round = 0; round < 6; ++round)
    {
        ThreadWorld world;
        const Disk disk = MakeDefaultDisk();
        world.rig.fake.Connect(1, Speed::High, true, disk);
        world.rig.Settle();
        REQUIRE(world.rig.manager->SlotOfPort(1) != 0);
        world.Start();
        {
            const GateGuard guard(world.gate);
            REQUIRE(Worker::RunNext());
        }
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
        std::atomic<int> finished = 0;
        std::atomic<int> mismatches = 0;
        std::atomic<int> failures_before_unplug = 0;
        std::atomic<bool> unplugged = false;
        std::atomic<int> successes_after_unplug = 0;
        std::vector<std::thread> threads;
        for (uint32_t volume = 0; volume < 2; ++volume)
        {
            threads.emplace_back(
                [&, volume]
                {
                    const GateGuard guard(world.gate);
                    std::mt19937 random(round * 7 + volume);
                    const uint64_t base = volume * 1024;
                    for (int index = 0; index < 400; ++index)
                    {
                        const uint32_t count = 1 + random() % 8;
                        const uint64_t sector = base + random() % (1024 - count);
                        std::vector<uint8_t> data(count * 512, static_cast<uint8_t>(index + volume));
                        const bool was_unplugged = unplugged.load();
                        const auto written = drive->Write(sector, count, data.data());
                        std::vector<uint8_t> read(data.size());
                        const auto done = written && drive->Read(sector, count, read.data());
                        if (done && read != data)
                        {
                            ++mismatches;
                        }
                        if (done && was_unplugged)
                        {
                            ++successes_after_unplug;
                        }
                        if (!done)
                        {
                            if (!was_unplugged)
                            {
                                ++failures_before_unplug;
                            }
                            break;
                        }
                    }
                    ++finished;
                });
        }
        threads.emplace_back(
            [&, round]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2 + round * 10));
                unplugged.store(true);
                world.Unplug();
                ++finished;
            });
        const bool joined = JoinWithin(threads, finished, std::chrono::seconds(30));
        REQUIRE(joined);
        REQUIRE(mismatches.load() == 0);
        world.Stop();
        REQUIRE(drive->Gone());
        REQUIRE(world.rig.hardware.Outstanding() >= world.rig.baseline);
    }
}

TEST_CASE("usb-storage T22: a thread waiting for recovery while detach runs",
          "[usb][usbstorage][concurrency]")
{
    for (int round = 0; round < 6; ++round)
    {
        ThreadWorld world;
        const Disk disk = MakeDefaultDisk();
        world.rig.fake.Connect(1, Speed::High, true, disk);
        world.rig.Settle();
        world.Start();
        {
            const GateGuard guard(world.gate);
            REQUIRE(Worker::RunNext());
        }
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
        disk->FailStage(Stage::Data, Fault::Hang);
        std::atomic<int> finished = 0;
        std::atomic<bool> read_failed = false;
        std::vector<std::thread> threads;
        threads.emplace_back(
            [&]
            {
                const GateGuard guard(world.gate);
                std::vector<uint8_t> out(512);
                read_failed.store(!drive->Read(0, 1, out.data()));
                ++finished;
            });
        threads.emplace_back(
            [&, round]
            {
                // 5 s of drive time pass in 50 ms; the unplug lands before, during or after recovery.
                std::this_thread::sleep_for(std::chrono::milliseconds(30 + round * 15));
                world.Unplug();
                ++finished;
            });
        REQUIRE(JoinWithin(threads, finished, std::chrono::seconds(30)));
        REQUIRE(read_failed.load());
        world.Stop();
        REQUIRE(drive->Gone());
    }
}

TEST_CASE("usb-storage T22: the worker's probe while an unplug happens",
          "[usb][usbstorage][concurrency]")
{
    for (int round = 0; round < 8; ++round)
    {
        ThreadWorld world;
        const Disk disk = MakeDefaultDisk();
        world.rig.fake.Connect(1, Speed::High, true, disk);
        world.rig.Settle();
        world.Start();
        std::atomic<int> finished = 0;
        std::vector<std::thread> threads;
        threads.emplace_back(
            [&]
            {
                const GateGuard guard(world.gate);
                Worker::RunNext();
                ++finished;
            });
        threads.emplace_back(
            [&, round]
            {
                std::this_thread::sleep_for(std::chrono::microseconds(round * 400));
                world.Unplug();
                ++finished;
            });
        REQUIRE(JoinWithin(threads, finished, std::chrono::seconds(30)));
        world.Stop();
        // Published or not, a drive that is gone is not left in the registry (I17).
        REQUIRE(TestDrive() == nullptr);
    }
}

TEST_CASE("usb-storage T22: the manager recovers a drive while another volume waits for its lock",
          "[usb][usbstorage][concurrency]")
{
    ThreadWorld world;
    const Disk disk = MakeDefaultDisk();
    world.rig.fake.Connect(1, Speed::High, true, disk);
    world.rig.Settle();
    world.Start();
    {
        const GateGuard guard(world.gate);
        REQUIRE(Worker::RunNext());
    }
    const Held drive(TestDrive());
    REQUIRE(drive.drive != nullptr);
    disk->FailStage(Stage::Data, Fault::Hang);
    std::atomic<int> finished = 0;
    std::atomic<int> first_failed = 0;
    std::atomic<int> second_ok = 0;
    std::vector<std::thread> threads;
    threads.emplace_back(
        [&]
        {
            const GateGuard guard(world.gate);
            std::vector<uint8_t> out(512);
            first_failed.store(drive->Read(0, 1, out.data()) ? 0 : 1);
            ++finished;
        });
    threads.emplace_back(
        [&]
        {
            // Queues behind the first on io_lock.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const GateGuard guard(world.gate);
            std::vector<uint8_t> out(512);
            second_ok.store(drive->Read(1, 1, out.data()) ? 1 : 0);
            ++finished;
        });
    REQUIRE(JoinWithin(threads, finished, std::chrono::seconds(30)));
    world.Stop();
    // The first read's retry succeeds after recovery; the second one waited on io_lock.
    REQUIRE(first_failed.load() == 0);
    REQUIRE(second_ok.load() == 1);
    REQUIRE(disk->BotResets() == 1);
    world.rig.fake.Disconnect(1);
    world.rig.Settle();
}

TEST_CASE("usb-storage T24: a thread waiting for recovery while the manager re-enumerates the device",
          "[usb][usbstorage][concurrency]")
{
    for (int round = 0; round < 4; ++round)
    {
        ThreadWorld world;
        const Disk disk = MakeDefaultDisk();
        world.rig.fake.Connect(1, Speed::High, true, disk);
        world.rig.Settle();
        world.Start();
        {
            const GateGuard guard(world.gate);
            REQUIRE(Worker::RunNext());
        }
        const Held drive(TestDrive());
        REQUIRE(drive.drive != nullptr);
        FailRecovery(disk);
        // Stands for the volume lock a filesystem thread holds while it reads. Detach and the
        // port reset must finish without it.
        std::mutex volume;
        std::atomic<int> finished = 0;
        std::atomic<int> failed = 0;
        std::vector<std::thread> threads;
        for (int reader = 0; reader < 2; ++reader)
        {
            threads.emplace_back(
                [&, reader, round]
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(reader * (5 + round * 5)));
                    std::unique_lock lock(volume, std::defer_lock);
                    while (!lock.try_lock())
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    const GateGuard guard(world.gate);
                    std::vector<uint8_t> out(512);
                    const auto result = drive->Read(0, 1, out.data());
                    failed += !result && result.error() == Error::Io ? 1 : 0;
                    ++finished;
                });
        }
        REQUIRE(JoinWithin(threads, finished, std::chrono::seconds(30)));
        {
            const GateGuard guard(world.gate);
            while (Worker::RunNext())
            {
            }
        }
        world.Stop();
        REQUIRE(failed.load() == 2);
        REQUIRE(drive->Gone());
        REQUIRE(world.rig.fake.PortResets(1) == 2);
        const Held fresh(TestDrive());
        REQUIRE(fresh.drive != nullptr);
        REQUIRE(fresh.drive != drive.drive);
        REQUIRE(disk->Count(0xA1, 0xFE) == 2);
        // The next round's world must not find this one's drive in the registry.
        world.rig.fake.Disconnect(1);
        world.rig.Settle();
        REQUIRE(fresh->Gone());
    }
}
