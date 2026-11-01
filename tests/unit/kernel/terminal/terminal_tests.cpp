#include <catch2/catch_test_macros.hpp>

#include "kernel/terminal/terminal.h"
#include "support/scheduler_stubs.h"
#include "support/terminal_device.h"

#include <string>

using FeatherOS::Terminals::Terminal;
namespace User = FeatherOS::User;

namespace
{

constexpr uint64_t Group = 5;

struct Fixture
{
    Fixture() : terminal(device.Device())
    {
        TestScheduler::Reset();
        FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr});
        terminal.SetForegroundGroup(Group);
    }

    void Type(const std::string& text)
    {
        terminal.Receive(text.data(), text.size());
    }

    std::string Read(size_t capacity = 64, uint64_t group = Group)
    {
        char buffer[64];
        const uint64_t count = terminal.Read(group, buffer, capacity);
        REQUIRE_FALSE(User::IsSyscallError(count));
        return std::string(buffer, count);
    }

    TestTerminalDevice device;
    Terminal terminal;
};

bool interrupted_caller = false;
uint64_t interrupted_group = 0;
uint64_t signalled_group = 0;
int signal_sent = 0;
int signals_sent = 0;

} // namespace

TEST_CASE("a canonical read returns one edited line and echoes it")
{
    Fixture fixture;
    fixture.Type("helo\x7flo\rnext\r");
    REQUIRE(fixture.Read() == "hello\n");
    REQUIRE(fixture.device.output == "helo<BS>lo\n");
    REQUIRE_FALSE(fixture.device.cursor_visible);
    REQUIRE(fixture.Read() == "next\n");
}

TEST_CASE("the part of a line a read does not take stays for the next read")
{
    Fixture fixture;
    fixture.Type("hello\r");
    REQUIRE(fixture.Read(3) == "hel");
    fixture.terminal.SetSettings({0, 0, 0, 0}, 9);
    REQUIRE(fixture.Read(2) == "lo");
    REQUIRE(fixture.Read() == "\n");
    REQUIRE(fixture.Read() == "");
}

TEST_CASE("line editing skips escape sequences and stops at the line's capacity")
{
    Fixture fixture;
    fixture.Type("a\x1b[Ab\x1bOPc\r");
    REQUIRE(fixture.Read() == "abc\n");

    char line[4];
    fixture.Type("abcdef\r");
    REQUIRE(fixture.terminal.ReadLine(Group, "> ", line, sizeof(line)) == 3);
    REQUIRE(std::string(line) == "abc");
    REQUIRE(fixture.device.output == "abc\n> abc\n");
}

TEST_CASE("echo off keeps typed text off the screen")
{
    Fixture fixture;
    fixture.terminal.SetSettings({User::TerminalCanonical | User::TerminalSignals, 1, 0, 0}, 3);
    fixture.Type("secret\r");
    REQUIRE(fixture.Read() == "secret\n");
    REQUIRE(fixture.device.output.empty());
}

TEST_CASE("Ctrl+C sends SIGINT to the foreground group while signals are on")
{
    Fixture fixture;
    FeatherOS::Terminals::SetProcessHooks({[](uint64_t group, int signal)
                                           {
                                               interrupted_group = group;
                                               signal_sent = signal;
                                               return true;
                                           },
                                           nullptr});
    fixture.terminal.Receive("\x03", 1);
    REQUIRE(interrupted_group == Group);
    REQUIRE(signal_sent == User::SignalInterrupt);
    REQUIRE(fixture.device.output == "^C\n");
    REQUIRE_FALSE(fixture.terminal.HasInput(Group));

    fixture.terminal.SetSettings({0, 0, 0, 0}, 3);
    fixture.terminal.Receive("\x03", 1);
    REQUIRE(fixture.Read() == "\x03");
}

