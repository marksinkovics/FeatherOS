#include <catch2/catch_test_macros.hpp>

#include "kernel/syscall.h"

namespace
{

using FeatherOS::Syscall::Action;
using FeatherOS::Syscall::Number;

struct OutputTarget
{
    char character = '\0';
    uint32_t write_count = 0;
    uint64_t last_arguments[3] = {};
};

bool CaptureCharacter(void* context, char character)
{
    auto* output = static_cast<OutputTarget*>(context);
    output->character = character;
    ++output->write_count;
    return true;
}

uint64_t RecordReadLine(void* context, uint64_t buffer, uint64_t capacity, uint64_t prompt)
{
    auto* output = static_cast<OutputTarget*>(context);
    output->last_arguments[0] = buffer;
    output->last_arguments[1] = capacity;
    output->last_arguments[2] = prompt;
    return 5;
}

uint64_t RecordSpawn(void* context, uint64_t command_line, uint64_t flags, uint64_t environment)
{
    auto* output = static_cast<OutputTarget*>(context);
    output->last_arguments[0] = command_line;
    output->last_arguments[1] = flags;
    output->last_arguments[2] = environment;
    return 42;
}

uint64_t RecordWait(void* context, uint64_t process_id, uint64_t status_pointer, uint64_t flags)
{
    auto* output = static_cast<OutputTarget*>(context);
    output->last_arguments[0] = process_id;
    output->last_arguments[1] = status_pointer;
    output->last_arguments[2] = flags;
    return FeatherOS::User::SyscallErrorInvalidArgument;
}

uint64_t ReadFixedClock(void*)
{
    return 1792364283;
}

uint64_t RecordExtendHeap(void* context, uint64_t increment)
{
    static_cast<OutputTarget*>(context)->last_arguments[0] = increment;
    return 0x600000;
}

uint64_t RecordSleep(void* context, uint64_t milliseconds)
{
    static_cast<OutputTarget*>(context)->last_arguments[1] = milliseconds;
    return 0;
}

uint64_t ReadFixedUptime(void*)
{
    return 65000;
}

uint64_t ReadFixedCpu(void*)
{
    return 3;
}

uint64_t ReadFixedProcessId(void*)
{
    return 12;
}

uint64_t ReadFixedParentProcessId(void*)
{
    return 4;
}

uint64_t RecordOpen(void* context, uint64_t path, uint64_t flags)
{
    auto* output = static_cast<OutputTarget*>(context);
    output->last_arguments[0] = path;
    output->last_arguments[1] = flags;
    return 7;
}

uint64_t RecordTransfer(void* context, uint64_t descriptor, uint64_t buffer, uint64_t length)
{
    auto* output = static_cast<OutputTarget*>(context);
    output->last_arguments[0] = descriptor;
    output->last_arguments[1] = buffer;
    output->last_arguments[2] = length;
    return length / 2;
}

uint64_t RecordDescriptor(void* context, uint64_t descriptor)
{
    static_cast<OutputTarget*>(context)->last_arguments[0] = descriptor;
    return 0;
}

uint64_t RecordSeekFrom(void* context, uint64_t descriptor, uint64_t offset, uint64_t whence)
{
    auto* output = static_cast<OutputTarget*>(context);
    output->last_arguments[0] = descriptor;
    output->last_arguments[1] = offset;
    output->last_arguments[2] = whence;
    return offset;
}

uint64_t RecordSeek(void* context, uint64_t descriptor, uint64_t offset)
{
    auto* output = static_cast<OutputTarget*>(context);
    output->last_arguments[0] = descriptor;
    output->last_arguments[1] = offset;
    return 0;
}

uint64_t RecordRemove(void* context, uint64_t path)
{
    static_cast<OutputTarget*>(context)->last_arguments[2] = path;
    return 0;
}

constexpr FeatherOS::Syscall::Handlers WriteOnly = {.write_character = CaptureCharacter};
constexpr FeatherOS::Syscall::Handlers AllHandlers = {
    .write_character = CaptureCharacter,
    .read_line = RecordReadLine,
    .spawn = RecordSpawn,
    .wait = RecordWait,
    .read_clock = ReadFixedClock,
    .extend_heap = RecordExtendHeap,
    .sleep = RecordSleep,
    .read_uptime = ReadFixedUptime,
    .current_cpu = ReadFixedCpu,
    .open = RecordOpen,
    .read = RecordTransfer,
    .write = RecordTransfer,
    .close = RecordDescriptor,
    .seek = RecordSeekFrom,
    .remove = RecordRemove,
    .read_directory = RecordTransfer,
    .make_directory = RecordRemove,
    .remove_directory = RecordRemove,
    .change_directory = RecordRemove,
    .current_directory = RecordSeek,
    .shutdown = RecordDescriptor,
    .stat = RecordSeek,
    .stat_descriptor = RecordSeek,
    .process_id = ReadFixedProcessId,
    .parent_process_id = ReadFixedParentProcessId,
    .duplicate = RecordSeekFrom,
    .descriptor_flags = RecordSeekFrom,
    .pipe = RecordSeek,
    .process_group = RecordDescriptor,
    .foreground_group = RecordSeek,
    .kill = RecordSeek,
    .signal_action = RecordSeekFrom,
    .signal_mask = RecordSeek,
    .pause = ReadFixedProcessId,
    .terminal_size = ReadFixedProcessId,
    .terminal_mode = RecordSeek,
    .thread_create = RecordSeekFrom,
    .thread_join = RecordSeek,
    .thread_id = ReadFixedProcessId,
    .futex_wait = RecordSeekFrom,
    .futex_wake = RecordSeek,
    .thread_detach = RecordDescriptor,
    .rename = RecordSeek,
};

FeatherOS::Syscall::Request MakeRequest(Number number, uint64_t first = 0, uint64_t second = 0,
                                        uint64_t third = 0)
{
    return {static_cast<uint64_t>(number), {first, second, third}};
}

} // namespace

