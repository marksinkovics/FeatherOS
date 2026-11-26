#include "kernel_runtime.h"

#include "kernel/arch/cpu_features.h"
#include "kernel/arch/keyboard.h"
#include "kernel/arch/pci.h"
#include "kernel/arch/rtc.h"
#include "kernel/arch/serial.h"
#include "kernel/boot_options.h"
#include "kernel/console.h"
#include "kernel/elf/elf_loader.h"
#include "kernel/filesystem/initramfs.h"
#include "kernel/filesystem/drivers.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"
#include "kernel/halt.h"
#include "kernel/input/input.h"
#include "kernel/arch/power.h"
#include "kernel/klogger.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/network/interface.h"
#include "kernel/network/neighbor_discovery.h"
#include "kernel/panic.h"
#include "kernel/pci/pci.h"
#include "kernel/usb/usb.h"
#include "kernel/process.h"
#include "kernel/process_arguments.h"
#include "kernel/scheduler.h"
#if defined(FEATHEROS_KERNEL_TESTS)
#include "kernel/kernel_test.h"
#endif
#include "kernel/smp.h"
#include "kernel/string/format.h"
#include "kernel/storage/block_device.h"
#include "kernel/storage/controllers.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/page_cache.h"
#include "kernel/storage/virtio_disk.h"
#include "kernel/time/timer.h"
#include "kernel/writeback.h"
#include "kernel/memory/virtual_memory.h"
#include "user/calendar.h"
#include "user/syscall_abi.h"

#include <string_view>

extern "C"
{
#include <limine.h>
}

