#include <catch2/catch_test_macros.hpp>

#include "libc/dns_message.h"

#include <string>
#include <vector>

namespace Dns = FeatherOS::LibC::Dns;

namespace
{

std::vector<uint8_t> Query(const char* name, uint16_t type, uint16_t id = 0x1234)
{
    std::vector<uint8_t> message(Dns::MaximumMessageSize);
    const size_t length = Dns::BuildQuery(message.data(), message.size(), id, name, type);
    message.resize(length);
    return message;
}

// The query turned into a response with the given code and answer records appended.
std::vector<uint8_t> Response(std::vector<uint8_t> query, uint8_t code, uint16_t answers,
                              const std::vector<uint8_t>& records)
{
    query[2] |= 0x80;
    query[3] = code;
    query[6] = static_cast<uint8_t>(answers >> 8);
    query[7] = static_cast<uint8_t>(answers);
    query.insert(query.end(), records.begin(), records.end());
    return query;
}

// A record whose owner is a pointer to the question's name at offset 12.
std::vector<uint8_t> Record(uint16_t type, std::vector<uint8_t> data)
{
    std::vector<uint8_t> record = {0xC0, 12, static_cast<uint8_t>(type >> 8),
                                   static_cast<uint8_t>(type), 0, 1, 0, 0, 0x0E, 0x10, 0,
                                   static_cast<uint8_t>(data.size())};
    record.insert(record.end(), data.begin(), data.end());
    return record;
}

} // namespace

TEST_CASE("a query asks for recursion and encodes the name as labels")
{
    const std::vector<uint8_t> query = Query("gateway.featheros.test.", Dns::TypeAaaa);
    const std::vector<uint8_t> expected = {
        0x12, 0x34, 0x01, 0, 0, 1, 0, 0, 0, 0, 0, 0, 7,   'g', 'a', 't', 'e', 'w', 'a', 'y',
        9,    'f',  'e',  'a', 't', 'h', 'e', 'r', 'o', 's', 4, 't', 'e', 's', 't', 0, 0, 28, 0, 1};
    REQUIRE(query == expected);
    REQUIRE(Query("gateway.featheros.test", Dns::TypeAaaa) == expected);
}

TEST_CASE("names with empty or long labels are not queried")
{
    REQUIRE(Query("", Dns::TypeA).empty());
    REQUIRE(Query(".", Dns::TypeA).empty());
    REQUIRE(Query("a..b", Dns::TypeA).empty());
    REQUIRE(Query(".a", Dns::TypeA).empty());
    REQUIRE(Query(std::string(64, 'x').c_str(), Dns::TypeA).empty());
    REQUIRE(Query(std::string(63, 'x').c_str(), Dns::TypeA).size() == 12 + 65 + 4);
    std::string long_name;
    for (int label = 0; label < 64; ++label)
    {
        long_name += "abc.";
    }
    REQUIRE(Query(long_name.c_str(), Dns::TypeA).empty());
    uint8_t small[20];
    REQUIRE(Dns::BuildQuery(small, sizeof(small), 1, "example.test", Dns::TypeA) == 0);
}

TEST_CASE("a response gives the addresses of the asked type, after a CNAME too")
{
    const std::vector<uint8_t> query = Query("alias.featheros.test", Dns::TypeA);
    std::vector<uint8_t> records = Record(5, {7, 'g', 'a', 't', 'e', 'w', 'a', 'y', 0xC0, 18});
    const std::vector<uint8_t> first = Record(Dns::TypeA, {10, 0, 2, 2});
    const std::vector<uint8_t> wrong_size = Record(Dns::TypeA, {10, 0, 2});
    const std::vector<uint8_t> second = Record(Dns::TypeA, {10, 0, 2, 3});
    records.insert(records.end(), first.begin(), first.end());
    records.insert(records.end(), wrong_size.begin(), wrong_size.end());
    records.insert(records.end(), second.begin(), second.end());
    const std::vector<uint8_t> response = Response(query, 0, 4, records);

    Dns::Addresses addresses{};
    REQUIRE(Dns::ParseResponse(response.data(), response.size(), 0x1234, Dns::TypeA,
                               &addresses) == Dns::Outcome::Answered);
    REQUIRE(addresses.count == 2);
    REQUIRE(addresses.bytes[0][3] == 2);
    REQUIRE(addresses.bytes[1][3] == 3);
}

