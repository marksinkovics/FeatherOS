#include <catch2/catch_test_macros.hpp>

#include "user/init_config.h"

#include <memory>
#include <string>
#include <vector>

namespace
{

using FeatherOS::User::Init::LineKind;
using FeatherOS::User::Init::Policy;
using FeatherOS::User::Init::Service;

LineKind Parse(const std::string& line, Service* service)
{
    return FeatherOS::User::Init::ParseLine(line.data(), line.size(), service);
}

} // namespace

TEST_CASE("init reads a policy and a command from each service line")
{
    Service service{};
    REQUIRE(Parse("console /dev/tty2 shell", &service) == LineKind::Service);
    REQUIRE(service.policy == Policy::Console);
    REQUIRE(std::string(service.terminal) == "/dev/tty2");
    REQUIRE(std::string(service.command) == "shell");
    REQUIRE(Parse("  once\techo hello   world \r", &service) == LineKind::Service);
    REQUIRE(service.policy == Policy::Once);
    REQUIRE(std::string(service.terminal).empty());
    REQUIRE(std::string(service.command) == "echo hello   world");
    REQUIRE(Parse("respawn sleep 100", &service) == LineKind::Service);
    REQUIRE(service.policy == Policy::Respawn);
    REQUIRE_FALSE(service.running);
}

TEST_CASE("init skips blank and comment lines and rejects malformed ones")
{
    Service service{};
    REQUIRE(Parse("", &service) == LineKind::Empty);
    REQUIRE(Parse("   \r", &service) == LineKind::Empty);
    REQUIRE(Parse("# console shell", &service) == LineKind::Empty);
    REQUIRE(Parse("forever shell", &service) == LineKind::Invalid);
    REQUIRE(Parse("once", &service) == LineKind::Invalid);
    REQUIRE(Parse("onceshell", &service) == LineKind::Invalid);
    REQUIRE(Parse("once " + std::string(FeatherOS::User::MaximumCommandLineLength, 'x'),
                  &service) == LineKind::Invalid);
}

TEST_CASE("init requires a console service to name its terminal")
{
    Service service{};
    REQUIRE(Parse("console\t/dev/tty1   shell -c ls", &service) == LineKind::Service);
    REQUIRE(std::string(service.terminal) == "/dev/tty1");
    REQUIRE(std::string(service.command) == "shell -c ls");
    REQUIRE(Parse("console shell", &service) == LineKind::Invalid);
    REQUIRE(Parse("console /dev/tty1", &service) == LineKind::Invalid);
    REQUIRE(Parse("console /dev/tty1   ", &service) == LineKind::Invalid);
    const std::string long_path =
        "/" + std::string(FeatherOS::User::Init::MaximumTerminalPathLength - 1, 't');
    REQUIRE(Parse("console " + long_path + " shell", &service) == LineKind::Invalid);
    REQUIRE(Parse("console " + long_path.substr(0, long_path.size() - 1) + " shell", &service) ==
            LineKind::Service);
    REQUIRE(Parse("console tty1 shell", &service) == LineKind::Invalid);
}

TEST_CASE("init disables a service that restarts too often")
{
    Service service{};
    REQUIRE(Parse("respawn crashy", &service) == LineKind::Service);
    for (uint32_t restart = 0; restart < FeatherOS::User::Init::MaximumRestarts; ++restart)
    {
        REQUIRE(FeatherOS::User::Init::AllowRestart(&service, 1000 + restart * 100));
    }
    REQUIRE_FALSE(FeatherOS::User::Init::AllowRestart(&service, 1600));

    // Restarts spread over more than the window keep being allowed.
    Service steady{};
    REQUIRE(Parse("respawn steady", &steady) == LineKind::Service);
    for (uint64_t restart = 0; restart < 20; ++restart)
    {
        REQUIRE(FeatherOS::User::Init::AllowRestart(&steady, restart * 3000));
    }
}

TEST_CASE("init reads env lines as NAME=value variables")
{
    Service service{};
    REQUIRE(Parse("env HOME=/home", &service) == LineKind::Variable);
    REQUIRE(std::string(service.command) == "HOME=/home");
    REQUIRE(Parse("  env\tGREETING=a=b \r", &service) == LineKind::Variable);
    REQUIRE(std::string(service.command) == "GREETING=a=b");
    REQUIRE(Parse("env EMPTY=", &service) == LineKind::Variable);
    REQUIRE(Parse("env", &service) == LineKind::Invalid);
    REQUIRE(Parse("env HOME", &service) == LineKind::Invalid);
    REQUIRE(Parse("env =value", &service) == LineKind::Invalid);
    REQUIRE(Parse("env A=1 B=2", &service) == LineKind::Invalid);
    REQUIRE(Parse("env NAME =x", &service) == LineKind::Invalid);
}

