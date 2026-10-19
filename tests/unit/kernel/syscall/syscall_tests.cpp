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

uint64_t RecordSpawn(void* context, uint64_t command_line)
{
    static_cast<OutputTarget*>(context)->last_arguments[0] = command_line;
    return 42;
}

uint64_t RecordWait(void* context, uint64_t process_id, uint64_t status_pointer)
{
    auto* output = static_cast<OutputTarget*>(context);
    output->last_arguments[0] = process_id;
    output->last_arguments[1] = status_pointer;
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

constexpr FeatherOS::Syscall::Handlers WriteOnly = {
    CaptureCharacter, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
constexpr FeatherOS::Syscall::Handlers AllHandlers = {
    CaptureCharacter, RecordReadLine,   RecordSpawn, RecordWait,
    ReadFixedClock,   RecordExtendHeap, RecordSleep};

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
    const FeatherOS::Syscall::Handlers no_output = {nullptr, nullptr, nullptr, nullptr,
                                                    nullptr, nullptr, nullptr};
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

    result =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::Spawn, 0x3000), AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(result.value == 42);
    REQUIRE(output.last_arguments[0] == 0x3000);

    result =
        FeatherOS::Syscall::Dispatch(MakeRequest(Number::Wait, 9, 0x4000), AllHandlers, &output);
    REQUIRE(result.action == Action::Return);
    REQUIRE(FeatherOS::User::IsSyscallError(result.value));
    REQUIRE(output.last_arguments[0] == 9);
    REQUIRE(output.last_arguments[1] == 0x4000);

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
}
