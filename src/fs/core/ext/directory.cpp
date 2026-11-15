// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ext/directory.h"

#include "common/little_endian.h"

namespace FeatherDisk::Ext
{
namespace
{

// The shortest record: the header and a name of up to four bytes.
constexpr std::size_t MinimumRecordLength = 12;

std::size_t RecordLengthFor(std::size_t name_length)
{
    return (EntryHeaderSize + name_length + 3) & ~std::size_t{3};
}

} // namespace

Result<RawEntry> ParseEntry(std::span<const std::uint8_t> block, std::size_t offset,
                            bool has_file_type, std::uint32_t inode_count)
{
    if (offset > block.size() || block.size() - offset < EntryHeaderSize)
    {
        return std::unexpected(Error::Corrupt);
    }
    const std::uint8_t* raw = block.data() + offset;
    const std::uint32_t inode = LoadLittleEndian<std::uint32_t>(raw);
    const std::uint16_t record_length = LoadLittleEndian<std::uint16_t>(raw + 4);
    // Without the filetype feature the name length is 16 bits wide.
    const std::size_t name_length =
        has_file_type ? raw[6] : LoadLittleEndian<std::uint16_t>(raw + 6);
    if (record_length < MinimumRecordLength || record_length % 4 != 0 ||
        record_length > block.size() - offset || record_length < RecordLengthFor(name_length) ||
        name_length > MaximumNameLength || inode > inode_count || (inode != 0 && name_length == 0))
    {
        return std::unexpected(Error::Corrupt);
    }
    return RawEntry{
        .inode = inode,
        .record_length = record_length,
        .file_type = has_file_type ? raw[7] : std::uint8_t{0},
        .name = {reinterpret_cast<const char*>(raw + EntryHeaderSize), name_length},
    };
}

std::optional<NodeType> TypeOfEntry(std::uint8_t file_type)
{
    switch (file_type)
    {
    case 1:
        return NodeType::File;
    case 2:
        return NodeType::Directory;
    case 3: // character device
    case 4: // block device
    case 5: // FIFO
    case 6: // socket
        return NodeType::Special;
    case 7:
        return NodeType::SymbolicLink;
    default:
        return std::nullopt;
    }
}

} // namespace FeatherDisk::Ext