TEST_CASE("init appends variables to an environment block until it is full")
{
    char block[16];
    block[0] = '\0';
    uint64_t used = 0;
    uint64_t count = 0;
    REQUIRE(FeatherOS::User::Init::AppendVariable(block, sizeof(block), &used, &count, "A=1"));
    REQUIRE(FeatherOS::User::Init::AppendVariable(block, sizeof(block), &used, &count, "BB=22"));
    REQUIRE(count == 2);
    REQUIRE(used == 10);
    REQUIRE(std::string(block, used + 1) == std::string("A=1\0BB=22\0\0", 11));
    // "CCC=3" would need 6 more bytes plus the final terminator.
    REQUIRE_FALSE(
        FeatherOS::User::Init::AppendVariable(block, sizeof(block), &used, &count, "CCC=3"));
    REQUIRE(FeatherOS::User::Init::AppendVariable(block, sizeof(block), &used, &count, "D=4"));
    REQUIRE(used == 14);
    REQUIRE(block[14] == '\0');
}

namespace
{

using FeatherOS::User::Init::AddBuiltIn;
using FeatherOS::User::Init::AddServiceFile;
using FeatherOS::User::Init::Configuration;
using FeatherOS::User::Init::DisableBuiltIns;
using FeatherOS::User::Init::EnsureConsole;
using FeatherOS::User::Init::FileResult;
using FeatherOS::User::Init::Reporter;

struct Messages
{
    std::vector<uint64_t> ignored;
    std::vector<std::string> disabled;
};

Reporter ReporterFor(Messages* messages)
{
    return Reporter{
        [](void* context, uint64_t line) {
            static_cast<Messages*>(context)->ignored.push_back(line);
        },
        [](void* context, const Service& service) {
            static_cast<Messages*>(context)->disabled.emplace_back(service.command);
        },
        messages};
}

constexpr char BuiltIns[] = "env PATH=/bin:/system\n"
                            "once echo starting\n"
                            "respawn dhcp\n"
                            "console /dev/tty1 shell\n"
                            "console /dev/tty2 shell\n";

std::vector<std::string> Commands(const Configuration& config)
{
    std::vector<std::string> commands;
    for (uint32_t index = 0; index < config.service_count; ++index)
    {
        commands.emplace_back(config.services[index].command);
    }
    return commands;
}

std::vector<std::string> Variables(const Configuration& config)
{
    std::vector<std::string> variables;
    for (uint64_t offset = 0; config.environment[offset] != '\0';
         offset += variables.back().size() + 1)
    {
        variables.emplace_back(config.environment + offset);
    }
    return variables;
}

std::unique_ptr<Configuration> Load(const std::string& built_in, Messages* messages)
{
    auto config = std::make_unique<Configuration>();
    AddBuiltIn(config.get(), built_in.data(), built_in.size(), ReporterFor(messages));
    return config;
}

FileResult Add(Configuration* config, const std::string& text, Messages* messages)
{
    return AddServiceFile(config, text.data(), text.size(), ReporterFor(messages));
}

using Strings = std::vector<std::string>;

} // namespace

TEST_CASE("init starts only the built-in services when nothing is added")
{
    Messages messages;
    const auto config = Load(BuiltIns, &messages);
    REQUIRE(Commands(*config) == Strings{"echo starting", "dhcp", "shell", "shell"});
    REQUIRE(Variables(*config) == Strings{"PATH=/bin:/system"});
    REQUIRE(config->services[0].built_in);
    REQUIRE(messages.ignored.empty());
}

TEST_CASE("init skips a built-in line that does not parse, repeats a terminal or does not fit")
{
    Messages messages;
    std::string text = "once echo a\nbogus\nconsole /dev/tty1 shell\nconsole /dev/tty1 shell\n";
    for (uint32_t index = 0; index < FeatherOS::User::Init::MaximumServices; ++index)
    {
        text += "once echo b\n";
    }
    const auto config = Load(text, &messages);
    REQUIRE(config->service_count == FeatherOS::User::Init::MaximumServices);
    REQUIRE(messages.ignored.size() == 2 + 2);
    REQUIRE(messages.ignored[0] == 2);
    REQUIRE(messages.ignored[1] == 4);
}

TEST_CASE("init adds the services of an /etc/init.d file after the built-in ones")
{
    Messages messages;
    const auto config = Load(BuiltIns, &messages);
    REQUIRE(Add(config.get(), "# mine\nonce uname\nrespawn /bin/httpd 8080\n", &messages) ==
            FileResult::Merged);
    REQUIRE(Commands(*config) ==
            Strings{"echo starting", "dhcp", "shell", "shell", "uname", "/bin/httpd 8080"});
    REQUIRE_FALSE(config->services[4].built_in);
    REQUIRE(Add(config.get(), "console /dev/tty3 shell\n", &messages) == FileResult::Merged);
    REQUIRE(config->service_count == 7);
    REQUIRE(messages.ignored.empty());
}

