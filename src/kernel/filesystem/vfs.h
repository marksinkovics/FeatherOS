#ifndef FEATHEROS_FILESYSTEM_VFS_H
#define FEATHEROS_FILESYSTEM_VFS_H

#include "kernel/filesystem/filesystem.h"
#include "kernel/filesystem/mount_table.h"

#include <stdint.h>

namespace FeatherOS::Procfs
{
struct MountStatus;
} // namespace FeatherOS::Procfs

// One tree over the mounted filesystems. At boot, with their type and source: the initramfs's
// "system" directory at "/system" (initramfs, initramfs), the RAM filesystem's two roots at "/etc"
// (tmpfs, etc) and "/tmp" (tmpfs, tmp), devices at "/dev" (devfs, devfs), generated process files
// at "/proc" (procfs, procfs), and the virtio block device's first FAT16 or ext2 volume at "/"
// (its type and its partition's name, such as ext2 and vda1). The disk's other volume is
// registered the same way, such as fat16 and vda2, but not mounted. A path belongs to the mount
// with the longest mount point that is a whole-component prefix of it, so a mount hides whatever
// its parent filesystem has at that path. Each of these instances keeps its contents while
// unmounted and can be mounted at one point at a time.
//
// The calls below take physical paths, without symbolic links, ".", ".." or repeated '/', and no
// driver follows a link: a call on a link acts on the link itself. ResolvePath turns any absolute
// path into its physical one first, following links across mounts.
namespace FeatherOS::VFS
{

using FileInfo = Filesystem::FileInfo;

enum class Follow : uint8_t
{
    // A link in the last component too, as open, stat, exec and chdir do.
    All,
    // Not one in the last component, as lstat, readlink, unlink, rename, symlink and link do.
    NotLast,
};

enum class WalkResult : uint8_t
{
    Done,
    // A missing directory before "..", or a link that cannot be read or is empty.
    NotFound,
    // ".." after something that is not a directory.
    NotDirectory,
    // More than User::MaximumSymbolicLinks links on the way.
    Loop,
    // A component over User::MaximumNameLength, or a path that grew past Path::MaximumLength.
    NameTooLong,
};

// One walk's buffers, for keeping them off the kernel stack: the result in path, and the rest of
// the path still to walk and the last link's target as work space.
struct PathWalk
{
    char path[Path::MaximumLength];
    char pending[Path::MaximumLength];
    char target[Path::MaximumLength];
};

struct FileHandle
{
    Filesystem::Driver* driver;
    Filesystem::Node node;
    uint64_t size;
    uint64_t offset;
    bool open;
};

// Names one file across all its handles.
struct FileKey
{
    const Filesystem::Driver* filesystem;
    uint64_t node;

