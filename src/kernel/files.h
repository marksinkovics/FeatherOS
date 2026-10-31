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
// Buffers are kernel memory.
namespace FeatherOS::Files
{

struct OpenFile;

enum class Kind : uint8_t
{
    None,
    Console,
    File,
    Pipe,
    Socket,
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

// Puts the console at descriptors 0, 1, and 2.
void OpenConsole(Table* table);
// Gives child every descriptor of parent that is not close-on-exec.
void Inherit(Table* child, const Table* parent);
// Reads and writes on the console go through the process instead of this table.
Kind KindOf(const Table* table, uint64_t descriptor);
uint64_t Open(Table* table, const char* path, uint64_t flags);
// A pipe read waits for data and a pipe write for space, without holding the filesystem lock,
// until interrupted says to stop, unless the open file is User::OpenNonBlocking.
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
// User::Poll* events for a descriptor other than the console, or PollInvalid if it is not open.
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
uint64_t Remove(const char* path);
uint64_t ReadDirectory(const char* path, uint64_t index, User::DirectoryEntry* entry);
uint64_t MakeDirectory(const char* path);
// 0 for a directory, otherwise SyscallErrorNotDirectory or SyscallErrorNotFound.
uint64_t CheckDirectory(const char* path);
// Fails unless the directory is empty.
uint64_t RemoveDirectory(const char* path);
// Within one filesystem; replaces a file at to, but neither a directory nor an open file.
uint64_t Rename(const char* from, const char* to);
uint64_t Stat(const char* path, User::FileStatus* status);
uint64_t StatDescriptor(Table* table, uint64_t descriptor, User::FileStatus* status);
void CloseAll(Table* table);

// A program file for Spawn. Initramfs files are used in place; others are copied into a kernel
// buffer, which lives as long as this object.
class ProgramFile
{
public:
    ProgramFile() = default;
    ~ProgramFile();
    ProgramFile(const ProgramFile&) = delete;
    ProgramFile& operator=(const ProgramFile&) = delete;

    // 0, or SyscallErrorNotFound, SyscallErrorIsDirectory, SyscallErrorNotExecutable for a device
    // or an empty file, or SyscallErrorNoMemory past User::MaximumProgramSize.
    uint64_t Read(const char* path);
    const uint8_t* Data() const;
    uint64_t Size() const;

private:
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    uint8_t* buffer = nullptr;
};
// Waits for any filesystem call in progress, then blocks all later ones, for shutdown.
void Quiesce();

} // namespace FeatherOS::Files

#endif
