#include "process.h"

#include "kernel/arch/cpu_features.h"
#include "kernel/arch/rtc.h"
#include "kernel/console.h"
#include "kernel/cpu.h"
#include "kernel/filesystem/initramfs.h"
#include "kernel/files.h"
#include "kernel/filesystem/path.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/tmpfs.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/panic.h"
#include "kernel/power.h"
#include "kernel/process_arguments.h"
#include "kernel/random.h"
#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "kernel/syscall.h"
#include "kernel/time/timer.h"
#if defined(ARCH_x86_64)
#include "kernel/arch/x86_64/exceptions.h"
#elif defined(ARCH_aarch64)
#include "kernel/arch/aarch64/exceptions.h"
#endif

#include <stddef.h>

namespace FeatherOS::Process
{
namespace
{

// Covers every user page: ELF segments, the stack, and the arguments block.
constexpr uint32_t DefaultProcessPageLimit = 1024;
constexpr uint32_t MaximumChildrenPerProcess = 16;
constexpr size_t MaximumCommandNameLength = 32;
// Room for the shell's prompt, which shows its working directory.
constexpr size_t MaximumPromptLength = User::MaximumPathLength + 8;
// init, and so the shell, start in /home, or in / on a disk without one.
constexpr char HomeDirectory[] = "/home";

#if defined(ARCH_x86_64)
constexpr uint16_t UserMachine = ELF::MachineX86_64;
constexpr char UserProgramSuffix[] = "-x86_64.elf";
#elif defined(ARCH_aarch64)
constexpr uint16_t UserMachine = ELF::MachineAArch64;
constexpr char UserProgramSuffix[] = "-aarch64.elf";
#endif

struct ProcessRecord
{
    ProcessRecord* next;
    ProcessId id;
    ProcessId parent;
    uint32_t live_children;
    Scheduler::TaskId task;
    char name[MaximumCommandNameLength + 1];
    // Set for the console service: its descendants form Ctrl+C jobs.
    bool session_leader;
    Files::Table files;
    // Relative paths in file syscalls start here; children inherit it.
    char working_directory[User::MaximumPathLength];
    // Bit n is set once descriptor n, one of the console descriptors 0-2, has been closed.
    uint8_t closed_console_descriptors;
    // The rest of a console line that a read did not take, with its '\n'.
    char console_input[User::MaximumCommandLineLength];
    uint8_t console_input_length;
    uint8_t console_input_offset;
    UserProgram program;
};

uint64_t hhdm_offset = 0;
Sync::SpinLock process_lock;
ProcessRecord* process_list = nullptr;
ProcessId next_process_id = 1;
// Only this process may read console input; zero means the kernel itself.
ProcessId foreground_process = 0;

volatile uint8_t* PhysicalBytes(uint64_t physical_address)
{
    if (physical_address > UINT64_MAX - hhdm_offset || hhdm_offset + physical_address > UINTPTR_MAX)
    {
        return nullptr;
    }
    return reinterpret_cast<volatile uint8_t*>(
        static_cast<uintptr_t>(hhdm_offset + physical_address));
}

bool AllocateZeroedPage(uint64_t* physical_address)
{
    if (!Memory::AllocatePageFrame(physical_address))
    {
        return false;
    }
    volatile uint8_t* page = PhysicalBytes(*physical_address);
    if (page == nullptr)
    {
        Memory::FreePageFrame(*physical_address);
        return false;
    }
    auto* words = reinterpret_cast<volatile uint64_t*>(page);
    for (uint64_t index = 0; index < Memory::PageSize / sizeof(uint64_t); ++index)
    {
        words[index] = 0;
    }
    return true;
}

bool AllocateProgramPage(UserProgram* program, uint64_t* physical_address)
{
    if (program->mapped_page_count >= program->page_limit || !AllocateZeroedPage(physical_address))
    {
        return false;
    }
    ++program->mapped_page_count;
    return true;
}

bool MapUserSegment(void* context, uint64_t virtual_address, uint64_t memory_size,
                    uint32_t segment_flags)
{
    auto* program = static_cast<UserProgram*>(context);
    if (memory_size == 0)
    {
        return true;
    }
    // Segments are copied through their own addresses, so they must lie in user space.
    if (program == nullptr || (segment_flags & ELF::SegmentFlagRead) == 0 ||
        virtual_address >= UserAddressLimit || memory_size > UserAddressLimit - virtual_address)
    {
        return false;
    }

    const uint64_t page_size = Memory::PageSize;
    uint64_t page_address = virtual_address & ~(page_size - 1);
    const uint64_t segment_end = virtual_address + memory_size;
    const uint64_t page_end = (segment_end + page_size - 1) & ~(page_size - 1);
    const Memory::PageFlags page_flags =
        Memory::PageFlags::Read | Memory::PageFlags::Write | Memory::PageFlags::User;

    while (page_address < page_end)
    {
        uint64_t physical_address;
        if (!AllocateProgramPage(program, &physical_address))
        {
            return false;
        }
        if (!Memory::MapPage(program->address_space, page_address, physical_address, page_flags))
        {
            Memory::FreePageFrame(physical_address);
            return false;
        }
        page_address += page_size;
    }
    if (page_end > program->heap_start)
    {
        program->heap_start = page_end;
    }
    return true;
}

bool CopyUserSegment(void*, uint64_t virtual_address, const uint8_t* source, uint64_t size)
{
    if (source == nullptr || virtual_address > UINTPTR_MAX ||
        size > UINTPTR_MAX - static_cast<uintptr_t>(virtual_address))
    {
        return false;
    }
    auto* destination =
        reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(virtual_address));
    const Arch::CpuFeatures::UserAccessWindow user_access;
    for (uint64_t index = 0; index < size; ++index)
    {
        destination[index] = source[index];
    }
    return true;
}

bool ZeroUserSegment(void*, uint64_t virtual_address, uint64_t size)
{
    if (virtual_address > UINTPTR_MAX ||
        size > UINTPTR_MAX - static_cast<uintptr_t>(virtual_address))
    {
        return false;
    }
    auto* destination =
        reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(virtual_address));
    const Arch::CpuFeatures::UserAccessWindow user_access;
    for (uint64_t index = 0; index < size; ++index)
    {
        destination[index] = 0;
    }
    return true;
}

