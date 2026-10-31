#include <catch2/catch_test_macros.hpp>

#include "libc/resolv_conf.h"

#include <string>

namespace ResolvConf = FeatherOS::LibC::ResolvConf;

namespace
{

ResolvConf::Config Parse(const std::string& text)
{
    ResolvConf::Config config{};
    config.timeout_seconds = 2;
    config.attempts = 2;
    ResolvConf::Parse(text.data(), text.size(), &config);
    return config;
}

} // namespace

TEST_CASE("resolv.conf name servers take either version and an optional port")
{
    const ResolvConf::Config config =
        Parse("nameserver 10.0.2.3\n\tnameserver   fec0::2#5353  # trailing words\r\n");
    REQUIRE(config.name_server_count == 2);
    REQUIRE(config.name_servers[0].address_length == 4);
    REQUIRE(config.name_servers[0].address[0] == 10);
    REQUIRE(config.name_servers[0].address[3] == 3);
    REQUIRE(config.name_servers[0].port == 53);
    REQUIRE(config.name_servers[1].address_length == 16);
    REQUIRE(config.name_servers[1].address[0] == 0xFE);
    REQUIRE(config.name_servers[1].address[15] == 2);
    REQUIRE(config.name_servers[1].port == 5353);
    REQUIRE(config.timeout_seconds == 2);
    REQUIRE(config.attempts == 2);
}

TEST_CASE("resolv.conf skips comments, other keywords and malformed name servers")
{
    const ResolvConf::Config config = Parse("# nameserver 10.0.0.1\n"
                                            "; nameserver 10.0.0.2\n"
                                            "search featheros.test\n"
                                            "domain featheros.test\n"
                                            "nameserver\n"
                                            "nameserver name.featheros.test\n"
                                            "nameserver 10.0.0.3#\n"
                                            "nameserver 10.0.0.4#0\n"
                                            "nameserver 10.0.0.5#65536\n"
                                            "nameserver 10.0.0.6#53x\n"
                                            "nameservers 10.0.0.7\n"
                                            "nameserver 10.0.0.8#65535");
    REQUIRE(config.name_server_count == 1);
    REQUIRE(config.name_servers[0].address[3] == 8);
    REQUIRE(config.name_servers[0].port == 65535);
}

TEST_CASE("resolv.conf keeps at most MaximumNameServers name servers")
{
    const ResolvConf::Config config = Parse("nameserver 10.0.0.1\nnameserver 10.0.0.2\n"
                                            "nameserver 10.0.0.3\nnameserver 10.0.0.4\n");
    REQUIRE(config.name_server_count == ResolvConf::MaximumNameServers);
    REQUIRE(config.name_servers[2].address[3] == 3);
}

TEST_CASE("resolv.conf options set the timeout and attempts within limits")
{
    ResolvConf::Config config = Parse("options timeout:5 rotate attempts:3\n");
    REQUIRE(config.name_server_count == 0);
    REQUIRE(config.timeout_seconds == 5);
    REQUIRE(config.attempts == 3);

    config = Parse("options timeout:0 attempts:0\n");
    REQUIRE(config.timeout_seconds == 1);
    REQUIRE(config.attempts == 1);

    config = Parse("options timeout:99999999999999999999 attempts:6\n");
    REQUIRE(config.timeout_seconds == ResolvConf::MaximumTimeoutSeconds);
    REQUIRE(config.attempts == ResolvConf::MaximumAttempts);

    config = Parse("options timeout: attempts:x timeout:-1 timeouts:9 ndots:1\noptions attempts:4");
    REQUIRE(config.timeout_seconds == 2);
    REQUIRE(config.attempts == 4);
}
