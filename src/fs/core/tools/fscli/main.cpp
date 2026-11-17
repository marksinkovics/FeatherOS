// SPDX-License-Identifier: MIT OR Apache-2.0
#include "disk.h"
#include "file_device.h"
#include "fs/ext2.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Fscli;

constexpr int ExitSuccess = 0;
constexpr int ExitFailure = 1;
constexpr int ExitUsage = 2;
constexpr std::size_t CopyChunk = 64 * 1024;
// PATH_MAX on Linux; ext2 stores a link target in one block of at most 4 KiB.
constexpr std::size_t MaximumLinkTarget = 4096;

constexpr const char* Usage = R"(usage: fscli <command> <image>[:<partition>] [arguments]

Reads and writes filesystems in a disk image or on a raw disk, which is opened read-only unless
the command writes.

commands:
  info <image>                        the partition table, each partition's start, size and
                                      filesystem, and the volume statistics
  ls <image>[:<partition>] [path]     list a directory (the root by default) or one file
  cat <image>[:<partition>] <path>    write a file's bytes to standard output
  cp <image>[:<partition>] <path> <destination>
                                      copy a file out; a destination directory keeps its name
  cp-in <image>[:<partition>] <host file> <path>
                                      copy a host file in, replacing a file of that name; a
                                      directory in the volume keeps the host file's name
  rm <image>[:<partition>] <path>     remove a file or symbolic link
  mkdir <image>[:<partition>] <path>  make a directory (mode 0755)
  rmdir <image>[:<partition>] <path>  remove an empty directory
  mv <image>[:<partition>] <from> <to>
                                      rename or move a file, link or directory, replacing a
                                      file or empty directory at <to>; a directory at <to>
                                      takes it under its own name
  ln [-s] <image>[:<partition>] <target> <path>
                                      give the file <target> a second name, or with -s make a
                                      symbolic link holding <target> as given; a directory at
                                      <path> takes it under <target>'s last name
  readlink <image>[:<partition>] <path>
                                      print where a symbolic link points

Partitions are numbered from 1, as `info` lists them. Without a number, fscli uses the whole
disk if it holds a filesystem, otherwise the first partition that does. `ls` shows each entry's
type and permissions, its size and its name, and where a symbolic link points. Symbolic links
are not followed. A copied-in file keeps the host file's permission bits and belongs to user
and group 0. A volume that cannot be written (an ext2 volume with errors or a journal, say)
mounts read-only, and the commands that write then fail. FAT16 holds no links.
Filesystems: fat16, ext2.

exit status: 0 on success, 1 when an operation fails, 2 for a usage error
)";

struct Target
{
    std::string path;
    std::optional<std::uint32_t> partition;
};

int Fail(const std::string& message)
{
    std::fprintf(stderr, "fscli: %s\n", message.c_str());
    return ExitFailure;
}

int UsageError(const std::string& message)
{
    std::fprintf(stderr, "fscli: %s\nTry 'fscli --help'.\n", message.c_str());
    return ExitUsage;
}

bool Exists(const std::string& path)
{
    struct stat status = {};
    return ::stat(path.c_str(), &status) == 0;
}

// A name that exists is taken whole, so an image whose name ends in ":2" still opens.
Target ParseTarget(std::string_view argument)
{
    const std::string whole(argument);
    const std::size_t colon = argument.rfind(':');
    if (Exists(whole) || colon == std::string_view::npos || colon == 0)
    {
        return {whole, std::nullopt};
    }
    const std::string_view digits = argument.substr(colon + 1);
    std::uint32_t number = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (digits.empty() || error != std::errc() || end != digits.data() + digits.size())
    {
        return {whole, std::nullopt};
    }
    return {std::string(argument.substr(0, colon)), number};
}

std::string FormatBytes(std::uint64_t bytes)
{
    constexpr const char* Units[] = {"bytes", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(Units))
    {
        value /= 1024.0;
        ++unit;
    }
    char text[32];
    if (unit == 0)
    {
        std::snprintf(text, sizeof(text), "%llu bytes", static_cast<unsigned long long>(bytes));
    }
    else
    {
        std::snprintf(text, sizeof(text), "%.1f %s", value, Units[unit]);
    }
    return text;
}