bool ProtectUserSegment(void* context, uint64_t virtual_address, uint64_t memory_size,
                        uint32_t segment_flags)
{
    auto* program = static_cast<UserProgram*>(context);
    if (program == nullptr || (segment_flags & ELF::SegmentFlagRead) == 0 ||
        virtual_address > UINT64_MAX - memory_size)
    {
        return false;
    }
    if (memory_size == 0)
    {
        return true;
    }

    const uint64_t page_size = Memory::PageSize;
    uint64_t page_address = virtual_address & ~(page_size - 1);
    const uint64_t segment_end = virtual_address + memory_size;
    if (segment_end > UINT64_MAX - (page_size - 1))
    {
        return false;
    }
    const uint64_t page_end = (segment_end + page_size - 1) & ~(page_size - 1);
    Memory::PageFlags page_flags = Memory::PageFlags::Read | Memory::PageFlags::User;
    if ((segment_flags & ELF::SegmentFlagWrite) != 0)
    {
        page_flags = page_flags | Memory::PageFlags::Write;
    }
    if ((segment_flags & ELF::SegmentFlagExecute) != 0)
    {
        page_flags = page_flags | Memory::PageFlags::Execute;
    }

    while (page_address < page_end)
    {
        if (!Memory::ProtectPage(program->address_space, page_address, page_flags))
        {
            return false;
        }
        page_address += page_size;
    }
    return true;
}

bool MapProgramPages(const ELF::Image& image, const User::ProgramArguments& arguments,
                     UserProgram* program)
{
    if (!Memory::CreateAddressSpace(&program->address_space))
    {
        return false;
    }
    program->execution_context.address_space_root_physical_address =
        program->address_space.root_physical_address;

    // Segment data is copied through user addresses, so the new address space must be active.
    if (!Memory::ActivateAddressSpace(program->address_space))
    {
        return false;
    }
    const ELF::LoadOperations operations = {
        program, MapUserSegment, CopyUserSegment, ZeroUserSegment, ProtectUserSegment,
    };
    const uint64_t load_bias = image.position_independent ? program->layout.image_base : 0;
    if (!ELF::LoadSegments(image, load_bias, operations))
    {
        return false;
    }

    uint64_t stack_physical_address;
    if (!AllocateProgramPage(program, &stack_physical_address))
    {
        return false;
    }
    if (!Memory::MapPage(program->address_space, program->layout.stack_top - Memory::PageSize,
                         stack_physical_address,
                         Memory::PageFlags::Read | Memory::PageFlags::Write |
                             Memory::PageFlags::User))
    {
        Memory::FreePageFrame(stack_physical_address);
        return false;
    }

    uint64_t arguments_physical_address;
    if (!AllocateProgramPage(program, &arguments_physical_address))
    {
        return false;
    }
    volatile uint8_t* arguments_page = PhysicalBytes(arguments_physical_address);
    const auto* source = reinterpret_cast<const uint8_t*>(&arguments);
    for (size_t index = 0; index < sizeof(arguments); ++index)
    {
        arguments_page[index] = source[index];
    }
    if (!Memory::MapPage(program->address_space, program->layout.arguments_address,
                         arguments_physical_address,
                         Memory::PageFlags::Read | Memory::PageFlags::User))
    {
        Memory::FreePageFrame(arguments_physical_address);
        return false;
    }

    program->entry_point = image.entry_point + load_bias;
#if defined(ARCH_x86_64)
    // A function starts with rsp 8 bytes past a 16-byte boundary, as a call leaves it. Code that
    // keeps SSE values on the stack relies on that alignment.
    program->stack_pointer = program->layout.stack_top - 8;
#else
    program->stack_pointer = program->layout.stack_top;
#endif
    program->heap_start += program->layout.heap_offset * Memory::PageSize;
    program->heap_break = program->heap_start;
    program->argument_pointer = program->layout.arguments_address;
    return true;
}

void ReleaseUserProgram(UserProgram* program)
{
    if (program->address_space.root_physical_address != 0 &&
        !Memory::DestroyAddressSpace(&program->address_space))
    {
        Panic("Could not release a user program address space");
    }
    program->address_space.root_physical_address = 0;
}

void RemoveProcess(ProcessRecord* process)
{
    {
        Sync::SpinLockGuard guard(process_lock);
        for (ProcessRecord* parent = process_list; parent != nullptr; parent = parent->next)
        {
            if (parent->id == process->parent && parent->live_children > 0)
            {
                --parent->live_children;
            }
        }
        for (ProcessRecord** link = &process_list; *link != nullptr; link = &(*link)->next)
        {
            if (*link == process)
            {
                *link = process->next;
                break;
            }
        }
    }
    Memory::Deallocate(process);
}

ProcessRecord* FindProcessLocked(ProcessId process_id)
{
    for (ProcessRecord* process = process_list; process != nullptr; process = process->next)
    {
        if (process->id == process_id)
        {
            return process;
        }
    }
    return nullptr;
}

ProcessRecord* FindProcess(ProcessId process_id)
{
    Sync::SpinLockGuard guard(process_lock);
    return FindProcessLocked(process_id);
}

bool DescendsFromLocked(const ProcessRecord* process, ProcessId ancestor)
{
    for (; process != nullptr; process = FindProcessLocked(process->parent))
    {
        if (process->id == ancestor)
        {
            return true;
        }
    }
    return false;
}

void RequestKillLocked(ProcessRecord* process)
{
    // The task exists only once the program has loaded; before that there is nothing to stop.
    if (process->task == 0)
    {
        return;
    }
    process->program.execution_context.kill_requested = true;
    Scheduler::WakeTask(process->task);
}