TEST_CASE("init removes the built-in services named in /etc/init.disabled")
{
    Messages messages;
    const auto config = Load(BuiltIns, &messages);
    const std::string list = "# no network here\n  dhcp  \r\n\nnothing\n";
    REQUIRE(DisableBuiltIns(config.get(), list.data(), list.size(), ReporterFor(&messages)) == 1);
    REQUIRE(Commands(*config) == Strings{"echo starting", "shell", "shell"});
    REQUIRE(messages.disabled == Strings{"dhcp"});

    const std::string terminal = "/dev/tty2\n";
    REQUIRE(DisableBuiltIns(config.get(), terminal.data(), terminal.size(),
                            ReporterFor(&messages)) == 1);
    REQUIRE(Commands(*config) == Strings{"echo starting", "shell"});
    REQUIRE(config->services[1].terminal == std::string("/dev/tty1"));

    const std::string program = "shell";
    REQUIRE(DisableBuiltIns(config.get(), program.data(), program.size(),
                            ReporterFor(&messages)) == 1);
    REQUIRE(Commands(*config) == Strings{"echo starting"});
}

TEST_CASE("init does not disable an /etc/init.d service by name")
{
    Messages messages;
    const auto config = Load(BuiltIns, &messages);
    REQUIRE(Add(config.get(), "respawn dhcp -r\n", &messages) == FileResult::Merged);
    const std::string list = "dhcp\n";
    DisableBuiltIns(config.get(), list.data(), list.size(), ReporterFor(&messages));
    REQUIRE(Commands(*config) == Strings{"echo starting", "shell", "shell", "dhcp -r"});
}

TEST_CASE("an /etc/init.d service replaces the built-in one that runs the same program")
{
    Messages messages;
    const auto config = Load(BuiltIns, &messages);
    REQUIRE(Add(config.get(), "respawn /sbin/dhcp -v\nconsole /dev/tty2 vi\n", &messages) ==
            FileResult::Merged);
    REQUIRE(Commands(*config) ==
            Strings{"echo starting", "shell", "/sbin/dhcp -v", "vi"});
    REQUIRE(config->services[3].terminal == std::string("/dev/tty2"));

    // Another /etc/init.d file's service of the same program stays.
    REQUIRE(Add(config.get(), "respawn dhcp\n", &messages) == FileResult::Merged);
    REQUIRE(Commands(*config) ==
            Strings{"echo starting", "shell", "/sbin/dhcp -v", "vi", "dhcp"});
}

TEST_CASE("a console on a terminal another /etc/init.d file holds skips the file")
{
    Messages messages;
    const auto config = Load(BuiltIns, &messages);
    REQUIRE(Add(config.get(), "console /dev/tty3 shell\n", &messages) == FileResult::Merged);
    REQUIRE(Add(config.get(), "once uname\nconsole /dev/tty3 vi\n", &messages) ==
            FileResult::TerminalTaken);
    REQUIRE(Commands(*config) == Strings{"echo starting", "dhcp", "shell", "shell", "shell"});
}

TEST_CASE("env lines of an /etc/init.d file override earlier ones, the last of all winning")
{
    Messages messages;
    const auto config = Load("env PATH=/bin\nenv HOME=/\nenv KEEP=1\n", &messages);
    REQUIRE(Add(config.get(), "env PATH=/sbin\nenv LANG=C\nenv PATH=/usr\n", &messages) ==
            FileResult::Merged);
    REQUIRE(Variables(*config) == Strings{"HOME=/", "KEEP=1", "LANG=C", "PATH=/usr"});
    REQUIRE(Add(config.get(), "env HOME=/home\n", &messages) == FileResult::Merged);
    REQUIRE(Variables(*config) == Strings{"KEEP=1", "LANG=C", "PATH=/usr", "HOME=/home"});
    REQUIRE(config->environment_count == 4);
}

TEST_CASE("a built-in env line repeated later in the same file wins")
{
    Messages messages;
    const auto config = Load("env A=1\nenv B=2\nenv A=3\n", &messages);
    REQUIRE(Variables(*config) == Strings{"B=2", "A=3"});
}

