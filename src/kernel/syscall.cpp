#include "syscall.h"

namespace FeatherOS::Syscall
{

Result Dispatch(const Request& request, const Handlers& handlers, void* context)
{
    const uint64_t* arguments = request.arguments;
    if (request.number == static_cast<uint64_t>(Number::Exit))
    {
        return {Action::Exit, arguments[0]};
    }
    if (request.number == static_cast<uint64_t>(Number::WriteCharacter))
    {
        const uint64_t character = arguments[0];
        const bool valid = (character >= 0x20 && character <= 0x7E) || character == '\n';
        if (valid && handlers.write_character != nullptr &&
            handlers.write_character(context, static_cast<char>(character)))
        {
            return {Action::Return, 0};
        }
        return {Action::Invalid, UINT64_MAX};
    }
    if (request.number == static_cast<uint64_t>(Number::ReadLine) && handlers.read_line != nullptr)
    {
        return {Action::Return,
                handlers.read_line(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::Spawn) && handlers.spawn != nullptr)
    {
        return {Action::Return, handlers.spawn(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::Wait) && handlers.wait != nullptr)
    {
        return {Action::Return, handlers.wait(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::ReadClock) &&
        handlers.read_clock != nullptr)
    {
        return {Action::Return, handlers.read_clock(context)};
    }
    if (request.number == static_cast<uint64_t>(Number::ExtendHeap) &&
        handlers.extend_heap != nullptr)
    {
        return {Action::Return, handlers.extend_heap(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::Sleep) && handlers.sleep != nullptr)
    {
        return {Action::Return, handlers.sleep(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::ReadUptime) &&
        handlers.read_uptime != nullptr)
    {
        return {Action::Return, handlers.read_uptime(context)};
    }
    if (request.number == static_cast<uint64_t>(Number::CurrentCpu) &&
        handlers.current_cpu != nullptr)
    {
        return {Action::Return, handlers.current_cpu(context)};
    }
    return {Action::Invalid, UINT64_MAX};
}

} // namespace FeatherOS::Syscall