// Builds "<name>-<arch>.elf" from the first word of command_line, which may only contain
// letters, digits, '_' or '-'.
bool BuildProgramPath(const char* command_line,
                      char (&path)[MaximumCommandNameLength + sizeof(UserProgramSuffix)])
{
    while (*command_line == ' ')
    {
        ++command_line;
    }
    size_t length = 0;
    for (; command_line[length] != '\0' && command_line[length] != ' '; ++length)
    {
        const char character = command_line[length];
        const bool valid =
            (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' || character == '-';
        if (!valid || length == MaximumCommandNameLength)
        {
            return false;
        }
        path[length] = character;
    }
    if (length == 0)
    {
        return false;
    }
    for (size_t index = 0; index < sizeof(UserProgramSuffix); ++index)
    {
        path[length + index] = UserProgramSuffix[index];
    }
    return true;
}

// User pointers are resolved through the process page tables and accessed through the direct
// map, so invalid or kernel addresses fail cleanly instead of faulting in the kernel.
volatile uint8_t* UserByte(const ProcessRecord& process, uint64_t address, bool write)
{
    uint64_t physical_address;
    bool writable;
    if (!Memory::TranslateUserPage(process.program.address_space, address, &physical_address,
                                   &writable) ||
        (write && !writable))
    {
        return nullptr;
    }
    return PhysicalBytes(physical_address);
}

bool UserRangeWritable(const ProcessRecord& process, uint64_t address, uint64_t size)
{
    if (size == 0 || address > UINT64_MAX - (size - 1))
    {
        return false;
    }
    for (uint64_t index = 0; index < size; ++index)
    {
        if (UserByte(process, address + index, true) == nullptr)
        {
            return false;
        }
    }
    return true;
}

bool UserRangeReadable(const ProcessRecord& process, uint64_t address, uint64_t size)
{
    if (size == 0 || address > UINT64_MAX - (size - 1))
    {
        return false;
    }
    for (uint64_t index = 0; index < size; ++index)
    {
        if (UserByte(process, address + index, false) == nullptr)
        {
            return false;
        }
    }
    return true;
}

bool CopyFromUser(const ProcessRecord& process, uint64_t address, void* destination, size_t size)
{
    if (!UserRangeReadable(process, address, size))
    {
        return false;
    }
    auto* bytes = static_cast<uint8_t*>(destination);
    for (size_t index = 0; index < size; ++index)
    {
        bytes[index] = *UserByte(process, address + index, false);
    }
    return true;
}

bool CopyToUser(const ProcessRecord& process, uint64_t address, const void* source, size_t size)
{
    if (!UserRangeWritable(process, address, size))
    {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(source);
    for (size_t index = 0; index < size; ++index)
    {
        *UserByte(process, address + index, true) = bytes[index];
    }
    return true;
}

// 0, SyscallErrorBadAddress, or SyscallErrorInvalidArgument when no terminator fits.
uint64_t CopyStringFromUser(const ProcessRecord& process, uint64_t address, char* buffer,
                            size_t capacity)
{
    for (size_t index = 0; index < capacity; ++index)
    {
        if (address > UINT64_MAX - index)
        {
            return User::SyscallErrorBadAddress;
        }
        volatile uint8_t* byte = UserByte(process, address + index, false);
        if (byte == nullptr)
        {
            return User::SyscallErrorBadAddress;
        }
        buffer[index] = static_cast<char>(*byte);
        if (buffer[index] == '\0')
        {
            return 0;
        }
    }
    return User::SyscallErrorInvalidArgument;
}

// Copies a path argument and resolves it against the process's working directory.
uint64_t CopyPathFromUser(const ProcessRecord& process, uint64_t address,
                          char (&path)[User::MaximumPathLength])
{
    char text[User::MaximumPathLength];
    const uint64_t copied = CopyStringFromUser(process, address, text, sizeof(text));
    if (copied == User::SyscallErrorBadAddress)
    {
        return copied;
    }
    return copied == 0 && Path::Resolve(process.working_directory, text, path) &&
                   Path::ReplaceProcSelf(path, process.id)
               ? 0
               : User::SyscallErrorNameTooLong;
}

// Copies a Spawn environment block: strings up to the empty one that ends it.
uint64_t CopyEnvironmentFromUser(const ProcessRecord& process, uint64_t address,
                                 char (&block)[User::MaximumEnvironmentSize])
{
    bool after_terminator = true;
    for (size_t index = 0; index < sizeof(block); ++index)
    {
        if (address > UINT64_MAX - index)
        {
            return User::SyscallErrorBadAddress;
        }
        volatile uint8_t* byte = UserByte(process, address + index, false);
        if (byte == nullptr)
        {
            return User::SyscallErrorBadAddress;
        }
        block[index] = static_cast<char>(*byte);
        if (block[index] == '\0' && after_terminator)
        {
            return 0;
        }
        after_terminator = block[index] == '\0';
    }
    return User::SyscallErrorInvalidArgument;
}

bool WriteCharacterSyscall(void*, char character)
{
    Console::WriteCharacter(character);
    return true;
}

uint64_t ReadLineSyscall(void* context, uint64_t buffer, uint64_t capacity, uint64_t prompt)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    char prompt_text[MaximumPromptLength];
    const uint64_t copied = CopyStringFromUser(*process, prompt, prompt_text, sizeof(prompt_text));
    if (copied != 0)
    {
        return copied;
    }
    char line[User::MaximumCommandLineLength];
    const size_t line_capacity =
        capacity < sizeof(line) ? static_cast<size_t>(capacity) : sizeof(line);
    if (line_capacity == 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    // Validate the destination before blocking so a bad buffer fails immediately.
    if (!UserRangeWritable(*process, buffer, line_capacity))
    {
        return User::SyscallErrorBadAddress;
    }
    if (process->id != foreground_process)
    {
        return User::SyscallErrorNotForeground;
    }
    size_t length;
    if (!Console::ReadLine(prompt_text, line, line_capacity, &length))
    {
        return User::SyscallErrorInterrupted;
    }
    if (!CopyToUser(*process, buffer, line, length + 1))
    {
        return User::SyscallErrorBadAddress;
    }
    return length;
}

uint64_t SpawnSyscall(void* context, uint64_t command_line, uint64_t flags, uint64_t environment)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    if ((flags & ~(User::SpawnBackground | User::SpawnSessionLeader)) != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    char line[User::MaximumCommandLineLength];
    const uint64_t copied = CopyStringFromUser(*process, command_line, line, sizeof(line));
    if (copied != 0)
    {
        return copied;
    }
    char environment_block[User::MaximumEnvironmentSize];
    if (environment != 0)
    {
        const uint64_t environment_copied =
            CopyEnvironmentFromUser(*process, environment, environment_block);
        if (environment_copied != 0)
        {
            return environment_copied;
        }
    }
    ProcessId child;
    switch (Spawn(line, environment != 0 ? environment_block : nullptr, process->id, flags, &child))
    {
    case SpawnStatus::Started:
        return child;
    case SpawnStatus::NotFound:
        return User::SyscallErrorNotFound;
    case SpawnStatus::InvalidArguments:
        return User::SyscallErrorInvalidArgument;
    case SpawnStatus::InvalidExecutable:
        return User::SyscallErrorNotExecutable;
    case SpawnStatus::NoResources:
        return User::SyscallErrorNoMemory;
    default:
        return User::SyscallErrorFailed;
    }
}

uint64_t WaitSyscall(void* context, uint64_t process_id, uint64_t status_pointer, uint64_t flags)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    if ((flags & ~User::WaitNoHang) != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    const ProcessRecord* child = FindProcess(process_id);
    if (child == nullptr || child->parent != process->id)
    {
        return User::SyscallErrorNotChild;
    }
    if (!UserRangeWritable(*process, status_pointer, sizeof(uint64_t)))
    {
        return User::SyscallErrorBadAddress;
    }
    if ((flags & User::WaitNoHang) != 0 &&
        Scheduler::GetState(child->task) != Scheduler::TaskState::Complete)
    {
        return User::SyscallErrorWouldBlock;
    }
    uint64_t exit_status;
    if (!Wait(process_id, &exit_status))
    {
        return User::SyscallErrorFailed;
    }
    if (!CopyToUser(*process, status_pointer, &exit_status, sizeof(exit_status)))
    {
        return User::SyscallErrorBadAddress;
    }
    return 0;
}

// Unmaps the whole pages between the new and the old break, and frees their frames. The page
// holding the new break stays, with its old contents.
uint64_t ShrinkHeap(UserProgram& program, uint64_t decrement)
{
    const uint64_t previous_break = program.heap_break;
    if (decrement > previous_break - program.heap_start)
    {
        return User::SyscallErrorInvalidArgument;
    }
    const uint64_t page_size = Memory::PageSize;
    const uint64_t new_break = previous_break - decrement;
    const uint64_t page_end = (previous_break + page_size - 1) & ~(page_size - 1);
    for (uint64_t page_address = (new_break + page_size - 1) & ~(page_size - 1);
         page_address < page_end; page_address += page_size)
    {
        uint64_t physical_address;
        if (Memory::UnmapUserPage(program.address_space, page_address, &physical_address))
        {
            Memory::FreePageFrame(physical_address);
            --program.mapped_page_count;
        }
    }
    program.heap_break = new_break;
    return previous_break;
}

// A negative increment, as a two's-complement value, shrinks the heap.
uint64_t ExtendHeapSyscall(void* context, uint64_t increment)
{
    UserProgram& program = static_cast<ProcessRecord*>(context)->program;
    if (static_cast<int64_t>(increment) < 0)
    {
        return ShrinkHeap(program, 0 - increment);
    }
    const uint64_t previous_break = program.heap_break;
    // The heap may grow up to, but never into, the stack region's guard page.
    const uint64_t stack_region_base =
        program.layout.stack_top - UserStackRegionPages * Memory::PageSize;
    if (previous_break > stack_region_base || increment > stack_region_base - previous_break)
    {
        return User::SyscallErrorInvalidArgument;
    }
    const uint64_t page_size = Memory::PageSize;
    const uint64_t new_break = previous_break + increment;
    uint64_t page_address = (previous_break + page_size - 1) & ~(page_size - 1);
    const uint64_t page_end = (new_break + page_size - 1) & ~(page_size - 1);
    // Fail before mapping anything when the page limit cannot cover the request.
    if ((page_end - page_address) / page_size > program.page_limit - program.mapped_page_count)
    {
        return User::SyscallErrorNoMemory;
    }
    for (; page_address < page_end; page_address += page_size)
    {
        // A call that ran out of physical memory part-way leaves its pages mapped above the
        // break; a later call reuses them.
        uint64_t mapped_address;
        bool writable;
        if (Memory::TranslateUserPage(program.address_space, page_address, &mapped_address,
                                      &writable))
        {
            continue;
        }
        uint64_t physical_address;
        if (!AllocateProgramPage(&program, &physical_address))
        {
            return User::SyscallErrorNoMemory;
        }
        if (!Memory::MapPage(program.address_space, page_address, physical_address,
                             Memory::PageFlags::Read | Memory::PageFlags::Write |
                                 Memory::PageFlags::User))
        {
            Memory::FreePageFrame(physical_address);
            --program.mapped_page_count;
            return User::SyscallErrorNoMemory;
        }
    }
    program.heap_break = new_break;
    return previous_break;
}

uint64_t SleepSyscall(void*, uint64_t milliseconds)
{
    const uint64_t ticks_per_second = Time::TicksPerSecond();
    constexpr uint64_t MaximumMilliseconds = 24ULL * 60 * 60 * 1000;
    if (ticks_per_second == 0 || milliseconds > MaximumMilliseconds)
    {
        return User::SyscallErrorInvalidArgument;
    }
    // Round up so the program sleeps at least as long as it asked.
    const uint64_t ticks = (milliseconds * ticks_per_second + 999) / 1000;
    Scheduler::SleepUntil(Time::TickCount() + ticks);
    return 0;
}

uint64_t ReadClockSyscall(void*)
{
    uint64_t seconds;
    return Arch::RealTimeClock::ReadUnixSeconds(&seconds) ? seconds : User::SyscallErrorFailed;
}

uint64_t ReadUptimeSyscall(void*)
{
    const uint64_t ticks_per_second = Time::TicksPerSecond();
    return ticks_per_second == 0 ? User::SyscallErrorFailed
                                 : Time::TickCount() * 1000 / ticks_per_second;
}

uint64_t CurrentCpuSyscall(void*)
{
    return Cpu::CurrentIndex();
}

// File data moves through a kernel buffer of this size, and each chunk is a separate filesystem
// call.
constexpr uint64_t FileChunkSize = 512;

uint64_t OpenSyscall(void* context, uint64_t path, uint64_t flags)
{
    auto* process = static_cast<ProcessRecord*>(context);
    char path_text[User::MaximumPathLength];
    const uint64_t copied = CopyPathFromUser(*process, path, path_text);
    if (copied != 0)
    {
        return copied;
    }
    return Files::Open(&process->files, path_text, flags);
}

bool IsConsoleDescriptor(const ProcessRecord& process, uint64_t descriptor)
{
    return descriptor < User::FirstFileDescriptor &&
           (process.closed_console_descriptors & (1U << descriptor)) == 0;
}

// Console reads return one line at a time, ending in '\n'; what does not fit the caller's buffer
// is kept for its next read.
uint64_t ReadConsole(ProcessRecord* process, uint64_t buffer, uint64_t length)
{
    if (!UserRangeWritable(*process, buffer, length))
    {
        return User::SyscallErrorBadAddress;
    }
    if (process->console_input_offset == process->console_input_length)
    {
        if (process->id != foreground_process)
        {
            return User::SyscallErrorNotForeground;
        }
        size_t line_length;
        if (!Console::ReadLine("", process->console_input, sizeof(process->console_input),
                               &line_length))
        {
            return User::SyscallErrorInterrupted;
        }
        process->console_input[line_length] = '\n';
        process->console_input_length = static_cast<uint8_t>(line_length + 1);
        process->console_input_offset = 0;
    }
    const uint64_t available = process->console_input_length - process->console_input_offset;
    const uint64_t count = length < available ? length : available;
    if (!CopyToUser(*process, buffer, process->console_input + process->console_input_offset,
                    count))
    {
        return User::SyscallErrorBadAddress;
    }
    process->console_input_offset = static_cast<uint8_t>(process->console_input_offset + count);
    return count;
}

uint64_t ReadSyscall(void* context, uint64_t descriptor, uint64_t buffer, uint64_t length)
{
    auto* process = static_cast<ProcessRecord*>(context);
    uint8_t chunk[FileChunkSize];
    if (descriptor < User::FirstFileDescriptor)
    {
        if (!IsConsoleDescriptor(*process, descriptor))
        {
            return User::SyscallErrorBadDescriptor;
        }
        return length == 0 ? 0 : ReadConsole(process, buffer, length);
    }
    if (length == 0)
    {
        return Files::Read(&process->files, descriptor, nullptr, 0);
    }
    // Checking the whole range first keeps a bad buffer from consuming file data.
    if (!UserRangeWritable(*process, buffer, length))
    {
        return User::SyscallErrorBadAddress;
    }
    uint64_t total = 0;
    while (total < length)
    {
        const uint64_t wanted = length - total < FileChunkSize ? length - total : FileChunkSize;
        const uint64_t count = Files::Read(&process->files, descriptor, chunk, wanted);
        if (User::IsSyscallError(count))
        {
            return total > 0 ? total : count;
        }
        if (count > 0 && !CopyToUser(*process, buffer + total, chunk, count))
        {
            return User::SyscallErrorBadAddress;
        }
        total += count;
        if (count < wanted)
        {
            break;
        }
    }
    return total;
}

uint64_t WriteSyscall(void* context, uint64_t descriptor, uint64_t buffer, uint64_t length)
{
    auto* process = static_cast<ProcessRecord*>(context);
    uint8_t chunk[FileChunkSize];
    const bool console = descriptor < User::FirstFileDescriptor;
    if (console && !IsConsoleDescriptor(*process, descriptor))
    {
        return User::SyscallErrorBadDescriptor;
    }
    if (length == 0)
    {
        return console ? 0 : Files::Write(&process->files, descriptor, nullptr, 0);
    }
    if (!UserRangeReadable(*process, buffer, length))
    {
        return User::SyscallErrorBadAddress;
    }
    uint64_t total = 0;
    while (total < length)
    {
        const uint64_t wanted = length - total < FileChunkSize ? length - total : FileChunkSize;
        if (!CopyFromUser(*process, buffer + total, chunk, wanted))
        {
            return User::SyscallErrorBadAddress;
        }
        if (console)
        {
            Console::Write(reinterpret_cast<const char*>(chunk), wanted);
            total += wanted;
            continue;
        }
        const uint64_t count = Files::Write(&process->files, descriptor, chunk, wanted);
        if (User::IsSyscallError(count))
        {
            return total > 0 ? total : count;
        }
        total += count;
        if (count < wanted)
        {
            break;
        }
    }
    return total;
}

uint64_t CloseSyscall(void* context, uint64_t descriptor)
{
    auto* process = static_cast<ProcessRecord*>(context);
    if (descriptor < User::FirstFileDescriptor)
    {
        if (!IsConsoleDescriptor(*process, descriptor))
        {
            return User::SyscallErrorBadDescriptor;
        }
        process->closed_console_descriptors |= static_cast<uint8_t>(1U << descriptor);
        return 0;
    }
    return Files::Close(&process->files, descriptor);
}

uint64_t SeekSyscall(void* context, uint64_t descriptor, uint64_t offset, uint64_t whence)
{
    auto* process = static_cast<ProcessRecord*>(context);
    if (descriptor < User::FirstFileDescriptor)
    {
        return IsConsoleDescriptor(*process, descriptor) ? User::SyscallErrorNotSeekable
                                                         : User::SyscallErrorBadDescriptor;
    }
    return Files::Seek(&process->files, descriptor, offset, whence);
}

// Copies the path argument and passes it to one of the path-only Files calls.
uint64_t PathSyscall(void* context, uint64_t path, uint64_t (*operation)(const char*))
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    char path_text[User::MaximumPathLength];
    const uint64_t copied = CopyPathFromUser(*process, path, path_text);
    if (copied != 0)
    {
        return copied;
    }
    return operation(path_text);
}

uint64_t RemoveSyscall(void* context, uint64_t path)
{
    return PathSyscall(context, path, Files::Remove);
}

uint64_t MakeDirectorySyscall(void* context, uint64_t path)
{
    return PathSyscall(context, path, Files::MakeDirectory);
}

uint64_t RemoveDirectorySyscall(void* context, uint64_t path)
{
    return PathSyscall(context, path, Files::RemoveDirectory);
}

uint64_t ChangeDirectorySyscall(void* context, uint64_t path)
{
    auto* process = static_cast<ProcessRecord*>(context);
    char path_text[User::MaximumPathLength];
    const uint64_t copied = CopyPathFromUser(*process, path, path_text);
    if (copied != 0)
    {
        return copied;
    }
    const uint64_t result = Files::CheckDirectory(path_text);
    if (User::IsSyscallError(result))
    {
        return result;
    }
    for (size_t index = 0; index < sizeof(path_text); ++index)
    {
        process->working_directory[index] = path_text[index];
        if (path_text[index] == '\0')
        {
            break;
        }
    }
    return 0;
}

uint64_t ShutdownSyscall(void* context, uint64_t action)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    // Only init (started by the kernel) and session leaders such as the shell may stop the system.
    if (process->parent != 0 && !process->session_leader)
    {
        return User::SyscallErrorNotPermitted;
    }
    if (action != User::ShutdownPowerOff && action != User::ShutdownRestart)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Power::Shutdown(action == User::ShutdownRestart ? Power::Action::Restart
                                                    : Power::Action::PowerOff,
                    process->id);
}

