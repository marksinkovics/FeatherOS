#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/terminal/pseudo_terminal.h"
#include "kernel/terminal/terminal.h"
#include "support/scheduler_stubs.h"

#include <string>
#include <vector>

namespace PseudoTerminals = FeatherOS::PseudoTerminals;
namespace Files = FeatherOS::Files;
namespace User = FeatherOS::User;
using FeatherOS::Terminals::Terminal;

namespace
{

constexpr uint64_t ReadWrite = User::OpenRead | User::OpenWrite;

// Host tests cannot block, so every wait that would block is cut short.
bool AlwaysInterrupted()
{
    return true;
}

// For the slave side, whose canonical reads give up at once while a signal is pending.
bool caller_interrupted = false;

bool CallerInterrupted()
{
    return caller_interrupted;
}

std::vector<std::string> SlaveNames()
{
    std::vector<std::string> names;
    User::DirectoryEntry entry{};
    for (uint64_t index = 0; Files::ReadDirectory("/dev/pts", index, &entry) == 0; ++index)
    {
        names.emplace_back(entry.name);
    }
    return names;
}

struct PairFixture
{
    PairFixture()
    {
        TestScheduler::Reset();
        caller_interrupted = false;
        FeatherOS::Terminals::SetProcessHooks({nullptr, CallerInterrupted});
        Files::OpenConsole(&table, nullptr);
        master = Files::Open(&table, "/dev/ptmx", ReadWrite);
        REQUIRE(master == 3);
        REQUIRE(Files::FindPseudoTerminal(&table, master, &pair) == 0);
        PseudoTerminals::SetLocked(pair, false);
        slave = Files::Open(&table, SlavePath().c_str(), ReadWrite);
        REQUIRE(slave == 4);
        REQUIRE(Files::FindTerminal(&table, slave, &terminal) == 0);
    }

    ~PairFixture()
    {
        Files::CloseAll(&table);
        FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr});
    }

    std::string SlavePath() const
    {
        return "/dev/pts/" + std::to_string(PseudoTerminals::Number(pair));
    }

    std::string ReadMaster(size_t length = 128)
    {
        std::string text(length, '\0');
        const uint64_t count =
            Files::Read(&table, master, text.data(), length, AlwaysInterrupted);
        if (count == User::SyscallErrorInterrupted)
        {
            return "";
        }
        REQUIRE_FALSE(User::IsSyscallError(count));
        text.resize(count);
        return text;
    }

    uint64_t WriteMaster(const std::string& text)
    {
        return Files::Write(&table, master, text.data(), text.size(), AlwaysInterrupted);
    }

    std::string ReadSlave()
    {
        char buffer[128];
        const uint64_t count =
            terminal->Read(FeatherOS::Terminals::AnyGroup, buffer, sizeof(buffer));
        REQUIRE_FALSE(User::IsSyscallError(count));
        return std::string(buffer, count);
    }

    Files::Table table{};
    uint64_t master = 0;
    uint64_t slave = 0;
    PseudoTerminals::Pair* pair = nullptr;
    Terminal* terminal = nullptr;
};

uint64_t interrupted_group = 0;

} // namespace

TEST_CASE("a pty master's writes are the slave's input, and the slave's output is the master's")
{
    PairFixture fixture;
    REQUIRE(Files::KindOf(&fixture.table, fixture.master) ==
            Files::Kind::PseudoTerminalMaster);
    REQUIRE(Files::KindOf(&fixture.table, fixture.slave) == Files::Kind::Terminal);
    REQUIRE(fixture.terminal == PseudoTerminals::TerminalOf(fixture.pair));
    User::FileStatus status{};
    REQUIRE(Files::StatDescriptor(&fixture.table, fixture.master, &status) == 0);
    REQUIRE(status.type == User::FileType::Device);
    REQUIRE(Files::StatDescriptor(&fixture.table, fixture.slave, &status) == 0);
    REQUIRE(status.type == User::FileType::Terminal);
    REQUIRE(SlaveNames() == std::vector<std::string>{"0"});

    REQUIRE(fixture.WriteMaster("helo\x7flo\r") == 8);
    REQUIRE(fixture.ReadSlave() == "hello\n");
    REQUIRE(fixture.ReadMaster() == "helo\b \blo\n");

    const char output[] = "out\tput\n";
    REQUIRE(fixture.terminal->Write(output, sizeof(output) - 1) == sizeof(output) - 1);
    REQUIRE(fixture.ReadMaster(4) == "out ");
    REQUIRE(fixture.ReadMaster() == "put\n");
    REQUIRE(fixture.ReadMaster().empty());
}

