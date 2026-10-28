#include "process.h"

#include "kernel/arch/cpu_features.h"
#include "kernel/arch/rtc.h"
#include "kernel/arch/thread_pointer.h"
#include "kernel/console.h"
#include "kernel/cpu.h"
#include "kernel/files.h"
#include "kernel/futex.h"
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
#include "kernel/smp.h"
#include "kernel/sync/spinlock.h"
#include "kernel/syscall.h"
#include "kernel/thread_local_storage.h"
#include "kernel/thread_stacks.h"
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

// Where a program named without a '/' is found.
constexpr char SystemDirectory[] = "/system";

#if defined(ARCH_x86_64)
constexpr uint16_t UserMachine = ELF::MachineX86_64;
#elif defined(ARCH_aarch64)
constexpr uint16_t UserMachine = ELF::MachineAArch64;
#endif

struct ProcessRecord;

// A thread other than the main one, whose context is in the process's program. A slot is free
// while id is 0; its stack slot is the slot's index plus one.
struct ThreadRecord
{
    ProcessRecord* process;
    uint64_t id;
    // 0 until the task exists.
    Scheduler::TaskId task;
    uint64_t entry;
    uint64_t block;
    uint64_t stack_pointer;
    // Set while a thread, or the exiting process, waits for this one to end.
    bool joining;
    // Nobody will join it: its slot is reclaimed once it has ended.
    bool detached;
    UserExecutionContext context;
};

struct ProcessRecord
{
    ProcessRecord* next;
    ProcessId id;
    ProcessId parent;
    // Ctrl+C and console input go to every process in the foreground group.
    ProcessId group;
    // The tick the process was spawned at, for ps.
    uint64_t start_tick;
    uint32_t live_children;
    Scheduler::TaskId task;
    char name[MaximumCommandNameLength + 1];
    // Set for the console service: its descendants form Ctrl+C jobs.
    bool session_leader;
    Files::Table files;
    // Relative paths in file syscalls start here; children inherit it.
    char working_directory[User::MaximumPathLength];
    // The rest of a console line that a read did not take, with its '\n'.
    char console_input[User::MaximumCommandLineLength];
    uint8_t console_input_length;
    uint8_t console_input_offset;
    UserProgram program;
    ThreadRecord threads[User::MaximumThreads - 1];
    uint64_t next_thread_id;
    // Set when exit, a fault or a terminating signal ends the process while threads may run.
    bool exiting;
    uint64_t exit_status;
};

uint64_t hhdm_offset = 0;
constinit Sync::SpinLock process_lock;
ProcessRecord* process_list = nullptr;
ProcessId next_process_id = 1;
// Only processes in this group may read console input; zero means the kernel itself.
ProcessId foreground_group = 0;

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