TEST_CASE("Ctrl+C nobody takes discards the line being edited")
{
    Fixture fixture;
    FeatherOS::Terminals::SetProcessHooks({[](uint64_t, int) { return false; }, nullptr});
    fixture.Type("ab");
    fixture.terminal.Receive("\x03", 1);
    fixture.Type("cd\r");
    char line[16];
    REQUIRE(fixture.terminal.ReadLine(Group, "$ ", line, sizeof(line)) == 2);
    REQUIRE(std::string(line) == "cd");
    REQUIRE(fixture.device.output == "$ ab^C\n$ cd\n");
}

TEST_CASE("Ctrl+Z sends SIGTSTP to the foreground group while signals are on")
{
    Fixture fixture;
    FeatherOS::Terminals::SetProcessHooks({[](uint64_t group, int signal)
                                           {
                                               interrupted_group = group;
                                               signal_sent = signal;
                                               return true;
                                           },
                                           nullptr});
    fixture.terminal.Receive("\x1a", 1);
    REQUIRE(interrupted_group == Group);
    REQUIRE(signal_sent == User::SignalTerminalStop);
    REQUIRE(fixture.device.output == "^Z\n");
    REQUIRE_FALSE(fixture.terminal.HasInput(Group));

    signal_sent = 0;
    REQUIRE(fixture.terminal.ReceiveAvailable("a\x1a" "b", 3) == 3);
    REQUIRE(signal_sent == User::SignalTerminalStop);

    fixture.terminal.SetSettings({0, 0, 0, 0}, 3);
    signal_sent = 0;
    fixture.terminal.Receive("\x1a", 1);
    REQUIRE(signal_sent == 0);
    REQUIRE(fixture.Read() == "ab\x1a");
}

TEST_CASE("Ctrl+Z nobody takes is ignored by line editing")
{
    Fixture fixture;
    FeatherOS::Terminals::SetProcessHooks({[](uint64_t, int) { return false; }, nullptr});
    fixture.Type("ab");
    fixture.terminal.Receive("\x1a", 1);
    fixture.Type("c\r");
    REQUIRE(fixture.Read() == "abc\n");
    REQUIRE(fixture.device.output == "abc\n");
}

TEST_CASE("only the foreground group reads, but anyone takes a kept line")
{
    Fixture fixture;
    char buffer[8];
    REQUIRE(fixture.terminal.Read(Group + 1, buffer, sizeof(buffer)) ==
            User::SyscallErrorNotForeground);
    REQUIRE(fixture.terminal.ReadLine(Group + 1, "", buffer, sizeof(buffer)) ==
            User::SyscallErrorNotForeground);
    fixture.Type("hi\r");
    REQUIRE_FALSE(fixture.terminal.HasInput(Group + 1));
    REQUIRE(fixture.terminal.HasInput(Group));
    REQUIRE(fixture.Read(1) == "h");
    REQUIRE(fixture.Read(8, Group + 1) == "i\n");
}

TEST_CASE("a raw read waits for its minimum and maps Enter to a newline")
{
    Fixture fixture;
    fixture.terminal.SetSettings(
        {User::TerminalCarriageReturnToNewline | User::TerminalEcho, 3, 0, 0}, 3);
    fixture.Type("a");
    TestScheduler::on_wait = [&fixture]
    {
        fixture.Type("b\r");
        TestScheduler::on_wait = nullptr;
    };
    REQUIRE(fixture.Read() == "ab\n");
    REQUIRE(TestScheduler::wait_count == 1);
    REQUIRE(fixture.device.output == "ab\n");
    REQUIRE_FALSE(fixture.device.cursor_visible);
}

TEST_CASE("a raw read with a timeout returns what arrived once it runs out")
{
    Fixture fixture;
    fixture.terminal.SetSettings({0, 0, 5, 0}, 3);
    TestScheduler::on_wait = [] { TestScheduler::ticks = TestScheduler::last_wait_deadline; };
    REQUIRE(fixture.Read() == "");
    REQUIRE(TestScheduler::last_wait_deadline == 50);

    fixture.terminal.SetSettings({0, 4, 5, 0}, 3);
    fixture.Type("xy");
    REQUIRE(fixture.Read() == "xy");
}

