// SPDX-License-Identifier: MIT OR Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "fat/names.h"

#include <array>
#include <string>

namespace
{

using FeatherDisk::Fat::ShortName;

std::string Raw(const ShortName& name)
{
    return std::string(reinterpret_cast<const char*>(name.data()), name.size());
}

std::string Basis(const char* name, std::uint32_t number)
{
    ShortName short_name{};
    FeatherDisk::Fat::MakeBasis(name, short_name);
    FeatherDisk::Fat::ApplyTail(short_name, number);
    return Raw(short_name);
}

} // namespace

TEST_CASE("FAT names that fit 8.3 with one case per part become short names")
{
    ShortName short_name{};
    std::uint8_t flags = 0xFF;
    REQUIRE(FeatherDisk::Fat::ToShortName("notes.txt", short_name, flags));
    REQUIRE(Raw(short_name) == "NOTES   TXT");
    REQUIRE(flags == 0x18);
    REQUIRE(FeatherDisk::Fat::ToShortName("README", short_name, flags));
    REQUIRE(Raw(short_name) == "README     ");
    REQUIRE(flags == 0);
    REQUIRE_FALSE(FeatherDisk::Fat::ToShortName("Docs.TXT", short_name, flags));
    REQUIRE_FALSE(FeatherDisk::Fat::ToShortName("my notes.txt", short_name, flags));
    REQUIRE_FALSE(FeatherDisk::Fat::ToShortName("verylongname.txt", short_name, flags));
    REQUIRE_FALSE(FeatherDisk::Fat::ToShortName("a.b.c", short_name, flags));
    REQUIRE_FALSE(FeatherDisk::Fat::ToShortName("a+b.txt", short_name, flags));
    REQUIRE_FALSE(FeatherDisk::Fat::ToShortName(".hidden", short_name, flags));

    std::array<std::uint8_t, 32> entry{};
    for (std::uint32_t index = 0; index < 11; ++index)
    {
        entry[index] = static_cast<std::uint8_t>("NOTES   TXT"[index]);
    }
    FeatherDisk::Fat::Name text{};
    entry[12] = 0x08;
    FeatherDisk::Fat::ShortNameText(entry.data(), text);
    REQUIRE(text.View() == "notes.TXT");
    entry[12] = 0x10;
    FeatherDisk::Fat::ShortNameText(entry.data(), text);
    REQUIRE(text.View() == "NOTES.txt");
}

TEST_CASE("FAT long names get numbered short names")
{
    REQUIRE(Basis("my notes.txt", 1) == "MYNOTE~1TXT");
    REQUIRE(Basis("my notes.txt", 12) == "MYNOT~12TXT");
    REQUIRE(Basis("a+b", 1) == "A_B~1      ");
    REQUIRE(Basis(".hidden", 1) == "HIDDEN~1   ");
    REQUIRE(Basis("archive.tar.gz", 1) == "ARCHIV~1GZ ");
    REQUIRE(Basis("...", 1) == "_~1        ");
    REQUIRE(Basis("x", 1234567) == "~1234567   ");
}

TEST_CASE("FAT name validation")
{
    REQUIRE(FeatherDisk::Fat::IsValidName("My Notes (2).txt"));
    REQUIRE(FeatherDisk::Fat::IsValidName(std::string(63, 'x')));
    REQUIRE_FALSE(FeatherDisk::Fat::IsValidName(std::string(64, 'x')));
    REQUIRE_FALSE(FeatherDisk::Fat::IsValidName(""));
    REQUIRE_FALSE(FeatherDisk::Fat::IsValidName("ends with space "));
    REQUIRE_FALSE(FeatherDisk::Fat::IsValidName("ends with dot."));
    REQUIRE_FALSE(FeatherDisk::Fat::IsValidName("a*b"));
    REQUIRE_FALSE(FeatherDisk::Fat::IsValidName("tab\there"));
    REQUIRE_FALSE(FeatherDisk::Fat::IsValidName("caf\xC3\xA9"));
    REQUIRE_FALSE(FeatherDisk::Fat::IsValidName(std::string("nul\0inside", 10)));
    REQUIRE(FeatherDisk::Fat::SameName("My Notes.TXT", "my notes.txt"));
    REQUIRE_FALSE(FeatherDisk::Fat::SameName("notes", "notes.txt"));
    REQUIRE_FALSE(FeatherDisk::Fat::SameName("notes.txt", "notes"));
}

TEST_CASE("FAT long-name entries round-trip and require a matching checksum")
{
    const char* name = "a much longer file name.text";
    REQUIRE(FeatherDisk::Fat::LongEntryCount(name) == 3);
    const ShortName short_name = {'A', 'M', 'U', 'C', 'H', 'L', '~', '1', 'T', 'E', 'X'};
    const std::uint8_t checksum = FeatherDisk::Fat::Checksum(short_name.data());
    std::array<std::array<std::uint8_t, 32>, 3> entries{};
    for (std::uint32_t index = 0; index < 3; ++index)
    {
        FeatherDisk::Fat::EncodeLongEntry(name, 3 - index, 3, checksum, entries[index].data());
    }
    REQUIRE(entries[0][0] == 0x43);
    REQUIRE(entries[2][0] == 0x01);

    std::array<std::uint8_t, 32> short_entry{};
    for (std::uint32_t index = 0; index < 11; ++index)
    {
        short_entry[index] = short_name[index];
    }
    FeatherDisk::Fat::LongNameReader reader;
    reader.Reset();
    for (std::uint32_t index = 0; index < 3; ++index)
    {
        reader.Feed(entries[index].data(), 10 + index);
    }
    FeatherDisk::Fat::Name text{};
    std::uint32_t first_slot = 0;
    REQUIRE(reader.Finish(short_entry.data(), text, first_slot));
    REQUIRE(text.View() == name);
    REQUIRE(first_slot == 10);

    // A different short name means the long name belongs to a deleted or replaced file.
    short_entry[0] = 'B';
    reader.Reset();
    for (std::uint32_t index = 0; index < 3; ++index)
    {
        reader.Feed(entries[index].data(), index);
    }
    REQUIRE_FALSE(reader.Finish(short_entry.data(), text, first_slot));

    // A missing middle entry breaks the sequence.
    short_entry[0] = 'A';
    reader.Reset();
    reader.Feed(entries[0].data(), 0);
    reader.Feed(entries[2].data(), 1);
    REQUIRE_FALSE(reader.Finish(short_entry.data(), text, first_slot));
}

TEST_CASE("FAT long names past 63 characters read as their short names")
{
    const std::string name(70, 'n');
    const ShortName short_name = {'N', 'N', 'N', 'N', 'N', 'N', '~', '1', ' ', ' ', ' '};
    const std::uint8_t checksum = FeatherDisk::Fat::Checksum(short_name.data());
    const std::uint32_t total = FeatherDisk::Fat::LongEntryCount(name);
    REQUIRE(total == 6);
    FeatherDisk::Fat::LongNameReader reader;
    reader.Reset();
    for (std::uint32_t index = 0; index < total; ++index)
    {
        std::array<std::uint8_t, 32> entry{};
        FeatherDisk::Fat::EncodeLongEntry(name, total - index, total, checksum, entry.data());
        reader.Feed(entry.data(), index);
    }
    std::array<std::uint8_t, 32> short_entry{};
    for (std::uint32_t index = 0; index < 11; ++index)
    {
        short_entry[index] = short_name[index];
    }
    FeatherDisk::Fat::Name text{};
    std::uint32_t first_slot = 0;
    REQUIRE_FALSE(reader.Finish(short_entry.data(), text, first_slot));
}
