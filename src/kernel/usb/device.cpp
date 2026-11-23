#include "device.h"

#include "kernel/klogger.h"
#include "kernel/usb/text.h"

namespace FeatherOS::Usb
{

constinit Sync::Mutex topology_lock;

namespace
{

constexpr uint32_t QuiesceLimitMs = 2000;
constexpr uint8_t HostToDeviceStandard = 0x00;
constexpr uint8_t SetConfigurationRequest = 9;

constinit KLogger logger;
constinit Bus* buses[MaxBuses] GUARDED_BY(topology_lock) = {};
constinit std::atomic<void (*)()> work_wake = nullptr;

const char* SpeedName(Speed speed, bool full)
{
    switch (speed)
    {
    case Speed::Low:
        return full ? "low-speed" : "low";
    case Speed::Full:
        return full ? "full-speed" : "full";
    case Speed::High:
        return full ? "high-speed" : "high";
    case Speed::Super:
        return full ? "super-speed" : "super";
    case Speed::SuperPlus:
        return full ? "super-speed+" : "super+";
    }
    return "unknown";
}

void PutDrivers(Text& text, const Device& device, const char* none)
{
    bool any = false;
    for (uint32_t index = 0; index < MaxInterfaces; ++index)
    {
        const Driver* driver = device.drivers[index];
        bool repeated = false;
        for (uint32_t earlier = 0; earlier < index; ++earlier)
        {
            repeated = repeated || device.drivers[earlier] == driver;
        }
        if (driver == nullptr || repeated)
        {
            continue;
        }
        text.Put(any ? ',' : ' ');
        text.Words(driver->name);
        any = true;
    }
    if (!any && none != nullptr)
    {
        text.Put(' ');
        text.Words(none);
    }
}

void Complete(Transfer& base)
{
    auto& transfer = static_cast<DeviceTransfer&>(base);
    Device* device = transfer.device;
    auto* callback = transfer.callback;
    if (callback != nullptr &&
        transfer.generation == device->generation.load(std::memory_order_acquire))
    {
        callback(transfer);
    }
    device->in_flight.fetch_sub(1, std::memory_order_release);
}

} // namespace

void SetWorkWake(void (*wake)())
{
    work_wake.store(wake);
}

void MakePath(char* out, size_t capacity, uint32_t bus, const Device* parent, uint32_t port)
{
    Text name(out, capacity);
    if (parent == nullptr)
    {
        name.Decimal(bus);
        name.Put('-');
    }
    else
    {
        name.Words(parent->path);
        name.Put('.');
    }
    name.Decimal(port);
    name.Finish();
}

void Device::Reset(HostController& host, uint32_t bus_number, uint32_t slot_number,
                   const Location& where, Bus* tree)
{
    host_ = &host;
    tree_ = tree;
    bus = bus_number;
    slot = slot_number;
    location = where;
    device = {};
    configuration = {};
    product[0] = '\0';
    number = 0;
    path[0] = '\0';
    parent = nullptr;
    parent_port = 0;
    attached = false;
    for (uint32_t index = 0; index < MaxInterfaces; ++index)
    {
        drivers[index] = nullptr;
        driver_state[index] = nullptr;
    }
    detaching.store(false);
}

HostController& Device::Host() const
{
    return *host_;
}

const Driver* DriverOf(const Device& device, uint32_t interface)
{
    return interface < MaxInterfaces ? device.drivers[interface] : nullptr;
}

void Device::PostWork()
{
    if (tree_ != nullptr)
    {
        tree_->PostWork(number, generation.load(std::memory_order_acquire));
    }
}

bool Device::Submit(DeviceTransfer& transfer)
{
    transfer.device = this;
    transfer.done = Complete;
    in_flight.fetch_add(1);
    transfer.generation = generation.load();
    if (detaching.load())
    {
        in_flight.fetch_sub(1);
        transfer.status = Status::Cancelled;
        transfer.actual = 0;
        return false;
    }
    if (!host_->Submit(slot, transfer))
    {
        in_flight.fetch_sub(1);
        return false;
    }
    return true;
}

Bus::Bus(HostController& host, uint32_t number, std::span<const Driver* const> drivers)
    : host_(host), number_(number), drivers_(drivers)
{
    Sync::MutexGuard guard(topology_lock);
    for (Bus*& entry : buses)
    {
        if (entry == nullptr)
        {
            entry = this;
            break;
        }
    }
}

Bus::~Bus()
{
    Sync::MutexGuard guard(topology_lock);
    for (Bus*& entry : buses)
    {
        if (entry == this)
        {
            entry = nullptr;
        }
    }
}

uint32_t Bus::Number() const
{
    return number_;
}

Device& Bus::Prepare(uint32_t slot, const Location& location)
{
    Device& device = devices_[slot];
    device.Reset(host_, number_, slot, location, this);
    return device;
}

Device* Bus::Find(uint32_t slot)
{
    return slot != 0 && slot <= MaxDeviceSlots && devices_[slot].attached ? &devices_[slot]
                                                                          : nullptr;
}

const Device* Bus::Find(uint32_t slot) const
{
    return slot != 0 && slot <= MaxDeviceSlots && devices_[slot].attached ? &devices_[slot]
                                                                          : nullptr;
}

Device* Bus::FindNumber(uint32_t number)
{
    for (Device& device : devices_)
    {
        if (device.attached && device.number == number)
        {
            return &device;
        }
    }
    return nullptr;
}

Device* Bus::ChildAt(const Device& parent, uint8_t port)
{
    for (Device& device : devices_)
    {
        if (device.attached && device.parent == &parent && device.parent_port == port)
        {
            return &device;
        }
    }
    return nullptr;
}

void Bus::PostWork(uint32_t number, uint32_t generation)
{
    {
        Sync::SpinLockGuard guard(work_lock_);
        if (work_count_ < WorkCapacity)
        {
            work_[(work_head_ + work_count_) % WorkCapacity] = {number, generation};
            ++work_count_;
            work_pending_.store(work_count_);
        }
    }
    void (*wake)() = work_wake.load();
    if (wake != nullptr)
    {
        wake();
    }
}

bool Bus::TakeWork(DeviceWork* work)
{
    Sync::SpinLockGuard guard(work_lock_);
    if (work_count_ == 0)
    {
        return false;
    }
    *work = work_[work_head_];
    work_head_ = (work_head_ + 1) % WorkCapacity;
    --work_count_;
    work_pending_.store(work_count_);
    return true;
}

bool Bus::HasWork() const
{
    return work_pending_.load() != 0;
}

void Bus::ClearWork()
{
    Sync::SpinLockGuard guard(work_lock_);
    work_count_ = 0;
    work_pending_.store(0);
}

uint32_t Bus::Count() const
{
    uint32_t count = 0;
    for (const Device& device : devices_)
    {
        count += device.attached ? 1 : 0;
    }
    return count;
}

Device* Bus::ChildOf(const Device& parent)
{
    for (Device& device : devices_)
    {
        if (device.attached && device.parent == &parent)
        {
            return &device;
        }
    }
    return nullptr;
}

uint32_t Bus::AllocateNumber()
{
    for (uint32_t number = RootHubNumber + 1; number <= MaxDeviceNumber; ++number)
    {
        uint32_t& word = used_[number / 32];
        if ((word & (1U << (number % 32))) == 0)
        {
            word |= 1U << (number % 32);
            return number;
        }
    }
    return 0;
}

void Bus::LogLine(const char* path, const char* message, bool warning)
{
    char line[96];
    Text text(line, sizeof(line));
    text.Words("USB ");
    text.Words(path);
    text.Words(": ");
    text.Words(message);
    if (text.Finish())
    {
        if (warning)
        {
            logger.Warning(line);
        }
        else
        {
            logger.Info(line);
        }
    }
}

const char* Bus::Bind(Device& device)
{
    EndpointDescriptor endpoints[MaxInterfaces * MaxInterfaceEndpoints];
    uint32_t endpoint_count = 0;
    uint32_t bound = 0;
    const ConfigurationView& configuration = device.configuration;
    for (uint32_t index = 0; index < configuration.interface_count && index < MaxInterfaces;
         ++index)
    {
        const InterfaceView& interface = configuration.interfaces[index];
        for (const Driver* driver : drivers_)
        {
            if (driver->matches(interface))
            {
                device.drivers[index] = driver;
                ++bound;
                for (uint32_t endpoint = 0; endpoint < interface.endpoint_count; ++endpoint)
                {
                    endpoints[endpoint_count++] = interface.endpoints[endpoint];
                }
                break;
            }
        }
    }
    if (bound == 0)
    {
        return nullptr;
    }
    const auto clear = [&device]
    {
        for (const Driver*& driver : device.drivers)
        {
            driver = nullptr;
        }
    };
    if (!host_.ConfigureEndpoints(
            device.slot, std::span<const EndpointDescriptor>(endpoints, endpoint_count), nullptr))
    {
        clear();
        return "could not configure its endpoints";
    }
    Memory::Dma::Buffer none{};
    uint32_t actual = 0;
    const SetupPacket select = {HostToDeviceStandard, SetConfigurationRequest, configuration.value,
                                0, 0};
    if (host_.Control(device.slot, select, none, &actual) != Status::Success)
    {
        clear();
        return "could not select its configuration";
    }
    const char* warning = nullptr;
    for (uint32_t index = 0; index < MaxInterfaces; ++index)
    {
        const Driver* driver = device.drivers[index];
        if (driver == nullptr)
        {
            continue;
        }
        if (!Succeeded(driver->attach(device, configuration.interfaces[index],
                                      &device.driver_state[index])))
        {
            device.drivers[index] = nullptr;
            device.driver_state[index] = nullptr;
            warning = "a driver could not attach";
        }
    }
    return warning;
}

void Bus::Attach(Device& device, Device* parent, uint8_t parent_port)
{
    char line[160];
    char path[MaxPathName];
    const char* warning;
    {
        Sync::MutexGuard guard(topology_lock);
        device.number = AllocateNumber();
        device.parent = parent;
        device.parent_port = parent_port;
        MakePath(device.path, sizeof(device.path), number_, parent,
                 parent == nullptr ? device.location.root_port : parent_port);
        warning = Bind(device);
        device.attached = true;
        for (uint32_t index = 0; index < MaxPathName; ++index)
        {
            path[index] = device.path[index];
        }
        Text text(line, sizeof(line));
        text.Words("USB ");
        text.Words(device.path);
        text.Words(": ");
        text.Words(SpeedName(device.location.speed, true));
        text.Put(' ');
        text.Hex(device.device.vendor, 4);
        text.Put(':');
        text.Hex(device.device.product, 4);
        if (device.product[0] != '\0')
        {
            text.Words(" \"");
            text.Words(device.product);
            text.Put('"');
        }
        PutDrivers(text, device, nullptr);
        if (!text.Finish())
        {
            line[0] = '\0';
        }
    }
    if (line[0] != '\0')
    {
        logger.Info(line);
    }
    if (warning != nullptr)
    {
        LogLine(path, warning, true);
    }
}

bool Bus::Quiesce(Device& device)
{
    device.detaching.store(true);
    host_.StopEndpoints(device.slot);
    for (uint32_t waited = 0; device.in_flight.load(std::memory_order_acquire) != 0; ++waited)
    {
        if (waited >= QuiesceLimitMs)
        {
            device.generation.fetch_add(1);
            return false;
        }
        host_.Delay(1);
    }
    device.generation.fetch_add(1);
    return true;
}

void Bus::Unbind(Device& device, bool stuck)
{
    for (uint32_t index = 0; index < MaxInterfaces; ++index)
    {
        const Driver* driver = device.drivers[index];
        if (driver != nullptr && !stuck)
        {
            driver->detach(device, device.driver_state[index]);
        }
        device.drivers[index] = nullptr;
        device.driver_state[index] = nullptr;
    }
}

void Bus::Detach(Device& device)
{
    for (Device* child = ChildOf(device); child != nullptr; child = ChildOf(device))
    {
        Detach(*child);
    }
    char path[MaxPathName];
    bool stuck;
    {
        Sync::MutexGuard guard(topology_lock);
        if (!device.attached)
        {
            return;
        }
        for (uint32_t index = 0; index < MaxPathName; ++index)
        {
            path[index] = device.path[index];
        }
        stuck = !Quiesce(device);
        Unbind(device, stuck);
        host_.DestroyDevice(device.slot);
        used_[device.number / 32] &= ~(1U << (device.number % 32));
        device.attached = false;
        device.detaching.store(false);
    }
    LogLine(path, "disconnected", false);
    if (stuck)
    {
        LogLine(path, "a transfer never completed; its driver state was kept", true);
    }
}

void Bus::UnbindDrivers(Device& device)
{
    bool stuck = false;
    {
        Sync::MutexGuard guard(topology_lock);
        if (!device.attached)
        {
            return;
        }
        stuck = !Quiesce(device);
        Unbind(device, stuck);
        device.detaching.store(false);
    }
    if (stuck)
    {
        LogLine(device.path, "a transfer never completed; its driver state was kept", true);
    }
}

void Bus::DropAll()
{
    for (bool dropped = true; dropped;)
    {
        dropped = false;
        for (uint32_t slot = 1; slot <= MaxDeviceSlots; ++slot)
        {
            Device& device = devices_[slot];
            if (!device.attached || ChildOf(device) != nullptr)
            {
                continue;
            }
            char path[MaxPathName];
            {
                Sync::MutexGuard guard(topology_lock);
                for (uint32_t index = 0; index < MaxPathName; ++index)
                {
                    path[index] = device.path[index];
                }
                Unbind(device, !Quiesce(device));
                used_[device.number / 32] &= ~(1U << (device.number % 32));
                device.attached = false;
                device.detaching.store(false);
            }
            LogLine(path, "disconnected", false);
            dropped = true;
        }
    }
    ClearWork();
}

size_t FormatDevices(char* buffer, size_t capacity)
{
    size_t used = 0;
    if (capacity == 0)
    {
        return 0;
    }
    {
        Sync::MutexGuard guard(topology_lock);
        for (const Bus* bus : buses)
        {
            for (uint32_t number = RootHubNumber + 1; bus != nullptr && number <= MaxDeviceNumber;
                 ++number)
            {
                for (const Device& device : bus->devices_)
                {
                    if (!device.attached || device.number != number)
                    {
                        continue;
                    }
                    char line[128];
                    Text text(line, sizeof(line));
                    const InterfaceView none{};
                    const InterfaceView& first = device.configuration.interface_count != 0
                                                     ? device.configuration.interfaces[0]
                                                     : none;
                    text.Words(device.path);
                    text.Words(" dev ");
                    text.Decimal(device.number);
                    text.Put(' ');
                    text.Words(SpeedName(device.location.speed, false));
                    text.Put(' ');
                    text.Hex(device.device.vendor, 4);
                    text.Put(':');
                    text.Hex(device.device.product, 4);
                    text.Words(" class ");
                    text.Hex(first.class_code, 2);
                    text.Put('/');
                    text.Hex(first.subclass, 2);
                    text.Put('/');
                    text.Hex(first.protocol, 2);
                    PutDrivers(text, device, "unbound");
                    text.Put('\n');
                    if (!text.Finish())
                    {
                        continue;
                    }
                    size_t length = 0;
                    while (line[length] != '\0')
                    {
                        ++length;
                    }
                    if (used + length + 1 > capacity)
                    {
                        buffer[used] = '\0';
                        return used;
                    }
                    for (size_t index = 0; index < length; ++index)
                    {
                        buffer[used + index] = line[index];
                    }
                    used += length;
                }
            }
        }
    }
    buffer[used] = '\0';
    return used;
}

} // namespace FeatherOS::Usb
