// SPDX-License-Identifier: MIT OR Apache-2.0
#include "fs/filesystem.h"

namespace FeatherDisk
{

Status Filesystem::SetAttributes(NodeId /*node*/, const AttributeChanges& /*changes*/)
{
    return std::unexpected(Error::Unsupported);
}

Result<std::size_t> Filesystem::Write(NodeId /*file*/, std::uint64_t /*offset*/,
                                      std::span<const std::uint8_t> /*data*/)
{
    return std::unexpected(Error::Unsupported);
}

Status Filesystem::Truncate(NodeId /*file*/, std::uint64_t /*size*/)
{
    return std::unexpected(Error::Unsupported);
}

Result<NodeId> Filesystem::Create(NodeId /*directory*/, std::string_view /*name*/,
                                  std::uint32_t /*mode*/)
{
    return std::unexpected(Error::Unsupported);
}

Result<NodeId> Filesystem::MakeDirectory(NodeId /*directory*/, std::string_view /*name*/,
                                         std::uint32_t /*mode*/)
{
    return std::unexpected(Error::Unsupported);
}

Status Filesystem::Remove(NodeId /*directory*/, std::string_view /*name*/)
{
    return std::unexpected(Error::Unsupported);
}

Status Filesystem::RemoveDirectory(NodeId /*directory*/, std::string_view /*name*/)
{
    return std::unexpected(Error::Unsupported);
}

Status Filesystem::Rename(NodeId /*from_directory*/, std::string_view /*from_name*/,
                          NodeId /*to_directory*/, std::string_view /*to_name*/)
{
    return std::unexpected(Error::Unsupported);
}

Status Filesystem::Link(NodeId /*node*/, NodeId /*directory*/, std::string_view /*name*/)
{
    return std::unexpected(Error::Unsupported);
}

Result<NodeId> Filesystem::SymbolicLink(NodeId /*directory*/, std::string_view /*name*/,
                                        std::string_view /*target*/)
{
    return std::unexpected(Error::Unsupported);
}

Result<std::size_t> Filesystem::ReadLink(NodeId /*link*/, std::span<char> /*buffer*/)
{
    return std::unexpected(Error::Unsupported);
}

Status Filesystem::Sync()
{
    return {};
}

Status Filesystem::MakeReadOnly()
{
    return std::unexpected(Error::Unsupported);
}

} // namespace FeatherDisk
