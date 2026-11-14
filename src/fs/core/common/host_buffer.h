// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_COMMON_HOST_BUFFER_H
#define FEATHERDISK_COMMON_HOST_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk
{

// Bytes from Host::Allocate, returned when the buffer goes out of scope. Empty when the
// allocation failed.
class HostBuffer
{
public:
    explicit HostBuffer(std::size_t byte_count);
    ~HostBuffer();
    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    bool Empty() const;
    std::span<std::uint8_t> Bytes();

private:
    std::uint8_t* data;
    std::size_t size;
};

} // namespace FeatherDisk

#endif
