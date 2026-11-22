// SPDX-License-Identifier: MIT OR Apache-2.0
#include "common/host_buffer.h"

#include "fs/host.h"

namespace FeatherDisk
{

HostBuffer::HostBuffer(std::size_t byte_count)
    : data(byte_count == 0 ? nullptr : static_cast<std::uint8_t*>(Host::Allocate(byte_count))),
      size(data == nullptr ? 0 : byte_count)
{
}

HostBuffer::~HostBuffer()
{
    Host::Free(data);
}

bool HostBuffer::Empty() const
{
    return data == nullptr;
}

std::span<std::uint8_t> HostBuffer::Bytes()
{
    return {data, size};
}

} // namespace FeatherDisk