TEST_CASE("an IPv6 answer has 16-byte records")
{
    const std::vector<uint8_t> query = Query("gateway.featheros.test", Dns::TypeAaaa);
    std::vector<uint8_t> address(16, 0);
    address[0] = 0xFE;
    address[1] = 0xC0;
    address[15] = 2;
    const std::vector<uint8_t> response =
        Response(query, 0, 2, [&] {
            std::vector<uint8_t> records = Record(Dns::TypeA, {10, 0, 2, 2});
            const std::vector<uint8_t> ipv6 = Record(Dns::TypeAaaa, address);
            records.insert(records.end(), ipv6.begin(), ipv6.end());
            return records;
        }());
    Dns::Addresses addresses{};
    REQUIRE(Dns::ParseResponse(response.data(), response.size(), 0x1234, Dns::TypeAaaa,
                               &addresses) == Dns::Outcome::Answered);
    REQUIRE(addresses.count == 1);
    REQUIRE(addresses.bytes[0][0] == 0xFE);
    REQUIRE(addresses.bytes[0][15] == 2);
}

TEST_CASE("response codes and unrelated messages")
{
    const std::vector<uint8_t> query = Query("nothing.featheros.test", Dns::TypeA);
    Dns::Addresses addresses{};
    const std::vector<uint8_t> missing = Response(query, 3, 0, {});
    REQUIRE(Dns::ParseResponse(missing.data(), missing.size(), 0x1234, Dns::TypeA, &addresses) ==
            Dns::Outcome::NoName);
    const std::vector<uint8_t> refused = Response(query, 5, 0, {});
    REQUIRE(Dns::ParseResponse(refused.data(), refused.size(), 0x1234, Dns::TypeA, &addresses) ==
            Dns::Outcome::Failed);
    const std::vector<uint8_t> empty = Response(query, 0, 0, {});
    REQUIRE(Dns::ParseResponse(empty.data(), empty.size(), 0x1234, Dns::TypeA, &addresses) ==
            Dns::Outcome::Answered);
    REQUIRE(addresses.count == 0);

    REQUIRE(Dns::ParseResponse(empty.data(), empty.size(), 0x4321, Dns::TypeA, &addresses) ==
            Dns::Outcome::Unrelated);
    REQUIRE(Dns::ParseResponse(empty.data(), empty.size(), 0x1234, Dns::TypeAaaa, &addresses) ==
            Dns::Outcome::Unrelated);
    REQUIRE(Dns::ParseResponse(query.data(), query.size(), 0x1234, Dns::TypeA, &addresses) ==
            Dns::Outcome::Unrelated);
}

TEST_CASE("a truncated response asks for TCP, which carries answers past 512 bytes")
{
    const std::vector<uint8_t> query = Query("big.featheros.test", Dns::TypeA);
    std::vector<uint8_t> truncated = Response(query, 0, 0, {});
    truncated[2] |= 0x02;
    Dns::Addresses addresses{};
    REQUIRE(Dns::ParseResponse(truncated.data(), truncated.size(), 0x1234, Dns::TypeA,
                               &addresses) == Dns::Outcome::Truncated);
    REQUIRE(Dns::ParseResponse(truncated.data(), truncated.size(), 0x4321, Dns::TypeA,
                               &addresses) == Dns::Outcome::Unrelated);
    truncated[3] = 3;
    REQUIRE(Dns::ParseResponse(truncated.data(), truncated.size(), 0x1234, Dns::TypeA,
                               &addresses) == Dns::Outcome::Truncated);
    char name[Dns::NameCapacity];
    const std::vector<uint8_t> pointer_query = Query("2.2.0.10.in-addr.arpa", Dns::TypePtr);
    std::vector<uint8_t> truncated_pointer = Response(pointer_query, 0, 0, {});
    truncated_pointer[2] |= 0x02;
    REQUIRE(Dns::ParsePointerResponse(truncated_pointer.data(), truncated_pointer.size(), 0x1234,
                                      name, sizeof(name)) == Dns::Outcome::Truncated);

    constexpr uint16_t RecordCount = 40;
    std::vector<uint8_t> records;
    for (uint16_t index = 0; index < RecordCount; ++index)
    {
        const std::vector<uint8_t> record =
            Record(Dns::TypeA, {10, 0, 3, static_cast<uint8_t>(index + 1)});
        records.insert(records.end(), record.begin(), record.end());
    }
    const std::vector<uint8_t> full = Response(query, 0, RecordCount, records);
    REQUIRE(full.size() > Dns::MaximumMessageSize);
    REQUIRE(Dns::ParseResponse(full.data(), full.size(), 0x1234, Dns::TypeA, &addresses) ==
            Dns::Outcome::Answered);
    REQUIRE(addresses.count == Dns::MaximumAddresses);
    REQUIRE(addresses.bytes[0][3] == 1);
    REQUIRE(addresses.bytes[Dns::MaximumAddresses - 1][3] == Dns::MaximumAddresses);
}

TEST_CASE("records running past the message fail the response")
{
    const std::vector<uint8_t> query = Query("gateway.featheros.test", Dns::TypeA);
    const std::vector<uint8_t> response = Response(query, 0, 1, Record(Dns::TypeA, {10, 0, 2, 2}));
    Dns::Addresses addresses{};
    REQUIRE(Dns::ParseResponse(response.data(), response.size() - 1, 0x1234, Dns::TypeA,
                               &addresses) == Dns::Outcome::Failed);
    const std::vector<uint8_t> extra = Response(query, 0, 2, Record(Dns::TypeA, {10, 0, 2, 2}));
    REQUIRE(Dns::ParseResponse(extra.data(), extra.size(), 0x1234, Dns::TypeA, &addresses) ==
            Dns::Outcome::Failed);
}