TEST_CASE("the slave opens once unlocked, and the pair goes away with its last descriptor")
{
    TestScheduler::Reset();
    Files::Table table{};
    const uint64_t master = Files::Open(&table, "/dev/ptmx", ReadWrite);
    REQUIRE(master == 0);
    REQUIRE(Files::Open(&table, "/dev/pts/0", ReadWrite) == User::SyscallErrorInputOutput);
    REQUIRE(Files::Open(&table, "/dev/pts/1", ReadWrite) == User::SyscallErrorNotFound);
    REQUIRE(Files::Open(&table, "/dev/pts/00", ReadWrite) == User::SyscallErrorNotFound);
    PseudoTerminals::Pair* pair = nullptr;
    REQUIRE(Files::FindPseudoTerminal(&table, master, &pair) == 0);
    Terminal* found = nullptr;
    REQUIRE(Files::FindTerminal(&table, master, &found) == User::SyscallErrorNotTerminal);
    PseudoTerminals::SetLocked(pair, false);
    const uint64_t slave = Files::Open(&table, "/dev/pts/0", ReadWrite);
    REQUIRE(slave == 1);
    REQUIRE(Files::FindPseudoTerminal(&table, slave, &pair) == User::SyscallErrorNotTerminal);

    REQUIRE(Files::Close(&table, master) == 0);
    REQUIRE(PseudoTerminals::InUse(0));
    REQUIRE(Files::Open(&table, "/dev/pts/0", ReadWrite) == User::SyscallErrorInputOutput);
    REQUIRE(Files::Close(&table, slave) == 0);
    REQUIRE_FALSE(PseudoTerminals::InUse(0));
    REQUIRE(SlaveNames().empty());
    REQUIRE(Files::Open(&table, "/dev/pts/0", ReadWrite) == User::SyscallErrorNotFound);
    Files::CloseAll(&table);
}

TEST_CASE("there are 16 pairs, and a closed one's number is used again")
{
    TestScheduler::Reset();
    Files::Table table{};
    for (uint64_t number = 0; number < User::MaximumPseudoTerminals; ++number)
    {
        REQUIRE(Files::Open(&table, "/dev/ptmx", ReadWrite) == number);
    }
    REQUIRE(SlaveNames().size() == User::MaximumPseudoTerminals);
    REQUIRE(SlaveNames().back() == "15");
    REQUIRE(Files::Open(&table, "/dev/ptmx", ReadWrite) == User::SyscallErrorNoDevice);
    REQUIRE(Files::Close(&table, 7) == 0);
    REQUIRE(Files::Open(&table, "/dev/ptmx", ReadWrite) == 7);
    PseudoTerminals::Pair* pair = nullptr;
    REQUIRE(Files::FindPseudoTerminal(&table, 7, &pair) == 0);
    REQUIRE(PseudoTerminals::Number(pair) == 7);
    Files::CloseAll(&table);
    REQUIRE(SlaveNames().empty());
}

TEST_CASE("a master write waits while the slave's input queue is full")
{
    PairFixture fixture;
    fixture.terminal->SetSettings({0, 1, 0, 0}, 9);
    const std::string input(300, 'x');
    REQUIRE(fixture.WriteMaster(input) == 256);
    REQUIRE(fixture.WriteMaster("y") == User::SyscallErrorInterrupted);
    REQUIRE(Files::PollEvents(&fixture.table, fixture.master) == 0);
    REQUIRE(Files::StatusFlags(&fixture.table, fixture.master, true, User::OpenNonBlocking) ==
            (ReadWrite | User::OpenNonBlocking));
    REQUIRE(fixture.WriteMaster("y") == User::SyscallErrorWouldBlock);

    REQUIRE(fixture.ReadSlave() == std::string(128, 'x'));
    REQUIRE(Files::PollEvents(&fixture.table, fixture.master) == User::PollWritable);
    REQUIRE(fixture.WriteMaster(std::string(200, 'z')) == 128);
}

TEST_CASE("slave output waits while the master does not read")
{
    PairFixture fixture;
    caller_interrupted = true;
    const std::string output(5000, 'o');
    REQUIRE(fixture.terminal->Write(output.data(), output.size()) == 4096);
    REQUIRE(Files::PollEvents(&fixture.table, fixture.master) ==
            (User::PollReadable | User::PollWritable));
    REQUIRE(fixture.terminal->Write("more", 4) == User::SyscallErrorInterrupted);
    REQUIRE(fixture.ReadMaster(4096) == std::string(4096, 'o'));
    REQUIRE(fixture.terminal->Write("more", 4) == 4);
    REQUIRE(fixture.ReadMaster() == "more");
}

TEST_CASE("Ctrl+C from the master interrupts the slave's foreground group")
{
    PairFixture fixture;
    FeatherOS::Terminals::SetProcessHooks({[](uint64_t group, int)
                                           {
                                               interrupted_group = group;
                                               return true;
                                           },
                                           CallerInterrupted});
    fixture.terminal->SetForegroundGroup(7);
    REQUIRE(fixture.WriteMaster("ab\x03" "cd") == 5);
    REQUIRE(interrupted_group == 7);
    REQUIRE(fixture.ReadMaster() == "^C\n");

    fixture.terminal->SetSettings({0, 5, 0, 0}, 9);
    REQUIRE(fixture.WriteMaster("\x03") == 1);
    char buffer[8];
    REQUIRE(fixture.terminal->Read(7, buffer, sizeof(buffer)) == 5);
    REQUIRE(std::string(buffer, 5) == "abcd\x03");
}