uint64_t CurrentDirectorySyscall(void* context, uint64_t buffer, uint64_t capacity)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    uint64_t length = 0;
    while (process->working_directory[length] != '\0')
    {
        ++length;
    }
    if (capacity <= length)
    {
        return User::SyscallErrorRange;
    }
    if (!CopyToUser(*process, buffer, process->working_directory, length + 1))
    {
        return User::SyscallErrorBadAddress;
    }
    return length;
}

uint64_t ReadDirectorySyscall(void* context, uint64_t path, uint64_t index, uint64_t entry)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    char path_text[User::MaximumPathLength];
    User::DirectoryEntry directory_entry;
    const uint64_t copied = CopyPathFromUser(*process, path, path_text);
    if (copied != 0)
    {
        return copied;
    }
    if (!UserRangeWritable(*process, entry, sizeof(directory_entry)))
    {
        return User::SyscallErrorBadAddress;
    }
    const uint64_t result = Files::ReadDirectory(path_text, index, &directory_entry);
    if (User::IsSyscallError(result))
    {
        return result;
    }
    return CopyToUser(*process, entry, &directory_entry, sizeof(directory_entry))
               ? 0
               : User::SyscallErrorBadAddress;
}

uint64_t CopyStatusToUser(const ProcessRecord& process, uint64_t address,
                          const User::FileStatus& status)
{
    return CopyToUser(process, address, &status, sizeof(status)) ? 0 : User::SyscallErrorBadAddress;
}

