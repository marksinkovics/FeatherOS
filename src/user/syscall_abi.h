#ifndef FEATHEROS_USER_SYSCALL_ABI_H
#define FEATHEROS_USER_SYSCALL_ABI_H

#include <stddef.h>
#include <stdint.h>

// Syscall numbers and result codes shared by the kernel and user programs.
// x86-64 passes the number in rax and arguments in rdi, rsi, rdx; AArch64 uses x8 and x0-x2.
namespace FeatherOS::User
{

enum class SyscallNumber : uint64_t
{
    Exit = 0,              // (status) -> does not return
    WriteCharacter = 1,    // (character) -> 0; printable ASCII or '\n'
    ReadLine = 2,          // (buffer, capacity, prompt) -> line length; foreground process only
    Spawn = 3,             // (command_line, flags, environment) -> process ID of the child
    Wait = 4,              // (process_id, status_pointer, flags) -> a WaitResult value
    ReadClock = 5,         // () -> current UTC time in nanoseconds since 1970-01-01
    ExtendHeap = 6,        // (signed increment) -> previous program break; 0 queries it
    Sleep = 7,             // (milliseconds) -> 0 after at least that long
    ReadUptime = 8,        // () -> milliseconds since the kernel's timer started
    CurrentCpu = 9,        // () -> index of the CPU running the caller
    Open = 10,             // (path, flags) -> file descriptor
    Read = 11,             // (descriptor, buffer, length) -> bytes read; 0 at end of file
    Write = 12,            // (descriptor, buffer, length) -> bytes written
    Close = 13,            // (descriptor) -> 0
    Seek = 14,             // (descriptor, offset, whence) -> the new offset, at most the size
    Remove = 15,           // (path) -> 0
    ReadDirectory = 16,    // (path, index, entry) -> 0; SyscallErrorNotFound past the end
    MakeDirectory = 17,    // (path) -> 0
    RemoveDirectory = 18,  // (path) -> 0; the directory must be empty
    ChangeDirectory = 19,  // (path) -> 0
    CurrentDirectory = 20, // (buffer, capacity) -> length of the working directory
    Shutdown = 21,         // (action) -> does not return; init and session leaders only
    Stat = 22,             // (path, status) -> 0
    StatDescriptor = 23,   // (descriptor, status) -> 0
    ProcessId = 24,        // () -> the caller's process ID
    ParentProcessId = 25,  // () -> the parent's process ID; 0 for init
    Duplicate = 26,        // (descriptor, target, flags) -> the new descriptor
    DescriptorFlags = 27,  // (descriptor, set, flags) -> the flags, after setting them if set
    Pipe = 28,             // (descriptors, flags) -> 0; stores the read end, then the write end
    ProcessGroup = 29,     // (process_id, or 0 for the caller) -> its process group
    ForegroundGroup = 30,  // (descriptor, set, group) -> the foreground group, after any set
    Kill = 31,             // (target, signal) -> 0; target as kill(2): a process, or a group
    SignalAction = 32,     // (signal, new_action, old_action) -> 0; either pointer may be null
    SignalMask = 33,       // (how, set) -> the previous blocked mask
    SignalReturn = 34,     // () -> resumes what a signal handler interrupted
    Pause = 35,            // () -> SyscallErrorInterrupted once a signal arrives
    TerminalSize = 36,     // (descriptor) -> rows in bits 32-63, columns in bits 0-31
    TerminalMode = 37,     // (descriptor, new_settings, old_settings) -> 0; either may be null
    ThreadCreate = 38,     // (entry, first, second) -> the new thread's ID
    ThreadExit = 39,       // (value) -> does not return; ends only the calling thread
    ThreadJoin = 40,       // (thread, value) -> 0 once the thread ended; stores its value
    ThreadId = 41,         // () -> the calling thread's ID; 1 for the main thread
    FutexWait = 42,        // (address, expected, milliseconds) -> 0 once woken; 0 ms waits forever
    FutexWake = 43,        // (address, count) -> how many waiters it woke
    ThreadDetach = 44,     // (thread) -> 0; its slot is freed once it ends, without a join
    Rename = 45,           // (from, to) -> 0; replaces a file at to, within one filesystem
    Socket = 46,           // (family, type, protocol) -> a socket descriptor
    Bind = 47,             // (descriptor, address) -> 0
    SendTo = 48,           // (descriptor, message, flags) -> bytes sent; flags must be 0
    ReceiveFrom = 49,      // (descriptor, message, flags) -> bytes received; flags must be 0
    SetSocketOption = 50,  // (descriptor, option, value) -> 0
    NetworkSettings = 51,  // (new_settings, old_settings, changes) -> 0; either pointer may be 0
    Connect = 52,          // (descriptor, address) -> 0, for TCP once the connection is open
    Listen = 53,           // (descriptor, backlog) -> 0
    Accept = 54,           // (descriptor, address, flags) -> the next connection's descriptor
    ShutdownSocket = 55,   // (descriptor, how) -> 0
    SocketName = 56,       // (descriptor, address, peer) -> 0; the peer's address if peer is 1
    Poll = 57,             // (descriptors, count, milliseconds) -> how many have events
    SetClock = 58,         // (nanoseconds since 1970-01-01, UTC) -> 0
    FileStatusFlags = 59,  // (descriptor, set, flags) -> the open file's Open* flags, after any set
    GetSocketOption = 60,  // (descriptor, option, value) -> 0; stores the option's value at value
    CreateSession = 61,    // () -> the new session's ID, the caller's process ID
    SessionId = 62,        // (process_id, or 0 for the caller) -> its session
    TerminalControl = 63,  // (descriptor, request, argument) -> the request's result
    Mount = 64,            // (source, target, request) -> 0; request points to a MountRequest
    Unmount = 65,          // (target) -> 0
    LinkStat = 66,         // (path, status) -> 0; a final symbolic link describes itself (lstat)
    ReadLink = 67,         // (path, buffer, capacity) -> bytes stored, without a terminator
    SymbolicLink = 68,     // (target, path) -> 0; path, not followed, becomes a link to target
    Link = 69,             // (from, to) -> 0; a second name for a file, on the same filesystem
    ResolvePath = 70,      // (path, buffer, capacity) -> length of the path without links
    Sync = 71,             // () -> 0 once every mounted filesystem has written its changes
};

// Spawn flags. A child inherits every descriptor that is not close-on-exec, and joins its
// parent's session and process group; its group takes the controlling terminal if the parent's
// group has it, but a background child's never does. SpawnSetGroup puts the child in the group in
// bits 32-63 instead, or in a new group of its own when those are 0; the group must be the
// caller's or hold one of the caller's children. SpawnSessionLeader starts a new session and
// group led by the child, and since no program runs between fork and exec here, the child takes
// the terminal at its descriptor 0 as its controlling terminal if no session has it, as if it had
// opened it. A session leader's descendants form Ctrl+C jobs, and the leader itself is never
// interrupted.
constexpr uint64_t SpawnBackground = 1U << 0;
constexpr uint64_t SpawnSessionLeader = 1U << 1;
constexpr uint64_t SpawnSetGroup = 1U << 2;
constexpr uint64_t SpawnGroupShift = 32;
// Spawn's command line starts with the program: a name without '/' is a file in /system, and
// anything else a path, relative to the caller's working directory. Any regular file may be
// tried; one that is not a program fails with SyscallErrorNotExecutable, and one whose segments
// need more pages than a process may have with SyscallErrorNoMemory.
// Shutdown actions.
constexpr uint64_t ShutdownPowerOff = 0;
constexpr uint64_t ShutdownRestart = 1;
// Wait flags: return SyscallErrorWouldBlock instead of blocking while the child runs, and also
// report a child that stopped or continued since the last report.
constexpr uint64_t WaitNoHang = 1U << 0;
constexpr uint64_t WaitStopped = 1U << 1;
constexpr uint64_t WaitContinued = 1U << 2;
// What Wait reports. Exited reaps the child and stores its exit status; Stopped stores the
// signal that stopped it, and Continued stores 0.
constexpr uint64_t WaitResultExited = 0;
constexpr uint64_t WaitResultStopped = 1;
constexpr uint64_t WaitResultContinued = 2;
// Seek origins; the offset is signed and added to the origin.
constexpr uint64_t SeekFromStart = 0;
constexpr uint64_t SeekFromCurrent = 1;
constexpr uint64_t SeekFromEnd = 2;

// Spawn's environment is "NAME=value" strings, each with its terminator, then an empty string.
// A null pointer gives the child an empty environment.
constexpr uint64_t MaximumEnvironmentSize = 1024;
// Threads per process, the main thread included. A new thread starts at entry with its first
// argument pointing to {first, second} at the top of its own stack; past the limit ThreadCreate
// fails with SyscallErrorWouldBlock.
constexpr uint64_t MaximumThreads = 16;
constexpr uint64_t MaximumEnvironmentVariables = 32;

// Paths are absolute ("/home/notes.txt") or relative to the process's working directory, which
// starts as its parent's ("/home" for the shell). "/" is the disk; "/system" is the read-only
// initramfs. Symbolic links are followed anywhere in a path, across mounts, at most
// MaximumSymbolicLinks times (SyscallErrorLoop past that), and ".." after one leads to the parent
// of its target. Open, Stat, ChangeDirectory, ReadDirectory, Spawn and Mount follow a link in the
// last component too; LinkStat, ReadLink, Remove, RemoveDirectory, MakeDirectory, Rename,
// SymbolicLink and Link (its from, as on Linux) act on the link itself. The working directory is
// kept without links.
constexpr uint64_t MaximumSymbolicLinks = 40;
constexpr uint64_t OpenRead = 1U << 0;
constexpr uint64_t OpenWrite = 1U << 1;
// With OpenWrite: create the file if it is missing, or empty it if it exists.
constexpr uint64_t OpenCreate = 1U << 2;
constexpr uint64_t OpenTruncate = 1U << 3;
// With OpenWrite: every write goes to the end of the file.
constexpr uint64_t OpenAppend = 1U << 4;
// The descriptor is not passed to children.
constexpr uint64_t OpenCloseOnExec = 1U << 5;
// Calls that would wait fail with SyscallErrorWouldBlock instead. The flag belongs to the open
// file, so descriptors that share it through Duplicate or Spawn share the flag too. FileStatusFlags
// changes it and OpenAppend, except on terminals, which always wait.
constexpr uint64_t OpenNonBlocking = 1U << 6;
// A session leader without a controlling terminal takes the first terminal it opens, unless it
// passes this flag.
constexpr uint64_t OpenNoControllingTerminal = 1U << 7;
// A symbolic link in the last component fails with SyscallErrorLoop instead of being followed.
constexpr uint64_t OpenNoFollow = 1U << 8;
// PATH_MAX: a path, its terminator included, as a program passes it and once it is resolved
// against the working directory; longer ones fail with SyscallErrorNameTooLong. NAME_MAX: one
// component, without its terminator.
constexpr uint64_t MaximumPathLength = 1024;
constexpr uint64_t MaximumNameLength = 255;
// Mount's third argument: the filesystem type's name, and MountReadOnly or 0. A filesystem that
// refuses writes, such as an ext2 volume that was not cleanly unmounted, mounts read-only anyway.
// With MountRemount only the target and MountReadOnly count (source and type may be null): the
// mount at target changes its mode.
struct MountRequest
{
    uint64_t type;
    uint64_t flags;
};
constexpr uint64_t MountReadOnly = 1U << 0;
constexpr uint64_t MountRemount = 1U << 5;
static_assert(sizeof(MountRequest) == 16, "the kernel and the C library share MountRequest");
// Descriptors 0-2 start as the console, unless the parent passed other files there. The console
// is a terminal: in its default mode a read returns one line, ending in '\n'. On a process's
// controlling terminal only the foreground group may read or change the mode: another group's
// read sends it SIGTTIN, and its change SIGTTOU, failing with SyscallErrorInterrupted (restarted
// once the group continues), or SyscallErrorInputOutput when the caller ignores or blocks
// SIGTTIN or its group is orphaned; a change that ignores or blocks SIGTTOU goes ahead. "/dev/tty"
// opens the caller's controlling terminal. A new descriptor
// gets the lowest free number below the soft descriptor limit.
constexpr uint64_t StandardInput = 0;
constexpr uint64_t StandardOutput = 1;
constexpr uint64_t StandardError = 2;
// A process's table starts at DefaultDescriptorLimit descriptors (its soft limit) and grows on
// demand; the hard limit starts at MaximumDescriptorLimit, which is also its ceiling.
constexpr uint64_t DefaultDescriptorLimit = 256;
constexpr uint64_t MaximumDescriptorLimit = 1024;
// RLIMIT_NOFILE's number, and the pair of limits laid out as struct rlimit.
constexpr uint64_t ResourceOpenFiles = 7;
struct ResourceLimits
{
    uint64_t current;
    uint64_t maximum;
};
// Duplicate flags: replace exactly the target descriptor, as dup2 does, rather than taking the
// lowest free one at or above it; and mark the new descriptor close-on-exec.
constexpr uint64_t DuplicateExact = 1U << 0;
constexpr uint64_t DuplicateCloseOnExec = 1U << 1;
// Pipe flags: both new descriptors are close-on-exec, and both ends are OpenNonBlocking.
constexpr uint64_t PipeCloseOnExec = 1U << 0;
constexpr uint64_t PipeNonBlocking = 1U << 1;
// DescriptorFlags bits.
constexpr uint64_t DescriptorCloseOnExec = 1U << 0;

// Socket families, types and protocols, with Linux's numbers. Stream sockets are TCP. Datagram
// sockets are UDP, the default protocol 0, or ICMP. ICMP datagram sockets send echo requests and
// receive the replies, like Linux's unprivileged ping sockets: the kernel sets each request's
// identifier to the socket's port and computes its checksum.
constexpr uint64_t SocketFamilyIpv4 = 2;
constexpr uint64_t SocketFamilyIpv6 = 10;
constexpr uint64_t SocketStream = 1;
constexpr uint64_t SocketDatagram = 2;
// Socket type flags, which Accept's flags take too: the descriptor is close-on-exec, and its open
// file OpenNonBlocking. A non-blocking TCP Connect fails with SyscallErrorInProgress once the
// handshake has started; the socket polls writable when it ends, and SocketOptionError tells how.
constexpr uint64_t SocketCloseOnExec = 1U << 19;
constexpr uint64_t SocketNonBlocking = 1U << 11;
constexpr uint64_t SocketProtocolIcmp = 1;
constexpr uint64_t SocketProtocolTcp = 6;
constexpr uint64_t SocketProtocolUdp = 17;
constexpr uint64_t SocketProtocolIcmpv6 = 58;
// Socket options. A receive timeout in milliseconds, after which ReceiveFrom fails with
// SyscallErrorWouldBlock; 0 waits forever. The error, which GetSocketOption only reads: a failed
// TCP connection's, or a UDP socket's from an ICMP message, which reading it clears; 0 for none.
constexpr uint64_t SocketOptionReceiveTimeout = 1;
constexpr uint64_t SocketOptionError = 2;
// SendTo flag, Linux's MSG_NOSIGNAL: a broken connection fails without raising SIGPIPE.
constexpr uint64_t SocketMessageNoSignal = 0x4000;
// SendTo and ReceiveFrom flag, Linux's MSG_DONTWAIT: this call is non-blocking.
constexpr uint64_t SocketMessageDontWait = 0x40;
// Listen keeps at most this many connections waiting for Accept; a larger backlog is cut down.
constexpr uint64_t MaximumListenBacklog = 16;
// ShutdownSocket's how: stop receiving, stop sending (TCP sends FIN), or both.
constexpr uint64_t SocketShutdownRead = 0;
constexpr uint64_t SocketShutdownWrite = 1;
constexpr uint64_t SocketShutdownBoth = 2;

// Poll takes an array of these, laid out as struct pollfd, and fills in returned_events. A
// negative descriptor is skipped; one that is not open reports PollInvalid. PollError, PollHangUp
// and PollInvalid are reported whether asked for or not. At most the soft descriptor limit of
// entries; more fail with SyscallErrorInvalidArgument.
struct PollDescriptor
{
    int32_t descriptor;
    int16_t events;
    int16_t returned_events;
};

constexpr int16_t PollReadable = 0x1;
constexpr int16_t PollWritable = 0x4;
constexpr int16_t PollError = 0x8;
constexpr int16_t PollHangUp = 0x10;
constexpr int16_t PollInvalid = 0x20;
// Poll's milliseconds: wait until a descriptor has events.
constexpr uint64_t PollForever = UINT64_MAX;

// An address of either IP version, which fills the first 4 or all 16 bytes of address, in network
// order. The port is in host order.
struct SocketAddress
{
    uint16_t family;
    uint16_t port;
    uint32_t scope;
    uint8_t address[16];
};

// The network interface's settings, which NetworkSettings stores in old_settings before applying
// the parts of new_settings that changes names. IPv4 is set by DHCP or by hand, and is 0.0.0.0
// until then; IPv6 comes from router advertisements and cannot be changed. The name servers set
// with NetworkChangeNameServers come first, then one a router advertised; port 0 means 53.
// NetworkChangeLink takes the link down, which resets the device and drops every address and
// connection off the machine, or brings it back up; IPv4 cannot be set while it is down.
constexpr uint64_t NetworkChangeIpv4 = 1U << 0;
constexpr uint64_t NetworkChangeNameServers = 1U << 1;
constexpr uint64_t NetworkChangeLink = 1U << 2;
constexpr uint64_t MaximumNameServers = 3;

struct NetworkSettings
{
    uint8_t mac[6];
    uint8_t ipv4_prefix_length;
    uint8_t ipv6_prefix_length;
    uint8_t ipv4_address[4];
    uint8_t ipv4_gateway[4];
    // All zero when missing.
    uint8_t ipv6_link_local[16];
    uint8_t ipv6_global[16];
    uint8_t ipv6_router[16];
    // 1 while the link is up, else 0.
    uint32_t link_up;
    uint32_t name_server_count;
    SocketAddress name_servers[MaximumNameServers];
};

// For SendTo and ReceiveFrom; address points to a SocketAddress. SendTo needs one, and ReceiveFrom
// stores the sender there unless it is 0.
struct SocketMessage
{
    uint64_t buffer;
    uint64_t length;
    uint64_t address;
};

// A mode's type bits, with Linux's values, as st_mode holds them; the low 12 bits are the
// permissions (07777). A directory entry's type is the same bits shifted down, as d_type.
constexpr uint32_t ModeTypeMask = 0170000;
constexpr uint32_t ModeFifo = 0010000;
constexpr uint32_t ModeCharacterDevice = 0020000;
constexpr uint32_t ModeDirectory = 0040000;
constexpr uint32_t ModeBlockDevice = 0060000;
constexpr uint32_t ModeRegular = 0100000;
constexpr uint32_t ModeSymbolicLink = 0120000;
constexpr uint32_t ModeSocket = 0140000;
constexpr uint32_t ModePermissionMask = 07777;
constexpr uint32_t EntryTypeShift = 12;

// ReadDirectory's entry. The node is the one Stat reports for the file, or 0 when the
// filesystem cannot tell; size is 0 for directories.
struct DirectoryEntry
{
    uint64_t node;
    uint64_t size;
    // The mode's type bits shifted down by EntryTypeShift (DT_DIR and the rest); 0 if unknown.
    uint32_t type;
    uint32_t reserved;
    char name[MaximumNameLength + 1];
};

static_assert(sizeof(DirectoryEntry) == 280 && offsetof(DirectoryEntry, size) == 8 &&
                  offsetof(DirectoryEntry, type) == 16 && offsetof(DirectoryEntry, name) == 24,
              "the kernel and the C library share DirectoryEntry's layout");

enum class FileType : uint32_t
{
    // A zeroed FileStatus; the kernel never reports it.
    Unknown = 0,
    Regular = 1,
    Directory = 2,
    Terminal = 3,
    // Under /dev, a pseudo-terminal's master, or an ext2 device node.
    Device = 4,
    Pipe = 5,
    Socket = 6,
    SymbolicLink = 7,
};

// Since 1970-01-01 UTC.
struct FileTime
{
    int64_t seconds;
    uint32_t nanoseconds;
    uint32_t reserved;
};

// Stat's and StatDescriptor's result, which the C library turns into struct stat. Every mount
// has a device number of its own, and node numbers are unique within it; pipes, sockets and
// terminals have device 0 and a node number of their own. Filesystems without owners or times,
// such as FAT16 and the virtual ones, report 0 for them.
struct FileStatus
{
    uint64_t device;
    uint64_t node;
    // Type and permission bits; permissions are stored, not enforced.
    uint32_t mode;
    uint32_t link_count;
    uint32_t owner;
    uint32_t group;
    uint64_t size;
    FileTime access_time;
    FileTime modify_time;
    FileTime change_time;
    // The kind of open file, which tells terminals apart from the other character devices.
    FileType type;
    // Nothing can be written there: /system, /proc, a read-only mount, or a pipe's read end.
    bool read_only;
};

static_assert(sizeof(FileTime) == 16 && sizeof(FileStatus) == 96 &&
                  offsetof(FileStatus, node) == 8 && offsetof(FileStatus, mode) == 16 &&
                  offsetof(FileStatus, link_count) == 20 && offsetof(FileStatus, owner) == 24 &&
                  offsetof(FileStatus, group) == 28 && offsetof(FileStatus, size) == 32 &&
                  offsetof(FileStatus, access_time) == 40 &&
                  offsetof(FileStatus, modify_time) == 56 &&
                  offsetof(FileStatus, change_time) == 72 && offsetof(FileStatus, type) == 88 &&
                  offsetof(FileStatus, read_only) == 92,
              "the kernel and the C library share FileStatus's layout");

// Results at or above SyscallErrorBase report failures instead of values. As on Linux, the top
// 4095 values are reserved, which no size, address, or process ID ever reaches.
constexpr uint64_t SyscallErrorBase = UINT64_MAX - 4094;
constexpr uint64_t SyscallErrorFailed = UINT64_MAX;
constexpr uint64_t SyscallErrorNotFound = UINT64_MAX - 1;
constexpr uint64_t SyscallErrorInvalidArgument = UINT64_MAX - 2;
constexpr uint64_t SyscallErrorNotForeground = UINT64_MAX - 3;
constexpr uint64_t SyscallErrorInterrupted = UINT64_MAX - 4;
constexpr uint64_t SyscallErrorTooManyOpenFiles = UINT64_MAX - 5;
// A disk file has either one writer or any number of readers, and an open file cannot be removed.
constexpr uint64_t SyscallErrorBusy = UINT64_MAX - 6;
constexpr uint64_t SyscallErrorWouldBlock = UINT64_MAX - 7;
constexpr uint64_t SyscallErrorNotPermitted = UINT64_MAX - 8;
// The descriptor is not open, or not open for that kind of access.
constexpr uint64_t SyscallErrorBadDescriptor = UINT64_MAX - 9;
// A pointer argument is not mapped, or not writable where the kernel must write.
constexpr uint64_t SyscallErrorBadAddress = UINT64_MAX - 10;
constexpr uint64_t SyscallErrorReadOnly = UINT64_MAX - 11;
constexpr uint64_t SyscallErrorExists = UINT64_MAX - 12;
constexpr uint64_t SyscallErrorNotDirectory = UINT64_MAX - 13;
constexpr uint64_t SyscallErrorNotEmpty = UINT64_MAX - 14;
// Terminals and pipes cannot seek.
constexpr uint64_t SyscallErrorNotSeekable = UINT64_MAX - 15;
constexpr uint64_t SyscallErrorNameTooLong = UINT64_MAX - 16;
constexpr uint64_t SyscallErrorIsDirectory = UINT64_MAX - 17;
// Wait names a process that is not the caller's child.
constexpr uint64_t SyscallErrorNotChild = UINT64_MAX - 18;
constexpr uint64_t SyscallErrorNoMemory = UINT64_MAX - 19;
constexpr uint64_t SyscallErrorNotExecutable = UINT64_MAX - 20;
// The result does not fit the caller's buffer.
constexpr uint64_t SyscallErrorRange = UINT64_MAX - 21;
// A write to a pipe that nobody can read any more.
constexpr uint64_t SyscallErrorBrokenPipe = UINT64_MAX - 22;
// A wait with a time limit reached it first.
constexpr uint64_t SyscallErrorTimedOut = UINT64_MAX - 23;
// A rename between two filesystems, such as the disk and /tmp.
constexpr uint64_t SyscallErrorCrossDevice = UINT64_MAX - 24;
// Socket calls on another kind of descriptor.
constexpr uint64_t SyscallErrorNotSocket = UINT64_MAX - 25;
constexpr uint64_t SyscallErrorFamilyNotSupported = UINT64_MAX - 26;
// A socket type and protocol the kernel does not provide.
constexpr uint64_t SyscallErrorProtocolNotSupported = UINT64_MAX - 27;
constexpr uint64_t SyscallErrorMessageTooLong = UINT64_MAX - 28;
constexpr uint64_t SyscallErrorAddressInUse = UINT64_MAX - 29;
// Binding to an address that is not the machine's own.
constexpr uint64_t SyscallErrorAddressNotAvailable = UINT64_MAX - 30;
constexpr uint64_t SyscallErrorNetworkUnreachable = UINT64_MAX - 31;
// The destination, or the gateway toward it, did not answer address resolution.
constexpr uint64_t SyscallErrorHostUnreachable = UINT64_MAX - 32;
constexpr uint64_t SyscallErrorNoBuffers = UINT64_MAX - 33;
constexpr uint64_t SyscallErrorDestinationRequired = UINT64_MAX - 34;
constexpr uint64_t SyscallErrorOptionNotSupported = UINT64_MAX - 35;
constexpr uint64_t SyscallErrorConnectionRefused = UINT64_MAX - 36;
constexpr uint64_t SyscallErrorConnectionReset = UINT64_MAX - 37;
constexpr uint64_t SyscallErrorNotConnected = UINT64_MAX - 38;
constexpr uint64_t SyscallErrorAlreadyConnected = UINT64_MAX - 39;
// Such as Listen on a datagram socket.
constexpr uint64_t SyscallErrorOperationNotSupported = UINT64_MAX - 40;
// A second Connect while the first is still opening the connection.
constexpr uint64_t SyscallErrorAlreadyInProgress = UINT64_MAX - 41;
// A non-blocking Connect that goes on in the background.
constexpr uint64_t SyscallErrorInProgress = UINT64_MAX - 42;
// The network link is down, so only loopback addresses can be reached.
constexpr uint64_t SyscallErrorNetworkDown = UINT64_MAX - 43;
// Terminal calls on another kind of descriptor, or tcsetpgrp on a terminal that is not the
// caller's controlling terminal.
constexpr uint64_t SyscallErrorNotTerminal = UINT64_MAX - 44;
// Opening "/dev/tty" without a controlling terminal.
constexpr uint64_t SyscallErrorNoDevice = UINT64_MAX - 45;
// Opening a pseudo-terminal's slave while it is locked or its master is closed.
constexpr uint64_t SyscallErrorInputOutput = UINT64_MAX - 46;
// Mounting a filesystem type the kernel does not have.
constexpr uint64_t SyscallErrorUnknownFilesystemType = UINT64_MAX - 47;
// More than MaximumSymbolicLinks links on the way, or OpenNoFollow on a link.
constexpr uint64_t SyscallErrorLoop = UINT64_MAX - 48;
constexpr uint64_t SyscallErrorNoSpace = UINT64_MAX - 49;
// A file, or a directory's subdirectories, past the filesystem's link limit.
constexpr uint64_t SyscallErrorTooManyLinks = UINT64_MAX - 50;
constexpr uint64_t SyscallErrorFileTooLarge = UINT64_MAX - 51;
// More disk files open in the system than the kernel tracks (as ENFILE); a process at its own
// descriptor limit gets SyscallErrorTooManyOpenFiles.
constexpr uint64_t SyscallErrorTooManyFilesInSystem = UINT64_MAX - 52;

constexpr bool IsSyscallError(uint64_t value)
{
    return value >= SyscallErrorBase;
}

// Signals, with Linux's numbers.
constexpr int SignalCount = 32;
constexpr int SignalHangUp = 1;
constexpr int SignalInterrupt = 2;
constexpr int SignalQuit = 3;
constexpr int SignalAbort = 6;
constexpr int SignalKill = 9;
constexpr int SignalUser1 = 10;
constexpr int SignalSegmentationFault = 11;
constexpr int SignalUser2 = 12;
constexpr int SignalPipe = 13;
constexpr int SignalAlarm = 14;
constexpr int SignalTerminate = 15;
constexpr int SignalChild = 17;
constexpr int SignalContinue = 18;
// SignalStop cannot be handled, blocked or ignored. Every stop signal's default action stops the
// process until SignalContinue; a stopped process still ends on SignalKill.
constexpr int SignalStop = 19;
constexpr int SignalTerminalStop = 20;
constexpr int SignalTerminalInput = 21;
constexpr int SignalTerminalOutput = 22;
constexpr int SignalWindowChange = 28;

// handler is SignalDefault, SignalIgnore, or the address of a function taking the signal number.
// It returns to restorer, which must make the SignalReturn syscall; the C library supplies one.
// mask lists further signals to block while the handler runs.
struct SignalAction
{
    uint64_t handler;
    uint64_t mask;
    uint64_t flags;
    uint64_t restorer;
};

constexpr uint64_t SignalDefault = 0;
constexpr uint64_t SignalIgnore = 1;
// SignalAction flags: go back to the default action once the handler starts, and do not block
// the signal itself while its handler runs.
constexpr uint64_t SignalResetHandler = 1U << 0;
constexpr uint64_t SignalNoDefer = 1U << 1;
// SignalMask operations.
constexpr uint64_t SignalMaskBlock = 0;
constexpr uint64_t SignalMaskUnblock = 1;
constexpr uint64_t SignalMaskSet = 2;

// A terminal's input settings. Canonical reads return one edited line at a time; otherwise a
// read returns once `minimum` bytes arrived, and `timeout`, in tenths of a second, limits the wait
// for the first byte (minimum 0) or between bytes. With TerminalSignals, Ctrl+C sends SIGINT
// instead of arriving as byte 3. On a controlling terminal only the foreground group may change
// them, and the process that did keeps them until it changes them back or exits, which restores
// the defaults.
struct TerminalSettings
{
    uint32_t flags;
    uint8_t minimum;
    uint8_t timeout;
    uint16_t reserved;
};

constexpr uint32_t TerminalCanonical = 1U << 0;
constexpr uint32_t TerminalEcho = 1U << 1;
constexpr uint32_t TerminalSignals = 1U << 2;
// Reads deliver Enter's '\r' as '\n'.
constexpr uint32_t TerminalCarriageReturnToNewline = 1U << 3;
// A read waiting for its first byte returns none once other output reached the screen and paused,
// with the cursor at the start of a row, so a line editor can draw its line again below it.
constexpr uint32_t TerminalWakeOnOutput = 1U << 4;
constexpr uint32_t TerminalDefaultFlags =
    TerminalCanonical | TerminalEcho | TerminalSignals | TerminalCarriageReturnToNewline;
constexpr uint32_t TerminalFlags = TerminalDefaultFlags | TerminalWakeOnOutput;

// Pseudo-terminals: opening "/dev/ptmx" creates a pair and returns its master, whose slave is
// "/dev/pts/N", at most MaximumPseudoTerminals pairs. Bytes written to the master are the slave's
// input, as if typed; what programs write to the slave is read from the master. The slave starts
// locked, and opening it fails with SyscallErrorInputOutput until TerminalControlUnlock, or once
// the master is closed. TerminalControl requests on a master, SyscallErrorNotTerminal on anything
// else: the pair's number, and locking the slave (argument 1) or unlocking it (argument 0).
// TerminalControlSetSize also works on any terminal: its argument holds the rows in bits 32-63 and
// the columns in bits 0-31, each at most MaximumTerminalDimension, and a change sends
// SignalWindowChange to the foreground group.
constexpr uint32_t MaximumPseudoTerminals = 16;
constexpr uint64_t TerminalControlNumber = 0;
constexpr uint64_t TerminalControlLock = 1;
constexpr uint64_t TerminalControlSetSize = 2;
constexpr uint32_t MaximumTerminalDimension = 0xFFFF;

// Exit statuses the kernel reports for programs it terminated: after a CPU exception, or because
// of signal n, as ExitStatusSignalBase + n.
constexpr uint64_t ExitStatusFault = UINT64_MAX;
constexpr uint64_t ExitStatusKernelAccessFault = UINT64_MAX - 1;
constexpr uint64_t ExitStatusSignalBase = UINT64_MAX - 0x100;

constexpr bool IsSignalExit(uint64_t status)
{
    return status > ExitStatusSignalBase && status < ExitStatusSignalBase + SignalCount;
}

} // namespace FeatherOS::User

#endif
