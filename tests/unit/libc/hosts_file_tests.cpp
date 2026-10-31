#include <catch2/catch_test_macros.hpp>

#include "libc/hosts_file.h"

#include <string>
#include <vector>

namespace Hosts = FeatherOS::LibC::Hosts;

namespace
{

// The names point into text, which must outlive the lines.
std::vector<Hosts::Line> Lines(const std::string& text)
{
    std::vector<Hosts::Line> lines;
    const char* cursor = text.data();
    Hosts::Line line{};
    while (Hosts::NextLine(&cursor, text.data() + text.size(), &line))
    {
        lines.push_back(line);
    }
    return lines;
}

std::string NameAt(const Hosts::Line& line, size_t index)
{
    return {line.names[index].text, line.names[index].length};
}

} // namespace

TEST_CASE("hosts lines give an address and its names")
{
    const std::string text =
        "127.0.0.1\tlocalhost\n::1  localhost ip6-localhost  ip6-loopback\r\n";
    const std::vector<Hosts::Line> lines = Lines(text);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].address_length == 4);
    REQUIRE(lines[0].address[0] == 127);
    REQUIRE(lines[0].address[3] == 1);
    REQUIRE(lines[0].name_count == 1);
    REQUIRE(NameAt(lines[0], 0) == "localhost");
    REQUIRE(lines[1].address_length == 16);
    REQUIRE(lines[1].address[15] == 1);
    REQUIRE(lines[1].name_count == 3);
    REQUIRE(NameAt(lines[1], 1) == "ip6-localhost");
    REQUIRE(NameAt(lines[1], 2) == "ip6-loopback");
}

TEST_CASE("hosts comments, blank and malformed lines are skipped")
{
    const std::string text = "# a comment\n"
                             "\n"
                             "   \t\n"
                             "10.0.0.1 server # trailing comment words\n"
                             "10.0.0.2\n"
                             "10.0.0.2 # only a comment\n"
                             "256.0.0.1 too-big\n"
                             "not-an-address name\n"
                             "#10.0.0.3 commented\n"
                             "fe80::1 last";
    const std::vector<Hosts::Line> lines = Lines(text);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].name_count == 1);
    REQUIRE(NameAt(lines[0], 0) == "server");
    REQUIRE(lines[1].address_length == 16);
    REQUIRE(lines[1].address[0] == 0xFE);
    REQUIRE(NameAt(lines[1], 0) == "last");
}

TEST_CASE("hosts lines keep at most MaximumNames names")
{
    std::string text = "10.0.0.1";
    for (size_t index = 0; index < Hosts::MaximumNames + 3; ++index)
    {
        text += " name" + std::to_string(index);
    }
    text += "\n10.0.0.2 next\n";
    const std::vector<Hosts::Line> lines = Lines(text);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].name_count == Hosts::MaximumNames);
    REQUIRE(NameAt(lines[1], 0) == "next");
}

TEST_CASE("hosts names match without case and a final dot")
{
    const std::string text = "10.0.0.1 Server.Example server\n";
    const std::vector<Hosts::Line> lines = Lines(text);
    REQUIRE(lines.size() == 1);
    REQUIRE(Hosts::Names(lines[0], "server.example"));
    REQUIRE(Hosts::Names(lines[0], "SERVER.example."));
    REQUIRE(Hosts::Names(lines[0], "server"));
    REQUIRE_FALSE(Hosts::Names(lines[0], "server.ex"));
    REQUIRE_FALSE(Hosts::Names(lines[0], "erver"));
    REQUIRE_FALSE(Hosts::Names(lines[0], ""));
    REQUIRE_FALSE(Hosts::Names(lines[0], "."));
}
