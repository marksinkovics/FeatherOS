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
        return {Action::Return, handlers.spawn(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::Wait) && handlers.wait != nullptr)
    {
        return {Action::Return, handlers.wait(context, arguments[0], arguments[1], arguments[2])};
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
    if (request.number == static_cast<uint64_t>(Number::Open) && handlers.open != nullptr)
    {
        return {Action::Return, handlers.open(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::Read) && handlers.read != nullptr)
    {
        return {Action::Return, handlers.read(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::Write) && handlers.write != nullptr)
    {
        return {Action::Return, handlers.write(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::Close) && handlers.close != nullptr)
    {
        return {Action::Return, handlers.close(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::Seek) && handlers.seek != nullptr)
    {
        return {Action::Return, handlers.seek(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::Remove) && handlers.remove != nullptr)
    {
        return {Action::Return, handlers.remove(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::ReadDirectory) &&
        handlers.read_directory != nullptr)
    {
        return {Action::Return,
                handlers.read_directory(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::MakeDirectory) &&
        handlers.make_directory != nullptr)
    {
        return {Action::Return, handlers.make_directory(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::RemoveDirectory) &&
        handlers.remove_directory != nullptr)
    {
        return {Action::Return, handlers.remove_directory(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::ChangeDirectory) &&
        handlers.change_directory != nullptr)
    {
        return {Action::Return, handlers.change_directory(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::CurrentDirectory) &&
        handlers.current_directory != nullptr)
    {
        return {Action::Return, handlers.current_directory(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::Shutdown) && handlers.shutdown != nullptr)
    {
        return {Action::Return, handlers.shutdown(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::Stat) && handlers.stat != nullptr)
    {
        return {Action::Return, handlers.stat(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::StatDescriptor) &&
        handlers.stat_descriptor != nullptr)
    {
        return {Action::Return, handlers.stat_descriptor(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::ProcessId) &&
        handlers.process_id != nullptr)
    {
        return {Action::Return, handlers.process_id(context)};
    }
    if (request.number == static_cast<uint64_t>(Number::ParentProcessId) &&
        handlers.parent_process_id != nullptr)
    {
        return {Action::Return, handlers.parent_process_id(context)};
    }
    if (request.number == static_cast<uint64_t>(Number::Duplicate) && handlers.duplicate != nullptr)
    {
        return {Action::Return,
                handlers.duplicate(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::DescriptorFlags) &&
        handlers.descriptor_flags != nullptr)
    {
        return {Action::Return,
                handlers.descriptor_flags(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::Pipe) && handlers.pipe != nullptr)
    {
        return {Action::Return, handlers.pipe(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::ProcessGroup) &&
        handlers.process_group != nullptr)
    {
        return {Action::Return, handlers.process_group(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::ForegroundGroup) &&
        handlers.foreground_group != nullptr)
    {
        return {Action::Return, handlers.foreground_group(context, arguments[0], arguments[1])};
    }
    return {Action::Invalid, UINT64_MAX};
}

} // namespace FeatherOS::Syscall