uint64_t StatSyscall(void* context, uint64_t path, uint64_t status)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    char path_text[User::MaximumPathLength];
    const uint64_t copied = CopyPathFromUser(*process, path, path_text);
    if (copied != 0)
    {
        return copied;
    }
    User::FileStatus file_status = {};
    const uint64_t result = Files::Stat(path_text, &file_status);
    return User::IsSyscallError(result) ? result : CopyStatusToUser(*process, status, file_status);
}

uint64_t StatDescriptorSyscall(void* context, uint64_t descriptor, uint64_t status)
{
    auto* process = static_cast<ProcessRecord*>(context);
    User::FileStatus file_status = {};
    if (descriptor < User::FirstFileDescriptor)
    {
        if (!IsConsoleDescriptor(*process, descriptor))
        {
            return User::SyscallErrorBadDescriptor;
        }
        file_status.type = User::FileType::Console;
        return CopyStatusToUser(*process, status, file_status);
    }
    const uint64_t result = Files::StatDescriptor(&process->files, descriptor, &file_status);
    return User::IsSyscallError(result) ? result : CopyStatusToUser(*process, status, file_status);
}

uint64_t ProcessIdSyscall(void* context)
{
    return static_cast<const ProcessRecord*>(context)->id;
}

uint64_t ParentProcessIdSyscall(void* context)
{
    return static_cast<const ProcessRecord*>(context)->parent;
}