TEST_CASE("closing the master hangs up the slave's terminal and session")
{
    TestScheduler::Reset();
    static Terminal* hung_up_terminal = nullptr;
    static uint64_t hung_up_session = 0;
    hung_up_terminal = nullptr;
    hung_up_session = 0;
    FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr, nullptr,
                                           [](Terminal* terminal, uint64_t session)
                                           {
                                               hung_up_terminal = terminal;
                                               hung_up_session = session;
                                           }});
    Files::Table table{};
    const uint64_t master = Files::Open(&table, "/dev/ptmx", ReadWrite);
    PseudoTerminals::Pair* pair = nullptr;
    REQUIRE(Files::FindPseudoTerminal(&table, master, &pair) == 0);
    Terminal* terminal = PseudoTerminals::TerminalOf(pair);
    const uint64_t tty = Files::Open(&table, "/dev/tty", ReadWrite, terminal);
    REQUIRE(tty == 1);
    REQUIRE(terminal->Acquire(5, 5));
    REQUIRE(Files::Close(&table, master) == 0);
    REQUIRE((hung_up_terminal == terminal && hung_up_session == 5));
    REQUIRE(terminal->Session() == 0);
    REQUIRE(PseudoTerminals::InUse(0));
    char buffer[8];
    REQUIRE(terminal->Read(5, buffer, sizeof(buffer)) == 0);
    REQUIRE(terminal->Write("late", 4) == User::SyscallErrorInputOutput);
    REQUIRE(Files::Open(&table, "/dev/pts/0", ReadWrite) == User::SyscallErrorInputOutput);

    REQUIRE(Files::Close(&table, tty) == 0);
    REQUIRE_FALSE(PseudoTerminals::InUse(0));
    REQUIRE(SlaveNames().empty());
    const uint64_t reopened = Files::Open(&table, "/dev/ptmx", ReadWrite);
    REQUIRE(Files::FindPseudoTerminal(&table, reopened, &pair) == 0);
    REQUIRE(PseudoTerminals::Number(pair) == 0);
    REQUIRE_FALSE(terminal->HungUp());
    Files::CloseAll(&table);
    FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr});
}

TEST_CASE("a slave writer waiting for room fails once the master closes")
{
    PairFixture fixture;
    const std::string output(4096, 'o');
    REQUIRE(fixture.terminal->Write(output.data(), output.size()) == 4096);
    TestScheduler::on_wait = [&fixture]
    {
        TestScheduler::on_wait = nullptr;
        REQUIRE(Files::Close(&fixture.table, fixture.master) == 0);
    };
    REQUIRE(fixture.terminal->Write("more", 4) == User::SyscallErrorInputOutput);
}

TEST_CASE("master reads end once every slave descriptor is closed")
{
    PairFixture fixture;
    REQUIRE(fixture.terminal->Write("bye\n", 4) == 4);
    REQUIRE(Files::Close(&fixture.table, fixture.slave) == 0);
    REQUIRE(Files::PollEvents(&fixture.table, fixture.master) ==
            (User::PollReadable | User::PollWritable | User::PollHangUp));
    REQUIRE(fixture.ReadMaster() == "bye\n");
    char buffer[8];
    REQUIRE(Files::Read(&fixture.table, fixture.master, buffer, sizeof(buffer), nullptr) == 0);

    // A slave opened again holds the pair open again.
    fixture.slave = Files::Open(&fixture.table, fixture.SlavePath().c_str(), ReadWrite);
    REQUIRE(fixture.slave == 4);
    REQUIRE(Files::PollEvents(&fixture.table, fixture.master) == User::PollWritable);
    REQUIRE(Files::Read(&fixture.table, fixture.master, buffer, sizeof(buffer),
                        AlwaysInterrupted) == User::SyscallErrorInterrupted);
}

TEST_CASE("before its slave first opens, a master read waits")
{
    TestScheduler::Reset();
    Files::Table table{};
    const uint64_t master = Files::Open(&table, "/dev/ptmx", ReadWrite);
    char buffer[8];
    REQUIRE(Files::Read(&table, master, buffer, sizeof(buffer), AlwaysInterrupted) ==
            User::SyscallErrorInterrupted);
    REQUIRE(Files::PollEvents(&table, master) == User::PollWritable);
    Files::CloseAll(&table);
    REQUIRE_FALSE(PseudoTerminals::InUse(0));
}

TEST_CASE("settings an exiting owner left on a pty are restored")
{
    PairFixture fixture;
    fixture.terminal->SetSettings({0, 1, 0, 0}, 9);
    PseudoTerminals::ReleaseSettings(8);
    REQUIRE(fixture.terminal->Settings().flags == 0);
    PseudoTerminals::ReleaseSettings(9);
    REQUIRE(fixture.terminal->Settings().flags == User::TerminalDefaultFlags);
    REQUIRE(fixture.ReadMaster() == "\x1b[?1049l\x1b[0m");
}
