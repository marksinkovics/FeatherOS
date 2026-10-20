#include "process.h"

#include "kernel/arch/cpu_features.h"
#include "kernel/arch/rtc.h"
#include "kernel/console.h"
#include "kernel/cpu.h"
#include "kernel/filesystem/initramfs.h"
#include "kernel/files.h"
#include "kernel/filesystem/path.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/panic.h"
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
constexpr char DefaultWorkingDirectory[] = "/disk";

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
    Files::Table files;
    // Relative paths in file syscalls start here; children inherit it.
    char working_directory[User::MaximumPathLength];
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
    program->stack_pointer = program->layout.stack_top;
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

bool CopyStringFromUser(const ProcessRecord& process, uint64_t address, char* buffer,
                        size_t capacity)
{
    for (size_t index = 0; index < capacity; ++index)
    {
        if (address > UINT64_MAX - index)
        {
            return false;
        }
        volatile uint8_t* byte = UserByte(process, address + index, false);
        if (byte == nullptr)
        {
            return false;
        }
        buffer[index] = static_cast<char>(*byte);
        if (buffer[index] == '\0')
        {
            return true;
        }
    }
    return false;
}

// Copies a path argument and resolves it against the process's working directory.
bool CopyPathFromUser(const ProcessRecord& process, uint64_t address,
                      char (&path)[User::MaximumPathLength])
{
    char text[User::MaximumPathLength];
    return CopyStringFromUser(process, address, text, sizeof(text)) &&
           Path::Resolve(process.working_directory, text, path);
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
    if (!CopyStringFromUser(*process, prompt, prompt_text, sizeof(prompt_text)))
    {
        return User::SyscallErrorInvalidArgument;
    }
    char line[User::MaximumCommandLineLength];
    const size_t line_capacity =
        capacity < sizeof(line) ? static_cast<size_t>(capacity) : sizeof(line);
    // Validate the destination before blocking so a bad buffer fails immediately.
    if (!UserRangeWritable(*process, buffer, line_capacity))
    {
        return User::SyscallErrorInvalidArgument;
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
        return User::SyscallErrorInvalidArgument;
    }
    return length;
}

uint64_t SpawnSyscall(void* context, uint64_t command_line)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    char line[User::MaximumCommandLineLength];
    if (!CopyStringFromUser(*process, command_line, line, sizeof(line)))
    {
        return User::SyscallErrorInvalidArgument;
    }
    ProcessId child;
    switch (Spawn(line, process->id, &child))
    {
    case SpawnStatus::Started:
        return child;
    case SpawnStatus::NotFound:
        return User::SyscallErrorNotFound;
    case SpawnStatus::InvalidArguments:
        return User::SyscallErrorInvalidArgument;
    default:
        return User::SyscallErrorFailed;
    }
}

uint64_t WaitSyscall(void* context, uint64_t process_id, uint64_t status_pointer)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    const ProcessRecord* child = FindProcess(process_id);
    if (child == nullptr || child->parent != process->id ||
        !UserRangeWritable(*process, status_pointer, sizeof(uint64_t)))
    {
        return User::SyscallErrorInvalidArgument;
    }
    uint64_t exit_status;
    if (!Wait(process_id, &exit_status))
    {
        return User::SyscallErrorFailed;
    }
    if (!CopyToUser(*process, status_pointer, &exit_status, sizeof(exit_status)))
    {
        return User::SyscallErrorInvalidArgument;
    }
    return 0;
}

