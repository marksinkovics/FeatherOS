// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_EXT_DIRECTORY_H
#define FEATHERDISK_EXT_DIRECTORY_H

#include "fs/error.h"
#include "fs/filesystem.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace FeatherDisk::Ext
{

// A directory is a file of blocks, each a linked list of entries that never crosses a block:
// inode number, record length (to the next entry), name length and, with the filetype feature,
// the node's type in what was the name length's upper byte. Inode 0 marks an unused record.
struct RawEntry
{
    std::uint32_t inode;
    std::uint16_t record_length;
    // 0 without the filetype feature or when the type is unknown.
    std::uint8_t file_type;
    std::string_view name;
};

constexpr std::size_t EntryHeaderSize = 8;

// The entry at offset of a directory block. Error::Corrupt when it does not fit the block, is
// shorter than its name, is not aligned to 4 bytes, or names an inode past inode_count.
Result<RawEntry> ParseEntry(std::span<const std::uint8_t> block, std::size_t offset,
                            bool has_file_type, std::uint32_t inode_count);
std::optional<NodeType> TypeOfEntry(std::uint8_t file_type);
// The filetype byte for an inode's mode.
std::uint8_t EntryFileType(std::uint16_t mode);
// The bytes a record needs for a name of name_length: the header and the name, rounded up to a
// multiple of 4.
std::size_t RecordLength(std::size_t name_length);
// Stores entry's header and name at offset, which must have room for them. Without the filetype
// feature the type byte is the name length's upper half and is written as 0.
void StoreEntry(std::span<std::uint8_t> block, std::size_t offset, const RawEntry& entry,
                bool has_file_type);

} // namespace FeatherDisk::Ext

#endif
