#include "process.h"

#include "kernel/arch/rtc.h"
#include "kernel/console.h"
#include "kernel/filesystem/initramfs.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/panic.h"
#include "kernel/process_arguments.h"
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
constexpr size_t MaximumPromptLength = 32;

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
    for (uint64_t offset = 0; offset < Memory::PageSize; ++offset)
    {
        page[offset] = 0;
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
    if (!ELF::LoadSegments(image, operations))
    {
        return false;
    }

    uint64_t stack_physical_address;
    if (!AllocateProgramPage(program, &stack_physical_address))
    {
        return false;
    }
    if (!Memory::MapPage(program->address_space, UserStackAddress, stack_physical_address,
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
    if (!Memory::MapPage(program->address_space, ArgumentsAddress, arguments_physical_address,
                         Memory::PageFlags::Read | Memory::PageFlags::User))
    {
        Memory::FreePageFrame(arguments_physical_address);
        return false;
    }

    program->entry_point = image.entry_point;
    program->stack_pointer = UserStackTop;
    program->heap_break = program->heap_start;
    program->argument_pointer = ArgumentsAddress;
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

ProcessRecord* FindProcess(ProcessId process_id)
{
    Sync::SpinLockGuard guard(process_lock);
    for (ProcessRecord* process = process_list; process != nullptr; process = process->next)
    {
        if (process->id == process_id)
        {
            return process;
        }
    }
    return nullptr;
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
    const size_t length = Console::ReadLine(prompt_text, line, line_capacity);
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
    if (increment > UserStackRegionBase - previous_break)
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

constexpr Syscall::Handlers ProcessSyscallHandlers = {
    WriteCharacterSyscall, ReadLineSyscall,   SpawnSyscall, WaitSyscall,
    ReadClockSyscall,      ExtendHeapSyscall, SleepSyscall,
};

} // namespace

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
    if (context == nullptr || context->program == nullptr ||
        fault_address < UserStackRegionBase + Memory::PageSize || fault_address >= UserStackTop)
    {
        return false;
    }
    auto* program = static_cast<UserProgram*>(context->program);
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
    User::ProgramArguments arguments;
    if (!BuildProgramArguments(command_line, ArgumentsAddress, &arguments))
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
        if (parent_record != nullptr)
        {
            if (parent_record->live_children == MaximumChildrenPerProcess)
            {
                Memory::Deallocate(process);
                return SpawnStatus::NoResources;
            }
            ++parent_record->live_children;
        }
        id = next_process_id++;
        process->id = id;
        process->parent = parent;
        process->next = process_list;
        process_list = process;
    }

    arguments.process_id = id;
    InitializeUserProgram(&process->program);
    process->program.execution_context.syscall_handlers = &ProcessSyscallHandlers;
    process->program.execution_context.syscall_context = process;
    if (!LoadUserProgram(image, arguments, &process->program) ||
        !Scheduler::CreateTask(RunUserProgram, &process->program, &process->task))
    {
        ReleaseUserProgram(&process->program);
        RemoveProcess(process);
        return SpawnStatus::NoResources;
    }
    {
        Sync::SpinLockGuard guard(process_lock);
        foreground_process = id;
    }
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
    return true;
}

} // namespace FeatherOS::Process
