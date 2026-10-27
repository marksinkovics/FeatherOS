#ifndef FEATHEROS_SYSCALL_H
#define FEATHEROS_SYSCALL_H

#include "user/syscall_abi.h"

#include <stdint.h>

namespace FeatherOS::Syscall
{

using Number = User::SyscallNumber;

enum class Action : uint8_t
{
    Return,
    // Exit ends the process; ExitThread only the calling thread.
    Exit,
    ExitThread,
    // The architecture code puts back the registers a signal handler's frame saved.
    SignalReturn,
    Invalid,
};

struct Result
{
    Action action;
    uint64_t value;
};

struct Request
{
    uint64_t number;
    uint64_t arguments[3];
};

using WriteCharacter = bool (*)(void* context, char character);

// Services behind each syscall. A null handler makes that syscall invalid for the caller.
// Handlers other than write_character return the value the user program receives.
struct Handlers
{
    WriteCharacter write_character;
    uint64_t (*read_line)(void* context, uint64_t buffer, uint64_t capacity, uint64_t prompt);
    uint64_t (*spawn)(void* context, uint64_t command_line, uint64_t flags, uint64_t environment);
    uint64_t (*wait)(void* context, uint64_t process_id, uint64_t status_pointer, uint64_t flags);
    uint64_t (*read_clock)(void* context);
    uint64_t (*extend_heap)(void* context, uint64_t increment);
    uint64_t (*sleep)(void* context, uint64_t milliseconds);
    uint64_t (*read_uptime)(void* context);
    uint64_t (*current_cpu)(void* context);
    uint64_t (*open)(void* context, uint64_t path, uint64_t flags);
    uint64_t (*read)(void* context, uint64_t descriptor, uint64_t buffer, uint64_t length);
    uint64_t (*write)(void* context, uint64_t descriptor, uint64_t buffer, uint64_t length);
    uint64_t (*close)(void* context, uint64_t descriptor);
    uint64_t (*seek)(void* context, uint64_t descriptor, uint64_t offset, uint64_t whence);
    uint64_t (*remove)(void* context, uint64_t path);
    uint64_t (*read_directory)(void* context, uint64_t path, uint64_t index, uint64_t entry);
    uint64_t (*make_directory)(void* context, uint64_t path);
    uint64_t (*remove_directory)(void* context, uint64_t path);
    uint64_t (*change_directory)(void* context, uint64_t path);
    uint64_t (*current_directory)(void* context, uint64_t buffer, uint64_t capacity);
    uint64_t (*shutdown)(void* context, uint64_t action);
    uint64_t (*stat)(void* context, uint64_t path, uint64_t status);
    uint64_t (*stat_descriptor)(void* context, uint64_t descriptor, uint64_t status);
    uint64_t (*process_id)(void* context);
    uint64_t (*parent_process_id)(void* context);
    uint64_t (*duplicate)(void* context, uint64_t descriptor, uint64_t target, uint64_t flags);
    uint64_t (*descriptor_flags)(void* context, uint64_t descriptor, uint64_t set, uint64_t flags);
    uint64_t (*pipe)(void* context, uint64_t descriptors, uint64_t flags);
    uint64_t (*process_group)(void* context, uint64_t process_id);
    uint64_t (*foreground_group)(void* context, uint64_t set, uint64_t group);
    uint64_t (*kill)(void* context, uint64_t target, uint64_t signal);
    uint64_t (*signal_action)(void* context, uint64_t signal, uint64_t new_action,
                              uint64_t old_action);
    uint64_t (*signal_mask)(void* context, uint64_t how, uint64_t set);
    uint64_t (*pause)(void* context);
    uint64_t (*terminal_size)(void* context);
    uint64_t (*terminal_mode)(void* context, uint64_t new_settings, uint64_t old_settings);
    uint64_t (*thread_create)(void* context, uint64_t entry, uint64_t first, uint64_t second);
    uint64_t (*thread_join)(void* context, uint64_t thread, uint64_t value);
    uint64_t (*thread_id)(void* context);
    uint64_t (*futex_wait)(void* context, uint64_t address, uint64_t expected,
                           uint64_t milliseconds);
    uint64_t (*futex_wake)(void* context, uint64_t address, uint64_t count);
    uint64_t (*thread_detach)(void* context, uint64_t thread);
    uint64_t (*rename)(void* context, uint64_t from, uint64_t to);
};

Result Dispatch(const Request& request, const Handlers& handlers, void* context);

} // namespace FeatherOS::Syscall

#endif
