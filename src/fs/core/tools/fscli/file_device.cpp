// SPDX-License-Identifier: MIT OR Apache-2.0
#include "file_device.h"

#include <cerrno>
#include <cstring>
#include <tuple>
#include <utility>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/disk.h>
#elif defined(__linux__)
#include <linux/fs.h>
#endif

namespace FeatherDisk::Fscli
{
namespace
{

constexpr std::uint32_t ImageSectorSize = 512;

std::string SystemError(int error)
{
    return std::strerror(error);
}

// Disks report their size only through ioctl; lseek and st_size give 0 for them on macOS.
std::expected<std::pair<std::uint32_t, std::uint64_t>, std::string> DiskGeometry(int descriptor)
{
#if defined(__APPLE__)
    std::uint32_t block_size = 0;
    std::uint64_t block_count = 0;
    if (ioctl(descriptor, DKIOCGETBLOCKSIZE, &block_size) != 0 ||
        ioctl(descriptor, DKIOCGETBLOCKCOUNT, &block_count) != 0)
    {
        return std::unexpected(SystemError(errno));
    }
    return std::pair{block_size, block_count};
#elif defined(__linux__)
    int block_size = 0;
    std::uint64_t bytes = 0;
    if (ioctl(descriptor, BLKSSZGET, &block_size) != 0 ||
        ioctl(descriptor, BLKGETSIZE64, &bytes) != 0 || block_size <= 0)
    {
        return std::unexpected(SystemError(errno));
    }
    const auto size = static_cast<std::uint32_t>(block_size);
    return std::pair{size, bytes / size};
#else
    (void)descriptor;
    return std::unexpected(std::string("raw disks are not supported on this system"));
#endif
}

} // namespace

std::expected<std::unique_ptr<FileDevice>, std::string> FileDevice::Open(const std::string& path,
                                                                         bool writable)
{
    const int descriptor = ::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (descriptor < 0)
    {
        return std::unexpected(SystemError(errno));
    }
    struct stat status = {};
    if (fstat(descriptor, &status) != 0)
    {
        const int error = errno;
        ::close(descriptor);
        return std::unexpected(SystemError(error));
    }
    std::uint32_t sector_size = ImageSectorSize;
    std::uint64_t sector_count = 0;
    if (S_ISREG(status.st_mode))
    {
        sector_count = static_cast<std::uint64_t>(status.st_size) / ImageSectorSize;
    }
    else if (S_ISBLK(status.st_mode) || S_ISCHR(status.st_mode))
    {
        const auto geometry = DiskGeometry(descriptor);
        if (!geometry)
        {
            ::close(descriptor);
            return std::unexpected(geometry.error());
        }
        std::tie(sector_size, sector_count) = *geometry;
    }
    else
    {
        ::close(descriptor);
        return std::unexpected(std::string(
            S_ISDIR(status.st_mode) ? "is a directory" : "not an image file or a disk"));
    }
    return std::unique_ptr<FileDevice>(
        new FileDevice(descriptor, writable, sector_size, sector_count));
}

FileDevice::FileDevice(int descriptor_, bool writable_, std::uint32_t sector_size_,
                       std::uint64_t sector_count_)
    : descriptor(descriptor_), writable(writable_), sector_size(sector_size_),
      sector_count(sector_count_)
{
}

FileDevice::~FileDevice()
{
    ::close(descriptor);
}

std::uint32_t FileDevice::SectorSize() const
{
    return sector_size;
}

std::uint64_t FileDevice::SectorCount() const
{
    return sector_count;
}

bool FileDevice::Covers(std::uint64_t first_sector, std::size_t byte_count) const
{
    return byte_count % sector_size == 0 && first_sector <= sector_count &&
           byte_count / sector_size <= sector_count - first_sector;
}

Status FileDevice::Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer)
{
    if (!Covers(first_sector, buffer.size()))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    std::size_t done = 0;
    while (done < buffer.size())
    {
        const auto offset = static_cast<off_t>(first_sector * sector_size + done);
        const ssize_t count =
            ::pread(descriptor, buffer.data() + done, buffer.size() - done, offset);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return std::unexpected(Error::Io);
        }
        done += static_cast<std::size_t>(count);
    }
    return {};
}

Status FileDevice::Write(std::uint64_t first_sector, std::span<const std::uint8_t> data)
{
    if (!writable)
    {
        return std::unexpected(Error::ReadOnly);
    }
    if (!Covers(first_sector, data.size()))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    std::size_t done = 0;
    while (done < data.size())
    {
        const auto offset = static_cast<off_t>(first_sector * sector_size + done);
        const ssize_t count = ::pwrite(descriptor, data.data() + done, data.size() - done, offset);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return std::unexpected(Error::Io);
        }
        done += static_cast<std::size_t>(count);
    }
    return {};
}

Status FileDevice::Flush()
{
    if (writable && ::fsync(descriptor) != 0)
    {
        return std::unexpected(Error::Io);
    }
    return {};
}

} // namespace FeatherDisk::Fscli