TEST_CASE("syscall exit returns the requested status")
{
    const FeatherOS::Syscall::Result result =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::Exit, 7), WriteOnly, nullptr);

    REQUIRE(result.action == Action::Exit);
    REQUIRE(result.value == 7);
}

TEST_CASE("syscall write-character accepts printable ASCII and newlines")
{
    OutputTarget output;
    FeatherOS::Syscall::Result result =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::WriteCharacter, 'A'), WriteOnly, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 0);
    REQUIRE(output.character == 'A');

    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::WriteCharacter, '\n'), WriteOnly,
                                          &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(output.character == '\n');
    REQUIRE(output.write_count == 2);
}

TEST_CASE("syscall dispatcher rejects invalid requests")
{
    OutputTarget output;
    REQUIRE(FeatherOS::Syscall::Dispatch({99, {0, 0, 0}}, AllHandlers, &output).action ==
            Action::Invalid);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::WriteCharacter, '\t'), WriteOnly,
                                         &output)
                .action == Action::Invalid);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::WriteCharacter, 0x80), WriteOnly,
                                         &output)
                .action == Action::Invalid);
    const FeatherOS::Syscall::Handlers no_output = {};
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::WriteCharacter, 'A'), no_output,
                                         &output)
                .action == Action::Invalid);
    REQUIRE(output.write_count == 0);
}

TEST_CASE("syscall dispatcher rejects process syscalls without handlers")
{
    OutputTarget output;
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::ReadLine), WriteOnly, &output).action ==
            Action::Invalid);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::Spawn), WriteOnly, &output).action ==
            Action::Invalid);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::Wait), WriteOnly, &output).action ==
            Action::Invalid);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::ReadClock), WriteOnly, &output)
                .action == Action::Invalid);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::ReadUptime), WriteOnly, &output)
                .action == Action::Invalid);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::CurrentCpu), WriteOnly, &output)
                .action == Action::Invalid);
    for (const Number number : {Number::Open, Number::Read, Number::Write, Number::Close,
                                Number::Seek, Number::Remove, Number::ReadDirectory,
                                Number::MakeDirectory, Number::RemoveDirectory,
                                Number::ChangeDirectory, Number::CurrentDirectory,
                                Number::Shutdown})
    {
        REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(number), WriteOnly, &output).action ==
                Action::Invalid);
    }
}

