#ifndef FEATHEROS_PROCESS_H
#define FEATHEROS_PROCESS_H

#include "kernel/elf/elf_loader.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/sync/spinlock.h"
#include "kernel/user_context.h"
#include "user/process.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Process
{

using ProcessId = uint64_t;

#if defined(ARCH_x86_64)
constexpr uint64_t UserStackAddress = 0x800000;
constexpr uint64_t UserAddressLimit = 0x0000800000000000;
constexpr uint64_t RandomStackWindowBase = 0x0000700000000000;
constexpr uint64_t DefaultImageBase = 0x400000;
#elif defined(ARCH_aarch64)
constexpr uint64_t UserStackAddress = 0x0000400000800000;
// The page-table code treats addresses with bit 47 set as kernel-half, as on x86-64.
constexpr uint64_t UserAddressLimit = 0x0000800000000000;
constexpr uint64_t RandomStackWindowBase = 0x0000600000000000;
constexpr uint64_t DefaultImageBase = 0x0000400000000000;
#else
#error "Unsupported architecture for user processes"
#endif
constexpr uint64_t UserStackTop = UserStackAddress + 4096;
// The stack grows down on demand through this region; its lowest page always stays unmapped.
constexpr uint64_t UserStackRegionPages = 64;
// Every program sees its read-only ProgramArguments block here, one guard page above its stack.
constexpr uint64_t ArgumentsAddress = UserStackAddress + 2 * 4096;
constexpr uint64_t RandomStackWindowPages = 1ULL << 28;
constexpr uint64_t RandomHeapOffsetPages = 1ULL << 16;
// Position-independent images load at a random page in this window, far below the stack window.
constexpr uint64_t RandomImageWindowBase = 0x0000100000000000;
constexpr uint64_t RandomImageWindowPages = 1ULL << 28;

struct UserLayout
{
    uint64_t image_base;
    uint64_t stack_top;
    uint64_t arguments_address;
    uint64_t heap_offset;
};

UserLayout DefaultUserLayout();
UserLayout RandomUserLayout();

// An ELF image loaded into its own address space, ready to run as a scheduler task. Its threads
// share it; execution_context is the main thread's.
struct UserProgram
{
    Memory::AddressSpace address_space;
    // Threads on different CPUs map pages at once: it guards the mappings and the page count.
    Sync::SpinLock memory_lock;
    uint32_t mapped_page_count;
    uint32_t page_limit;
    // Bit n is set once thread stack slot n has been used; slot 0 is the main thread's.
    uint32_t stack_slots;
    // The PT_TLS template in the program's memory; each thread's TLS block starts as a copy.
    uint64_t tls_template;
    uint64_t tls_file_size;
    uint64_t tls_memory_size;
    uint64_t tls_alignment;
    uint64_t entry_point;
    uint64_t stack_pointer;
    uint64_t argument_pointer;
    uint64_t heap_start;
    uint64_t heap_break;
    UserLayout layout;
    UserExecutionContext execution_context;
};

enum class SpawnStatus : uint8_t
{
    Started,
    NotFound,
    InvalidExecutable,
    InvalidArguments,
    NoResources,
};

void Initialize(uint64_t hhdm_offset);

void InitializeUserProgram(UserProgram* program);
// Creates an address space for image with its segments, a stack, and a copy of arguments.
// Leaves the caller's address space active. On failure, the caller destroys
// program->address_space if its root is set.
bool LoadUserProgram(const ELF::Image& image, const User::ProgramArguments& arguments,
                     UserProgram* program);
// Maps a stack page for a not-present user fault in the growable stack region.
bool HandleUserPageFault(UserExecutionContext* context, uint64_t fault_address);
// For signal frames: copy between kernel memory and the current program's memory, mapping
// stack pages on demand. False for memory the program cannot write or read.
bool CopyToCurrentUser(UserExecutionContext* context, uint64_t address, const void* source,
                       size_t size);
bool CopyFromCurrentUser(UserExecutionContext* context, uint64_t address, void* destination,
                         size_t size);
// Scheduler task entry: runs a loaded program in user mode and returns its exit status.
uint64_t RunUserProgram(void* program);

// Starts "<name>-<arch>.elf" from the initramfs, where name is the first word of command_line.
// environment is a kernel copy of a Spawn environment block, at most
// User::MaximumEnvironmentSize bytes, or null for none. Unless flags has User::SpawnBackground,
// the child becomes the foreground process that receives console input.
SpawnStatus Spawn(const char* command_line, const char* environment, ProcessId parent,
                  uint64_t flags, ProcessId* process_id);
// Waits for a spawned process to exit, then releases its task and memory and returns console
// input to its parent.
bool Wait(ProcessId process_id, uint64_t* exit_status);
// For shutdown: marks every process except caller and its ancestors for termination and waits up
// to two seconds for them to exit. False if some are still running.
bool StopOtherProcesses(ProcessId caller);
// Ctrl+C: marks the foreground job and its descendants for termination. Returns false when only
// the session leader holds the console.
bool InterruptForegroundJob();
// "kernel" when no process holds the console.
void ForegroundProgramName(char* buffer, size_t capacity);

} // namespace FeatherOS::Process

#endif