    bool operator==(const FileKey&) const = default;
};

// The filesystems that need no disk, in the order "/" lists them: /system, /etc, /dev, /proc and
// /tmp. Those already mounted somewhere are left alone.
bool MountBuiltIn();
// Mounts the block device's first partition with a FAT16 or ext2 volume at "/", replacing an
// earlier root, and registers each FAT16, ext2, ext3 and ext4 volume under its type and partition
// name; ext3 and ext4 are never the root before Part 6b. A volume mounted elsewhere is kept; one
// that refuses writes becomes a read-only root.
bool MountBlockDevice();
// Copies the initramfs's "etc" files into /etc, so they can be changed until the next boot.
bool LoadEtc();
// point is an absolute path without ".", "..", repeated or trailing '/'. UnknownType for a type
// nothing registered, NotFound for a source the type does not have, Busy if the point is taken or
// the instance is mounted elsewhere, Failed if the driver could not reopen its volume. A driver
// that is read-only or whose volume refuses writes mounts read-only whatever mode asks; callers
// mount one at a time.
MountResult Mount(const char* type, const char* source, const char* point,
                  Filesystem::MountMode mode = Filesystem::MountMode::ReadWrite);
// Busy while one of its files is open, an operation on it is in progress or another mount point
// lies inside it; otherwise detaches the driver (a journalled volume is closed, so the medium is
// clean) and removes the mount, which Failed leaves in place.
MountResult Unmount(const char* point);
// Syncs every mounted driver; false if one failed.
bool SyncAll();
// Syncs every mounted driver that wants it in the background; false if one failed. Callers hold
// the file system lock.
bool SyncPeriodic();
// Detaches every mounted driver without removing it, as shutting down does; false if one failed.
bool DetachAll();
// The type and source of the filesystem at "/", as /proc/mounts names them, and whether it is
// mounted read-only; false with nothing there.
bool DescribeRoot(const char** type, const char** source, bool* read_only);
// The index-th mount, in mount order, for /proc/mounts; false past the last one.
bool DescribeMount(uint32_t index, Procfs::MountStatus* status);

// A handle keeps its mount busy until Close.
bool Open(const char* path, FileHandle* file);
bool Read(FileHandle* file, void* buffer, uint64_t length, uint64_t* bytes_read);
// Devices accept any offset; other files at most their size.
bool Seek(FileHandle* file, uint64_t offset);
// Any file, directory or link, without following links. info->device is the mount's number.
bool Stat(const char* path, FileInfo* info);
// What Stat reports for an open file, with the handle's size.
bool StatOpen(const FileHandle& file, FileInfo* info);
void Close(FileHandle* file);
// Disk, /tmp and /etc files can be created and changed, and devices take writes. A handle caches
// the file's size, so other handles to the same file do not see its changes.
bool Create(const char* path, FileHandle* file);
// Writes at the handle's offset and advances it; *bytes_written may be short if space runs out.
bool Write(FileHandle* file, const void* buffer, uint64_t length, uint64_t* bytes_written);
bool Truncate(FileHandle* file, uint64_t size);
bool Remove(const char* path);
bool MakeDirectory(const char* path);
bool IsDirectory(const char* path);
// True where nothing can be created or changed: /system and /proc, the bare names kernel code
// uses for the initramfs, /dev apart from its devices, and read-only mounts.
bool IsReadOnly(const char* path);
// "/" and the mount points, which can never be removed.
bool IsMountPoint(const char* path);
// Disk, /tmp and /etc files are tracked while open, so they are not removed or written twice at
// once.
bool IsTracked(const FileHandle& file);
FileKey KeyOf(const FileHandle& file);
bool IsWritable(const FileHandle& file);
bool IsDevice(const FileHandle& file);
// The node Devices::Find gave for a device; false for anything else.
bool DeviceNode(const FileHandle& file, uint32_t* node);
bool RemoveDirectory(const char* path);
bool SameFilesystem(const char* left, const char* right);
// Within one filesystem; to must not exist, unless it names from in another case on the disk.
bool Rename(const char* from, const char* to);
// Lists a directory, such as "/", "/home/docs", or "/system": first the mount points directly
// inside it, then its own entries. False past the last entry.
bool ReadDirectory(const char* path, uint32_t index, Filesystem::DirectoryEntry* entry);

// Puts the physical path that names what path names in walk->path. path is absolute, or a bare
// name kernel code uses for a file in /system, which is taken as it is. Every symbolic link on the
// way is replaced by its target, ".." leads to the parent of what the path named so far (after a
// link, the target's parent), and the last component is followed as follow says. A missing
// component is left for the call that follows to report, except before "..".
WalkResult ResolvePath(const char* path, Follow follow, PathWalk* walk);
// For ResolvePath: the end of the first component of path that is a symbolic link, looking only
// past its first `from` characters, or 0 if there is none. Each mount's part of the path is asked
// of its own driver, and a mount point is never a link. path is physical; its characters are
// changed during the call and put back.
uint32_t FirstLink(char* path, uint32_t from);
// The target of the link at path; as Filesystem::Driver::ReadLink.
bool ReadLink(const char* path, char* buffer, uint32_t capacity, uint32_t* length);
bool SymbolicLink(const char* path, const char* target);
// Both on one filesystem.
bool Link(const char* from, const char* to);
bool SupportsLinks(const char* path, Filesystem::LinkKind kind);
// Why the last Create, Write, Truncate, Remove, MakeDirectory, RemoveDirectory, Rename,
// SymbolicLink or Link that failed did, as a User::SyscallError code: SyscallErrorReadOnly on a
// read-only mount, otherwise what the driver says (SyscallErrorFailed when it cannot say).
// Callers hold the file system lock across the call and this one.
uint64_t LastError();

} // namespace FeatherOS::VFS

#endif
