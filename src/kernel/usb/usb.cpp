#include "usb.h"

#include "kernel/arch/pci.h"
#include "kernel/boot_options.h"
#include "kernel/input/input.h"
#include "kernel/klogger.h"
#include "kernel/pci/pci.h"
#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "kernel/usb/device.h"
#include "kernel/usb/hid/driver.h"
#include "kernel/usb/manager.h"
#include "kernel/usb/storage/driver.h"
#include "kernel/usb/xhci/controller.h"
#include "user/thread_safety.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Usb
{
namespace
{

constexpr uint32_t MaxControllers = 4;
constexpr uint32_t WatchdogMilliseconds = 1000;
constexpr uint32_t PollMilliseconds = 10;
constexpr uint8_t SerialBusClass = 0x0C;
constexpr uint8_t UsbSubclass = 0x03;
constexpr uint8_t XhciInterface = 0x30;
constexpr uint8_t EhciInterface = 0x20;
constexpr uint8_t OhciInterface = 0x10;
constexpr uint8_t UhciInterface = 0x00;

constinit KLogger logger;
constinit Xhci::Controller* controllers[MaxControllers] = {};
constinit Manager* managers[MaxControllers] = {};
constinit uint32_t controller_count = 0;

constinit Sync::SpinLock manager_lock;
Scheduler::WaitQueue manager_wait;
bool manager_pending GUARDED_BY(manager_lock) = true;

void WakeManager()
{
    Sync::SpinLockGuard guard(manager_lock);
    manager_pending = true;
    Scheduler::WakeAll(manager_wait);
}

uint64_t EventsTask(void* context)
{
    auto* controller = static_cast<Xhci::Controller*>(context);
    Xhci::Hardware& hardware = controller->Machine();
    for (;;)
    {
        controller->ProcessEvents();
        if (controller->HasWork())
        {
            WakeManager();
        }
        Hid::ServiceRepeat(*controller, hardware.NowMilliseconds());
        const bool polling = controller->Interrupts() == Xhci::InterruptKind::Polling;
        const uint64_t period =
            hardware.NowMilliseconds() + (polling ? PollMilliseconds : WatchdogMilliseconds);
        const uint64_t repeat = Hid::NextRepeat(*controller);
        hardware.WaitForEvents(repeat < period ? repeat : period);
    }
}

// Runs wherever a lock key was pressed, perhaps in an interrupt handler.
void OnLockChange(uint8_t)
{
    WakeManager();
}

uint64_t ManagerTask(void*)
{
    for (;;)
    {
        {
            Sync::SpinLockGuard guard(manager_lock);
            while (!manager_pending)
            {
                Scheduler::WaitOn(manager_wait, manager_lock);
            }
            manager_pending = false;
        }
        for (uint32_t index = 0; index < controller_count; ++index)
        {
            managers[index]->Run();
        }
        Hid::SendLeds();
    }
}

void LogUnsupported(const Pci::Function& function, const char* kind)
{
    constexpr char Digits[] = "0123456789abcdef";
    char line[64];
    size_t length = 0;
    auto put = [&](char value) { line[length++] = value; };
    for (const char* text = "USB: "; *text != '\0'; ++text)
    {
        put(*text);
    }
    for (const char* text = kind; *text != '\0'; ++text)
    {
        put(*text);
    }
    for (const char* text = " at "; *text != '\0'; ++text)
    {
        put(*text);
    }
    put(Digits[function.address.bus >> 4]);
    put(Digits[function.address.bus & 0xF]);
    put(':');
    put(Digits[function.address.device >> 4]);
    put(Digits[function.address.device & 0xF]);
    put('.');
    put(Digits[function.address.function & 0xF]);
    for (const char* text = " not supported"; *text != '\0'; ++text)
    {
        put(*text);
    }
    put('\0');
    logger.Warning(line);
}

struct Scan
{
    Pci::ConfigSpace* config;
};

bool Visit(const Pci::Function& function, void* context)
{
    auto* scan = static_cast<Scan*>(context);
    if (function.class_code != SerialBusClass || function.subclass != UsbSubclass)
    {
        return true;
    }
    switch (function.prog_if)
    {
    case XhciInterface:
        if (controller_count < MaxControllers)
        {
            Xhci::Controller* controller = Xhci::Controller::Probe(*scan->config, function);
            if (controller != nullptr)
            {
                controllers[controller_count++] = controller;
            }
        }
        break;
    case EhciInterface:
        LogUnsupported(function, "EHCI");
        break;
    case OhciInterface:
        LogUnsupported(function, "OHCI");
        break;
    case UhciInterface:
        LogUnsupported(function, "UHCI");
        break;
    default:
        break;
    }
    return true;
}

bool StartTask(Scheduler::TaskEntry entry, void* context, Scheduler::TaskPriority priority)
{
    Scheduler::TaskId task;
    return Scheduler::CreateTask(entry, context, &task) && Scheduler::SetPriority(task, priority);
}

} // namespace

bool Initialize(const void* platform_data)
{
    if (BootOptions::SelectedUsb() == BootOptions::UsbMode::Off)
    {
        return false;
    }
    Pci::ConfigSpace* config = Arch::Pci::Initialize(platform_data);
    if (config == nullptr)
    {
        return false;
    }
    Scan scan = {config};
    Pci::Enumerate(*config, Visit, &scan);
    if (controller_count == 0)
    {
        return false;
    }
    for (uint32_t index = 0; index < controller_count; ++index)
    {
        char line[128];
        if (controllers[index]->Describe(line, sizeof(line)))
        {
            logger.Info(line);
        }
    }
    for (uint32_t index = 0; index < controller_count; ++index)
    {
        managers[index] = new Manager(*controllers[index], index + 1);
    }
    Input::SetLedCallback(OnLockChange);
    SetWorkWake(WakeManager);
    bool started = StartTask(ManagerTask, nullptr, Scheduler::TaskPriority::Normal);
    for (uint32_t index = 0; index < controller_count; ++index)
    {
        started =
            StartTask(EventsTask, controllers[index], Scheduler::TaskPriority::High) && started;
    }
    if (!started)
    {
        logger.Error("USB: could not start the USB tasks");
    }
    return started;
}

} // namespace FeatherOS::Usb