TEST_CASE("reverse names list the address backwards under in-addr.arpa or ip6.arpa")
{
    char name[Dns::NameCapacity];
    const uint8_t ipv4[4] = {10, 0, 2, 255};
    REQUIRE(Dns::BuildReverseName(ipv4, 4, name, sizeof(name)) == 23);
    REQUIRE(std::string(name) == "255.2.0.10.in-addr.arpa");

    uint8_t ipv6[16] = {0xFE, 0xC0};
    ipv6[15] = 0x2B;
    REQUIRE(Dns::BuildReverseName(ipv6, 16, name, sizeof(name)) == 72);
    REQUIRE(std::string(name) ==
            "b.2.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.c.e.f.ip6.arpa");

    char small[24];
    REQUIRE(Dns::BuildReverseName(ipv4, 4, small, sizeof(small) - 1) == 0);
    REQUIRE(Dns::BuildReverseName(ipv4, 4, small, sizeof(small)) == 23);
    REQUIRE(Dns::BuildReverseName(ipv4, 5, name, sizeof(name)) == 0);
}

TEST_CASE("a PTR response gives the first host name, following compression and CNAMEs")
{
    const std::vector<uint8_t> query = Query("2.2.0.10.in-addr.arpa", Dns::TypePtr);
    // The answer's name ends with a pointer to "arpa" in the question.
    const size_t arpa = 12 + 2 + 2 + 2 + 3 + 8;
    REQUIRE(query[arpa] == 4);
    std::vector<uint8_t> records = Record(5, {1, 'x', 0xC0, 12});
    const std::vector<uint8_t> bad = Record(Dns::TypePtr, {3, 'a', ' ', 'b', 0});
    const std::vector<uint8_t> good = Record(
        Dns::TypePtr, {7, 'g', 'a', 't', 'e', 'w', 'a', 'y', 0xC0, static_cast<uint8_t>(arpa)});
    const std::vector<uint8_t> later = Record(Dns::TypePtr, {5, 'l', 'a', 't', 'e', 'r', 0});
    records.insert(records.end(), bad.begin(), bad.end());
    records.insert(records.end(), good.begin(), good.end());
    records.insert(records.end(), later.begin(), later.end());
    const std::vector<uint8_t> response = Response(query, 0, 4, records);

    char name[Dns::NameCapacity];
    REQUIRE(Dns::ParsePointerResponse(response.data(), response.size(), 0x1234, name,
                                      sizeof(name)) == Dns::Outcome::Answered);
    REQUIRE(std::string(name) == "gateway.arpa");
    char small[12];
    REQUIRE(Dns::ParsePointerResponse(response.data(), response.size(), 0x1234, small,
                                      sizeof(small)) == Dns::Outcome::Answered);
    REQUIRE(std::string(small) == "later");
}

TEST_CASE("PTR names that loop or run out are skipped")
{
    const std::vector<uint8_t> query = Query("1.0.0.127.in-addr.arpa", Dns::TypePtr);
    const auto data_offset = static_cast<uint8_t>(query.size() + 12);
    // A pointer to itself, then a label followed by a pointer back to that label.
    const std::vector<uint8_t> self = Record(Dns::TypePtr, {0xC0, data_offset});
    std::vector<uint8_t> records = self;
    const auto second_data = static_cast<uint8_t>(query.size() + self.size() + 12);
    const std::vector<uint8_t> forward = Record(Dns::TypePtr, {1, 'a', 0xC0, second_data});
    const std::vector<uint8_t> past_record = Record(Dns::TypePtr, {3, 'a', 'b'});
    records.insert(records.end(), forward.begin(), forward.end());
    records.insert(records.end(), past_record.begin(), past_record.end());
    std::vector<uint8_t> response = Response(query, 0, 3, records);
    char name[Dns::NameCapacity];
    REQUIRE(Dns::ParsePointerResponse(response.data(), response.size(), 0x1234, name,
                                      sizeof(name)) == Dns::Outcome::Answered);
    REQUIRE(std::string(name).empty());

    const std::vector<uint8_t> missing = Response(query, 3, 0, {});
    REQUIRE(Dns::ParsePointerResponse(missing.data(), missing.size(), 0x1234, name,
                                      sizeof(name)) == Dns::Outcome::NoName);
    const std::vector<uint8_t> addresses = Response(Query("a.test", Dns::TypeA), 0, 0, {});
    REQUIRE(Dns::ParsePointerResponse(addresses.data(), addresses.size(), 0x1234, name,
                                      sizeof(name)) == Dns::Outcome::Unrelated);
}