std::string Name(const DirectoryEntry& entry)
{
    return {entry.name.data(), entry.name_length};
}

char TypeMarker(NodeType type)
{
    switch (type)
    {
    case NodeType::File:
        return '-';
    case NodeType::Directory:
        return 'd';
    case NodeType::SymbolicLink:
        return 'l';
    case NodeType::Special:
        return '?';
    }
    return '?';
}

// As in ls -l, set-user-ID, set-group-ID and sticky bits take the execute places.
std::string ModeText(NodeType type, std::uint32_t mode)
{
    std::string text = "?rwxrwxrwx";
    text[0] = TypeMarker(type);
    for (std::size_t bit = 0; bit < 9; ++bit)
    {
        if ((mode & (0400U >> bit)) == 0)
        {
            text[1 + bit] = '-';
        }
    }
    const auto special = [&](std::uint32_t flag, std::size_t place, char with_execute)
    {
        if ((mode & flag) != 0)
        {
            text[place] =
                text[place] == '-' ? static_cast<char>(with_execute - 'a' + 'A') : with_execute;
        }
    };
    special(04000, 3, 's');
    special(02000, 6, 's');
    special(01000, 9, 't');
    return text;
}

// The last component, ignoring trailing slashes.
std::string BaseName(std::string_view path)
{
    while (path.size() > 1 && path.back() == '/')
    {
        path.remove_suffix(1);
    }
    const std::size_t slash = path.rfind('/');
    return std::string(
        slash == std::string_view::npos || path.size() == 1 ? path : path.substr(slash + 1));
}

std::string ErrorText(Error error)
{
    return std::string(Describe(error));
}

std::optional<std::string> NotReadable(NodeType type)
{
    switch (type)
    {
    case NodeType::File:
        return std::nullopt;
    case NodeType::Directory:
        return ErrorText(Error::IsDirectory);
    case NodeType::SymbolicLink:
        return "is a symbolic link, which fscli does not follow";
    case NodeType::Special:
        return "is a device, FIFO or socket";
    }
    return "cannot be read";
}

std::expected<std::unique_ptr<FileDevice>, int> OpenDevice(const std::string& path,
                                                           bool writable = false)
{
    auto device = FileDevice::Open(path, writable);
    if (!device)
    {
        return std::unexpected(Fail(path + ": " + device.error()));
    }
    return std::move(*device);
}

std::expected<std::unique_ptr<MountedVolume>, int>
MountTarget(BlockDevice& device, const Target& target, bool writable = false)
{
    auto mounted =
        MountedVolume::Open(device, target.partition, MountOptions{.read_only = !writable});
    if (!mounted)
    {
        return std::unexpected(Fail(target.path + ": " + mounted.error()));
    }
    if (writable && !(*mounted)->Mounted().GetCapabilities().writable)
    {
        return std::unexpected(Fail(target.path + ": the volume mounted read-only"));
    }
    return std::move(*mounted);
}

// The directory part and the last component of a path in a volume, without trailing slashes.
std::pair<std::string, std::string> SplitPath(std::string_view path)
{
    while (path.size() > 1 && path.back() == '/')
    {
        path.remove_suffix(1);
    }
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos)
    {
        return {std::string(), std::string(path)};
    }
    return {std::string(path.substr(0, slash)), std::string(path.substr(slash + 1))};
}

int Finish(MountedVolume& mounted, const Target& target)
{
    if (const Status unmounted = mounted.Unmount(); !unmounted)
    {
        return Fail(target.path + ": unmounting failed: " + ErrorText(unmounted.error()));
    }
    return ExitSuccess;
}

