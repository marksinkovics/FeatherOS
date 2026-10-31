#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/pipe.h"

#include <string>
#include <vector>

namespace
{

// Host tests cannot block, so every wait that would block is cut short.
bool AlwaysInterrupted()
{
    return true;
}

struct PipeTable
{
    PipeTable()
    {
        FeatherOS::Files::OpenConsole(&table);
        REQUIRE(FeatherOS::Files::CreatePipe(&table, 0, &reader, &writer) == 0);
    }

    ~PipeTable()
    {
        FeatherOS::Files::CloseAll(&table);
    }

    FeatherOS::Files::Table table{};
    uint64_t reader = 0;
    uint64_t writer = 0;
};

std::string ReadSome(FeatherOS::Files::Table* table, uint64_t descriptor, size_t length)
{
    std::string text(length, '\0');
    const uint64_t count =
        FeatherOS::Files::Read(table, descriptor, text.data(), length, AlwaysInterrupted);
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(count));
    text.resize(count);
    return text;
}

} // namespace

TEST_CASE("a pipe takes the two lowest free descriptors and passes bytes in order")
{
    PipeTable pipe;
    REQUIRE(pipe.reader == 3);
    REQUIRE(pipe.writer == 4);
    REQUIRE(FeatherOS::Files::KindOf(&pipe.table, pipe.reader) == FeatherOS::Files::Kind::Pipe);
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, "hello ", 6) == 6);
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, "pipe", 4) == 4);
    FeatherOS::User::FileStatus status{};
    REQUIRE(FeatherOS::Files::StatDescriptor(&pipe.table, pipe.reader, &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Pipe);
    REQUIRE(status.size == 10);
    REQUIRE(ReadSome(&pipe.table, pipe.reader, 4) == "hell");
    REQUIRE(ReadSome(&pipe.table, pipe.reader, 100) == "o pipe");
    REQUIRE(FeatherOS::Files::Read(&pipe.table, pipe.reader, nullptr, 1, AlwaysInterrupted) ==
            FeatherOS::User::SyscallErrorInterrupted);
    REQUIRE(FeatherOS::Files::Read(&pipe.table, pipe.writer, nullptr, 1) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.reader, "x", 1) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    REQUIRE(FeatherOS::Files::Seek(&pipe.table, pipe.reader, 0, FeatherOS::User::SeekFromStart) ==
            FeatherOS::User::SyscallErrorNotSeekable);
}

TEST_CASE("a pipe reads as ended once every write end is closed")
{
    PipeTable pipe;
    const uint64_t copy = FeatherOS::Files::Duplicate(&pipe.table, pipe.writer, 0, 0);
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, "last", 4) == 4);
    REQUIRE(FeatherOS::Files::Close(&pipe.table, pipe.writer) == 0);
    REQUIRE(ReadSome(&pipe.table, pipe.reader, 10) == "last");
    // A copy of the write end still keeps it open.
    REQUIRE(FeatherOS::Files::Read(&pipe.table, pipe.reader, nullptr, 1, AlwaysInterrupted) ==
            FeatherOS::User::SyscallErrorInterrupted);
    REQUIRE(FeatherOS::Files::Close(&pipe.table, copy) == 0);
    char byte = 0;
    REQUIRE(FeatherOS::Files::Read(&pipe.table, pipe.reader, &byte, 1, AlwaysInterrupted) == 0);
}

TEST_CASE("writing a pipe with no reader left fails")
{
    PipeTable pipe;
    FeatherOS::Files::Table child{};
    FeatherOS::Files::Inherit(&child, &pipe.table);
    REQUIRE(FeatherOS::Files::Close(&pipe.table, pipe.reader) == 0);
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, "kept", 4) == 4);
    REQUIRE(ReadSome(&child, pipe.reader, 4) == "kept");
    FeatherOS::Files::CloseAll(&child);
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, "lost", 4) ==
            FeatherOS::User::SyscallErrorBrokenPipe);
}

TEST_CASE("a full pipe waits for room, and data wraps around its buffer")
{
    PipeTable pipe;
    const std::vector<char> large(FeatherOS::Pipes::Capacity, 'a');
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, large.data(), large.size(),
                                    AlwaysInterrupted) == large.size());
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, "x", 1, AlwaysInterrupted) ==
            FeatherOS::User::SyscallErrorInterrupted);
    REQUIRE(ReadSome(&pipe.table, pipe.reader, 3000) == std::string(3000, 'a'));
    std::string pattern;
    for (int index = 0; index < 2500; ++index)
    {
        pattern.push_back(static_cast<char>('a' + index % 26));
    }
    // 1096 bytes are left, so this write wraps past the end of the buffer.
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, pattern.data(), pattern.size(),
                                    AlwaysInterrupted) == pattern.size());
    REQUIRE(ReadSome(&pipe.table, pipe.reader, 4096) == std::string(1096, 'a') + pattern);
}

TEST_CASE("a write too large for the room left stops short in whole atomic pieces")
{
    PipeTable pipe;
    const std::vector<char> filler(FeatherOS::Pipes::Capacity - 600, 'f');
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, filler.data(), filler.size(),
                                    AlwaysInterrupted) == filler.size());
    const std::vector<char> rest(1000, 'r');
    REQUIRE(FeatherOS::Files::Write(&pipe.table, pipe.writer, rest.data(), rest.size(),
                                    AlwaysInterrupted) == FeatherOS::Pipes::AtomicSize);
}