TEST_CASE("a signal ends a read that has no bytes yet")
{
    Fixture fixture;
    interrupted_caller = false;
    FeatherOS::Terminals::SetProcessHooks({nullptr, [] { return interrupted_caller; }});
    TestScheduler::on_wait = [] { interrupted_caller = true; };
    char buffer[8];
    REQUIRE(fixture.terminal.Read(Group, buffer, sizeof(buffer)) ==
            User::SyscallErrorInterrupted);

    interrupted_caller = false;
    fixture.terminal.SetSettings({0, 1, 0, 0}, 3);
    REQUIRE(fixture.terminal.Read(Group, buffer, sizeof(buffer)) ==
            User::SyscallErrorInterrupted);
    FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr});
}

TEST_CASE("a read watching output returns once output paused")
{
    Fixture fixture;
    fixture.terminal.SetSettings({User::TerminalWakeOnOutput, 1, 0, 0}, 3);
    TestScheduler::on_wait = [&fixture]
    {
        if (fixture.device.output_count == 0)
        {
            fixture.terminal.Write("log\n", 4);
        }
        TestScheduler::ticks = TestScheduler::last_wait_deadline;
    };
    REQUIRE(fixture.Read() == "");
    REQUIRE(fixture.device.fresh_rows == 1);
}

TEST_CASE("the process that changed the mode keeps it until it releases it")
{
    Fixture fixture;
    const User::TerminalSettings raw = {0, 1, 0, 0};
    fixture.terminal.SetSettings(raw, 7);
    fixture.terminal.ReleaseSettings(8);
    REQUIRE(fixture.terminal.Settings().flags == 0);
    REQUIRE(fixture.device.resets == 0);
    fixture.terminal.ReleaseSettings(7);
    REQUIRE(fixture.terminal.Settings().flags == User::TerminalDefaultFlags);
    REQUIRE(fixture.device.resets == 1);

    fixture.terminal.SetSettings(raw, 7);
    fixture.terminal.SetSettings(FeatherOS::Terminals::DefaultSettings, 7);
    fixture.terminal.ReleaseSettings(7);
    REQUIRE(fixture.device.resets == 1);
}

TEST_CASE("output keeps text and escape sequences and drops other control bytes")
{
    Fixture fixture;
    const std::string text = "a\tb\x01\x80\x1b[7mc\r\b\n";
    fixture.terminal.Write(text.data(), text.size());
    REQUIRE(fixture.device.output == "a b?\x1b[7mc\r\b\n");

    const std::string long_text(300, 'x');
    fixture.device.output.clear();
    fixture.terminal.Write(long_text.data(), long_text.size());
    REQUIRE(fixture.device.output == long_text);
}

TEST_CASE("the input queue is bounded and keeps a key's bytes together")
{
    Fixture fixture;
    std::string expected;
    for (uint32_t index = 0; index < 254; ++index)
    {
        const char byte = static_cast<char>('a' + index % 26);
        fixture.terminal.Receive(&byte, 1);
        expected += byte;
    }
    // Three bytes do not fit in the two left, so none of them is queued.
    fixture.Type("\x1b[A");
    fixture.Type("xy");
    expected += "xy";
    fixture.Type("z");
    REQUIRE(TestScheduler::wake_count == 255);
    fixture.terminal.SetSettings({0, 0, 0, 0}, 3);
    std::string read;
    for (std::string chunk = fixture.Read(); !chunk.empty(); chunk = fixture.Read())
    {
        read += chunk;
    }
    REQUIRE(read == expected);
}

TEST_CASE("the size and foreground group are the terminal's own")
{
    Fixture fixture;
    uint32_t rows = 0;
    uint32_t columns = 0;
    fixture.terminal.Size(&rows, &columns);
    REQUIRE((rows == 24 && columns == 80));
    fixture.terminal.SetSize(47, 128);
    fixture.terminal.Size(&rows, &columns);
    REQUIRE((rows == 47 && columns == 128));

    Terminal other(fixture.device.Device());
    REQUIRE(other.ForegroundGroup() == 0);
    REQUIRE(fixture.terminal.ForegroundGroup() == Group);
}

