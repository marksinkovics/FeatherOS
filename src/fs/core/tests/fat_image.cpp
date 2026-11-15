// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/fat_image.h"

#include "common/little_endian.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace FeatherDisk::Tests
{

Fat16Image::Fat16Image(Fat16Layout image_layout)
    : layout(image_layout), device(512, image_layout.sectors)
{
    std::uint8_t* boot = device.Sector(0).data();
    StoreLittleEndian<std::uint16_t>(boot + 11, 512);
    boot[13] = 1;
    StoreLittleEndian<std::uint16_t>(boot + 14, 1);
    boot[16] = static_cast<std::uint8_t>(layout.fat_count);
    StoreLittleEndian(boot + 17, static_cast<std::uint16_t>(layout.root_entries));
    StoreLittleEndian(boot + 19, static_cast<std::uint16_t>(layout.sectors));
    boot[21] = 0xF8;
    StoreLittleEndian(boot + 22, static_cast<std::uint16_t>(layout.fat_sectors));
    boot[510] = 0x55;
    boot[511] = 0xAA;
}

MemoryDevice& Fat16Image::Device()
{
    return device;
}

std::uint32_t Fat16Image::RootDirectorySector() const
{
    return 1 + layout.fat_count * layout.fat_sectors;
}

std::uint8_t* Fat16Image::RootEntry(std::uint32_t index)
{
    return device.Sector(RootDirectorySector() + index / 16).data() + (index % 16) * 32;
}

std::uint16_t Fat16Image::FatEntry(std::uint32_t cluster, std::uint32_t copy)
{
    const std::uint8_t* fat = device.Sector(1 + copy * layout.fat_sectors + cluster / 256).data();
    return LoadLittleEndian<std::uint16_t>(fat + (cluster % 256) * 2);
}

void Fat16Image::SetFatEntry(std::uint32_t cluster, std::uint16_t value)
{
    for (std::uint32_t copy = 0; copy < layout.fat_count; ++copy)
    {
        std::uint8_t* fat = device.Sector(1 + copy * layout.fat_sectors + cluster / 256).data();
        StoreLittleEndian(fat + (cluster % 256) * 2, value);
    }
}

void Fat16Image::AddRootFile(const std::string& contents)
{
    std::uint8_t* entry = RootEntry(0);
    std::memcpy(entry, "BLOCK   TXT", 11);
    entry[11] = 0x20;
    StoreLittleEndian<std::uint16_t>(entry + 26, 2);
    StoreLittleEndian(entry + 28, static_cast<std::uint32_t>(contents.size()));

    const std::uint32_t data_start = RootDirectorySector() + (layout.root_entries * 32 + 511) / 512;
    const std::size_t first_chunk = std::min<std::size_t>(contents.size(), 512);
    std::memcpy(device.Sector(data_start).data(), contents.data(), first_chunk);
    if (contents.size() > first_chunk)
    {
        std::memcpy(device.Sector(data_start + 1).data(), contents.data() + first_chunk,
                    contents.size() - first_chunk);
    }
    SetFatEntry(2, contents.size() > first_chunk ? 3 : 0xFFF8);
    SetFatEntry(3, 0xFFF8);
}

MountedFilesystem::MountedFilesystem(FilesystemType& filesystem_type, BlockDevice& volume,
                                     bool read_only)
    : type(filesystem_type), filesystem(nullptr)
{
    filesystem = Value(type.Mount(volume, {.read_only = read_only}));
}

MountedFilesystem::~MountedFilesystem()
{
    if (filesystem != nullptr)
    {
        static_cast<void>(type.Unmount(*filesystem));
    }
}

Filesystem& MountedFilesystem::operator*()
{
    return *filesystem;
}

Filesystem* MountedFilesystem::operator->()
{
    return filesystem;
}

Result<NodeId> Walk(Filesystem& filesystem, std::string_view path)
{
    NodeId node = filesystem.Root();
    while (!path.empty())
    {
        const std::size_t separator = path.find('/');
        const std::string_view component = path.substr(0, separator);
        path =
            separator == std::string_view::npos ? std::string_view{} : path.substr(separator + 1);
        if (component.empty())
        {
            continue;
        }
        const Result<NodeId> next = filesystem.Lookup(node, component);
        if (!next)
        {
            return next;
        }
        node = *next;
    }
    return node;
}

std::pair<NodeId, std::string> Parent(Filesystem& filesystem, std::string_view path)
{
    const std::size_t separator = path.rfind('/');
    if (separator == std::string_view::npos)
    {
        return {filesystem.Root(), std::string(path)};
    }
    return {Value(Walk(filesystem, path.substr(0, separator))),
            std::string(path.substr(separator + 1))};
}

std::string ReadAll(Filesystem& filesystem, NodeId file)
{
    const Attributes attributes = Value(filesystem.GetAttributes(file));
    std::string contents(attributes.size, '\0');
    std::span<std::uint8_t> buffer(reinterpret_cast<std::uint8_t*>(contents.data()),
                                   contents.size());
    REQUIRE(Value(filesystem.Read(file, 0, buffer)) == contents.size());
    return contents;
}

NodeId WriteNewFile(Filesystem& filesystem, std::string_view path, const std::string& contents)
{
    const auto [directory, name] = Parent(filesystem, path);
    const NodeId file = Value(filesystem.Create(directory, name, 0644));
    const std::span<const std::uint8_t> data(reinterpret_cast<const std::uint8_t*>(contents.data()),
                                             contents.size());
    REQUIRE(Value(filesystem.Write(file, 0, data)) == contents.size());
    return file;
}

std::vector<std::string> List(Filesystem& filesystem, NodeId directory)
{
    std::vector<std::string> names;
    std::uint64_t cookie = 0;
    DirectoryEntry entry{};
    while (Value(filesystem.ReadDirectory(directory, cookie, entry)))
    {
        names.emplace_back(entry.name.data(), entry.name_length);
        if (entry.type == NodeType::Directory)
        {
            names.back() += '/';
        }
    }
    return names;
}

std::vector<std::string> Sorted(std::vector<std::string> names)
{
    std::sort(names.begin(), names.end());
    return names;
}

std::string Pattern(std::size_t length)
{
    std::string text(length, '\0');
    for (std::size_t index = 0; index < length; ++index)
    {
        text[index] = static_cast<char>('a' + index % 23);
    }
    return text;
}

std::uint32_t FreeBlocks(Filesystem& filesystem)
{
    return static_cast<std::uint32_t>(Value(filesystem.Statistics()).free_blocks);
}

} // namespace FeatherDisk::Tests