void PrintStatistics(Filesystem& filesystem)
{
    const Result<VolumeStatistics> statistics = filesystem.Statistics();
    if (!statistics)
    {
        std::printf("  statistics: %s\n", ErrorText(statistics.error()).c_str());
        return;
    }
    const VolumeStatistics& value = *statistics;
    std::printf("  %llu blocks of %u bytes, %llu free (%s of %s free)\n",
                static_cast<unsigned long long>(value.total_blocks), value.block_size,
                static_cast<unsigned long long>(value.free_blocks),
                FormatBytes(value.free_blocks * value.block_size).c_str(),
                FormatBytes(value.total_blocks * value.block_size).c_str());
    if (value.total_nodes != 0)
    {
        std::printf("  %llu nodes, %llu free\n", static_cast<unsigned long long>(value.total_nodes),
                    static_cast<unsigned long long>(value.free_nodes));
    }
}

// ext2, ext3 or ext4, and the feature names in dumpe2fs's order.
void PrintExtFeatures(BlockDevice& volume)
{
    const Result<Ext::ExtFeatures> features = Ext::DescribeFeatures(volume);
    if (!features)
    {
        return;
    }
    std::string line = "  format: " + std::string(features->variant) + ", features:";
    const auto add = [&line](Ext::FeatureSet set, std::uint32_t bits)
    {
        for (unsigned shift = 0; shift < 32; ++shift)
        {
            const std::uint32_t bit = 1U << shift;
            if ((bits & bit) == 0)
            {
                continue;
            }
            const std::string_view name = Ext::FeatureName(set, bit);
            line += ' ';
            if (name.empty())
            {
                std::array<char, 24> unknown{};
                std::snprintf(unknown.data(), unknown.size(), "unknown_0x%x", bit);
                line += unknown.data();
            }
            else
            {
                line += name;
            }
        }
    };
    add(Ext::FeatureSet::Compatible, features->compatible);
    add(Ext::FeatureSet::Incompatible, features->incompatible);
    add(Ext::FeatureSet::ReadOnlyCompatible, features->read_only_compatible);
    std::printf("%s\n", line.c_str());
}

// The filesystem's type, and its statistics when it mounts.
void DescribeVolume(BlockDevice& device, Partition::Range range)
{
    Partition::Volume volume(device, range);
    FilesystemType* type = Detect(volume);
    if (type == nullptr)
    {
        std::printf("unknown filesystem\n");
        return;
    }
    std::printf("%.*s\n", static_cast<int>(type->Name().size()), type->Name().data());
    if (type == &Ext::Ext2Type() || type == &Ext::Ext3Type() || type == &Ext::Ext4Type())
    {
        PrintExtFeatures(volume);
    }
    auto mounted = MountedVolume::Open(device, range, MountOptions{.read_only = true});
    if (!mounted)
    {
        std::printf("  not mounted: %s\n", mounted.error().c_str());
        return;
    }
    PrintStatistics((*mounted)->Mounted());
}

int Info(const std::vector<std::string_view>& arguments)
{
    if (arguments.size() != 1)
    {
        return UsageError("info takes one image");
    }
    const std::string path(arguments[0]);
    auto device = OpenDevice(path);
    if (!device)
    {
        return device.error();
    }
    BlockDevice& disk = **device;
    std::printf("disk: %s, %llu sectors of %u bytes (%s)\n", path.c_str(),
                static_cast<unsigned long long>(disk.SectorCount()), disk.SectorSize(),
                FormatBytes(disk.SectorCount() * disk.SectorSize()).c_str());
    const Layout layout = ReadLayout(disk);
    if (layout.whole_device)
    {
        std::printf("table: none, one filesystem on the whole disk\nfilesystem: ");
        DescribeVolume(disk, {0, disk.SectorCount()});
        return ExitSuccess;
    }
    std::printf("table: %.*s\n", static_cast<int>(Describe(layout.scheme).size()),
                Describe(layout.scheme).data());
    if (layout.partitions.empty())
    {
        std::printf("no partitions or filesystems found\n");
        return ExitSuccess;
    }
    for (std::size_t index = 0; index < layout.partitions.size(); ++index)
    {
        const Partition::Range& range = layout.partitions[index];
        std::printf("partition %zu: start %llu, %llu sectors (%s), ", index + 1,
                    static_cast<unsigned long long>(range.first_sector),
                    static_cast<unsigned long long>(range.sector_count),
                    FormatBytes(range.sector_count * disk.SectorSize()).c_str());
        DescribeVolume(disk, range);
    }
    return ExitSuccess;
}