constexpr Syscall::Handlers ProcessSyscallHandlers = {
    WriteCharacterSyscall,
    ReadLineSyscall,
    SpawnSyscall,
    WaitSyscall,
    ReadClockSyscall,
    ExtendHeapSyscall,
    SleepSyscall,
    ReadUptimeSyscall,
    CurrentCpuSyscall,
    OpenSyscall,
    ReadSyscall,
    WriteSyscall,
    CloseSyscall,
    SeekSyscall,
    RemoveSyscall,
    ReadDirectorySyscall,
    MakeDirectorySyscall,
    RemoveDirectorySyscall,
    ChangeDirectorySyscall,
    CurrentDirectorySyscall,
    ShutdownSyscall,
    StatSyscall,
    StatDescriptorSyscall,
    ProcessIdSyscall,
    ParentProcessIdSyscall,
};

// Children never outlive their parent: an exiting process stops and reaps any still running.
uint64_t RunProcess(void* context)
{
    auto* process = static_cast<ProcessRecord*>(context);
    const uint64_t exit_status = RunUserProgram(&process->program);
    Files::CloseAll(&process->files);
    for (;;)
    {
        ProcessId child = 0;
        {
            Sync::SpinLockGuard guard(process_lock);
            // A child may itself be waiting for its own children, so stop the whole subtree.
            for (ProcessRecord* candidate = process_list; candidate != nullptr;
                 candidate = candidate->next)
            {
                if (candidate != process && candidate->task != 0 &&
                    DescendsFromLocked(candidate, process->id))
                {
                    RequestKillLocked(candidate);
                    if (candidate->parent == process->id)
                    {
                        child = candidate->id;
                    }
                }
            }
        }
        if (child == 0 || !Wait(child, nullptr))
        {
            return exit_status;
        }
    }
}

// Processes are listed by ascending ID.
bool ProcessAt(uint32_t index, uint64_t* process_id)
{
    Sync::SpinLockGuard guard(process_lock);
    for (const ProcessRecord* process = process_list; process != nullptr; process = process->next)
    {
        uint32_t smaller = 0;
        for (const ProcessRecord* other = process_list; other != nullptr; other = other->next)
        {
            smaller += other->id < process->id ? 1 : 0;
        }
        if (smaller == index)
        {
            *process_id = process->id;
            return true;
        }
    }
    return false;
}

char StateLetter(Scheduler::TaskState state)
{
    switch (state)
    {
    case Scheduler::TaskState::Ready:
    case Scheduler::TaskState::Running:
        return 'R';
    case Scheduler::TaskState::Blocked:
    case Scheduler::TaskState::Sleeping:
        return 'S';
    default:
        return 'Z';
    }
}

