#include "driver.h"

#include "kernel/klogger.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/worker.h"
#include "kernel/usb/storage/drive.h"
#include "kernel/usb/text.h"

#include <atomic>

namespace FeatherOS::Usb::Storage
{
namespace
{

constexpr uint8_t MassStorageClass = 8;
constexpr uint8_t ScsiSubclass = 6;
constexpr uint8_t BulkOnlyProtocol = 0x50;

constinit KLogger logger;
constinit std::atomic<Environment*> environment = nullptr;

bool HasBulkPair(const InterfaceView& interface)
{
    bool in = false;
    bool out = false;
    for (uint32_t index = 0; index < interface.endpoint_count; ++index)
    {
        const EndpointDescriptor& endpoint = interface.endpoints[index];
        if (endpoint.type == EndpointType::Bulk)
        {
            in = in || (endpoint.address & 0x80U) != 0;
            out = out || (endpoint.address & 0x80U) == 0;
        }
    }
    return in && out;
}

bool Matches(const InterfaceView& interface)
{
    if (interface.class_code != MassStorageClass)
    {
        return false;
    }
    if (interface.subclass != ScsiSubclass || interface.protocol != BulkOnlyProtocol ||
        !HasBulkPair(interface))
    {
        char line[96];
        Text text(line, sizeof(line));
        text.Words("Storage: mass-storage interface ");
        text.Decimal(interface.number);
        text.Words(" with subclass ");
        text.Hex(interface.subclass, 2);
        text.Words(" protocol ");
        text.Hex(interface.protocol, 2);
        text.Words(" is not supported");
        if (text.Finish())
        {
            logger.Warning(line);
        }
        return false;
    }
    return true;
}

void ProbeJob(void* context)
{
    auto* drive = static_cast<Drive*>(context);
    if (drive->Probe())
    {
        if (FeatherOS::Storage::Disk* disk = drive->AcquireDisk())
        {
            FeatherOS::Storage::Scan(*disk);
            disk->Release();
        }
    }
    drive->Release();
}

Usb::Status Attach(Device& device, const InterfaceView& interface, void** state)
{
    Environment* env = environment.load();
    if (env == nullptr)
    {
        return Usb::Status::Stall;
    }
    Drive* drive = Drive::Create(device, interface, *env);
    if (drive == nullptr)
    {
        return Usb::Status::ControllerError;
    }
    drive->MaxLun();
    drive->Retain();
    if (!FeatherOS::Storage::Worker::Post(ProbeJob, drive))
    {
        logger.Warning("Storage: the storage worker is not running; the drive is ignored");
        drive->Release();
        drive->Detached();
        return Usb::Status::Stall;
    }
    *state = drive;
    return Usb::Status::Success;
}

void Detach(Device&, void* state)
{
    static_cast<Drive*>(state)->Detached();
}

bool Service(Device& device, void* state)
{
    return static_cast<Drive*>(state)->Service(device);
}

} // namespace

const Driver MassStorageDriver = {"usb-storage", Matches, Attach, Detach, Service, true};

void SetEnvironment(Environment* value)
{
    environment.store(value);
}

} // namespace FeatherOS::Usb::Storage
