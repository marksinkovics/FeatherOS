#ifndef FEATHEROS_FILES_H
#define FEATHEROS_FILES_H

#include "kernel/filesystem/vfs.h"
#include "kernel/network/socket.h"
#include "kernel/pipe.h"
#include "user/syscall_abi.h"

#include <stdint.h>

// Per-process file descriptors. A descriptor names an open file, which dup and Spawn can share
// between descriptors and processes; it keeps the offset and closes when its last descriptor
// does. Functions return the value a syscall hands back: a result, or a User::SyscallError code.
// Buffers are kernel memory. Paths are absolute, and may hold symbolic links, "." and "..": each
// call walks its paths (VFS::ResolvePath) under the filesystem lock, following a link in the last
// component unless it says otherwise, and fails with SyscallErrorLoop past
// User::MaximumSymbolicLinks links.
namespace FeatherOS::Terminals
{
class Terminal;
} // namespace FeatherOS::Terminals

namespace FeatherOS::PseudoTerminals
{
struct Pair;
} // namespace FeatherOS::PseudoTerminals

namespace FeatherOS::Files
{

struct OpenFile;

enum class Kind : uint8_t
{
    None,
    Terminal,
    File,
    Pipe,
    Socket,
    // A pseudo-terminal's master; its slave is a Terminal.
    PseudoTerminalMaster,
};

struct Slot
{
    OpenFile* file;
    bool close_on_exec;
};

// A zero-filled table has no descriptors at all, not even the console.
struct Table
{
    Slot slots[User::MaximumDescriptors];
};

// Puts the console, whose terminal is console, at descriptors 0, 1, and 2.
void OpenConsole(Table* table, Terminals::Terminal* console);
// Gives child every descriptor of parent that is not close-on-exec.
void Inherit(Table* child, const Table* parent);
// Reads and writes on terminals go through the process instead of this table.
Kind KindOf(const Table* table, uint64_t descriptor);
// "/dev/tty" opens controlling_terminal, or fails with SyscallErrorNoDevice if it is null.
// "/dev/ptmx" creates a pseudo-terminal pair and opens its master, and "/dev/pts/N" opens a slave.
// With User::OpenNoFollow a link in the last component fails with SyscallErrorLoop.
uint64_t Open(Table* table, const char* path, uint64_t flags,
              Terminals::Terminal* controlling_terminal = nullptr);
// The terminal behind a descriptor, or SyscallErrorNotTerminal for another kind. Terminals live
// as long as the kernel.
uint64_t FindTerminal(Table* table, uint64_t descriptor, Terminals::Terminal** terminal);
// The pair behind a master descriptor, or SyscallErrorNotTerminal for another kind. It stays in
// use while the caller's descriptor holds it.
uint64_t FindPseudoTerminal(Table* table, uint64_t descriptor, PseudoTerminals::Pair** pair);
// A pipe or pty master read waits for data and a write for space, without holding the filesystem
// lock, until interrupted says to stop, unless the open file is User::OpenNonBlocking.
uint64_t Read(Table* table, uint64_t descriptor, void* buffer, uint64_t length,
              Pipes::InterruptCheck interrupted = nullptr);
uint64_t Write(Table* table, uint64_t descriptor, const void* buffer, uint64_t length,
               Pipes::InterruptCheck interrupted = nullptr);
// Puts a new pipe's read end and write end at the two lowest free descriptors.
uint64_t CreatePipe(Table* table, uint64_t flags, uint64_t* read_end, uint64_t* write_end);
// Puts a new socket at the lowest free descriptor.
uint64_t CreateSocket(Table* table, uint64_t family, uint64_t type, uint64_t protocol);
// Puts an open socket, such as a connection Accept returned, at the lowest free descriptor; on
// failure the socket is closed. flags are User::SocketCloseOnExec and User::SocketNonBlocking.
uint64_t AddSocket(Table* table, Network::Sockets::Socket* socket, uint64_t flags);
// User::Poll* events for a descriptor other than a terminal, or PollInvalid if it is not open.
int16_t PollEvents(Table* table, uint64_t descriptor);
// The socket stays open while the caller uses it, since the caller's descriptor holds it.
// SyscallErrorNotSocket for another kind of descriptor. nonblocking, if not null, receives whether
// the open file is User::OpenNonBlocking.
uint64_t FindSocket(Table* table, uint64_t descriptor, Network::Sockets::Socket** socket,
                    bool* nonblocking = nullptr);
// offset is signed, added to the origin whence names; returns the new offset.
uint64_t Seek(Table* table, uint64_t descriptor, uint64_t offset, uint64_t whence);
uint64_t Close(Table* table, uint64_t descriptor);
// With User::DuplicateExact, makes target name descriptor's file, closing what target named;
// otherwise uses the lowest free descriptor at or above target. Returns the new descriptor.
uint64_t Duplicate(Table* table, uint64_t descriptor, uint64_t target, uint64_t flags);
// Returns the descriptor's User::Descriptor* flags, after setting them to value if set is true.
uint64_t DescriptorFlags(Table* table, uint64_t descriptor, bool set, uint64_t value);
// Returns the open file's access, User::OpenAppend and User::OpenNonBlocking flags, after setting
// the last two from value if set is true; value's access flags are ignored.
uint64_t StatusFlags(Table* table, uint64_t descriptor, bool set, uint64_t value);
uint64_t CountOpen(const Table* table);
// A file or a symbolic link, not followed.
uint64_t Remove(const char* path);
uint64_t ReadDirectory(const char* path, uint64_t index, User::DirectoryEntry* entry);
// A link at path, not followed, makes it SyscallErrorExists.
uint64_t MakeDirectory(const char* path);
// 0 for a directory, otherwise SyscallErrorNotDirectory or SyscallErrorNotFound.
uint64_t CheckDirectory(const char* path);
// Checks the directory and copies its physical path, without links, into working_directory under
// the filesystem lock, so an unmount sees either the old working directory or the new one.
uint64_t ChangeDirectory(const char* path, char (&working_directory)[User::MaximumPathLength]);
// Mounts the instance of type named source at point, which must be a directory or not exist, in an
// existing directory. SyscallErrorUnknownFilesystemType for a type nothing registered,
// SyscallErrorNotFound for an unknown source, SyscallErrorBusy if the point is taken or the
// instance is mounted elsewhere, SyscallErrorInputOutput if its volume could not be reopened. A
// filesystem that refuses writes mounts read-only whatever mode asks.
uint64_t Mount(const char* source, const char* point, const char* type,
               Filesystem::MountMode mode = Filesystem::MountMode::ReadWrite);
// True if some process's working directory is point or lies inside it.
using WorkingDirectoryCheck = bool (*)(const char* point);
// SyscallErrorNotFound if point does not exist, SyscallErrorInvalidArgument if it is not a mount
// point, SyscallErrorBusy while a file on it is open, a working directory is inside it or another
// mount point lies inside it, and SyscallErrorInputOutput if its data could not be synced.
uint64_t Unmount(const char* point, WorkingDirectoryCheck in_use);
// SyscallErrorInputOutput if a mounted filesystem could not write its changes.
uint64_t SyncAll();
// What the writeback task calls: syncs the mounts that want it, SyscallErrorInputOutput if one
// failed.
uint64_t SyncPeriodic();
// Fails unless the directory is empty; a link is SyscallErrorNotDirectory.
uint64_t RemoveDirectory(const char* path);
// Within one filesystem; replaces a file or link at to, but neither a directory nor an open file.
// Links in the last components are not followed.
uint64_t Rename(const char* from, const char* to);
uint64_t Stat(const char* path, User::FileStatus* status);
// Stat that describes a link in the last component itself, as lstat does.
uint64_t LinkStat(const char* path, User::FileStatus* status);
// Copies at most capacity bytes of the target of the link at path (not followed), without a
// terminator, and returns how many. SyscallErrorInvalidArgument for something else than a link.
uint64_t ReadLink(const char* path, char* buffer, uint64_t capacity);
// Makes path (not followed) a symbolic link to target, stored as given and not checked; an empty
// target fails with SyscallErrorNotFound. SyscallErrorExists if path exists, SyscallErrorReadOnly
// on a read-only filesystem, SyscallErrorNotPermitted where the filesystem cannot hold links
// (FAT16 and the virtual filesystems, as Linux's vfat answers).
uint64_t SymbolicLink(const char* target, const char* path);
// A second name, to, for the file from; neither is followed, as on Linux. SyscallErrorCrossDevice
// across filesystems, SyscallErrorNotPermitted for a directory or where the filesystem cannot hold
// hard links (every writable one so far), and the other errors as SymbolicLink.
uint64_t Link(const char* from, const char* to);
// Copies the physical path of what path names, with its terminator, as realpath gives it.
// SyscallErrorNotFound if it does not exist, SyscallErrorRange if it does not fit; returns its
// length.
uint64_t ResolvePath(const char* path, char* buffer, uint64_t capacity);
uint64_t StatDescriptor(Table* table, uint64_t descriptor, User::FileStatus* status);
void CloseAll(Table* table);

// A program file for Spawn, open for reading while it is loaded, so it cannot be written,
// replaced or removed meanwhile.
class ProgramFile
{
public:
    ProgramFile() = default;
    ~ProgramFile();
    ProgramFile(const ProgramFile&) = delete;
    ProgramFile& operator=(const ProgramFile&) = delete;

    // 0, or SyscallErrorNotFound, SyscallErrorIsDirectory, SyscallErrorNotExecutable for a device
    // or an empty file, SyscallErrorBusy while it is open for writing, or
    // SyscallErrorTooManyOpenFiles.
    uint64_t Open(const char* path);
    uint64_t Size() const;
    // Fills the whole buffer or fails.
    bool ReadAt(uint64_t offset, void* buffer, uint64_t size);

private:
    VFS::FileHandle handle = {};
    bool open = false;
    bool tracked = false;
};
// Waits for any filesystem call in progress, blocks all later ones and detaches every mount (a
// journalled volume is closed), for shutdown; false if one did not close cleanly.
bool Quiesce();

} // namespace FeatherOS::Files

#endif
