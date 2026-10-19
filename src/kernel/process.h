#ifndef FEATHEROS_PROCESS_H
#define FEATHEROS_PROCESS_H

#include "kernel/elf/elf_loader.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/user_context.h"
#include "user/process.h"

#include <stdint.h>

namespace FeatherOS::Process
{

using ProcessId = uint64_t;

#if defined(ARCH_x86_64)
constexpr uint64_t UserStackAddress = 0x800000;
constexpr uint64_t UserAddressLimit = 0x0000800000000000;
#elif defined(ARCH_aarch64)
constexpr uint64_t UserStackAddress = 0x0000400000800000;
constexpr uint64_t UserAddressLimit = 0x0001000000000000;
#else
#error "Unsupported architecture for user processes"
#endif
constexpr uint64_t UserStackTop = UserStackAddress + 4096;
// The stack grows down on demand through this region; its lowest page always stays unmapped.
constexpr uint64_t UserStackRegionPages = 64;
constexpr uint64_t UserStackRegionBase = UserStackTop - UserStackRegionPages * 4096;
// Every program sees its read-only ProgramArguments block here, one guard page above its stack.
constexpr uint64_t ArgumentsAddress = UserStackAddress + 2 * 4096;

// An ELF image loaded into its own address space, ready to run as a scheduler task.
struct UserProgram
{
    Memory::AddressSpace address_space;
    uint32_t mapped_page_count;
    uint32_t page_limit;
    uint64_t entry_point;
    uint64_t stack_pointer;
    uint64_t argument_pointer;
    uint64_t heap_start;
    uint64_t heap_break;
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
// Scheduler task entry: runs a loaded program in user mode and returns its exit status.
uint64_t RunUserProgram(void* program);

// Starts "<name>-<arch>.elf" from the initramfs, where name is the first word of command_line,
// and makes it the foreground process that receives console input.
SpawnStatus Spawn(const char* command_line, ProcessId parent, ProcessId* process_id);
// Waits for a spawned process to exit, then releases its task and memory and returns console
// input to its parent.
bool Wait(ProcessId process_id, uint64_t* exit_status);

} // namespace FeatherOS::Process

#endif