namespace FeatherOS::KernelRuntime
{
namespace
{

constexpr char RootInit[] = "/sbin/init";
constexpr char RescueInit[] = "init";
constexpr uint64_t ControllerWaitMilliseconds = 60000;

Framebuffer framebuffer;
Term terms[Console::Count];

bool display_ready = false;

bool AllocateBackBuffer(size_t bytes, uint64_t hhdm_offset, uint8_t** back_buffer, size_t* capacity)
{
    const uint64_t pages = (bytes + Memory::PageSize - 1) / Memory::PageSize;
    uint64_t physical;
    if (!Memory::AllocateContiguousPageFrames(pages, &physical) ||
        physical > UINTPTR_MAX - hhdm_offset)
    {
        return false;
    }
    *back_buffer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(hhdm_offset + physical));
    *capacity = static_cast<size_t>(pages * Memory::PageSize);
    return true;
}

void InitializeDisplay(KLogger& logger, limine_framebuffer* framebuffer_response,
                       uint64_t hhdm_offset)
{
    if (framebuffer_response == nullptr)
    {
        logger.Warning("No supported framebuffer; using serial console only");
        return;
    }
    const size_t required = Framebuffer::RequiredBytes(framebuffer_response);
    if (required == 0)
    {
        logger.Warning("Framebuffer mode not supported; using serial console only");
        return;
    }
    uint8_t* back_buffer = nullptr;
    size_t capacity = 0;
    if (!AllocateBackBuffer(required, hhdm_offset, &back_buffer, &capacity))
    {
        logger.Warning("No memory for the framebuffer's back buffer; using serial console only");
        return;
    }
    if (!framebuffer.init(framebuffer_response, back_buffer, capacity))
    {
        logger.Warning("Framebuffer mode not supported; using serial console only");
        return;
    }
    for (uint32_t console = 0; console < Console::Count; ++console)
    {
        terms[console].Init(&framebuffer);
        terms[console].SetShown(console == 0);
    }
    logger.AttachTerminal(&terms[0]);
    display_ready = true;
    logger.Info("Framebuffer console initialized");
    char message[64];
    if (String::FormatTo(message, sizeof(message), "Display: {}x{}, console {}x{}",
                         framebuffer.GetWidth(), framebuffer.GetHeight(), terms[0].Columns(),
                         terms[0].ContentRows() + 1))
    {
        logger.Info(message);
    }
    if (Term::FontStatus() != Text::FontStatus::Ok)
    {
        logger.Warning("Console font rejected, cells draw as missing boxes");
    }
}

void MountInitramfs(KLogger& logger, const BootInfo& boot_info)
{
    if (!Initramfs::Mount(boot_info.initramfs_data, boot_info.initramfs_size))
    {
        Panic("Initramfs archive is missing or invalid");
    }
    if (!VFS::MountBuiltIn())
    {
        Panic("Could not mount /system, /etc, /dev, /proc and /tmp");
    }
    if (!VFS::LoadEtc())
    {
        Panic("Initramfs etc files do not fit in /etc");
    }

    VFS::FileInfo file_info;
    VFS::FileHandle file;
    if (!VFS::Stat("hello.txt", &file_info) || file_info.size < sizeof("FeatherOS") - 1 ||
        !VFS::Open("hello.txt", &file))
    {
        Panic("VFS could not stat/open initramfs hello.txt");
    }

    char prefix[sizeof("FeatherOS") - 1];
    uint64_t bytes_read;
    if (!VFS::Read(&file, prefix, sizeof(prefix), &bytes_read) || bytes_read != sizeof(prefix))
    {
        Panic("VFS could not read initramfs hello.txt");
    }
    for (size_t index = 0; index < sizeof(prefix); ++index)
    {
        if (prefix[index] != "FeatherOS"[index])
        {
            Panic("VFS returned invalid initramfs file data");
        }
    }
    if (!VFS::Seek(&file, 0))
    {
        Panic("VFS could not seek in initramfs hello.txt");
    }
    VFS::Close(&file);
    logger.Info("VFS: hello.txt read passed");
}

void LoadTestExecutable(KLogger& logger, const BootInfo& boot_info)
{
    // Only the user-preemption test build needs the platform data for its timer.
    (void)boot_info;
    constexpr char test_path[] = "test";
#if defined(ARCH_x86_64)
    constexpr uint16_t user_machine = ELF::MachineX86_64;
#elif defined(ARCH_aarch64)
    constexpr uint16_t user_machine = ELF::MachineAArch64;
#endif

    Initramfs::File file;
    if (!Initramfs::Open("system", test_path, &file))
    {
        Panic("Initramfs ELF test executable is missing");
    }
    ELF::MemoryFile memory_file = {file.data, file.size};
    const ELF::Source source = {&memory_file, ELF::ReadMemoryFile};
    ELF::Image image;
    if (!ELF::Parse(source, file.size, user_machine, &image))
    {
        Panic("Initramfs ELF test executable is invalid");
    }

    Process::UserProgram console_program;
    Process::UserProgram isolation_program;
    Process::InitializeUserProgram(&console_program);
    Process::InitializeUserProgram(&isolation_program);
#if defined(FEATHEROS_KERNEL_TESTS)
    const bool user_preemption_test = KernelTest::Selected(KernelTest::Mode::UserPreemption);
#else
    constexpr bool user_preemption_test = false;
#endif
    User::ProgramArguments console_arguments;
    User::ProgramArguments isolation_arguments;
    if (!Process::BuildProgramArguments("", Process::ArgumentsAddress, &console_arguments) ||
        !Process::BuildProgramArguments("", Process::ArgumentsAddress, &isolation_arguments))
    {
        Panic("Could not build ELF test program arguments");
    }
    console_arguments.process_id = 11;
    console_arguments.user_preemption_test = user_preemption_test ? 1 : 0;
    isolation_arguments.process_id = 22;
    isolation_arguments.test_case = 1;
    isolation_arguments.user_preemption_test = user_preemption_test ? 1 : 0;
    const bool console_loaded =
        Process::LoadUserProgram(image, source, console_arguments, &console_program);
    const bool isolation_loaded =
        console_loaded &&
        Process::LoadUserProgram(image, source, isolation_arguments, &isolation_program);
    if (!console_loaded || !isolation_loaded)
    {
        if (console_program.address_space.root_physical_address != 0)
        {
            Memory::DestroyAddressSpace(&console_program.address_space);
        }
        if (isolation_program.address_space.root_physical_address != 0)
        {
            Memory::DestroyAddressSpace(&isolation_program.address_space);
        }
        Panic("Could not load isolated ELF user programs");
    }

    Scheduler::TaskId console_task;
    Scheduler::TaskId isolation_task;
    if (!Scheduler::Initialize())
    {
        Panic("Could not initialize user-program scheduler");
    }
    if (user_preemption_test && !Time::InitializeTimer(100, boot_info.platform_data))
    {
        Panic("Could not initialize timer for user-mode preemption test");
    }
    if (!Scheduler::CreateTask(Process::RunUserProgram, &console_program, &console_task) ||
        !Scheduler::CreateTask(Process::RunUserProgram, &isolation_program, &isolation_task))
    {
        if (user_preemption_test)
        {
            Time::StopTimer();
        }
        Panic("Could not create user-program scheduler tasks");
    }
    if (user_preemption_test)
    {
        Scheduler::EnablePreemption();
    }

    uint64_t console_exit_status = UINT64_MAX;
    uint64_t isolation_exit_status = UINT64_MAX;
    const bool console_completed = Scheduler::Wait(console_task, &console_exit_status);
    const bool isolation_completed =
        console_completed && Scheduler::Wait(isolation_task, &isolation_exit_status);
    const bool tasks_reaped = isolation_completed && Scheduler::ReapTask(console_task) &&
                              Scheduler::ReapTask(isolation_task);
    if (user_preemption_test)
    {
        Scheduler::DisablePreemption();
        Time::StopTimer();
    }
    Memory::ActivateKernelAddressSpace();
    const bool spaces_destroyed = Memory::DestroyAddressSpace(&console_program.address_space) &&
                                  Memory::DestroyAddressSpace(&isolation_program.address_space);
    // The isolation program passes when reading kernel memory faults with a protection error.
    if (!console_completed || !isolation_completed || !tasks_reaped || !spaces_destroyed ||
        console_exit_status != 0 || isolation_exit_status != User::ExitStatusKernelAccessFault)
    {
        Panic("Scheduled user programs did not complete or release their resources");
    }
    logger.Info("ELF64: two isolated user programs exited and were reaped");
    if (user_preemption_test)
    {
        Serial::Write("KERN/SCHEDULER: user-mode preemption passed\n");
    }
#if defined(ARCH_x86_64)
    logger.Info("ELF64: ring 3 console and exit syscalls passed");
    logger.Info("ELF64: ring 3 kernel-memory isolation passed");
#elif defined(ARCH_aarch64)
    logger.Info("ELF64: EL0 console and exit syscalls passed");
    logger.Info("ELF64: EL0 kernel-memory isolation passed");
#endif
    logger.Info("ELF64: test image loaded into user address space");
}

bool LogPciFunction(const Pci::Function& function, void* context)
{
    char line[48];
    if (Pci::Describe(function, line, sizeof(line)))
    {
        static_cast<KLogger*>(context)->Info(line);
    }
    return true;
}

void LogPciDevices(KLogger& logger, const BootInfo& boot_info)
{
    Pci::ConfigSpace* config = Arch::Pci::Initialize(boot_info.platform_data);
    if (config == nullptr)
    {
        logger.Warning("PCI: no host bridge");
        return;
    }
    Pci::Enumerate(*config, LogPciFunction, &logger);
}

void MountRamRoot(KLogger& logger)
{
    logger.Info("Storage: no disk; running from the initramfs");
    if (!VFS::MountRamRoot())
    {
        Panic("Could not mount the RAM root");
    }
    logger.Info("VFS: / is tmpfs root, read-write");
    logger.Info("VFS: /etc is the initramfs's RAM copy: there is no disk");
}

void MountAndVerifyDisk(KLogger& logger, const BootInfo& boot_info)
{
    // The controller probes below wait with deadlines, which only a tick ends.
    if (!Time::InitializeTimer(100, boot_info.platform_data))
    {
        Panic("Periodic timer initialization failed");
    }
    Time::WaitForNextTick();
    logger.Info("Timer started at 100 Hz");
    if (!BlockDevice::InitializeCache(boot_info.hhdm_offset))
    {
        Panic("Page cache initialization failed");
    }
    if (!Storage::Initialize())
    {
        Panic("Disk registry initialization failed");
    }
    Filesystem::Drivers::AttachStorage();
    Storage::DeferScans(true);
    if (BlockDevice::Initialize(boot_info.platform_data, boot_info.hhdm_offset) &&
        BlockDevice::SectorCount() >= 2)
    {
        BlockDevice::RegisterVirtioDisk();
    }
    Storage::StartControllers(boot_info.platform_data);
    Storage::WaitForControllers(ControllerWaitMilliseconds);
    const BootOptions::Root requested = BootOptions::SelectedRoot();
    const bool names_disk = requested.request == BootOptions::RootRequest::Partition;
    Storage::Disk* const boot = Storage::ChooseBootDisk(names_disk ? requested.disk : nullptr);
    if (boot == nullptr)
    {
        Storage::DeferScans(false);
        MountRamRoot(logger);
        return;
    }
    Filesystem::Drivers::SetBootDisk(*boot);
    boot->Release();
    char message[128];
    const bool disk_found = names_disk && std::string_view(boot->Name()) == requested.disk;
    // A name that is neither the boot disk's nor of a registered disk leaves the default root.
    const size_t partition = disk_found ? requested.partition : 0;
    const char* root_type = nullptr;
    const char* root_source = nullptr;
    bool root_read_only = false;
    Storage::ReadOnlyReason read_only_reason = Storage::ReadOnlyReason::None;
    bool honored = true;
    if (!VFS::MountBlockDevice(partition, &honored) ||
        !VFS::DescribeRoot(&root_type, &root_source, &root_read_only, &read_only_reason))
    {
        if (!String::FormatTo(message, sizeof(message),
                              "No FAT16, ext2, ext3 or ext4 root filesystem on {}", boot->Name()))
        {
            Panic("No FAT16, ext2, ext3 or ext4 root filesystem on the disk");
        }
        Panic(message);
    }
    Storage::DeferScans(false);
    char wanted[Storage::MaxDiskName + 8];
    if (names_disk &&
        !Storage::PartitionName(requested.disk, static_cast<uint32_t>(requested.partition), wanted,
                                sizeof(wanted)))
    {
        Panic("Root name formatting failed");
    }
    if (requested.request == BootOptions::RootRequest::Malformed)
    {
        logger.Warning(
            "VFS: root= is not a partition such as vda1 or nvme0n1p2, using the default root");
    }
    else if (names_disk && !disk_found)
    {
        if (!String::FormatTo(message, sizeof(message),
                              "VFS: root={}: no disk {}, using the default root", wanted,
                              requested.disk))
        {
            Panic("Root warning formatting failed");
        }
        logger.Warning(message);
    }
    else if (!honored)
    {
        if (!String::FormatTo(message, sizeof(message),
                              "VFS: root={} is not a usable partition, using the default root",
                              wanted))
        {
            Panic("Root warning formatting failed");
        }
        logger.Warning(message);
    }
    if (!String::FormatTo(message, sizeof(message), "VFS: / is {} {}, {}", root_source, root_type,
                          !root_read_only ? "read-write"
                          : read_only_reason == Storage::ReadOnlyReason::Policy
                              ? "read-only: the storage policy (storage=rw to write)"
                              : "read-only: the volume refused writes"))
    {
        Panic("Root filesystem message formatting failed");
    }
    if (root_read_only)
    {
        logger.Warning(message);
    }
    else
    {
        logger.Info(message);
    }
    if (root_read_only)
    {
        logger.Info("VFS: /etc is the initramfs's RAM copy: the root is read-only");
    }
    else if (VFS::UseRootEtc())
    {
        logger.Info("VFS: /etc is the root's own directory");
    }
    else
    {
        logger.Warning(
            "VFS: /etc is the initramfs's RAM copy: the root's /etc could not be filled");
    }
#if defined(FEATHEROS_KERNEL_TESTS)
    if (KernelTest::Selected(KernelTest::Mode::Storage))
    {
        KernelTest::RunStorageTests(boot_info.hhdm_offset);
    }
#endif
}

void LogCpuProtections(KLogger& logger)
{
    const Arch::CpuFeatures::Protections protections = Arch::CpuFeatures::Enabled();
#if defined(ARCH_x86_64)
    const char* execute = " SMEP";
    const char* access = " SMAP";
    const char* random = " RDRAND";
#elif defined(ARCH_aarch64)
    const char* execute = "";
    const char* access = " PAN";
    const char* random = " RNDR";
#endif
    const bool any = protections.execute_protection || protections.access_protection ||
                     protections.hardware_random;
    char message[64];
    if (!String::FormatTo(message, sizeof(message), "CPU protections:{}{}{}{}",
                          protections.execute_protection ? execute : "",
                          protections.access_protection ? access : "",
                          protections.hardware_random ? random : "", any ? "" : " none"))
    {
        Panic("CPU protection summary formatting failed");
    }
    logger.Info(message);
}

void LogBootSummary(KLogger& logger, const Memory::Summary& memory_summary)
{
    LogCpuProtections(logger);
#if defined(ARCH_x86_64)
    logger.Info("Architecture: x86_64");
#elif defined(ARCH_aarch64)
    logger.Info("Architecture: aarch64");
#endif
    char message[96];
    if (!String::FormatTo(message, sizeof(message),
                          "Memory map coverage: {} MiB, usable RAM: {} MiB",
                          memory_summary.total_bytes / (1024 * 1024),
                          memory_summary.usable_bytes / (1024 * 1024)))
    {
        Panic("Memory summary formatting failed");
    }
    logger.Info(message);

    uint64_t seconds;
    char clock_text[User::FormattedCalendarTimeLength];
    if (!Arch::RealTimeClock::ReadUnixSeconds(&seconds) ||
        !User::FormatCalendarTime(User::CalendarFromUnixSeconds(seconds), clock_text))
    {
        logger.Warning("Wall clock unavailable");
        return;
    }
    if (!String::FormatTo(message, sizeof(message), "Wall clock: {}",
                          static_cast<const char*>(clock_text)))
    {
        Panic("Wall clock formatting failed");
    }
    logger.Info(message);
}

void StartSecondaryCpus(KLogger& logger, limine_smp_response* smp)
{
    const uint32_t cpu_count = Smp::StartSecondaryCpus(smp);
    char message[64];
    if (!String::FormatTo(message, sizeof(message), "SMP: {} CPUs online with IPIs and timers",
                          cpu_count))
    {
        Panic("SMP summary formatting failed");
    }
    logger.Info(message);
#if defined(FEATHEROS_KERNEL_TESTS)
    if (KernelTest::Selected(KernelTest::Mode::Smp))
    {
        KernelTest::RunSmpTests();
    }
#endif
}

void StartNetwork(KLogger& logger, const BootInfo& boot_info)
{
    if (!Network::Initialize(boot_info.platform_data, boot_info.hhdm_offset))
    {
        logger.Warning("Network device unavailable");
        return;
    }
    char mac[Network::FormattedMacLength];
    Network::FormatMac(Network::LocalMac(), mac);
    char message[64];
    if (!String::FormatTo(message, sizeof(message), "Network: virtio-net {}",
                          static_cast<const char*>(mac)))
    {
        Panic("Network summary formatting failed");
    }
    logger.Info(message);

    Network::StartIpv6();
    Network::WaitForRouter();
    Network::Ipv6Configuration ipv6;
    Network::ReadIpv6Configuration(&ipv6);
    char link_local[Network::FormattedIpv6Length];
    char global[Network::FormattedIpv6Length];
    char router[Network::FormattedIpv6Length];
    Network::FormatIpv6(ipv6.link_local, link_local);
    Network::FormatIpv6(ipv6.global, global);
    Network::FormatIpv6(ipv6.router, router);
    char ipv6_message[192];
    const bool formatted =
        Network::IsUnspecified(ipv6.global)
            ? String::FormatTo(ipv6_message, sizeof(ipv6_message),
                               "Network: IPv6 {}, no router advertisement",
                               static_cast<const char*>(link_local))
            : String::FormatTo(
                  ipv6_message, sizeof(ipv6_message), "Network: IPv6 {} and {}/{} via {}",
                  static_cast<const char*>(link_local), static_cast<const char*>(global),
                  ipv6.prefix_length, static_cast<const char*>(router));
    if (!formatted)
    {
        Panic("Network IPv6 summary formatting failed");
    }
    logger.Info(ipv6_message);
#if defined(FEATHEROS_KERNEL_TESTS)
    if (KernelTest::Selected(KernelTest::Mode::Network))
    {
        KernelTest::RunNetworkTests();
    }
#endif
}

[[noreturn]] void RunConsole(KLogger& logger, const BootInfo& boot_info)
{
    Input::SetTerminal(Console::Terminal());
    // Secondary CPUs start their timers at the boot CPU's rate.
    StartSecondaryCpus(logger, boot_info.smp);
    StartNetwork(logger, boot_info);
    Writeback::Start();
    logger.Info("Kernel initialization complete");
#if defined(FEATHEROS_KERNEL_TESTS)
    if (KernelTest::Selected(KernelTest::Mode::Pci))
    {
        KernelTest::RunPciTests(boot_info.platform_data);
    }
#endif
    Usb::Initialize(boot_info.platform_data);
#if defined(FEATHEROS_KERNEL_TESTS)
    if (KernelTest::Selected(KernelTest::Mode::UsbStorage))
    {
        KernelTest::StartUsbStorageTests();
    }
    if (KernelTest::Selected(KernelTest::Mode::Nvme))
    {
        KernelTest::StartNvmeTests();
    }
    if (KernelTest::Selected(KernelTest::Mode::Ahci))
    {
        KernelTest::StartAhciTests();
    }
    if (KernelTest::Selected(KernelTest::Mode::StoragePolicy))
    {
        KernelTest::StartStoragePolicyTests();
    }
#endif
    if (!Arch::Keyboard::Initialize(boot_info.platform_data, boot_info.hhdm_offset))
    {
        if (BootOptions::SelectedPs2() == BootOptions::Ps2Mode::Off)
        {
            logger.Info("Keyboard: ps2=off, no PS/2 or virtio keyboard");
        }
        else
        {
            logger.Warning("Keyboard device unavailable; use serial input");
        }
    }
    Console::Initialize(display_ready ? terms : nullptr, display_ready ? &framebuffer : nullptr);
#if defined(FEATHEROS_KERNEL_TESTS)
    if (KernelTest::Selected(KernelTest::Mode::Input))
    {
        KernelTest::StartConsoleReports();
    }
#endif

    // init starts and supervises the services in /etc/init.conf; restart it if it ever exits.
    for (;;)
    {
        Process::ProcessId init;
        if (Process::Spawn(RootInit, nullptr, 0, 0, &init) == Process::SpawnStatus::Started)
        {
            logger.Info("Init: starting /sbin/init from the root");
        }
        else if (Process::Spawn(RescueInit, nullptr, 0, 0, &init) == Process::SpawnStatus::Started)
        {
            logger.Info("Init: starting /system/init from the initramfs");
        }
        else
        {
            Panic("Could not start init");
        }
        uint64_t exit_status;
        if (!Process::Wait(init, &exit_status))
        {
            Panic("Could not wait for init");
        }
        char message[64];
        if (!String::FormatTo(message, sizeof(message), "init exited with status {}; restarting",
                              exit_status))
        {
            Panic("init status formatting failed");
        }
        logger.Warning(message);
    }
}

} // namespace

[[noreturn]] void Run(KLogger& logger, const BootInfo& boot_info)
{
    Process::Initialize(boot_info.hhdm_offset);
    Arch::Power::Initialize(boot_info.acpi_rsdp_physical, boot_info.platform_data);
    InitializeDisplay(logger, boot_info.framebuffer, boot_info.hhdm_offset);
    MountInitramfs(logger, boot_info);
    LoadTestExecutable(logger, boot_info);
    LogPciDevices(logger, boot_info);
    MountAndVerifyDisk(logger, boot_info);
    LogBootSummary(logger, boot_info.memory_summary);
    RunConsole(logger, boot_info);
}

} // namespace FeatherOS::KernelRuntime