bool DescribeProcess(uint64_t process_id, Procfs::ProcessStatus* status)
{
    Sync::SpinLockGuard guard(process_lock);
    const ProcessRecord* process = FindProcessLocked(process_id);
    if (process == nullptr)
    {
        return false;
    }
    uint32_t length = 0;
    for (; process->name[length] != '\0' && length < sizeof(status->name) - 1; ++length)
    {
        status->name[length] = process->name[length];
    }
    status->name[length] = '\0';
    status->state = StateLetter(Scheduler::GetState(process->task));
    status->process_id = process->id;
    status->parent_id = process->parent;
    status->pages = process->program.mapped_page_count;
    status->page_limit = process->program.page_limit;
    status->open_files = 0;
    for (const Files::OpenFile& file : process->files.files)
    {
        status->open_files += file.used ? 1 : 0;
    }
    return true;
}

void DescribeMemory(Procfs::MemoryStatus* status)
{
    Memory::PageAllocatorStats stats = {};
    Memory::GetPageAllocatorStats(&stats);
    status->total_bytes = stats.total_frames * Memory::PageSize;
    status->free_bytes = stats.free_frames * Memory::PageSize;
    status->kernel_heap_bytes = Memory::HeapMappedBytes();
    status->tmp_bytes = Tmpfs::UsedBytes();
}

} // namespace

UserLayout DefaultUserLayout()
{
    return {DefaultImageBase, UserStackTop, ArgumentsAddress, 0};
}

UserLayout RandomUserLayout()
{
    // Keep the whole stack region inside the window and leave room for the guard and arguments
    // pages above the stack top.
    constexpr uint64_t SlotCount = RandomStackWindowPages - UserStackRegionPages - 2;
    const uint64_t stack_top =
        RandomStackWindowBase +
        (UserStackRegionPages + Random::Next() % SlotCount) * Memory::PageSize;
    const uint64_t image_base =
        RandomImageWindowBase + (Random::Next() % RandomImageWindowPages) * Memory::PageSize;
    return {image_base, stack_top, stack_top + Memory::PageSize,
            Random::Next() % RandomHeapOffsetPages};
}

void Initialize(uint64_t offset)
{
    Procfs::SetSource({ProcessAt, DescribeProcess, DescribeMemory});
    hhdm_offset = offset;
    Sync::SpinLockGuard guard(process_lock);
    process_list = nullptr;
    next_process_id = 1;
    foreground_process = 0;
}

void InitializeUserProgram(UserProgram* program)
{
    program->address_space.root_physical_address = 0;
    program->mapped_page_count = 0;
    program->page_limit = DefaultProcessPageLimit;
    program->entry_point = 0;
    program->stack_pointer = 0;
    program->argument_pointer = 0;
    program->heap_start = 0;
    program->heap_break = 0;
    program->layout = DefaultUserLayout();
    program->execution_context.address_space_root_physical_address = 0;
    program->execution_context.kernel_stack_pointer = 0;
    program->execution_context.previous_kernel_stack_pointer = 0;
    program->execution_context.exit_status = User::ExitStatusFault;
    program->execution_context.active = false;
    program->execution_context.exit_requested = false;
    program->execution_context.preemption_enabled = false;
    program->execution_context.syscall_handlers = nullptr;
    program->execution_context.syscall_context = nullptr;
    program->execution_context.program = program;
    program->execution_context.kill_requested = false;
}

bool LoadUserProgram(const ELF::Image& image, const User::ProgramArguments& arguments,
                     UserProgram* program)
{
    if (program == nullptr)
    {
        return false;
    }
    program->execution_context.preemption_enabled = arguments.user_preemption_test != 0;
    const bool loaded = MapProgramPages(image, arguments, program);
    // Loading may run inside a syscall, so restore the caller's address space, not the kernel's.
    Scheduler::ActivateCurrentAddressSpace();
    return loaded;
}

bool HandleUserPageFault(UserExecutionContext* context, uint64_t fault_address)
{
    if (context == nullptr || context->program == nullptr)
    {
        return false;
    }
    auto* program = static_cast<UserProgram*>(context->program);
    const uint64_t stack_top = program->layout.stack_top;
    const uint64_t stack_region_base = stack_top - UserStackRegionPages * Memory::PageSize;
    if (fault_address < stack_region_base + Memory::PageSize || fault_address >= stack_top)
    {
        return false;
    }
    const uint64_t page_address = fault_address & ~(Memory::PageSize - 1);
    uint64_t physical_address;
    if (!AllocateProgramPage(program, &physical_address))
    {
        return false;
    }
    if (!Memory::MapPage(program->address_space, page_address, physical_address,
                         Memory::PageFlags::Read | Memory::PageFlags::Write |
                             Memory::PageFlags::User))
    {
        Memory::FreePageFrame(physical_address);
        --program->mapped_page_count;
        return false;
    }
    return true;
}

uint64_t RunUserProgram(void* context)
{
    auto* program = static_cast<UserProgram*>(context);
    if (program == nullptr || !Memory::ActivateAddressSpace(program->address_space))
    {
        Memory::ActivateKernelAddressSpace();
        return User::ExitStatusFault;
    }
#if defined(ARCH_x86_64)
    Arch::X86_64::RunUserTest(program->entry_point, program->stack_pointer,
                              program->argument_pointer, &program->execution_context);
#elif defined(ARCH_aarch64)
    Arch::AArch64::RunUserTest(program->entry_point, program->stack_pointer,
                               program->argument_pointer, &program->execution_context);
#endif
    Memory::ActivateKernelAddressSpace();
    return program->execution_context.exit_status;
}

