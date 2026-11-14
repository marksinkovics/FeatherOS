// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_ERROR_H
#define FEATHERDISK_ERROR_H

#include <cstdint>
#include <expected>

namespace FeatherDisk
{

enum class Error : std::uint8_t
{
    Io,
    // The on-disk structures contradict each other or their checksums.
    Corrupt,
    NotFound,
    Exists,
    NotDirectory,
    IsDirectory,
    NotEmpty,
    NoSpace,
    ReadOnly,
    InvalidArgument,
    NameTooLong,
    FileTooLarge,
    TooManyLinks,
    Busy,
    OutOfMemory,
    Unsupported,
};

template <typename T> using Result = std::expected<T, Error>;
using Status = std::expected<void, Error>;

} // namespace FeatherDisk

#endif