TEST_CASE("syscall dispatcher routes process syscalls and returns handler results")
{
    OutputTarget output;
    FeatherOS::Syscall::Result result = FeatherOS::Syscall::Dispatch(
        MakeRequest(Number::ReadLine, 0x1000, 64, 0x2000), AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 5);
    REQUIRE(output.last_arguments[0] == 0x1000);
    REQUIRE(output.last_arguments[1] == 64);
    REQUIRE(output.last_arguments[2] == 0x2000);

    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::Spawn, 0x3000, 2, 0x3800),
                                          AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 42);
    REQUIRE(output.last_arguments[0] == 0x3000);
    REQUIRE(output.last_arguments[1] == 2);
    REQUIRE(output.last_arguments[2] == 0x3800);

    result =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::Wait, 9, 0x4000, 1), AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(FeatherOS::User::IsSyscallError(result.value));
    REQUIRE(output.last_arguments[0] == 9);
    REQUIRE(output.last_arguments[1] == 0x4000);
    REQUIRE(output.last_arguments[2] == 1);

    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::ReadClock), AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 1792364283);

    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::ExtendHeap, 8192), AllHandlers,
                                          &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 0x600000);
    REQUIRE(output.last_arguments[0] == 8192);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::ExtendHeap, 1), WriteOnly, nullptr)
                .action == Action::Invalid);

    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::Sleep, 250), AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 0);
    REQUIRE(output.last_arguments[1] == 250);

    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::ReadUptime), AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 65000);

    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::CurrentCpu), AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 3);

    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::ProcessId), AllHandlers, &output);
    REQUIRE(result.value == 12);
    result =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::ParentProcessId), AllHandlers, &output);
    REQUIRE(result.value == 4);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::ProcessId), WriteOnly, nullptr)
                .action == Action::Invalid);
}

TEST_CASE("syscall dispatcher routes file syscalls with their arguments")
{
    OutputTarget output;
    FeatherOS::Syscall::Result result =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::Open, 0x5000, 6), AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 7);
    REQUIRE(output.last_arguments[0] == 0x5000);
    REQUIRE(output.last_arguments[1] == 6);

    for (const Number number : {Number::Read, Number::Write, Number::ReadDirectory})
    {
        result =
            FeatherOS::Syscall::Dispatch(MakeRequest(number, 4, 0x6000, 64), AllHandlers, &output);
        REQUIRE(result.value == 32);
        REQUIRE(output.last_arguments[0] == 4);
        REQUIRE(output.last_arguments[1] == 0x6000);
        REQUIRE(output.last_arguments[2] == 64);
    }

    result =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::Seek, 5, 99, 2), AllHandlers, &output);
    REQUIRE(result.value == 99);
    REQUIRE(output.last_arguments[0] == 5);
    REQUIRE(output.last_arguments[1] == 99);
    REQUIRE(output.last_arguments[2] == 2);
    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::Close, 6), AllHandlers, &output);
    REQUIRE(output.last_arguments[0] == 6);
    for (const Number number : {Number::Remove, Number::MakeDirectory, Number::RemoveDirectory,
                                Number::ChangeDirectory})
    {
        output.last_arguments[2] = 0;
        result = FeatherOS::Syscall::Dispatch(MakeRequest(number, 0x7000), AllHandlers, &output);
        REQUIRE(result.value == 0);
        REQUIRE(output.last_arguments[2] == 0x7000);
    }
    result = FeatherOS::Syscall::Dispatch(MakeRequest(Number::CurrentDirectory, 0x8000, 64),
                                          AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(output.last_arguments[0] == 0x8000);
    REQUIRE(output.last_arguments[1] == 64);

    for (const Number number : {Number::Stat, Number::StatDescriptor})
    {
        output.last_arguments[0] = 0;
        output.last_arguments[1] = 0;
        result = FeatherOS::Syscall::Dispatch(MakeRequest(number, 0x9000, 0xA000), AllHandlers,
                                              &output);
        REQUIRE(result.value == 0);
        REQUIRE(output.last_arguments[0] == 0x9000);
        REQUIRE(output.last_arguments[1] == 0xA000);
    }
}