TEST_CASE("a size change sends SIGWINCH to the foreground group")
{
    Fixture fixture;
    signals_sent = 0;
    FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr,
                                           [](uint64_t group, int signal)
                                           {
                                               signalled_group = group;
                                               signal_sent = signal;
                                               ++signals_sent;
                                           }});
    fixture.terminal.SetSize(30, 100);
    REQUIRE((signals_sent == 1 && signalled_group == Group &&
             signal_sent == User::SignalWindowChange));
    fixture.terminal.SetSize(30, 100);
    REQUIRE(signals_sent == 1);
    fixture.terminal.SetForegroundGroup(0);
    fixture.terminal.SetSize(31, 100);
    REQUIRE(signals_sent == 1);
    uint32_t rows = 0;
    uint32_t columns = 0;
    fixture.terminal.Size(&rows, &columns);
    REQUIRE((rows == 31 && columns == 100));
    FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr});
}

TEST_CASE("a session takes a free terminal and loses it when its leader exits")
{
    TestTerminalDevice device;
    Terminal terminal(device.Device());
    REQUIRE(terminal.Session() == 0);
    REQUIRE_FALSE(terminal.Acquire(0, 3));
    REQUIRE(terminal.Acquire(3, 3));
    REQUIRE(terminal.Session() == 3);
    REQUIRE(terminal.ForegroundGroup() == 3);
    REQUIRE_FALSE(terminal.Acquire(9, 9));

    terminal.Release(9);
    REQUIRE(terminal.Session() == 3);
    terminal.Release(3);
    REQUIRE(terminal.Session() == 0);
    REQUIRE(terminal.ForegroundGroup() == 0);
    REQUIRE(terminal.Acquire(9, 9));
}

TEST_CASE("the foreground group applies only to the session the terminal controls")
{
    Fixture fixture;
    Terminal& terminal = fixture.terminal;
    REQUIRE(terminal.Acquire(3, 3));
    terminal.SetForegroundGroup(Group);
    const FeatherOS::Terminals::Caller foreground = {3, Group};
    const FeatherOS::Terminals::Caller background = {3, 7};
    const FeatherOS::Terminals::Caller outsider = {9, 9};
    REQUIRE(terminal.AccessGroup(foreground) == Group);
    REQUIRE(terminal.AccessGroup(background) == 7);
    REQUIRE(terminal.AccessGroup(outsider) == FeatherOS::Terminals::AnyGroup);
    REQUIRE(terminal.InForeground(FeatherOS::Terminals::AnyGroup));
    REQUIRE_FALSE(terminal.InForeground(7));

    char buffer[8];
    REQUIRE(terminal.Read(terminal.AccessGroup(background), buffer, sizeof(buffer)) ==
            User::SyscallErrorNotForeground);
    terminal.Receive("x\r", 2);
    REQUIRE_FALSE(terminal.HasInput(7));
    REQUIRE(terminal.HasInput(FeatherOS::Terminals::AnyGroup));
    REQUIRE(terminal.Read(terminal.AccessGroup(outsider), buffer, sizeof(buffer)) == 2);

    REQUIRE(terminal.ForegroundGroupFor(background) == Group);
    REQUIRE(terminal.ForegroundGroupFor(outsider) == User::SyscallErrorNotTerminal);
}

TEST_CASE("tcsetpgrp rules: the session's terminal, to a group within it")
{
    TestTerminalDevice device;
    Terminal terminal(device.Device());
    const FeatherOS::Terminals::Caller leader = {3, 3};
    const FeatherOS::Terminals::Caller member = {3, Group};
    REQUIRE(terminal.ChangeForegroundGroup(leader, Group, true) == User::SyscallErrorNotTerminal);
    REQUIRE(terminal.Acquire(3, 3));

    REQUIRE(terminal.ChangeForegroundGroup(leader, 1, false) == User::SyscallErrorNotPermitted);
    REQUIRE(terminal.ChangeForegroundGroup(leader, Group, true) == 0);
    REQUIRE(terminal.ForegroundGroup() == Group);
    REQUIRE(terminal.ChangeForegroundGroup(member, 3, true) == 0);
    REQUIRE(terminal.ForegroundGroup() == 3);
    REQUIRE(terminal.ChangeForegroundGroup({9, 9}, 9, true) ==
            User::SyscallErrorNotTerminal);
    REQUIRE(terminal.ForegroundGroup() == 3);
}