void PrintEntry(Filesystem& filesystem, const Attributes& attributes, const std::string& name)
{
    std::string line = ModeText(attributes.type, attributes.mode);
    char size[32];
    std::snprintf(size, sizeof(size), " %10llu  ",
                  static_cast<unsigned long long>(attributes.size));
    line += size;
    line += name;
    if (attributes.type == NodeType::SymbolicLink)
    {
        std::vector<char> target(MaximumLinkTarget);
        const Result<std::size_t> count = filesystem.ReadLink(attributes.node, target);
        line += " -> ";
        line += count ? std::string(target.data(), *count) : "(" + ErrorText(count.error()) + ")";
    }
    std::printf("%s\n", line.c_str());
}

int List(const std::vector<std::string_view>& arguments)
{
    if (arguments.empty() || arguments.size() > 2)
    {
        return UsageError("ls takes an image and at most one path");
    }
    const Target target = ParseTarget(arguments[0]);
    const std::string path = arguments.size() == 2 ? std::string(arguments[1]) : "/";
    auto device = OpenDevice(target.path);
    if (!device)
    {
        return device.error();
    }
    auto mounted = MountTarget(**device, target);
    if (!mounted)
    {
        return mounted.error();
    }
    Filesystem& filesystem = (*mounted)->Mounted();
    const Result<NodeId> node = (*mounted)->Walk(path);
    const Result<Attributes> attributes =
        node ? filesystem.GetAttributes(*node) : Result<Attributes>(std::unexpected(node.error()));
    if (!attributes)
    {
        return Fail(target.path + ": " + path + ": " + ErrorText(attributes.error()));
    }
    if (attributes->type != NodeType::Directory)
    {
        PrintEntry(filesystem, *attributes, BaseName(path));
        return ExitSuccess;
    }
    std::uint64_t cookie = 0;
    DirectoryEntry entry = {};
    while (true)
    {
        const Result<bool> more = filesystem.ReadDirectory(*node, cookie, entry);
        if (!more)
        {
            return Fail(target.path + ": " + path + ": " + ErrorText(more.error()));
        }
        if (!*more)
        {
            return ExitSuccess;
        }
        const Result<Attributes> child = filesystem.GetAttributes(entry.node);
        if (!child)
        {
            return Fail(target.path + ": " + Name(entry) + ": " + ErrorText(child.error()));
        }
        PrintEntry(filesystem, *child, Name(entry));
    }
}

// Calls sink with each chunk of the file in order; sink returns false to stop with a failure.
template <typename Sink>
std::expected<void, std::string> ReadFile(MountedVolume& mounted, const std::string& path,
                                          Sink sink)
{
    Filesystem& filesystem = mounted.Mounted();
    const Result<NodeId> node = mounted.Walk(path);
    if (!node)
    {
        return std::unexpected(path + ": " + ErrorText(node.error()));
    }
    const Result<Attributes> attributes = filesystem.GetAttributes(*node);
    if (!attributes)
    {
        return std::unexpected(path + ": " + ErrorText(attributes.error()));
    }
    if (const std::optional<std::string> reason = NotReadable(attributes->type))
    {
        return std::unexpected(path + ": " + *reason);
    }
    std::vector<std::uint8_t> buffer(CopyChunk);
    std::uint64_t offset = 0;
    while (true)
    {
        const Result<std::size_t> count = filesystem.Read(*node, offset, buffer);
        if (!count)
        {
            return std::unexpected(path + ": " + ErrorText(count.error()));
        }
        if (*count == 0)
        {
            return {};
        }
        if (!sink(std::span<const std::uint8_t>(buffer.data(), *count)))
        {
            return std::unexpected(std::string());
        }
        offset += *count;
    }
}