TEST_CASE("syscall duplicate and descriptor-flags pass three arguments")
{
    OutputTarget output;
    FeatherOS::Syscall::Result result = FeatherOS::Syscall::Dispatch(
        MakeRequest(Number::Duplicate, 4, 1, FeatherOS::User::DuplicateExact), AllHandlers,
        &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(output.last_arguments[0] == 4);
    REQUIRE(output.last_arguments[1] == 1);
    REQUIRE(output.last_arguments[2] == FeatherOS::User::DuplicateExact);
    result = FeatherOS::Syscall::Dispatch(
        MakeRequest(Number::DescriptorFlags, 5, 1, FeatherOS::User::DescriptorCloseOnExec),
        AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(output.last_arguments[0] == 5);
    REQUIRE(output.last_arguments[2] == FeatherOS::User::DescriptorCloseOnExec);
}

TEST_CASE("syscall signal-return is left to the architecture code")
{
    OutputTarget output;
    const FeatherOS::Syscall::Result result =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::SignalReturn), WriteOnly, &output);
    REQUIRE(result.action == Action::SignalReturn);
    const FeatherOS::Syscall::Result kill = FeatherOS::Syscall::Dispatch(
        MakeRequest(Number::Kill, 12, FeatherOS::User::SignalTerminate), AllHandlers, &output);
    REQUIRE(kill.action == Action::Return);
    REQUIRE(output.last_arguments[0] == 12);
    REQUIRE(output.last_arguments[1] == FeatherOS::User::SignalTerminate);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::Pause), WriteOnly, &output).action ==
            Action::Invalid);
    const auto mode = FeatherOS::Syscall::Dispatch(MakeRequest(Number::TerminalMode, 0x7000, 0x7100),
                                                   AllHandlers, &output);
    REQUIRE(mode.action == Action::Return);
    REQUIRE(output.last_arguments[0] == 0x7000);
    REQUIRE(output.last_arguments[1] == 0x7100);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::TerminalMode), WriteOnly, &output)
                .action == Action::Invalid);
    const auto create = FeatherOS::Syscall::Dispatch(
        MakeRequest(Number::ThreadCreate, 0x1000, 0x2000, 0x3000), AllHandlers, &output);
    REQUIRE(create.action == Action::Return);
    REQUIRE(output.last_arguments[0] == 0x1000);
    REQUIRE(output.last_arguments[1] == 0x2000);
    REQUIRE(output.last_arguments[2] == 0x3000);
    const auto join = FeatherOS::Syscall::Dispatch(MakeRequest(Number::ThreadJoin, 3, 0x4000),
                                                   AllHandlers, &output);
    REQUIRE(join.action == Action::Return);
    REQUIRE(output.last_arguments[0] == 3);
    REQUIRE(output.last_arguments[1] == 0x4000);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::ThreadId), AllHandlers, &output)
                .action == Action::Return);
    // Ending one thread needs a program that can have threads.
    const auto thread_exit =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::ThreadExit, 42), AllHandlers, &output);
    REQUIRE(thread_exit.action == Action::ExitThread);
    REQUIRE(thread_exit.value == 42);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::ThreadExit, 1), WriteOnly, &output)
                .action == Action::Invalid);
    const auto futex_wait = FeatherOS::Syscall::Dispatch(
        MakeRequest(Number::FutexWait, 0x5000, 2, 30), AllHandlers, &output);
    REQUIRE(futex_wait.action == Action::Return);
    REQUIRE(output.last_arguments[0] == 0x5000);
    REQUIRE(output.last_arguments[1] == 2);
    REQUIRE(output.last_arguments[2] == 30);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::FutexWake, 0x5000, 1), AllHandlers,
                                         &output)
                .action == Action::Return);
    REQUIRE(output.last_arguments[1] == 1);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::ThreadDetach, 4), AllHandlers,
                                         &output)
                .action == Action::Return);
    REQUIRE(output.last_arguments[0] == 4);
    REQUIRE(FeatherOS::Syscall::Dispatch(MakeRequest(Number::Rename, 0x7000, 0x7100), AllHandlers,
                                         &output)
                .action == Action::Return);
    REQUIRE(output.last_arguments[0] == 0x7000);
    REQUIRE(output.last_arguments[1] == 0x7100);
}

