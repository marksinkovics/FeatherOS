// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/filesystem_helpers.h"

#include <algorithm>
#include <span>

namespace FeatherDisk::Tests
{

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
