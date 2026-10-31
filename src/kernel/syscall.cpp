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
    if (request.number == static_cast<uint64_t>(Number::ThreadExit) &&
        handlers.thread_create != nullptr)
    {
        return {Action::ExitThread, arguments[0]};
    }
    if (request.number == static_cast<uint64_t>(Number::SignalReturn))
    {
        return {Action::SignalReturn, 0};
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
    if (request.number == static_cast<uint64_t>(Number::Kill) && handlers.kill != nullptr)
    {
        return {Action::Return, handlers.kill(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::SignalAction) &&
        handlers.signal_action != nullptr)
    {
        return {Action::Return,
                handlers.signal_action(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::SignalMask) &&
        handlers.signal_mask != nullptr)
    {
        return {Action::Return, handlers.signal_mask(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::Pause) && handlers.pause != nullptr)
    {
        return {Action::Return, handlers.pause(context)};
    }
    if (request.number == static_cast<uint64_t>(Number::TerminalSize) &&
        handlers.terminal_size != nullptr)
    {
        return {Action::Return, handlers.terminal_size(context)};
    }
    if (request.number == static_cast<uint64_t>(Number::TerminalMode) &&
        handlers.terminal_mode != nullptr)
    {
        return {Action::Return, handlers.terminal_mode(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::ThreadCreate) &&
        handlers.thread_create != nullptr)
    {
        return {Action::Return,
                handlers.thread_create(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::ThreadJoin) &&
        handlers.thread_join != nullptr)
    {
        return {Action::Return, handlers.thread_join(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::ThreadId) && handlers.thread_id != nullptr)
    {
        return {Action::Return, handlers.thread_id(context)};
    }
    if (request.number == static_cast<uint64_t>(Number::FutexWait) &&
        handlers.futex_wait != nullptr)
    {
        return {Action::Return,
                handlers.futex_wait(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::FutexWake) &&
        handlers.futex_wake != nullptr)
    {
        return {Action::Return, handlers.futex_wake(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::ThreadDetach) &&
        handlers.thread_detach != nullptr)
    {
        return {Action::Return, handlers.thread_detach(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::Rename) && handlers.rename != nullptr)
    {
        return {Action::Return, handlers.rename(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::Socket) && handlers.socket != nullptr)
    {
        return {Action::Return, handlers.socket(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::Bind) && handlers.bind != nullptr)
    {
        return {Action::Return, handlers.bind(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::SendTo) && handlers.send_to != nullptr)
    {
        return {Action::Return,
                handlers.send_to(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::ReceiveFrom) &&
        handlers.receive_from != nullptr)
    {
        return {Action::Return,
                handlers.receive_from(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::SetSocketOption) &&
        handlers.set_socket_option != nullptr)
    {
        return {Action::Return,
                handlers.set_socket_option(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::NetworkSettings) &&
        handlers.network_settings != nullptr)
    {
        return {Action::Return,
                handlers.network_settings(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::Connect) && handlers.connect != nullptr)
    {
        return {Action::Return, handlers.connect(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::Listen) && handlers.listen != nullptr)
    {
        return {Action::Return, handlers.listen(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::Accept) && handlers.accept != nullptr)
    {
        return {Action::Return, handlers.accept(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::ShutdownSocket) &&
        handlers.shutdown_socket != nullptr)
    {
        return {Action::Return, handlers.shutdown_socket(context, arguments[0], arguments[1])};
    }
    if (request.number == static_cast<uint64_t>(Number::SocketName) &&
        handlers.socket_name != nullptr)
    {
        return {Action::Return,
                handlers.socket_name(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::Poll) && handlers.poll != nullptr)
    {
        return {Action::Return, handlers.poll(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::SetClock) && handlers.set_clock != nullptr)
    {
        return {Action::Return, handlers.set_clock(context, arguments[0])};
    }
    if (request.number == static_cast<uint64_t>(Number::FileStatusFlags) &&
        handlers.file_status_flags != nullptr)
    {
        return {Action::Return,
                handlers.file_status_flags(context, arguments[0], arguments[1], arguments[2])};
    }
    if (request.number == static_cast<uint64_t>(Number::GetSocketOption) &&
        handlers.get_socket_option != nullptr)
    {
        return {Action::Return,
                handlers.get_socket_option(context, arguments[0], arguments[1], arguments[2])};
    }
    return {Action::Invalid, UINT64_MAX};
}

} // namespace FeatherOS::Syscall
