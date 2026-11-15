// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_FSCLI_FILE_DEVICE_H
#define FEATHERDISK_FSCLI_FILE_DEVICE_H

#include "fs/block_device.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace FeatherDisk::Fscli
{

// An image file, with 512-byte sectors and any partial last sector left out, or a raw disk with
// the sector size the system reports.
class FileDevice final : public BlockDevice
{
public:
    // The error is a message for the user.
    static std::expected<std::unique_ptr<FileDevice>, std::string> Open(const std::string& path,
                                                                        bool writable);
    ~FileDevice();

    std::uint32_t SectorSize() const override;
    std::uint64_t SectorCount() const override;
    Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override;
    Status Write(std::uint64_t first_sector, std::span<const std::uint8_t> data) override;
    Status Flush() override;

private:
    FileDevice(int descriptor, bool writable, std::uint32_t sector_size,
               std::uint64_t sector_count);
    bool Covers(std::uint64_t first_sector, std::size_t byte_count) const;

    int descriptor;
    bool writable;
    std::uint32_t sector_size;
    std::uint64_t sector_count;
};

} // namespace FeatherDisk::Fscli

#endif
