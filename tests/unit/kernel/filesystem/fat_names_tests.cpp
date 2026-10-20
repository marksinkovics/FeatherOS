#include <catch2/catch_test_macros.hpp>

#include "kernel/filesystem/fat_names.h"

#include <array>
#include <string>

namespace
{

std::string Raw(const uint8_t (&name)[11])
{
    return std::string(reinterpret_cast<const char*>(name), sizeof(name));
}

std::string Basis(const char* name, uint32_t number)
{
    uint8_t short_name[11];
    FeatherOS::FatNames::MakeBasis(name, short_name);
    FeatherOS::FatNames::ApplyTail(short_name, number);
    return Raw(short_name);
}

} // namespace

TEST_CASE("FAT names that fit 8.3 with one case per part become short names")
{
    uint8_t short_name[11];
    uint8_t flags = 0xFF;
    REQUIRE(FeatherOS::FatNames::ToShortName("notes.txt", short_name, &flags));
    REQUIRE(Raw(short_name) == "NOTES   TXT");
    REQUIRE(flags == 0x18);
    REQUIRE(FeatherOS::FatNames::ToShortName("README", short_name, &flags));
    REQUIRE(Raw(short_name) == "README     ");
    REQUIRE(flags == 0);
    REQUIRE(FeatherOS::FatNames::ToShortName("Docs.TXT", short_name, &flags) == false);
    REQUIRE_FALSE(FeatherOS::FatNames::ToShortName("my notes.txt", short_name, &flags));
    REQUIRE_FALSE(FeatherOS::FatNames::ToShortName("verylongname.txt", short_name, &flags));
    REQUIRE_FALSE(FeatherOS::FatNames::ToShortName("a.b.c", short_name, &flags));
    REQUIRE_FALSE(FeatherOS::FatNames::ToShortName("a+b.txt", short_name, &flags));

    uint8_t entry[32] = {};
    for (uint32_t index = 0; index < 11; ++index)
    {
        entry[index] = static_cast<uint8_t>("NOTES   TXT"[index]);
    }
    char text[64];
    entry[12] = 0x08;
    FeatherOS::FatNames::ShortNameText(entry, text);
    REQUIRE(std::string(text) == "notes.TXT");
}

TEST_CASE("FAT long names get numbered short names")
{
    REQUIRE(Basis("my notes.txt", 1) == "MYNOTE~1TXT");
    REQUIRE(Basis("my notes.txt", 12) == "MYNOT~12TXT");
    REQUIRE(Basis("a+b", 1) == "A_B~1      ");
    REQUIRE(Basis(".hidden", 1) == "HIDDEN~1   ");
    REQUIRE(Basis("archive.tar.gz", 1) == "ARCHIV~1GZ ");
}

TEST_CASE("FAT name validation")
{
    REQUIRE(FeatherOS::FatNames::IsValidName("My Notes (2).txt"));
    REQUIRE(FeatherOS::FatNames::IsValidName(std::string(63, 'x').c_str()));
    REQUIRE_FALSE(FeatherOS::FatNames::IsValidName(std::string(64, 'x').c_str()));
    REQUIRE_FALSE(FeatherOS::FatNames::IsValidName(""));
    REQUIRE_FALSE(FeatherOS::FatNames::IsValidName("ends with space "));
    REQUIRE_FALSE(FeatherOS::FatNames::IsValidName("ends with dot."));
    REQUIRE_FALSE(FeatherOS::FatNames::IsValidName("a*b"));
    REQUIRE_FALSE(FeatherOS::FatNames::IsValidName("tab\there"));
    REQUIRE(FeatherOS::FatNames::SameName("My Notes.TXT", "my notes.txt"));
    REQUIRE_FALSE(FeatherOS::FatNames::SameName("notes", "notes.txt"));
}

TEST_CASE("FAT long-name entries round-trip and require a matching checksum")
{
    const char* name = "a much longer file name.text";
    REQUIRE(FeatherOS::FatNames::LongEntryCount(name) == 3);
    const uint8_t short_name[11] = {'A', 'M', 'U', 'C', 'H', 'L', '~', '1', 'T', 'E', 'X'};
    const uint8_t checksum = FeatherOS::FatNames::Checksum(short_name);
    std::array<std::array<uint8_t, 32>, 3> entries{};
    for (uint32_t index = 0; index < 3; ++index)
    {
        FeatherOS::FatNames::EncodeLongEntry(name, 3 - index, 3, checksum, entries[index].data());
    }
    REQUIRE(entries[0][0] == 0x43);
    REQUIRE(entries[2][0] == 0x01);

    uint8_t short_entry[32] = {};
    for (uint32_t index = 0; index < 11; ++index)
    {
        short_entry[index] = short_name[index];
    }
    FeatherOS::FatNames::LongNameReader reader;
    reader.Reset();
    for (uint32_t index = 0; index < 3; ++index)
    {
        reader.Feed(entries[index].data(), 10 + index);
    }
    char text[64];
    uint32_t first_slot = 0;
    REQUIRE(reader.Finish(short_entry, text, &first_slot));
    REQUIRE(std::string(text) == name);
    REQUIRE(first_slot == 10);

    // A different short name means the long name belongs to a deleted or replaced file.
    short_entry[0] = 'B';
    reader.Reset();
    for (uint32_t index = 0; index < 3; ++index)
    {
        reader.Feed(entries[index].data(), index);
    }
    REQUIRE_FALSE(reader.Finish(short_entry, text, &first_slot));

    // A missing middle entry breaks the sequence.
    short_entry[0] = 'A';
    reader.Reset();
    reader.Feed(entries[0].data(), 0);
    reader.Feed(entries[2].data(), 1);
    REQUIRE_FALSE(reader.Finish(short_entry, text, &first_slot));
}