int Cat(const std::vector<std::string_view>& arguments)
{
    if (arguments.size() != 2)
    {
        return UsageError("cat takes an image and a path");
    }
    const Target target = ParseTarget(arguments[0]);
    auto device = OpenDevice(target.path);
    if (!device)
    {
        return device.error();
    }
    auto mounted = MountTarget(**device, target);
    if (!mounted)
    {
        return mounted.error();
    }
    const auto read =
        ReadFile(**mounted, std::string(arguments[1]), [](std::span<const std::uint8_t> bytes)
                 { return std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size(); });
    if (!read)
    {
        return Fail(read.error().empty() ? "writing to standard output failed"
                                         : target.path + ": " + read.error());
    }
    return std::fflush(stdout) == 0 ? ExitSuccess : Fail("writing to standard output failed");
}

bool WriteAll(int descriptor, std::span<const std::uint8_t> bytes)
{
    while (!bytes.empty())
    {
        const ssize_t count = ::write(descriptor, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

int Copy(const std::vector<std::string_view>& arguments)
{
    if (arguments.size() != 3)
    {
        return UsageError("cp takes an image, a path in it and a host destination");
    }
    const Target target = ParseTarget(arguments[0]);
    const std::string source(arguments[1]);
    std::string destination(arguments[2]);
    struct stat status = {};
    if (::stat(destination.c_str(), &status) == 0 && S_ISDIR(status.st_mode))
    {
        destination += "/" + BaseName(source);
    }
    auto device = OpenDevice(target.path);
    if (!device)
    {
        return device.error();
    }
    auto mounted = MountTarget(**device, target);
    if (!mounted)
    {
        return mounted.error();
    }
    // Opened only once the source is known to be a file, so a failed lookup leaves nothing behind.
    const Result<NodeId> node = (*mounted)->Walk(source);
    const Result<Attributes> attributes = node ? (*mounted)->Mounted().GetAttributes(*node)
                                               : Result<Attributes>(std::unexpected(node.error()));
    if (!attributes)
    {
        return Fail(target.path + ": " + source + ": " + ErrorText(attributes.error()));
    }
    if (const std::optional<std::string> reason = NotReadable(attributes->type))
    {
        return Fail(target.path + ": " + source + ": " + *reason);
    }
    const int output = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (output < 0)
    {
        return Fail(destination + ": " + std::strerror(errno));
    }
    int write_error = 0;
    const auto read = ReadFile(**mounted, source,
                               [output, &write_error](std::span<const std::uint8_t> bytes)
                               {
                                   if (WriteAll(output, bytes))
                                   {
                                       return true;
                                   }
                                   write_error = errno;
                                   return false;
                               });
    const bool closed = ::close(output) == 0;
    if (!read || !closed)
    {
        ::unlink(destination.c_str());
        if (read)
        {
            return Fail(destination + ": " + std::strerror(errno));
        }
        return Fail(read.error().empty() ? destination + ": " + std::strerror(write_error)
                                         : target.path + ": " + read.error());
    }
    return ExitSuccess;
}

// Writes the host file to node from its start, in chunks.
std::expected<void, std::string> CopyInto(Filesystem& filesystem, NodeId node, int input)
{
    std::vector<std::uint8_t> buffer(CopyChunk);
    std::uint64_t offset = 0;
    while (true)
    {
        const ssize_t count = ::read(input, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count < 0)
        {
            return std::unexpected(std::string(std::strerror(errno)));
        }
        if (count == 0)
        {
            return {};
        }
        const std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(count));
        const Result<std::size_t> written = filesystem.Write(node, offset, chunk);
        if (!written)
        {
            return std::unexpected(ErrorText(written.error()));
        }
        if (*written < chunk.size())
        {
            return std::unexpected(ErrorText(Error::NoSpace));
        }
        offset += *written;
    }
}

int CopyIn(const std::vector<std::string_view>& arguments)
{
    if (arguments.size() != 3)
    {
        return UsageError("cp-in takes an image, a host file and a path in the image");
    }
    const Target target = ParseTarget(arguments[0]);
    const std::string source(arguments[1]);
    const std::string destination(arguments[2]);
    const int input = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (input < 0)
    {
        return Fail(source + ": " + std::strerror(errno));
    }
    struct stat status = {};
    if (::fstat(input, &status) != 0 || !S_ISREG(status.st_mode))
    {
        ::close(input);
        return Fail(source + ": not a regular file");
    }
    const auto result = [&]() -> int
    {
        auto device = OpenDevice(target.path, true);
        if (!device)
        {
            return device.error();
        }
        auto mounted = MountTarget(**device, target, true);
        if (!mounted)
        {
            return mounted.error();
        }
        Filesystem& filesystem = (*mounted)->Mounted();
        const auto fail = [&](const std::string& path, const std::string& reason)
        { return Fail(target.path + ": " + path + ": " + reason); };

        // Into a directory under the host file's name, or at the path itself.
        auto [directory_path, name] = SplitPath(destination);
        if (const Result<NodeId> named = (*mounted)->Walk(destination); named)
        {
            const Result<Attributes> attributes = filesystem.GetAttributes(*named);
            if (attributes && attributes->type == NodeType::Directory)
            {
                directory_path = destination;
                name = BaseName(source);
            }
        }
        if (name.empty() || name == "/" || name == "." || name == "..")
        {
            return fail(destination, "needs a file name");
        }
        const std::string path = directory_path + "/" + name;
        const Result<NodeId> directory = (*mounted)->Walk(directory_path);
        if (!directory)
        {
            return fail(directory_path.empty() ? "/" : directory_path,
                        ErrorText(directory.error()));
        }
        bool created = false;
        Result<NodeId> node = filesystem.Lookup(*directory, name);
        if (node)
        {
            const Result<Attributes> attributes = filesystem.GetAttributes(*node);
            if (!attributes)
            {
                return fail(path, ErrorText(attributes.error()));
            }
            if (const std::optional<std::string> reason = NotReadable(attributes->type))
            {
                return fail(path, *reason);
            }
            if (const Status emptied = filesystem.Truncate(*node, 0); !emptied)
            {
                return fail(path, ErrorText(emptied.error()));
            }
        }
        else if (node.error() == Error::NotFound)
        {
            node = filesystem.Create(*directory, name, status.st_mode & 07777);
            if (!node)
            {
                return fail(path, ErrorText(node.error()));
            }
            created = true;
        }
        else
        {
            return fail(path, ErrorText(node.error()));
        }
        if (const auto copied = CopyInto(filesystem, *node, input); !copied)
        {
            // A file this command made goes again, so a failed copy leaves nothing behind.
            if (created)
            {
                static_cast<void>(filesystem.Remove(*directory, name));
            }
            return fail(path, copied.error());
        }
        return Finish(**mounted, target);
    }();
    ::close(input);
    return result;
}

int Remove(const std::vector<std::string_view>& arguments)
{
    if (arguments.size() != 2)
    {
        return UsageError("rm takes an image and a path");
    }
    const Target target = ParseTarget(arguments[0]);
    const std::string path(arguments[1]);
    auto device = OpenDevice(target.path, true);
    if (!device)
    {
        return device.error();
    }
    auto mounted = MountTarget(**device, target, true);
    if (!mounted)
    {
        return mounted.error();
    }
    Filesystem& filesystem = (*mounted)->Mounted();
    const auto [directory_path, name] = SplitPath(path);
    if (name.empty() || name == "/" || name == "." || name == "..")
    {
        return Fail(target.path + ": " + path + ": " + ErrorText(Error::IsDirectory));
    }
    const Result<NodeId> directory = (*mounted)->Walk(directory_path);
    const Result<NodeId> node =
        directory ? filesystem.Lookup(*directory, name) : Result<NodeId>(directory);
    const Result<Attributes> attributes =
        node ? filesystem.GetAttributes(*node) : Result<Attributes>(std::unexpected(node.error()));
    if (!attributes)
    {
        return Fail(target.path + ": " + path + ": " + ErrorText(attributes.error()));
    }
    if (attributes->type == NodeType::Directory)
    {
        return Fail(target.path + ": " + path + ": " + ErrorText(Error::IsDirectory));
    }
    if (const Status removed = filesystem.Remove(*directory, name); !removed)
    {
        return Fail(target.path + ": " + path + ": " + ErrorText(removed.error()));
    }
    return Finish(**mounted, target);
}

// A volume opened for writing, for the commands that change it.
struct WritableVolume
{
    std::unique_ptr<FileDevice> device;
    std::unique_ptr<MountedVolume> mounted;
};

std::expected<WritableVolume, int> OpenWritable(const Target& target)
{
    auto device = OpenDevice(target.path, true);
    if (!device)
    {
        return std::unexpected(device.error());
    }
    auto mounted = MountTarget(**device, target, true);
    if (!mounted)
    {
        return std::unexpected(mounted.error());
    }
    return WritableVolume{.device = std::move(*device), .mounted = std::move(*mounted)};
}

// A name in a directory of the volume.
struct Place
{
    NodeId directory;
    std::string name;
};

// The directory holding path's last component, which must be a name; the error is a message.
std::expected<Place, std::string> PlaceOf(MountedVolume& mounted, std::string_view path)
{
    const auto [directory_path, name] = SplitPath(path);
    if (name.empty() || name == "/" || name == "." || name == "..")
    {
        return std::unexpected(std::string(path) + ": needs a name");
    }
    const Result<NodeId> directory = mounted.Walk(directory_path);
    if (!directory)
    {
        return std::unexpected((directory_path.empty() ? "/" : directory_path) + ": " +
                               ErrorText(directory.error()));
    }
    return Place{.directory = *directory, .name = name};
}

// Inside path under name when path is a directory, as mv and ln take it, otherwise path itself.
std::expected<Place, std::string> PlaceAt(MountedVolume& mounted, std::string_view path,
                                          const std::string& name)
{
    if (const Result<NodeId> named = mounted.Walk(path); named)
    {
        const Result<Attributes> attributes = mounted.Mounted().GetAttributes(*named);
        if (attributes && attributes->type == NodeType::Directory)
        {
            return Place{.directory = *named, .name = name};
        }
    }
    return PlaceOf(mounted, path);
}

int ChangeDirectory(const std::vector<std::string_view>& arguments, bool make)
{
    if (arguments.size() != 2)
    {
        return UsageError(make ? "mkdir takes an image and a path"
                               : "rmdir takes an image and a path");
    }
    const Target target = ParseTarget(arguments[0]);
    const std::string path(arguments[1]);
    auto volume = OpenWritable(target);
    if (!volume)
    {
        return volume.error();
    }
    MountedVolume& mounted = *volume->mounted;
    const auto place = PlaceOf(mounted, path);
    if (!place)
    {
        return Fail(target.path + ": " + place.error());
    }
    Filesystem& filesystem = mounted.Mounted();
    const Status changed =
        make
            ? filesystem.MakeDirectory(place->directory, place->name, 0755).transform([](NodeId) {})
            : filesystem.RemoveDirectory(place->directory, place->name);
    if (!changed)
    {
        return Fail(target.path + ": " + path + ": " + ErrorText(changed.error()));
    }
    return Finish(mounted, target);
}

int Move(const std::vector<std::string_view>& arguments)
{
    if (arguments.size() != 3)
    {
        return UsageError("mv takes an image and two paths in it");
    }
    const Target target = ParseTarget(arguments[0]);
    const std::string from(arguments[1]);
    const std::string to(arguments[2]);
    auto volume = OpenWritable(target);
    if (!volume)
    {
        return volume.error();
    }
    MountedVolume& mounted = *volume->mounted;
    const auto source = PlaceOf(mounted, from);
    if (!source)
    {
        return Fail(target.path + ": " + source.error());
    }
    const auto destination = PlaceAt(mounted, to, BaseName(from));
    if (!destination)
    {
        return Fail(target.path + ": " + destination.error());
    }
    if (const Status renamed = mounted.Mounted().Rename(source->directory, source->name,
                                                        destination->directory, destination->name);
        !renamed)
    {
        return Fail(target.path + ": " + from + ": " + ErrorText(renamed.error()));
    }
    return Finish(mounted, target);
}

int MakeLink(std::vector<std::string_view> arguments)
{
    const bool symbolic = !arguments.empty() && arguments.front() == "-s";
    if (symbolic)
    {
        arguments.erase(arguments.begin());
    }
    if (arguments.size() != 3)
    {
        return UsageError("ln takes [-s], an image, a target and a path");
    }
    const Target target = ParseTarget(arguments[0]);
    const std::string link_target(arguments[1]);
    const std::string path(arguments[2]);
    auto volume = OpenWritable(target);
    if (!volume)
    {
        return volume.error();
    }
    MountedVolume& mounted = *volume->mounted;
    Filesystem& filesystem = mounted.Mounted();
    const auto place = PlaceAt(mounted, path, BaseName(link_target));
    if (!place)
    {
        return Fail(target.path + ": " + place.error());
    }
    Status made;
    if (symbolic)
    {
        made = filesystem.SymbolicLink(place->directory, place->name, link_target)
                   .transform([](NodeId) {});
    }
    else
    {
        const Result<NodeId> source = mounted.Walk(link_target);
        if (!source)
        {
            return Fail(target.path + ": " + link_target + ": " + ErrorText(source.error()));
        }
        made = filesystem.Link(*source, place->directory, place->name);
    }
    if (!made)
    {
        return Fail(target.path + ": " + path + ": " + ErrorText(made.error()));
    }
    return Finish(mounted, target);
}

int ReadLink(const std::vector<std::string_view>& arguments)
{
    if (arguments.size() != 2)
    {
        return UsageError("readlink takes an image and a path");
    }
    const Target target = ParseTarget(arguments[0]);
    const std::string path(arguments[1]);
    auto device = OpenDevice(target.path);
    if (!device)
    {
        return device.error();
    }
    auto mounted = MountTarget(**device, target);
    if (!mounted)
    {
        return mounted.error();
    }
    Filesystem& filesystem = (*mounted)->Mounted();
    const Result<NodeId> node = (*mounted)->Walk(path);
    const Result<Attributes> attributes =
        node ? filesystem.GetAttributes(*node) : Result<Attributes>(std::unexpected(node.error()));
    if (!attributes)
    {
        return Fail(target.path + ": " + path + ": " + ErrorText(attributes.error()));
    }
    if (attributes->type != NodeType::SymbolicLink)
    {
        return Fail(target.path + ": " + path + ": not a symbolic link");
    }
    std::vector<char> buffer(MaximumLinkTarget);
    const Result<std::size_t> count = filesystem.ReadLink(*node, buffer);
    if (!count)
    {
        return Fail(target.path + ": " + path + ": " + ErrorText(count.error()));
    }
    std::printf("%.*s\n", static_cast<int>(*count), buffer.data());
    return std::fflush(stdout) == 0 ? ExitSuccess : Fail("writing to standard output failed");
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string_view> arguments(argv + 1, argv + argc);
    if (arguments.empty())
    {
        std::fputs(Usage, stderr);
        return ExitUsage;
    }
    const std::string_view command = arguments.front();
    arguments.erase(arguments.begin());
    if (command == "--help" || command == "-h" || command == "help")
    {
        std::fputs(Usage, stdout);
        return ExitSuccess;
    }
    if (command == "info")
    {
        return Info(arguments);
    }
    if (command == "ls")
    {
        return List(arguments);
    }
    if (command == "cat")
    {
        return Cat(arguments);
    }
    if (command == "cp")
    {
        return Copy(arguments);
    }
    if (command == "cp-in")
    {
        return CopyIn(arguments);
    }
    if (command == "rm")
    {
        return Remove(arguments);
    }
    if (command == "mkdir" || command == "rmdir")
    {
        return ChangeDirectory(arguments, command == "mkdir");
    }
    if (command == "mv")
    {
        return Move(arguments);
    }
    if (command == "ln")
    {
        return MakeLink(arguments);
    }
    if (command == "readlink")
    {
        return ReadLink(arguments);
    }
    return UsageError("unknown command '" + std::string(command) + "'");
}
