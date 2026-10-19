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
    Exit,
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
    uint64_t (*spawn)(void* context, uint64_t command_line);
    uint64_t (*wait)(void* context, uint64_t process_id, uint64_t status_pointer);
    uint64_t (*read_clock)(void* context);
};

Result Dispatch(const Request& request, const Handlers& handlers, void* context);

} // namespace FeatherOS::Syscall

#endif