TEST_CASE("a file that does not parse is skipped whole and the rest is kept")
{
    Messages messages;
    const auto config = Load(BuiltIns, &messages);
    REQUIRE(Add(config.get(), "once uname\nbogus line\nenv X=1\nrespawn dhcp\n", &messages) ==
            FileResult::Invalid);
    REQUIRE(messages.ignored == std::vector<uint64_t>{2});
    REQUIRE(Commands(*config) == Strings{"echo starting", "dhcp", "shell", "shell"});
    REQUIRE(Variables(*config) == Strings{"PATH=/bin:/system"});
    REQUIRE(config->services[1].built_in);
    REQUIRE(Add(config.get(), "once uname\n", &messages) == FileResult::Merged);
    REQUIRE(config->service_count == 5);

    REQUIRE(Add(config.get(), std::string("\xff\x00 not a service\nrespawn\n", 25),
                &messages) == FileResult::Invalid);
    REQUIRE(Add(config.get(), "console /dev/tty5 shell\nconsole /dev/tty5 vi\n", &messages) ==
            FileResult::Invalid);
    REQUIRE(config->service_count == 5);
}

TEST_CASE("an /etc/init.d file that does not fit is skipped and changes nothing")
{
    Messages messages;
    const auto config = Load(BuiltIns, &messages);
    std::string many;
    for (uint32_t index = 0; index < FeatherOS::User::Init::MaximumServices - 3; ++index)
    {
        many += "once uname\n";
    }
    REQUIRE(Add(config.get(), many, &messages) == FileResult::NoRoom);
    REQUIRE(config->service_count == 4);

    // A replaced built-in frees its slot.
    many.erase(0, 11);
    REQUIRE(Add(config.get(), many + "respawn dhcp\n", &messages) == FileResult::Merged);
    REQUIRE(config->service_count == FeatherOS::User::Init::MaximumServices);

    const auto crowded = Load("env BIG=" + std::string(700, 'b') + "\nonce echo a\n", &messages);
    const auto before = Variables(*crowded);
    REQUIRE(Add(crowded.get(), "once echo z\nenv SMALL=" + std::string(400, 's') + "\n",
                &messages) == FileResult::NoRoom);
    REQUIRE(Variables(*crowded) == before);
    REQUIRE(crowded->service_count == 1);
}

TEST_CASE("init adds a shell on tty1 when no console service is left")
{
    Messages messages;
    const auto config = Load("once echo a\nrespawn dhcp\nconsole /dev/tty2 shell\n", &messages);
    const std::string list = "shell\n";
    DisableBuiltIns(config.get(), list.data(), list.size(), ReporterFor(&messages));
    REQUIRE(EnsureConsole(config.get(), ReporterFor(&messages)));
    REQUIRE(Commands(*config) == Strings{"echo a", "dhcp", "shell"});
    REQUIRE(config->services[2].terminal == std::string("/dev/tty1"));
    REQUIRE_FALSE(EnsureConsole(config.get(), ReporterFor(&messages)));
    REQUIRE(config->service_count == 3);
}

TEST_CASE("init adds nothing when a console exists, even an /etc/init.d one on another terminal")
{
    Messages messages;
    const auto config = Load("once echo a\n", &messages);
    REQUIRE(Add(config.get(), "console /dev/tty3 vi\n", &messages) == FileResult::Merged);
    REQUIRE_FALSE(EnsureConsole(config.get(), ReporterFor(&messages)));
    REQUIRE(Commands(*config) == Strings{"echo a", "vi"});
}

TEST_CASE("ProgramName takes the first word without its directory")
{
    Service service{};
    REQUIRE(Parse("once /system/echo hello /x", &service) == LineKind::Service);
    char name[32];
    FeatherOS::User::Init::ProgramName(service, name, sizeof(name));
    REQUIRE(std::string(name) == "echo");
    FeatherOS::User::Init::ProgramName(service, name, 3);
    REQUIRE(std::string(name) == "ec");
}

TEST_CASE("SetVariable replaces a value in place of the old one or leaves the block alone")
{
    char block[24];
    block[0] = '\0';
    uint64_t used = 0;
    uint64_t count = 0;
    using FeatherOS::User::Init::SetVariable;
    REQUIRE(SetVariable(block, sizeof(block), &used, &count, "A=1"));
    REQUIRE(SetVariable(block, sizeof(block), &used, &count, "AB=2"));
    REQUIRE(SetVariable(block, sizeof(block), &used, &count, "A=long value"));
    REQUIRE(count == 2);
    REQUIRE(std::string(block, used + 1) == std::string("AB=2\0A=long value\0\0", 19));
    REQUIRE_FALSE(SetVariable(block, sizeof(block), &used, &count, "AB=0123456789"));
    REQUIRE(std::string(block, used + 1) == std::string("AB=2\0A=long value\0\0", 19));
    REQUIRE(SetVariable(block, sizeof(block), &used, &count, "AB=x"));
    REQUIRE(count == 2);
}