TEST_CASE("pipe descriptors can be close-on-exec")
{
    FeatherOS::Files::Table table{};
    uint64_t reader = 0;
    uint64_t writer = 0;
    REQUIRE(FeatherOS::Files::CreatePipe(&table, FeatherOS::User::PipeCloseOnExec, &reader,
                                         &writer) == 0);
    REQUIRE(reader == 0);
    REQUIRE(FeatherOS::Files::DescriptorFlags(&table, writer, false, 0) ==
            FeatherOS::User::DescriptorCloseOnExec);
    FeatherOS::Files::Table child{};
    FeatherOS::Files::Inherit(&child, &table);
    REQUIRE(FeatherOS::Files::CountOpen(&child) == 0);
    REQUIRE(FeatherOS::Files::CreatePipe(&table, 4, &reader, &writer) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    FeatherOS::Files::CloseAll(&table);
}

TEST_CASE("a non-blocking pipe fails where it would wait")
{
    FeatherOS::Files::Table table{};
    uint64_t reader = 0;
    uint64_t writer = 0;
    REQUIRE(FeatherOS::Files::CreatePipe(&table, FeatherOS::User::PipeNonBlocking, &reader,
                                         &writer) == 0);
    REQUIRE(FeatherOS::Files::StatusFlags(&table, reader, false, 0) ==
            (FeatherOS::User::OpenRead | FeatherOS::User::OpenNonBlocking));
    REQUIRE(FeatherOS::Files::StatusFlags(&table, writer, false, 0) ==
            (FeatherOS::User::OpenWrite | FeatherOS::User::OpenNonBlocking));
    char text[8];
    // A wait would end in SyscallErrorInterrupted instead.
    REQUIRE(FeatherOS::Files::Read(&table, reader, text, sizeof(text), AlwaysInterrupted) ==
            FeatherOS::User::SyscallErrorWouldBlock);

    const std::vector<char> filler(FeatherOS::Pipes::Capacity - 600, 'f');
    REQUIRE(FeatherOS::Files::Write(&table, writer, filler.data(), filler.size(),
                                    AlwaysInterrupted) == filler.size());
    const std::vector<char> rest(1000, 'r');
    REQUIRE(FeatherOS::Files::Write(&table, writer, rest.data(), rest.size(), AlwaysInterrupted) ==
            FeatherOS::Pipes::AtomicSize);
    // 88 bytes are left, too few for a piece of AtomicSize, but enough for a smaller write.
    REQUIRE(FeatherOS::Files::Write(&table, writer, rest.data(), rest.size(), AlwaysInterrupted) ==
            FeatherOS::User::SyscallErrorWouldBlock);
    REQUIRE(FeatherOS::Files::Write(&table, writer, "x", 1, AlwaysInterrupted) == 1);

    std::vector<char> drained(FeatherOS::Pipes::Capacity);
    REQUIRE(FeatherOS::Files::Read(&table, reader, drained.data(), drained.size(),
                                   AlwaysInterrupted) == FeatherOS::Pipes::Capacity - 87);
    REQUIRE(FeatherOS::Files::Close(&table, writer) == 0);
    REQUIRE(FeatherOS::Files::Read(&table, reader, text, sizeof(text), AlwaysInterrupted) == 0);
    FeatherOS::Files::CloseAll(&table);
}

TEST_CASE("status flags belong to the open file, which dup and inheritance share")
{
    PipeTable pipe;
    const uint64_t copy = FeatherOS::Files::Duplicate(&pipe.table, pipe.reader, 0, 0);
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(copy));
    FeatherOS::Files::Table child{};
    FeatherOS::Files::Inherit(&child, &pipe.table);

    // Access flags in the value are ignored; others are refused.
    REQUIRE(FeatherOS::Files::StatusFlags(&pipe.table, copy, true,
                                          FeatherOS::User::OpenRead |
                                              FeatherOS::User::OpenNonBlocking) ==
            (FeatherOS::User::OpenRead | FeatherOS::User::OpenNonBlocking));
    REQUIRE(FeatherOS::Files::StatusFlags(&pipe.table, copy, true, FeatherOS::User::OpenCreate) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::StatusFlags(&child, pipe.reader, false, 0) ==
            (FeatherOS::User::OpenRead | FeatherOS::User::OpenNonBlocking));
    char text[4];
    REQUIRE(FeatherOS::Files::Read(&pipe.table, pipe.reader, text, sizeof(text),
                                   AlwaysInterrupted) == FeatherOS::User::SyscallErrorWouldBlock);

    REQUIRE(FeatherOS::Files::StatusFlags(&child, pipe.reader, true, 0) ==
            FeatherOS::User::OpenRead);
    REQUIRE(FeatherOS::Files::Read(&pipe.table, copy, text, sizeof(text), AlwaysInterrupted) ==
            FeatherOS::User::SyscallErrorInterrupted);
    REQUIRE(FeatherOS::Files::StatusFlags(&pipe.table, 30, false, 0) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    FeatherOS::Files::CloseAll(&child);
}

TEST_CASE("the console's status flags cannot change")
{
    PipeTable pipe;
    const uint64_t console = FeatherOS::User::OpenRead | FeatherOS::User::OpenWrite;
    REQUIRE(FeatherOS::Files::StatusFlags(&pipe.table, FeatherOS::User::StandardInput, false, 0) ==
            console);
    REQUIRE(FeatherOS::Files::StatusFlags(&pipe.table, FeatherOS::User::StandardInput, true,
                                          FeatherOS::User::OpenNonBlocking) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::StatusFlags(&pipe.table, FeatherOS::User::StandardOutput, true,
                                          console) == console);
}