uint64_t ExtendHeapSyscall(void* context, uint64_t increment)
{
    UserProgram& program = static_cast<ProcessRecord*>(context)->program;
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
    for (; page_address < page_end; page_address += page_size)
    {
        uint64_t physical_address;
        if (!AllocateProgramPage(&program, &physical_address))
        {
            return User::SyscallErrorFailed;
        }
        if (!Memory::MapPage(program.address_space, page_address, physical_address,
                             Memory::PageFlags::Read | Memory::PageFlags::Write |
                                 Memory::PageFlags::User))
        {
            Memory::FreePageFrame(physical_address);
            --program.mapped_page_count;
            return User::SyscallErrorFailed;
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
    if (!CopyPathFromUser(*process, path, path_text))
    {
        return User::SyscallErrorInvalidArgument;
    }
    return Files::Open(&process->files, path_text, flags);
}

uint64_t ReadSyscall(void* context, uint64_t descriptor, uint64_t buffer, uint64_t length)
{
    auto* process = static_cast<ProcessRecord*>(context);
    uint8_t chunk[FileChunkSize];
    if (length == 0)
    {
        return Files::Read(&process->files, descriptor, nullptr, 0);
    }
    // Checking the whole range first keeps a bad buffer from consuming file data.
    if (!UserRangeWritable(*process, buffer, length))
    {
        return User::SyscallErrorInvalidArgument;
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
            return User::SyscallErrorInvalidArgument;
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
    if (length == 0)
    {
        return Files::Write(&process->files, descriptor, nullptr, 0);
    }
    if (!UserRangeReadable(*process, buffer, length))
    {
        return User::SyscallErrorInvalidArgument;
    }
    uint64_t total = 0;
    while (total < length)
    {
        const uint64_t wanted = length - total < FileChunkSize ? length - total : FileChunkSize;
        if (!CopyFromUser(*process, buffer + total, chunk, wanted))
        {
            return User::SyscallErrorInvalidArgument;
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
    return Files::Close(&static_cast<ProcessRecord*>(context)->files, descriptor);
}

uint64_t SeekSyscall(void* context, uint64_t descriptor, uint64_t offset)
{
    return Files::Seek(&static_cast<ProcessRecord*>(context)->files, descriptor, offset);
}

// Copies the path argument and passes it to one of the path-only Files calls.
uint64_t PathSyscall(void* context, uint64_t path, uint64_t (*operation)(const char*))
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    char path_text[User::MaximumPathLength];
    if (!CopyPathFromUser(*process, path, path_text))
    {
        return User::SyscallErrorInvalidArgument;
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
    if (!CopyPathFromUser(*process, path, path_text))
    {
        return User::SyscallErrorInvalidArgument;
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

uint64_t CurrentDirectorySyscall(void* context, uint64_t buffer, uint64_t capacity)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    uint64_t length = 0;
    while (process->working_directory[length] != '\0')
    {
        ++length;
    }
    if (capacity <= length || !CopyToUser(*process, buffer, process->working_directory, length + 1))
    {
        return User::SyscallErrorInvalidArgument;
    }
    return length;
}

uint64_t ReadDirectorySyscall(void* context, uint64_t path, uint64_t index, uint64_t entry)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    char path_text[User::MaximumPathLength];
    User::DirectoryEntry directory_entry;
    if (!CopyPathFromUser(*process, path, path_text) ||
        !UserRangeWritable(*process, entry, sizeof(directory_entry)))
    {
        return User::SyscallErrorInvalidArgument;
    }
    const uint64_t result = Files::ReadDirectory(path_text, index, &directory_entry);
    if (User::IsSyscallError(result))
    {
        return result;
    }
    return CopyToUser(*process, entry, &directory_entry, sizeof(directory_entry))
               ? 0
               : User::SyscallErrorInvalidArgument;
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

SpawnStatus Spawn(const char* command_line, ProcessId parent, ProcessId* process_id)
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
    if (!BuildProgramArguments(command_line, layout.arguments_address, &arguments))
    {
        return SpawnStatus::InvalidArguments;
    }

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
        const char* working_directory = DefaultWorkingDirectory;
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

bool InterruptForegroundJob()
{
    Sync::SpinLockGuard guard(process_lock);
    // The job is the foreground process's ancestor that the session leader started. The leader
    // (the shell, whose parent is the kernel) is never interrupted.
    const ProcessRecord* job = FindProcessLocked(foreground_process);
    while (job != nullptr && job->parent != 0)
    {
        const ProcessRecord* parent = FindProcessLocked(job->parent);
        if (parent == nullptr || parent->parent == 0)
        {
            break;
        }
        job = parent;
    }
    if (job == nullptr || job->parent == 0)
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