TEST_CASE("a background group's terminal access: SIGTTOU declined goes ahead, SIGTTIN fails")
{
    using FeatherOS::Terminals::BackgroundAccess;
    using FeatherOS::Terminals::DecideBackgroundAccess;
    REQUIRE(DecideBackgroundAccess(User::SignalTerminalInput, false, false) ==
            BackgroundAccess::Signal);
    REQUIRE(DecideBackgroundAccess(User::SignalTerminalOutput, false, false) ==
            BackgroundAccess::Signal);
    REQUIRE(DecideBackgroundAccess(User::SignalTerminalInput, true, false) ==
            BackgroundAccess::Fail);
    REQUIRE(DecideBackgroundAccess(User::SignalTerminalOutput, true, false) ==
            BackgroundAccess::Allowed);
    REQUIRE(DecideBackgroundAccess(User::SignalTerminalInput, false, true) ==
            BackgroundAccess::Fail);
    REQUIRE(DecideBackgroundAccess(User::SignalTerminalOutput, false, true) ==
            BackgroundAccess::Fail);
    REQUIRE(DecideBackgroundAccess(User::SignalTerminalOutput, true, true) ==
            BackgroundAccess::Allowed);
}

TEST_CASE("a hang-up ends reads, fails writes and detaches the session")
{
    Fixture fixture;
    static FeatherOS::Terminals::Terminal* hung_up_terminal = nullptr;
    static uint64_t hung_up_session = 0;
    hung_up_terminal = nullptr;
    hung_up_session = 0;
    FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr, nullptr,
                                           [](Terminal* terminal, uint64_t session)
                                           {
                                               hung_up_terminal = terminal;
                                               hung_up_session = session;
                                           }});
    REQUIRE(fixture.terminal.Acquire(4, Group));
    fixture.terminal.SetSettings({0, 1, 0, 0}, 9);
    fixture.Type("typed\r");
    // A reader waiting for more input wakes to the hang-up.
    TestScheduler::on_wait = [&fixture] { fixture.terminal.HangUp(); };
    char buffer[8];
    REQUIRE(fixture.terminal.Read(Group, buffer, sizeof(buffer)) == 6);
    REQUIRE(fixture.terminal.Read(Group, buffer, sizeof(buffer)) == 0);
    REQUIRE((hung_up_terminal == &fixture.terminal && hung_up_session == 4));
    REQUIRE(fixture.terminal.HungUp());
    REQUIRE((fixture.terminal.Session() == 0 && fixture.terminal.ForegroundGroup() == 0));
    REQUIRE_FALSE(fixture.terminal.Acquire(6, 6));

    fixture.device.output.clear();
    REQUIRE(fixture.terminal.Write("late", 4) == User::SyscallErrorInputOutput);
    REQUIRE(fixture.terminal.ReadLine(Group, "> ", buffer, sizeof(buffer)) ==
            User::SyscallErrorInputOutput);
    fixture.terminal.ReleaseSettings(9);
    REQUIRE(fixture.terminal.Settings().flags == User::TerminalDefaultFlags);
    REQUIRE((fixture.device.output.empty() && fixture.device.resets == 0));

    hung_up_session = 0;
    fixture.terminal.HangUp();
    REQUIRE(hung_up_session == 0);
    fixture.terminal.Reset(fixture.device.Device());
    REQUIRE_FALSE(fixture.terminal.HungUp());
    FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr});
}

TEST_CASE("a canonical read waiting for its line ends at a hang-up")
{
    Fixture fixture;
    fixture.Type("part");
    TestScheduler::on_wait = [&fixture] { fixture.terminal.HangUp(); };
    char buffer[8];
    REQUIRE(fixture.terminal.Read(Group, buffer, sizeof(buffer)) == 0);
}