// Maps a zeroed page of a thread stack slot unless one is there; memory_lock must be held.
bool MapStackPageLocked(UserProgram* program, uint64_t page_address)
{
    uint64_t physical_address;
    bool writable;
    // Another CPU may have mapped it first.
    if (Memory::TranslateUserPage(program->address_space, page_address, &physical_address,
                                  &writable))
    {
        return true;
    }
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

// Builds a thread's TLS area at the top of its stack slot: the program's template, zeros up to
// the block's size, and the control block, cleared first since a reused slot holds the last
// thread's data. The program's address space must be active. The thread's stack starts below
// *area_bottom.
bool SetUpThreadArea(UserProgram* program, uint64_t slot_top, uint64_t* thread_pointer,
                     uint64_t* area_bottom)
{
#if defined(ARCH_x86_64)
    constexpr auto Variant = ThreadLocalStorage::Variant::BlockBelowPointer;
#elif defined(ARCH_aarch64)
    constexpr auto Variant = ThreadLocalStorage::Variant::BlockAbovePointer;
#endif
    ThreadLocalStorage::Layout layout;
    if (!ThreadLocalStorage::Place(Variant, slot_top, program->tls_memory_size,
                                   program->tls_alignment, &layout))
    {
        return false;
    }
    {
        Sync::SpinLockGuard guard(program->memory_lock);
        for (uint64_t page = layout.bottom & ~(Memory::PageSize - 1); page < slot_top;
             page += Memory::PageSize)
        {
            if (!MapStackPageLocked(program, page))
            {
                return false;
            }
        }
    }
    auto* area = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(layout.bottom));
    const auto* source =
        reinterpret_cast<const volatile uint8_t*>(static_cast<uintptr_t>(program->tls_template));
    const uint64_t block_offset = layout.block - layout.bottom;
    const Arch::CpuFeatures::UserAccessWindow user_access;
    for (uint64_t index = 0; index < slot_top - layout.bottom; ++index)
    {
        area[index] = 0;
    }
    for (uint64_t index = 0; index < program->tls_file_size; ++index)
    {
        area[block_offset + index] = source[index];
    }
#if defined(ARCH_x86_64)
    // The x86-64 ABI reads the thread pointer back from its first word, as %fs:0.
    auto* self =
        reinterpret_cast<volatile uint64_t*>(static_cast<uintptr_t>(layout.thread_pointer));
    *self = layout.thread_pointer;
#endif
    *thread_pointer = layout.thread_pointer;
    *area_bottom = layout.bottom;
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
    if (image.has_tls)
    {
        program->tls_template = image.tls_virtual_address + load_bias;
        program->tls_file_size = image.tls_file_size;
        program->tls_memory_size = image.tls_memory_size;
        program->tls_alignment = image.tls_alignment;
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
    uint64_t stack_start = 0;
    if (!SetUpThreadArea(program, program->layout.stack_top,
                         &program->execution_context.thread_pointer, &stack_start))
    {
        return false;
    }
#if defined(ARCH_x86_64)
    // A function starts with rsp 8 bytes past a 16-byte boundary, as a call leaves it. Code that
    // keeps SSE values on the stack relies on that alignment.
    program->stack_pointer = stack_start - 8;
#else
    program->stack_pointer = stack_start;
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

// Wakes the process if it sleeps or waits, so blocking calls can give up for the signal.
void SendSignalLocked(ProcessRecord* process, int signal)
{
    // The task exists only once the program has loaded; before that there is nothing to signal.
    if (process->task == 0)
    {
        return;
    }
    // SIGKILL reaches every thread; other signals go to the main thread, or the first live one.
    const bool main_running = Scheduler::GetState(process->task) != Scheduler::TaskState::Complete;
    if (signal == User::SignalKill || main_running)
    {
        Signals::Send(&process->program.execution_context.signals, signal);
        Scheduler::WakeTask(process->task);
    }
    for (ThreadRecord& thread : process->threads)
    {
        if (thread.task == 0 || (signal != User::SignalKill && Scheduler::GetState(thread.task) ==
                                                                   Scheduler::TaskState::Complete))
        {
            continue;
        }
        Signals::Send(&thread.context.signals, signal);
        Scheduler::WakeTask(thread.task);
        if (signal != User::SignalKill && !main_running)
        {
            return;
        }
    }
}

void RequestKillLocked(ProcessRecord* process)
{
    SendSignalLocked(process, User::SignalKill);
}

// The calling thread's context; its process is the syscall context.
UserExecutionContext* CallerContext()
{
    return static_cast<UserExecutionContext*>(Scheduler::CurrentUserContext());
}

// A signal the caller would take, such as SIGINT from Ctrl+C; blocking calls then give up. It
// takes no locks, so the scheduler may call it with its own lock held.
bool CallerInterrupted()
{
    const auto* context = static_cast<const UserExecutionContext*>(Scheduler::CurrentUserContext());
    return context != nullptr && Signals::Deliverable(context->signals) != 0;
}

bool InForegroundGroup(const ProcessRecord& process)
{
    Sync::SpinLockGuard guard(process_lock);
    return process.group == foreground_group;
}

// A process may use its own group, or one that holds one of its children.
bool GroupJoinableLocked(const ProcessRecord& caller, ProcessId group)
{
    if (group == caller.group)
    {
        return true;
    }
    for (const ProcessRecord* other = process_list; other != nullptr; other = other->next)
    {
        if (other->group == group && other->parent == caller.id)
        {
            return true;
        }
    }
    return false;
}

bool BuildProgramPath(const char* program, const char* working_directory,
                      char (&path)[Path::MaximumLength])
{
    bool named = true;
    for (const char* character = program; *character != '\0'; ++character)
    {
        named = named && *character != '/';
    }
    return Path::Resolve(named ? SystemDirectory : working_directory, program, path);
}

// The last part of the program's path, as much as fits.
void SetProcessName(ProcessRecord* process, const char* program)
{
    const char* name = program;
    for (const char* character = program; *character != '\0'; ++character)
    {
        if (*character == '/' && character[1] != '\0')
        {
            name = character + 1;
        }
    }
    size_t length = 0;
    for (; name[length] != '\0' && name[length] != '/' && length < MaximumCommandNameLength;
         ++length)
    {
        process->name[length] = name[length];
    }
    process->name[length] = '\0';
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
    if (!InForegroundGroup(*process))
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
    constexpr uint64_t KnownFlags =
        User::SpawnBackground | User::SpawnSessionLeader | User::SpawnSetGroup;
    const uint64_t low_flags = flags & ((1ULL << User::SpawnGroupShift) - 1);
    if ((low_flags & ~KnownFlags) != 0 ||
        ((low_flags & User::SpawnSetGroup) == 0 && (flags >> User::SpawnGroupShift) != 0))
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
    case SpawnStatus::IsDirectory:
        return User::SyscallErrorIsDirectory;
    case SpawnStatus::NameTooLong:
        return User::SyscallErrorNameTooLong;
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
    // A signal ends the wait early; the child stays to be waited for again.
    if (!Scheduler::Wait(child->task, nullptr, CallerInterrupted))
    {
        return User::SyscallErrorInterrupted;
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

// Frees frames unmapped from program once no CPU can still reach them through its TLB.
void FreeUnmappedFrames(const UserProgram& program, const uint64_t* frames, size_t count)
{
    if (count == 0)
    {
        return;
    }
    Memory::ShootDownUserMappings(program.address_space);
    for (size_t index = 0; index < count; ++index)
    {
        Memory::FreePageFrame(frames[index]);
    }
}

// Unmaps the whole pages between the new and the old break, and frees their frames. The page
// holding the new break stays, with its old contents. Pages are unmapped under the memory lock
// and freed after it, since freeing waits for the other CPUs running the process.
uint64_t ShrinkHeap(UserProgram& program, uint64_t decrement)
{
    const uint64_t page_size = Memory::PageSize;
    // Sized from the request, before the lock: at most one frame per page it spans, plus one.
    const size_t capacity =
        static_cast<size_t>(decrement / page_size > program.page_limit ? program.page_limit
                                                                       : decrement / page_size + 1);
    auto* frames = static_cast<uint64_t*>(Memory::AllocateZeroed(capacity, sizeof(uint64_t)));
    if (frames == nullptr)
    {
        return User::SyscallErrorNoMemory;
    }
    size_t count = 0;
    uint64_t previous_break = 0;
    {
        Sync::SpinLockGuard guard(program.memory_lock);
        previous_break = program.heap_break;
        if (decrement > previous_break - program.heap_start)
        {
            Memory::Deallocate(frames);
            return User::SyscallErrorInvalidArgument;
        }
        const uint64_t new_break = previous_break - decrement;
        const uint64_t page_end = (previous_break + page_size - 1) & ~(page_size - 1);
        for (uint64_t page_address = (new_break + page_size - 1) & ~(page_size - 1);
             page_address < page_end && count < capacity; page_address += page_size)
        {
            uint64_t physical_address;
            if (Memory::UnmapUserPage(program.address_space, page_address, &physical_address))
            {
                frames[count++] = physical_address;
                --program.mapped_page_count;
            }
        }
        program.heap_break = new_break;
    }
    FreeUnmappedFrames(program, frames, count);
    Memory::Deallocate(frames);
    return previous_break;
}

// A joined thread's stack, TLS area included, goes back; the next thread in the slot maps fresh
// pages as it needs them.
void ReleaseStackSlot(UserProgram& program, uint32_t slot)
{
    const uint64_t top = ThreadStacks::Top(program.layout.stack_top, UserStackRegionPages, slot);
    const uint64_t bottom = top - (UserStackRegionPages - 1) * Memory::PageSize;
    uint64_t frames[UserStackRegionPages];
    size_t count = 0;
    {
        Sync::SpinLockGuard guard(program.memory_lock);
        for (uint64_t page = bottom; page < top; page += Memory::PageSize)
        {
            if (Memory::UnmapUserPage(program.address_space, page, &frames[count]))
            {
                ++count;
                --program.mapped_page_count;
            }
        }
    }
    FreeUnmappedFrames(program, frames, count);
}

// A negative increment, as a two's-complement value, shrinks the heap.
uint64_t ExtendHeapSyscall(void* context, uint64_t increment)
{
    UserProgram& program = static_cast<ProcessRecord*>(context)->program;
    if (static_cast<int64_t>(increment) < 0)
    {
        return ShrinkHeap(program, 0 - increment);
    }
    Sync::SpinLockGuard guard(program.memory_lock);
    const uint64_t previous_break = program.heap_break;
    // The heap may grow up to, but never into, the lowest thread stack slot's guard page.
    const uint64_t stack_region_base =
        ThreadStacks::Bottom(program.layout.stack_top, UserStackRegionPages, User::MaximumThreads);
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
    // Round up so the program sleeps at least as long as it asked. A signal ends it early, and
    // other wakeups go back to sleep.
    if (milliseconds == 0)
    {
        Scheduler::Yield();
        return 0;
    }
    const uint64_t ticks = (milliseconds * ticks_per_second + 999) / 1000;
    const uint64_t deadline = Time::TickCount() + ticks;
    while (Time::TickCount() < deadline)
    {
        if (CallerInterrupted())
        {
            return User::SyscallErrorInterrupted;
        }
        Scheduler::SleepUntil(deadline);
    }
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

// Canonical console reads return one line at a time, ending in '\n'; what does not fit the
// caller's buffer is kept for its next read, even if the settings change in between.
uint64_t ReadConsole(ProcessRecord* process, uint64_t buffer, uint64_t length)
{
    if (!UserRangeWritable(*process, buffer, length))
    {
        return User::SyscallErrorBadAddress;
    }
    if (process->console_input_offset == process->console_input_length &&
        (Console::Settings().flags & User::TerminalCanonical) == 0)
    {
        if (!InForegroundGroup(*process))
        {
            return User::SyscallErrorNotForeground;
        }
        char bytes[FileChunkSize];
        size_t count = 0;
        if (!Console::ReadBytes(bytes, length < sizeof(bytes) ? length : sizeof(bytes), &count))
        {
            return User::SyscallErrorInterrupted;
        }
        return count == 0 || CopyToUser(*process, buffer, bytes, count)
                   ? count
                   : User::SyscallErrorBadAddress;
    }
    if (process->console_input_offset == process->console_input_length)
    {
        if (!InForegroundGroup(*process))
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
    const Files::Kind kind = Files::KindOf(&process->files, descriptor);
    if (kind == Files::Kind::None)
    {
        return User::SyscallErrorBadDescriptor;
    }
    if (kind == Files::Kind::Console)
    {
        return length == 0 ? 0 : ReadConsole(process, buffer, length);
    }
    // One chunk at most, since a pipe read returns as soon as any data is there.
    if (kind == Files::Kind::Pipe)
    {
        if (!UserRangeWritable(*process, buffer, length))
        {
            return User::SyscallErrorBadAddress;
        }
        const uint64_t wanted = length < FileChunkSize ? length : FileChunkSize;
        const uint64_t count =
            Files::Read(&process->files, descriptor, chunk, wanted, CallerInterrupted);
        if (User::IsSyscallError(count) || count == 0)
        {
            return count;
        }
        return CopyToUser(*process, buffer, chunk, count) ? count : User::SyscallErrorBadAddress;
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
    const Files::Kind kind = Files::KindOf(&process->files, descriptor);
    if (kind == Files::Kind::None)
    {
        return User::SyscallErrorBadDescriptor;
    }
    const bool console = kind == Files::Kind::Console;
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
        const uint64_t count =
            Files::Write(&process->files, descriptor, chunk, wanted, CallerInterrupted);
        // As on Unix, writing to a pipe nobody reads also raises SIGPIPE, which ends the writer
        // unless it handles or ignores it.
        if (count == User::SyscallErrorBrokenPipe)
        {
            Signals::Send(&CallerContext()->signals, User::SignalPipe);
        }
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

uint64_t SeekSyscall(void* context, uint64_t descriptor, uint64_t offset, uint64_t whence)
{
    return Files::Seek(&static_cast<ProcessRecord*>(context)->files, descriptor, offset, whence);
}

uint64_t DuplicateSyscall(void* context, uint64_t descriptor, uint64_t target, uint64_t flags)
{
    return Files::Duplicate(&static_cast<ProcessRecord*>(context)->files, descriptor, target,
                            flags);
}

uint64_t ProcessGroupSyscall(void* context, uint64_t process_id)
{
    const auto* caller = static_cast<const ProcessRecord*>(context);
    Sync::SpinLockGuard guard(process_lock);
    const ProcessRecord* target = process_id == 0 ? caller : FindProcessLocked(process_id);
    return target == nullptr ? User::SyscallErrorNotFound : target->group;
}

// As with Shutdown, only init and session leaders such as the shell may move the console.
uint64_t ForegroundGroupSyscall(void* context, uint64_t set, uint64_t group)
{
    const auto* caller = static_cast<const ProcessRecord*>(context);
    {
        Sync::SpinLockGuard guard(process_lock);
        if (set == 0)
        {
            return foreground_group;
        }
        if (caller->parent != 0 && !caller->session_leader)
        {
            return User::SyscallErrorNotPermitted;
        }
        if (!GroupJoinableLocked(*caller, group))
        {
            return User::SyscallErrorInvalidArgument;
        }
        foreground_group = group;
    }
    Console::NotifyStatusChanged();
    return group;
}

// target as kill(2): a process ID, 0 for the caller's group, -1 for every process but init and
// the caller, or -group. Signal 0 only checks that the target exists. init cannot be signaled.
uint64_t KillSyscall(void* context, uint64_t target, uint64_t signal)
{
    const auto* caller = static_cast<const ProcessRecord*>(context);
    const auto signal_number = static_cast<int>(signal);
    if (signal > static_cast<uint64_t>(User::SignalCount) ||
        (signal != 0 && !Signals::IsValid(signal_number)))
    {
        return User::SyscallErrorInvalidArgument;
    }
    const auto requested = static_cast<int64_t>(target);
    Sync::SpinLockGuard guard(process_lock);
    bool found = false;
    for (ProcessRecord* candidate = process_list; candidate != nullptr; candidate = candidate->next)
    {
        const bool matches = requested > 0    ? candidate->id == static_cast<ProcessId>(requested)
                             : requested == 0 ? candidate->group == caller->group
                             : requested == -1
                                 ? candidate != caller
                                 : candidate->group == static_cast<ProcessId>(-requested);
        if (!matches)
        {
            continue;
        }
        if (candidate->parent == 0)
        {
            if (requested > 0)
            {
                return User::SyscallErrorNotPermitted;
            }
            continue;
        }
        found = true;
        if (signal != 0)
        {
            SendSignalLocked(candidate, signal_number);
        }
    }
    return found ? 0 : User::SyscallErrorNotFound;
}

uint64_t SignalActionSyscall(void* context, uint64_t signal, uint64_t new_action,
                             uint64_t old_action)
{
    auto* process = static_cast<ProcessRecord*>(context);
    User::SignalAction requested;
    User::SignalAction previous;
    if (new_action != 0 && !CopyFromUser(*process, new_action, &requested, sizeof(requested)))
    {
        return User::SyscallErrorBadAddress;
    }
    if (old_action != 0 && !UserRangeWritable(*process, old_action, sizeof(previous)))
    {
        return User::SyscallErrorBadAddress;
    }
    const int number = static_cast<int>(signal > INT32_MAX ? 0 : signal);
    const uint64_t result = Signals::SetAction(&CallerContext()->signals, number,
                                               new_action != 0 ? &requested : nullptr, &previous);
    // Actions belong to the process, so every thread's copy changes with the caller's.
    if (result == 0 && new_action != 0)
    {
        Sync::SpinLockGuard guard(process_lock);
        Signals::SetAction(&process->program.execution_context.signals, number, &requested,
                           nullptr);
        for (ThreadRecord& thread : process->threads)
        {
            if (thread.id != 0)
            {
                Signals::SetAction(&thread.context.signals, number, &requested, nullptr);
            }
        }
    }
    if (result == 0 && old_action != 0 &&
        !CopyToUser(*process, old_action, &previous, sizeof(previous)))
    {
        return User::SyscallErrorBadAddress;
    }
    return result;
}

// Each thread has its own mask.
uint64_t SignalMaskSyscall(void*, uint64_t how, uint64_t set)
{
    return Signals::ChangeMask(&CallerContext()->signals, how, set);
}

// Ends the process for exit, a fault or a terminating signal in any thread: the first status
// wins, and every other thread gets SIGKILL.
void ExitProcess(ProcessRecord* process, uint64_t status)
{
    Sync::SpinLockGuard guard(process_lock);
    if (process->exiting)
    {
        return;
    }
    process->exiting = true;
    process->exit_status = status;
    SendSignalLocked(process, User::SignalKill);
}

void RunInUserMode(UserProgram* program, uint64_t entry, uint64_t stack_pointer, uint64_t argument,
                   UserExecutionContext* context)
{
    if (!Memory::ActivateAddressSpace(program->address_space))
    {
        context->exit_status = User::ExitStatusFault;
        context->thread_exit_only = false;
        Memory::ActivateKernelAddressSpace();
        return;
    }
    Arch::ThreadPointer::Write(context->thread_pointer);
#if defined(ARCH_x86_64)
    Arch::X86_64::RunUserTest(entry, stack_pointer, argument, context);
#elif defined(ARCH_aarch64)
    Arch::AArch64::RunUserTest(entry, stack_pointer, argument, context);
#endif
    Memory::ActivateKernelAddressSpace();
}

// A thread's task; its result is the value ThreadJoin reports.
uint64_t RunThread(void* context)
{
    auto* thread = static_cast<ThreadRecord*>(context);
    RunInUserMode(&thread->process->program, thread->entry, thread->stack_pointer, thread->block,
                  &thread->context);
    if (!thread->context.thread_exit_only)
    {
        ExitProcess(thread->process, thread->context.exit_status);
    }
    return thread->context.exit_status;
}

uint64_t ThreadCreateSyscall(void* context, uint64_t entry, uint64_t first, uint64_t second)
{
    auto* process = static_cast<ProcessRecord*>(context);
    UserExecutionContext* caller = CallerContext();
    if (entry == 0 || entry >= UserAddressLimit)
    {
        return User::SyscallErrorInvalidArgument;
    }
    ThreadRecord* thread = nullptr;
    uint32_t slot = 0;
    {
        Sync::SpinLockGuard guard(process_lock);
        if (process->exiting)
        {
            return User::SyscallErrorInterrupted;
        }
        for (uint32_t index = 0; index < User::MaximumThreads - 1 && thread == nullptr; ++index)
        {
            ThreadRecord& candidate = process->threads[index];
            // A detached thread that has ended gives its slot back here.
            if (candidate.id != 0 && candidate.detached && candidate.task != 0 &&
                !candidate.joining &&
                Scheduler::GetState(candidate.task) == Scheduler::TaskState::Complete)
            {
                Scheduler::ReapTask(candidate.task);
                candidate.id = 0;
                candidate.task = 0;
            }
            if (candidate.id == 0)
            {
                thread = &candidate;
                slot = index + 1;
            }
        }
        if (thread == nullptr)
        {
            return User::SyscallErrorWouldBlock;
        }
        thread->id = process->next_thread_id++;
        thread->task = 0;
        thread->joining = false;
        thread->detached = false;
    }
    UserProgram& program = process->program;
    {
        Sync::SpinLockGuard guard(program.memory_lock);
        program.stack_slots |= 1U << slot;
    }
    const uint64_t stack_top =
        ThreadStacks::Top(program.layout.stack_top, UserStackRegionPages, slot);
    uint64_t thread_pointer = 0;
    uint64_t area_bottom = 0;
    const bool area_ready = SetUpThreadArea(&program, stack_top, &thread_pointer, &area_bottom);
    const uint64_t block = area_bottom - 2 * sizeof(uint64_t);
    const uint64_t values[2] = {first, second};
    if (!area_ready || !CopyToCurrentUser(caller, block, values, sizeof(values)))
    {
        Sync::SpinLockGuard guard(process_lock);
        thread->id = 0;
        return User::SyscallErrorNoMemory;
    }
    thread->process = process;
    thread->entry = entry;
    thread->block = block;
#if defined(ARCH_x86_64)
    // As at any function's start: 8 bytes past a 16-byte boundary.
    thread->stack_pointer = block - 8;
#else
    thread->stack_pointer = block;
#endif
    // Field by field: the AArch64 kernel has no memcpy for a structure copy.
    UserExecutionContext& thread_context = thread->context;
    const UserExecutionContext& main_context = program.execution_context;
    thread_context.address_space_root_physical_address =
        main_context.address_space_root_physical_address;
    thread_context.kernel_stack_pointer = 0;
    thread_context.previous_kernel_stack_pointer = 0;
    thread_context.exit_status = User::ExitStatusFault;
    thread_context.active = false;
    thread_context.exit_requested = false;
    thread_context.preemption_enabled = true;
    thread_context.syscall_handlers = main_context.syscall_handlers;
    thread_context.syscall_context = main_context.syscall_context;
    thread_context.program = main_context.program;
    thread_context.thread_id = thread->id;
    thread_context.thread_exit_only = false;
    thread_context.thread_pointer = thread_pointer;
    // A new thread keeps the caller's mask and the process's actions, with nothing pending.
    thread_context.signals.pending = 0;
    thread_context.signals.blocked = caller->signals.blocked;
    for (uint32_t signal = 0; signal < User::SignalCount; ++signal)
    {
        User::SignalAction& action = thread_context.signals.actions[signal];
        const User::SignalAction& source = caller->signals.actions[signal];
        action.handler = source.handler;
        action.mask = source.mask;
        action.flags = source.flags;
        action.restorer = source.restorer;
    }
    Scheduler::TaskId task = 0;
    if (!Scheduler::CreateTask(RunThread, thread, &task))
    {
        Sync::SpinLockGuard guard(process_lock);
        thread->id = 0;
        return User::SyscallErrorNoMemory;
    }
    Sync::SpinLockGuard guard(process_lock);
    thread->task = task;
    // The process may have begun exiting while the task was created.
    if (process->exiting)
    {
        Signals::Send(&thread->context.signals, User::SignalKill);
        Scheduler::WakeTask(task);
    }
    return thread->id;
}

uint64_t ThreadJoinSyscall(void* context, uint64_t thread_id, uint64_t value)
{
    auto* process = static_cast<ProcessRecord*>(context);
    if (thread_id == CallerContext()->thread_id || thread_id == 1)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (value != 0 && !UserRangeWritable(*process, value, sizeof(uint64_t)))
    {
        return User::SyscallErrorBadAddress;
    }
    ThreadRecord* thread = nullptr;
    {
        Sync::SpinLockGuard guard(process_lock);
        for (ThreadRecord& candidate : process->threads)
        {
            if (candidate.id == thread_id && candidate.task != 0)
            {
                thread = &candidate;
            }
        }
        if (thread == nullptr)
        {
            return User::SyscallErrorNotFound;
        }
        if (thread->joining || thread->detached)
        {
            return User::SyscallErrorInvalidArgument;
        }
        thread->joining = true;
    }
    uint64_t result = 0;
    if (!Scheduler::Wait(thread->task, &result, CallerInterrupted))
    {
        Sync::SpinLockGuard guard(process_lock);
        thread->joining = false;
        return User::SyscallErrorInterrupted;
    }
    Scheduler::ReapTask(thread->task);
    // While the slot is still taken, so no new thread maps pages in it meanwhile.
    ReleaseStackSlot(process->program, static_cast<uint32_t>(thread - process->threads) + 1);
    {
        Sync::SpinLockGuard guard(process_lock);
        thread->task = 0;
        thread->joining = false;
        thread->id = 0;
    }
    if (value != 0 && !CopyToUser(*process, value, &result, sizeof(result)))
    {
        return User::SyscallErrorBadAddress;
    }
    return 0;
}

uint64_t ThreadIdSyscall(void*)
{
    return CallerContext()->thread_id;
}

uint64_t ThreadDetachSyscall(void* context, uint64_t thread_id)
{
    auto* process = static_cast<ProcessRecord*>(context);
    Sync::SpinLockGuard guard(process_lock);
    for (ThreadRecord& thread : process->threads)
    {
        if (thread.id != thread_id || thread.task == 0)
        {
            continue;
        }
        if (thread.joining || thread.detached)
        {
            return User::SyscallErrorInvalidArgument;
        }
        thread.detached = true;
        if (Scheduler::GetState(thread.task) == Scheduler::TaskState::Complete)
        {
            Scheduler::ReapTask(thread.task);
            thread.id = 0;
            thread.task = 0;
        }
        return 0;
    }
    return User::SyscallErrorNotFound;
}

bool ReadUserWord(const void* owner, uint64_t address, uint32_t* value)
{
    return CopyFromUser(*static_cast<const ProcessRecord*>(owner), address, value, sizeof(*value));
}

// The word must be 4-byte aligned; a wait of 0 milliseconds has no time limit.
uint64_t FutexWaitSyscall(void* context, uint64_t address, uint64_t expected, uint64_t milliseconds)
{
    const uint64_t ticks_per_second = Time::TicksPerSecond();
    if (address % sizeof(uint32_t) != 0 || expected > UINT32_MAX || ticks_per_second == 0 ||
        milliseconds > 24ULL * 60 * 60 * 1000)
    {
        return User::SyscallErrorInvalidArgument;
    }
    const uint64_t deadline =
        milliseconds == 0 ? 0 : Time::TickCount() + (milliseconds * ticks_per_second + 999) / 1000;
    return Futex::Wait(context, address, static_cast<uint32_t>(expected), deadline, ReadUserWord,
                       CallerInterrupted);
}

uint64_t FutexWakeSyscall(void* context, uint64_t address, uint64_t count)
{
    if (address % sizeof(uint32_t) != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    return Futex::Wake(context, address, count);
}

// Every thread but the main one, which the process's own task runs, has ended once this returns.
void WaitForThreads(ProcessRecord* process)
{
    for (;;)
    {
        ThreadRecord* thread = nullptr;
        bool remaining = false;
        {
            Sync::SpinLockGuard guard(process_lock);
            for (ThreadRecord& candidate : process->threads)
            {
                remaining = remaining || candidate.id != 0;
                if (thread == nullptr && candidate.id != 0 && candidate.task != 0 &&
                    !candidate.joining)
                {
                    thread = &candidate;
                    thread->joining = true;
                }
            }
        }
        if (thread == nullptr)
        {
            // A thread still being created or joined by another ends soon: SIGKILL reached it.
            if (!remaining)
            {
                return;
            }
            Scheduler::Yield();
            continue;
        }
        Scheduler::Wait(thread->task, nullptr);
        Scheduler::ReapTask(thread->task);
        Sync::SpinLockGuard guard(process_lock);
        thread->task = 0;
        thread->joining = false;
        thread->id = 0;
    }
}

uint64_t TerminalSizeSyscall(void*)
{
    uint32_t rows = 0;
    uint32_t columns = 0;
    Console::Size(&rows, &columns);
    return (static_cast<uint64_t>(rows) << 32) | columns;
}

uint64_t TerminalModeSyscall(void* context, uint64_t new_settings, uint64_t old_settings)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    User::TerminalSettings requested;
    if (new_settings != 0 && !CopyFromUser(*process, new_settings, &requested, sizeof(requested)))
    {
        return User::SyscallErrorBadAddress;
    }
    if (old_settings != 0 && !UserRangeWritable(*process, old_settings, sizeof(requested)))
    {
        return User::SyscallErrorBadAddress;
    }
    if (new_settings != 0 &&
        ((requested.flags & ~User::TerminalDefaultFlags) != 0 || requested.reserved != 0))
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (new_settings != 0 && !InForegroundGroup(*process))
    {
        return User::SyscallErrorNotForeground;
    }
    const User::TerminalSettings previous = Console::Settings();
    if (new_settings != 0)
    {
        Console::SetSettings(requested, process->id);
    }
    if (old_settings != 0 && !CopyToUser(*process, old_settings, &previous, sizeof(previous)))
    {
        return User::SyscallErrorBadAddress;
    }
    return 0;
}

// Waits until a signal arrives; the program then takes it on the way back to user mode.
uint64_t PauseSyscall(void*)
{
    while (!CallerInterrupted())
    {
        Scheduler::SleepUntil(Time::TickCount() + Time::TicksPerSecond());
    }
    return User::SyscallErrorInterrupted;
}

uint64_t PipeSyscall(void* context, uint64_t descriptors, uint64_t flags)
{
    auto* process = static_cast<ProcessRecord*>(context);
    uint64_t ends[2];
    if (!UserRangeWritable(*process, descriptors, sizeof(ends)))
    {
        return User::SyscallErrorBadAddress;
    }
    const uint64_t result = Files::CreatePipe(&process->files, flags, &ends[0], &ends[1]);
    if (User::IsSyscallError(result))
    {
        return result;
    }
    if (!CopyToUser(*process, descriptors, ends, sizeof(ends)))
    {
        Files::Close(&process->files, ends[0]);
        Files::Close(&process->files, ends[1]);
        return User::SyscallErrorBadAddress;
    }
    return 0;
}

uint64_t DescriptorFlagsSyscall(void* context, uint64_t descriptor, uint64_t set, uint64_t flags)
{
    return Files::DescriptorFlags(&static_cast<ProcessRecord*>(context)->files, descriptor,
                                  set != 0, flags);
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

uint64_t RenameSyscall(void* context, uint64_t from, uint64_t to)
{
    const auto* process = static_cast<const ProcessRecord*>(context);
    char from_text[User::MaximumPathLength];
    char to_text[User::MaximumPathLength];
    uint64_t copied = CopyPathFromUser(*process, from, from_text);
    if (copied == 0)
    {
        copied = CopyPathFromUser(*process, to, to_text);
    }
    return copied != 0 ? copied : Files::Rename(from_text, to_text);
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
    .write_character = WriteCharacterSyscall,
    .read_line = ReadLineSyscall,
    .spawn = SpawnSyscall,
    .wait = WaitSyscall,
    .read_clock = ReadClockSyscall,
    .extend_heap = ExtendHeapSyscall,
    .sleep = SleepSyscall,
    .read_uptime = ReadUptimeSyscall,
    .current_cpu = CurrentCpuSyscall,
    .open = OpenSyscall,
    .read = ReadSyscall,
    .write = WriteSyscall,
    .close = CloseSyscall,
    .seek = SeekSyscall,
    .remove = RemoveSyscall,
    .read_directory = ReadDirectorySyscall,
    .make_directory = MakeDirectorySyscall,
    .remove_directory = RemoveDirectorySyscall,
    .change_directory = ChangeDirectorySyscall,
    .current_directory = CurrentDirectorySyscall,
    .shutdown = ShutdownSyscall,
    .stat = StatSyscall,
    .stat_descriptor = StatDescriptorSyscall,
    .process_id = ProcessIdSyscall,
    .parent_process_id = ParentProcessIdSyscall,
    .duplicate = DuplicateSyscall,
    .descriptor_flags = DescriptorFlagsSyscall,
    .pipe = PipeSyscall,
    .process_group = ProcessGroupSyscall,
    .foreground_group = ForegroundGroupSyscall,
    .kill = KillSyscall,
    .signal_action = SignalActionSyscall,
    .signal_mask = SignalMaskSyscall,
    .pause = PauseSyscall,
    .terminal_size = TerminalSizeSyscall,
    .terminal_mode = TerminalModeSyscall,
    .thread_create = ThreadCreateSyscall,
    .thread_join = ThreadJoinSyscall,
    .thread_id = ThreadIdSyscall,
    .futex_wait = FutexWaitSyscall,
    .futex_wake = FutexWakeSyscall,
    .thread_detach = ThreadDetachSyscall,
    .rename = RenameSyscall,
};

// Children never outlive their parent: an exiting process stops and reaps any still running.
uint64_t RunProcess(void* context)
{
    auto* process = static_cast<ProcessRecord*>(context);
    const uint64_t main_status = RunUserProgram(&process->program);
    // After pthread_exit in the main thread, the process lives on until its last thread ends.
    if (!process->program.execution_context.thread_exit_only)
    {
        ExitProcess(process, main_status);
    }
    WaitForThreads(process);
    const uint64_t exit_status = process->exiting ? process->exit_status : 0;
    process->exit_status = exit_status;
    Files::CloseAll(&process->files);
    Console::ReleaseSettings(process->id);
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
            break;
        }
    }
    {
        Sync::SpinLockGuard guard(process_lock);
        ProcessRecord* parent = FindProcessLocked(process->parent);
        if (parent != nullptr)
        {
            SendSignalLocked(parent, User::SignalChild);
        }
    }
    return exit_status;
}

uint64_t TicksToMilliseconds(uint64_t ticks)
{
    const uint64_t ticks_per_second = Time::TicksPerSecond();
    return ticks_per_second == 0 ? 0 : ticks * 1000 / ticks_per_second;
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
    // A process whose main thread used pthread_exit shows the state of a thread still running.
    Scheduler::TaskState state = Scheduler::GetState(process->task);
    status->threads = state != Scheduler::TaskState::Complete ? 1 : 0;
    uint64_t run_ticks = 0;
    uint32_t last_cpu = 0;
    Scheduler::GetUsage(process->task, &run_ticks, &last_cpu);
    for (const ThreadRecord& thread : process->threads)
    {
        uint64_t thread_ticks = 0;
        uint32_t thread_cpu = 0;
        if (thread.task == 0 || !Scheduler::GetUsage(thread.task, &thread_ticks, &thread_cpu))
        {
            continue;
        }
        run_ticks += thread_ticks;
        const Scheduler::TaskState thread_state = Scheduler::GetState(thread.task);
        if (thread_state != Scheduler::TaskState::Complete)
        {
            ++status->threads;
            if (state == Scheduler::TaskState::Complete)
            {
                state = thread_state;
                last_cpu = thread_cpu;
            }
        }
    }
    status->state = StateLetter(state);
    status->process_id = process->id;
    status->parent_id = process->parent;
    status->group = process->group;
    status->pages = process->program.mapped_page_count;
    status->page_limit = process->program.page_limit;
    status->open_files = 0;
    for (const Files::Slot& slot : process->files.slots)
    {
        status->open_files += slot.file != nullptr ? 1 : 0;
    }
    status->cpu_milliseconds = TicksToMilliseconds(run_ticks);
    status->last_cpu = last_cpu;
    status->start_milliseconds = TicksToMilliseconds(process->start_tick);
    return true;
}

static_assert(Cpu::MaximumCpus <= Procfs::MaximumCpus, "/proc/stat reports every CPU");

void DescribeSystem(Procfs::SystemStatus* status)
{
    status->uptime_milliseconds = TicksToMilliseconds(Time::TickCount());
    status->cpu_count = Smp::OnlineCpuCount();
    for (uint32_t cpu = 0; cpu < Procfs::MaximumCpus; ++cpu)
    {
        status->busy_milliseconds[cpu] =
            cpu < Cpu::MaximumCpus ? TicksToMilliseconds(Scheduler::BusyTickCount(cpu)) : 0;
    }
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
    // Keep every thread's stack region inside the window and leave room for the guard and
    // arguments pages above the stack top.
    constexpr uint64_t StackPages = UserStackRegionPages * User::MaximumThreads;
    constexpr uint64_t SlotCount = RandomStackWindowPages - StackPages - 2;
    const uint64_t stack_top =
        RandomStackWindowBase + (StackPages + Random::Next() % SlotCount) * Memory::PageSize;
    const uint64_t image_base =
        RandomImageWindowBase + (Random::Next() % RandomImageWindowPages) * Memory::PageSize;
    return {image_base, stack_top, stack_top + Memory::PageSize,
            Random::Next() % RandomHeapOffsetPages};
}

void Initialize(uint64_t offset)
{
    Procfs::SetSource({ProcessAt, DescribeProcess, DescribeMemory, DescribeSystem});
    hhdm_offset = offset;
    Sync::SpinLockGuard guard(process_lock);
    process_list = nullptr;
    next_process_id = 1;
    foreground_group = 0;
}

void InitializeUserProgram(UserProgram* program)
{
    program->address_space.root_physical_address = 0;
    program->mapped_page_count = 0;
    program->page_limit = DefaultProcessPageLimit;
    program->stack_slots = 1;
    program->tls_template = 0;
    program->tls_file_size = 0;
    program->tls_memory_size = 0;
    program->tls_alignment = 1;
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
    program->execution_context.thread_id = 1;
    program->execution_context.thread_exit_only = false;
    program->execution_context.thread_pointer = 0;
    Signals::Reset(&program->execution_context.signals);
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
    Sync::SpinLockGuard guard(program->memory_lock);
    uint32_t slot = 0;
    if (!ThreadStacks::Find(program->layout.stack_top, UserStackRegionPages, User::MaximumThreads,
                            fault_address, &slot) ||
        (program->stack_slots & (1U << slot)) == 0)
    {
        return false;
    }
    return MapStackPageLocked(program, fault_address & ~(Memory::PageSize - 1));
}

bool CopyToCurrentUser(UserExecutionContext* context, uint64_t address, const void* source,
                       size_t size)
{
    auto* process = static_cast<ProcessRecord*>(context->syscall_context);
    if (process == nullptr || size == 0 || address > UINT64_MAX - size)
    {
        return false;
    }
    // A frame below the stack pointer may reach pages the stack has not touched yet.
    for (uint64_t page = address & ~(Memory::PageSize - 1); page < address + size;
         page += Memory::PageSize)
    {
        uint64_t physical_address;
        bool writable;
        if (!Memory::TranslateUserPage(process->program.address_space, page, &physical_address,
                                       &writable) &&
            !HandleUserPageFault(context, page))
        {
            return false;
        }
    }
    return CopyToUser(*process, address, source, size);
}

bool CopyFromCurrentUser(UserExecutionContext* context, uint64_t address, void* destination,
                         size_t size)
{
    const auto* process = static_cast<const ProcessRecord*>(context->syscall_context);
    return process != nullptr && CopyFromUser(*process, address, destination, size);
}

uint64_t RunUserProgram(void* context)
{
    auto* program = static_cast<UserProgram*>(context);
    if (program == nullptr)
    {
        return User::ExitStatusFault;
    }
    RunInUserMode(program, program->entry_point, program->stack_pointer, program->argument_pointer,
                  &program->execution_context);
    return program->execution_context.exit_status;
}

SpawnStatus Spawn(const char* command_line, const char* environment, ProcessId parent,
                  uint64_t flags, ProcessId* process_id)
{
    if (command_line == nullptr || process_id == nullptr)
    {
        return SpawnStatus::InvalidArguments;
    }
    // The arguments block holds pointers into itself, so its address is chosen first.
    const UserLayout layout = RandomUserLayout();
    User::ProgramArguments arguments;
    if (!BuildProgramArguments(command_line, layout.arguments_address, &arguments) ||
        !CopyEnvironment(environment, User::MaximumEnvironmentSize, &arguments))
    {
        return SpawnStatus::InvalidArguments;
    }
    if (arguments.argument_count == 0)
    {
        return SpawnStatus::NotFound;
    }
    // The first argument, without the quotes that may group a path with spaces.
    const char* program = arguments.argument_storage;
    char working_directory[User::MaximumPathLength] = "/";
    {
        Sync::SpinLockGuard guard(process_lock);
        const ProcessRecord* parent_record = FindProcessLocked(parent);
        for (size_t index = 0; parent_record != nullptr && index < sizeof(working_directory);
             ++index)
        {
            working_directory[index] = parent_record->working_directory[index];
            if (working_directory[index] == '\0')
            {
                break;
            }
        }
    }
    char path[Path::MaximumLength];
    if (!BuildProgramPath(program, working_directory, path))
    {
        return SpawnStatus::NameTooLong;
    }
    Files::ProgramFile file;
    switch (file.Read(path))
    {
    case 0:
        break;
    case User::SyscallErrorNotFound:
        return SpawnStatus::NotFound;
    case User::SyscallErrorIsDirectory:
        return SpawnStatus::IsDirectory;
    case User::SyscallErrorNotExecutable:
        return SpawnStatus::InvalidExecutable;
    default:
        return SpawnStatus::NoResources;
    }
    ELF::Image image;
    if (!ELF::Parse(file.Data(), file.Size(), UserMachine, &image))
    {
        return SpawnStatus::InvalidExecutable;
    }

    // Checked outside the process lock, since it reads the disk.
    const bool home_exists = parent == 0 && Files::CheckDirectory(HomeDirectory) == 0;
    auto* process = static_cast<ProcessRecord*>(Memory::AllocateZeroed(1, sizeof(ProcessRecord)));
    if (process == nullptr)
    {
        return SpawnStatus::NoResources;
    }
    ProcessId id = 0;
    const ProcessRecord* inherited_from = nullptr;
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
        const ProcessId requested_group =
            (flags & User::SpawnSetGroup) != 0 ? flags >> User::SpawnGroupShift : 0;
        if (requested_group != 0 &&
            (parent_record == nullptr || !GroupJoinableLocked(*parent_record, requested_group)))
        {
            Memory::Deallocate(process);
            return SpawnStatus::InvalidArguments;
        }
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
        process->next_thread_id = 2;
        process->parent = parent;
        process->session_leader = (flags & User::SpawnSessionLeader) != 0;
        process->start_tick = Time::TickCount();
        if (requested_group != 0)
        {
            process->group = requested_group;
        }
        else if (process->session_leader || (flags & User::SpawnSetGroup) != 0 ||
                 parent_record == nullptr)
        {
            process->group = id;
        }
        else
        {
            process->group = parent_record->group;
        }
        process->next = process_list;
        process_list = process;
        inherited_from = parent_record;
    }
    // The parent is the caller, which cannot change its descriptors while it waits here.
    if (inherited_from != nullptr)
    {
        Files::Inherit(&process->files, &inherited_from->files);
    }
    else
    {
        Files::OpenConsole(&process->files);
    }

    SetProcessName(process, program);
    arguments.process_id = id;
    InitializeUserProgram(&process->program);
    process->program.layout = layout;
    process->program.execution_context.syscall_handlers = &ProcessSyscallHandlers;
    process->program.execution_context.syscall_context = process;
    if (inherited_from != nullptr)
    {
        Signals::Inherit(&process->program.execution_context.signals,
                         inherited_from->program.execution_context.signals);
    }
    const bool loaded = LoadUserProgram(image, arguments, &process->program);
    // Spawned programs are time-sliced: user mode runs with interrupts unmasked, and each timer
    // tick may switch to another ready task.
    process->program.execution_context.preemption_enabled = true;
    if (!loaded || !Scheduler::CreateTask(RunProcess, process, &process->task))
    {
        Files::CloseAll(&process->files);
        ReleaseUserProgram(&process->program);
        RemoveProcess(process);
        return SpawnStatus::NoResources;
    }
    // A child takes the console only from a parent that has it, so a background job cannot take
    // it from the shell by starting a program of its own.
    if ((flags & User::SpawnBackground) == 0)
    {
        Sync::SpinLockGuard guard(process_lock);
        const ProcessRecord* parent_record = FindProcessLocked(parent);
        if (parent_record == nullptr || parent_record->group == foreground_group)
        {
            foreground_group = process->group;
        }
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
        *exit_status = process->exit_status;
    }
    ReleaseUserProgram(&process->program);
    {
        // Once the foreground group's last process is reaped, the console returns to the waiter.
        Sync::SpinLockGuard guard(process_lock);
        bool group_remains = false;
        for (const ProcessRecord* other = process_list; other != nullptr; other = other->next)
        {
            group_remains = group_remains || (other != process && other->group == process->group);
        }
        const ProcessRecord* waiter = FindProcessLocked(process->parent);
        if (foreground_group == process->group && !group_remains)
        {
            foreground_group = waiter != nullptr ? waiter->group : 0;
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

// Session leaders (the shell) and init are never interrupted, so Ctrl+C at the shell's own prompt
// only discards the line.
bool InterruptForegroundJob()
{
    Sync::SpinLockGuard guard(process_lock);
    bool interrupted = false;
    for (ProcessRecord* candidate = process_list; candidate != nullptr; candidate = candidate->next)
    {
        if (candidate->group == foreground_group && !candidate->session_leader &&
            candidate->parent != 0)
        {
            SendSignalLocked(candidate, User::SignalInterrupt);
            interrupted = true;
        }
    }
    return interrupted;
}

// The group's leader, or another member if the leader has gone.
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
        if (candidate->group == foreground_group &&
            (foreground == nullptr || candidate->id == foreground_group))
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
