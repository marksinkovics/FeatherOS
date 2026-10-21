#include "kernel_runtime.h"

#include "kernel/arch/cpu_features.h"
#include "kernel/arch/keyboard.h"
#include "kernel/arch/rtc.h"
#include "kernel/arch/serial.h"
#include "kernel/console.h"
#include "kernel/elf/elf_loader.h"
#include "kernel/filesystem/initramfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"
#include "kernel/halt.h"
#include "kernel/input/input.h"
#include "kernel/arch/power.h"
#include "kernel/klogger.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/process_arguments.h"
#include "kernel/scheduler.h"
#if defined(FEATHEROS_SMP_TEST) || defined(FEATHEROS_STORAGE_TEST)
#include "kernel/kernel_test.h"
#endif
#include "kernel/smp.h"
#include "kernel/string/format.h"
#include "kernel/storage/block_device.h"
#include "kernel/time/timer.h"
#include "kernel/memory/virtual_memory.h"
#include "user/calendar.h"
#include "user/syscall_abi.h"

extern "C"
{
#include <limine.h>
}

namespace FeatherOS::KernelRuntime
{
namespace
{

Framebuffer framebuffer;
Term term;

bool display_ready = false;

void InitializeDisplay(KLogger& logger, limine_framebuffer* framebuffer_response)
{
    if (framebuffer_response != nullptr && framebuffer.init(framebuffer_response))
    {
        term.Init(&framebuffer);
        logger.AttachTerminal(&term);
        display_ready = true;
        logger.Info("Framebuffer console initialized");
    }
    else
    {
        logger.Warning("No supported framebuffer; using serial console only");
    }
}

void MountInitramfs(KLogger& logger, const BootInfo& boot_info)
{
    if (!Initramfs::Mount(boot_info.initramfs_data, boot_info.initramfs_size))
    {
        Panic("Initramfs archive is missing or invalid");
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
#if defined(ARCH_x86_64)
    constexpr char test_path[] = "test-x86_64.elf";
    constexpr uint16_t user_machine = ELF::MachineX86_64;
#elif defined(ARCH_aarch64)
    constexpr char test_path[] = "test-aarch64.elf";
    constexpr uint16_t user_machine = ELF::MachineAArch64;
#endif

    Initramfs::File file;
    ELF::Image image;
    if (!Initramfs::Open(test_path, &file) ||
        !ELF::Parse(file.data, file.size, user_machine, &image))
    {
        Panic("Initramfs ELF test executable is invalid");
    }

    Process::UserProgram console_program;
    Process::UserProgram isolation_program;
    Process::InitializeUserProgram(&console_program);
    Process::InitializeUserProgram(&isolation_program);
#if defined(FEATHEROS_USER_PREEMPTION_TEST)
    constexpr bool user_preemption_test = true;
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
        Process::LoadUserProgram(image, console_arguments, &console_program);
    const bool isolation_loaded =
        console_loaded && Process::LoadUserProgram(image, isolation_arguments, &isolation_program);
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
#if defined(FEATHEROS_USER_PREEMPTION_TEST)
    if (!Time::InitializeTimer(100, boot_info.platform_data))
    {
        Panic("Could not initialize timer for user-mode preemption test");
    }
#endif
    if (!Scheduler::CreateTask(Process::RunUserProgram, &console_program, &console_task) ||
        !Scheduler::CreateTask(Process::RunUserProgram, &isolation_program, &isolation_task))
    {
#if defined(FEATHEROS_USER_PREEMPTION_TEST)
        Time::StopTimer();
#endif
        Panic("Could not create user-program scheduler tasks");
    }
#if defined(FEATHEROS_USER_PREEMPTION_TEST)
    Scheduler::EnablePreemption();
#endif

    uint64_t console_exit_status = UINT64_MAX;
    uint64_t isolation_exit_status = UINT64_MAX;
    const bool console_completed = Scheduler::Wait(console_task, &console_exit_status);
    const bool isolation_completed =
        console_completed && Scheduler::Wait(isolation_task, &isolation_exit_status);
    const bool tasks_reaped = isolation_completed && Scheduler::ReapTask(console_task) &&
                              Scheduler::ReapTask(isolation_task);
#if defined(FEATHEROS_USER_PREEMPTION_TEST)
    Scheduler::DisablePreemption();
    Time::StopTimer();
#endif
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
#if defined(FEATHEROS_USER_PREEMPTION_TEST)
    Serial::Write("KERN/SCHEDULER: user-mode preemption passed\n");
#endif
#if defined(ARCH_x86_64)
    logger.Info("ELF64: ring 3 console and exit syscalls passed");
    logger.Info("ELF64: ring 3 kernel-memory isolation passed");
#elif defined(ARCH_aarch64)
    logger.Info("ELF64: EL0 console and exit syscalls passed");
    logger.Info("ELF64: EL0 kernel-memory isolation passed");
#endif
    logger.Info("ELF64: test image loaded into user address space");
}

void MountAndVerifyDisk(KLogger& logger, const BootInfo& boot_info)
{
    if (!BlockDevice::Initialize(boot_info.platform_data, boot_info.hhdm_offset) ||
        BlockDevice::SectorCount() < 2)
    {
        Panic("Virtio block device initialization failed");
    }
    if (!VFS::MountBlockDevice())
    {
        Panic("MBR partition or FAT16 mount failed");
    }
#if defined(FEATHEROS_STORAGE_TEST)
    KernelTest::RunStorageTests();
#endif

    constexpr char expected_contents[] = "FeatherOS FAT16 volume OK";
    VFS::FileInfo file_info;
    VFS::FileHandle file;
    if (!VFS::Stat("/BLOCK.TXT", &file_info) || file_info.size != sizeof(expected_contents) - 1)
    {
        Panic("VFS could not stat FAT16 BLOCK.TXT");
    }
    if (!VFS::Open("/BLOCK.TXT", &file))
    {
        Panic("VFS could not open FAT16 BLOCK.TXT");
    }

    char contents[sizeof(expected_contents) - 1];
    uint64_t bytes_read;
    if (!VFS::Read(&file, contents, sizeof(contents), &bytes_read) ||
        bytes_read != sizeof(contents))
    {
        Panic("VFS could not read FAT16 BLOCK.TXT");
    }
    for (size_t index = 0; index < sizeof(contents); ++index)
    {
        if (contents[index] != expected_contents[index])
        {
            Panic("VFS returned invalid FAT16 file data");
        }
    }
    if (!VFS::Seek(&file, 0))
    {
        Panic("VFS could not seek in FAT16 BLOCK.TXT");
    }
    VFS::Close(&file);
    logger.Info("VFS: FAT16 BLOCK.TXT read passed");
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
#if defined(FEATHEROS_SMP_TEST)
    KernelTest::RunSmpTests();
#endif
}

[[noreturn]] void RunConsole(KLogger& logger, const BootInfo& boot_info)
{
    if (!Time::InitializeTimer(100, boot_info.platform_data))
    {
        Panic("Periodic timer initialization failed");
    }
    Time::WaitForNextTick();
    logger.Info("Timer started at 100 Hz");
    // Secondary CPUs start their timers at the boot CPU's rate.
    StartSecondaryCpus(logger, boot_info.smp);
    logger.Info("Kernel initialization complete");
    if (!Arch::Keyboard::Initialize(boot_info.platform_data, boot_info.hhdm_offset))
    {
        logger.Warning("Keyboard device unavailable; use serial input");
    }
    Console::Initialize(display_ready ? &term : nullptr);

    // init starts and supervises the services in /init.conf; restart it if it ever exits.
    for (;;)
    {
        Process::ProcessId init;
        if (Process::Spawn("init", 0, 0, &init) != Process::SpawnStatus::Started)
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
    InitializeDisplay(logger, boot_info.framebuffer);
    MountInitramfs(logger, boot_info);
    LoadTestExecutable(logger, boot_info);
    MountAndVerifyDisk(logger, boot_info);
    LogBootSummary(logger, boot_info.memory_summary);
    RunConsole(logger, boot_info);
}

} // namespace FeatherOS::KernelRuntime