SpawnStatus Spawn(const char* command_line, const char* environment, ProcessId parent,
                  uint64_t flags, ProcessId* process_id)
{
    if (command_line == nullptr || process_id == nullptr)
    {
        return SpawnStatus::InvalidArguments;
    }
    char path[MaximumCommandNameLength + sizeof(UserProgramSuffix)];
    Initramfs::File file;
    if (!BuildProgramPath(command_line, path) || !Initramfs::Open(path, &file))
    {
        return SpawnStatus::NotFound;
    }
    ELF::Image image;
    if (!ELF::Parse(file.data, file.size, UserMachine, &image))
    {
        return SpawnStatus::InvalidExecutable;
    }
    // The arguments block holds pointers into itself, so its address is chosen first.
    const UserLayout layout = RandomUserLayout();
    User::ProgramArguments arguments;
    if (!BuildProgramArguments(command_line, layout.arguments_address, &arguments) ||
        !CopyEnvironment(environment, User::MaximumEnvironmentSize, &arguments))
    {
        return SpawnStatus::InvalidArguments;
    }

    // Checked outside the process lock, since it reads the disk.
    const bool home_exists = parent == 0 && Files::CheckDirectory(HomeDirectory) == 0;
    auto* process = static_cast<ProcessRecord*>(Memory::AllocateZeroed(1, sizeof(ProcessRecord)));
    if (process == nullptr)
    {
        return SpawnStatus::NoResources;
    }
    ProcessId id = 0;
    {
        Sync::SpinLockGuard guard(process_lock);
        ProcessRecord* parent_record = nullptr;
        for (ProcessRecord* candidate = process_list; candidate != nullptr;
             candidate = candidate->next)
        {
            if (candidate->id == parent)
            {
                parent_record = candidate;
            }
        }
        const char* working_directory = home_exists ? HomeDirectory : "/";
        if (parent_record != nullptr)
        {
            if (parent_record->live_children == MaximumChildrenPerProcess)
            {
                Memory::Deallocate(process);
                return SpawnStatus::NoResources;
            }
            ++parent_record->live_children;
            working_directory = parent_record->working_directory;
        }
        for (size_t index = 0; index < sizeof(process->working_directory); ++index)
        {
            process->working_directory[index] = working_directory[index];
            if (working_directory[index] == '\0')
            {
                break;
            }
        }
        id = next_process_id++;
        process->id = id;
        process->parent = parent;
        process->session_leader = (flags & User::SpawnSessionLeader) != 0;
        process->next = process_list;
        process_list = process;
    }

    size_t path_length = 0;
    while (path[path_length] != '\0')
    {
        ++path_length;
    }
    const size_t name_length = path_length - (sizeof(UserProgramSuffix) - 1);
    for (size_t index = 0; index < name_length; ++index)
    {
        process->name[index] = path[index];
    }
    process->name[name_length] = '\0';
    arguments.process_id = id;
    InitializeUserProgram(&process->program);
    process->program.layout = layout;
    process->program.execution_context.syscall_handlers = &ProcessSyscallHandlers;
    process->program.execution_context.syscall_context = process;
    const bool loaded = LoadUserProgram(image, arguments, &process->program);
    // Spawned programs are time-sliced: user mode runs with interrupts unmasked, and each timer
    // tick may switch to another ready task.
    process->program.execution_context.preemption_enabled = true;
    if (!loaded || !Scheduler::CreateTask(RunProcess, process, &process->task))
    {
        ReleaseUserProgram(&process->program);
        RemoveProcess(process);
        return SpawnStatus::NoResources;
    }
    if ((flags & User::SpawnBackground) == 0)
    {
        Sync::SpinLockGuard guard(process_lock);
        foreground_process = id;
    }
    Console::NotifyStatusChanged();
    *process_id = id;
    return SpawnStatus::Started;
}

bool Wait(ProcessId process_id, uint64_t* exit_status)
{
    ProcessRecord* process = FindProcess(process_id);
    if (process == nullptr || !Scheduler::Wait(process->task, nullptr) ||
        !Scheduler::ReapTask(process->task))
    {
        return false;
    }
    if (exit_status != nullptr)
    {
        *exit_status = process->program.execution_context.exit_status;
    }
    ReleaseUserProgram(&process->program);
    {
        Sync::SpinLockGuard guard(process_lock);
        if (foreground_process == process_id)
        {
            foreground_process = process->parent;
        }
    }
    RemoveProcess(process);
    Console::NotifyStatusChanged();
    return true;
}

bool StopOtherProcesses(ProcessId caller)
{
    const auto others_running = [caller](bool request_kill)
    {
        Sync::SpinLockGuard guard(process_lock);
        const ProcessRecord* caller_record = FindProcessLocked(caller);
        bool running = false;
        for (ProcessRecord* candidate = process_list; candidate != nullptr;
             candidate = candidate->next)
        {
            if (candidate->task == 0 || DescendsFromLocked(caller_record, candidate->id) ||
                Scheduler::GetState(candidate->task) == Scheduler::TaskState::Complete)
            {
                continue;
            }
            if (request_kill)
            {
                RequestKillLocked(candidate);
            }
            running = true;
        }
        return running;
    };
    others_running(true);
    const uint64_t deadline = Time::TickCount() + 2 * Time::TicksPerSecond();
    while (Time::TickCount() < deadline)
    {
        if (!others_running(false))
        {
            return true;
        }
        Scheduler::SleepUntil(Time::TickCount() + 1);
    }
    return !others_running(false);
}

bool InterruptForegroundJob()
{
    Sync::SpinLockGuard guard(process_lock);
    // The job is the foreground process's ancestor that a session leader started. Session leaders
    // (the shell) and processes outside any session (init) are never interrupted.
    const ProcessRecord* job = FindProcessLocked(foreground_process);
    while (job != nullptr && !job->session_leader)
    {
        const ProcessRecord* parent = FindProcessLocked(job->parent);
        if (parent == nullptr || parent->session_leader)
        {
            break;
        }
        job = parent;
    }
    const ProcessRecord* leader = job == nullptr ? nullptr : FindProcessLocked(job->parent);
    if (job == nullptr || job->session_leader || leader == nullptr || !leader->session_leader)
    {
        return false;
    }
    for (ProcessRecord* candidate = process_list; candidate != nullptr; candidate = candidate->next)
    {
        if (DescendsFromLocked(candidate, job->id))
        {
            RequestKillLocked(candidate);
        }
    }
    return true;
}

void ForegroundProgramName(char* buffer, size_t capacity)
{
    if (buffer == nullptr || capacity == 0)
    {
        return;
    }
    Sync::SpinLockGuard guard(process_lock);
    const ProcessRecord* foreground = nullptr;
    for (const ProcessRecord* candidate = process_list; candidate != nullptr;
         candidate = candidate->next)
    {
        if (candidate->id == foreground_process)
        {
            foreground = candidate;
        }
    }
    const char* name = foreground != nullptr ? foreground->name : "kernel";
    size_t length = 0;
    for (; name[length] != '\0' && length < capacity - 1; ++length)
    {
        buffer[length] = name[length];
    }
    buffer[length] = '\0';
}

} // namespace FeatherOS::Process
