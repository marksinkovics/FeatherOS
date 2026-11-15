// SPDX-License-Identifier: MIT OR Apache-2.0
#include "disk.h"

#include "fs/ext2.h"
#include "fs/fat.h"

#include <array>
#include <cstdio>

namespace FeatherDisk::Fscli
{
namespace
{

// Partition::Find stores at most this many.
constexpr std::size_t MaximumPartitions = 128;

} // namespace

std::span<FilesystemType* const> FilesystemTypes()
{
    static const std::array<FilesystemType*, 2> types = {&Fat::Fat16Type(), &Ext::Ext2Type()};
    return types;
}

FilesystemType* Detect(BlockDevice& volume)
{
    for (FilesystemType* type : FilesystemTypes())
    {
        if (type->Probe(volume))
        {
            return type;
        }
    }
    return nullptr;
}

std::string_view Describe(Error error)
{
    switch (error)
    {
    case Error::Io:
        return "input/output error";
    case Error::Corrupt:
        return "the filesystem is damaged";
    case Error::NotFound:
        return "not found";
    case Error::Exists:
        return "already exists";
    case Error::NotDirectory:
        return "not a directory";
    case Error::IsDirectory:
        return "is a directory";
    case Error::NotEmpty:
        return "directory not empty";
    case Error::NoSpace:
        return "no space left on the volume";
    case Error::ReadOnly:
        return "read-only";
    case Error::InvalidArgument:
        return "invalid argument";
    case Error::NameTooLong:
        return "name too long";
    case Error::FileTooLarge:
        return "file too large";
    case Error::TooManyLinks:
        return "too many links";
    case Error::Busy:
        return "busy";
    case Error::OutOfMemory:
        return "out of memory";
    case Error::Unsupported:
        return "not supported by this filesystem";
    }
    return "unknown error";
}

std::string_view Describe(Partition::Scheme scheme)
{
    switch (scheme)
    {
    case Partition::Scheme::None:
        return "none";
    case Partition::Scheme::Mbr:
        return "mbr";
    case Partition::Scheme::Gpt:
        return "gpt";
    }
    return "unknown";
}

Layout ReadLayout(BlockDevice& device)
{
    Layout layout = {};
    Partition::Volume whole(device, {0, device.SectorCount()});
    if (Detect(whole) != nullptr)
    {
        layout.whole_device = true;
        return layout;
    }
    layout.scheme = Partition::Identify(device);
    layout.partitions.resize(MaximumPartitions);
    layout.partitions.resize(Partition::Find(device, layout.partitions));
    return layout;
}

std::expected<std::unique_ptr<MountedVolume>, std::string>
MountedVolume::Open(BlockDevice& device, std::optional<std::uint32_t> partition,
                    MountOptions options)
{
    const Layout layout = ReadLayout(device);
    if (layout.whole_device)
    {
        if (partition)
        {
            return std::unexpected(
                std::string("has no partition table; leave out the partition number"));
        }
        return Open(device, Partition::Range{0, device.SectorCount()}, options);
    }
    if (partition)
    {
        if (*partition == 0 || *partition > layout.partitions.size())
        {
            return std::unexpected("has no partition " + std::to_string(*partition));
        }
        return Open(device, layout.partitions[*partition - 1], options);
    }
    for (const Partition::Range& range : layout.partitions)
    {
        Partition::Volume candidate(device, range);
        if (Detect(candidate) != nullptr)
        {
            return Open(device, range, options);
        }
    }
    return std::unexpected(std::string("no filesystem found"));
}

std::expected<std::unique_ptr<MountedVolume>, std::string>
MountedVolume::Open(BlockDevice& device, Partition::Range range, MountOptions options)
{
    std::unique_ptr<MountedVolume> mounted(new MountedVolume(device, range));
    mounted->type = Detect(mounted->volume);
    if (mounted->type == nullptr)
    {
        return std::unexpected(std::string("no filesystem found"));
    }
    const Result<Filesystem*> filesystem = mounted->type->Mount(mounted->volume, options);
    if (!filesystem)
    {
        return std::unexpected("cannot mount " + std::string(mounted->type->Name()) + ": " +
                               std::string(Describe(filesystem.error())));
    }
    mounted->filesystem = *filesystem;
    return mounted;
}

MountedVolume::MountedVolume(BlockDevice& device, Partition::Range range) : volume(device, range) {}

MountedVolume::~MountedVolume()
{
    if (filesystem == nullptr)
    {
        return;
    }
    const Status unmounted = type->Unmount(*filesystem);
    if (!unmounted)
    {
        std::fprintf(stderr, "fscli: unmounting failed: %.*s\n",
                     static_cast<int>(Describe(unmounted.error()).size()),
                     Describe(unmounted.error()).data());
    }
}

Filesystem& MountedVolume::Mounted()
{
    return *filesystem;
}

FilesystemType& MountedVolume::Type()
{
    return *type;
}

Result<NodeId> MountedVolume::Walk(std::string_view path)
{
    NodeId node = filesystem->Root();
    while (!path.empty())
    {
        const std::size_t slash = path.find('/');
        const std::string_view name = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
        if (name.empty())
        {
            continue;
        }
        const Result<NodeId> next = filesystem->Lookup(node, name);
        if (!next)
        {
            return next;
        }
        node = *next;
    }
    return node;
}

} // namespace FeatherDisk::Fscli
