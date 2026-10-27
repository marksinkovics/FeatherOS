#ifndef FEATHEROS_USER_SYSCALL_ABI_H
#define FEATHEROS_USER_SYSCALL_ABI_H

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
    Wait = 4,              // (process_id, status_pointer, flags) -> 0 once the child has exited
    ReadClock = 5,         // () -> current UTC time in seconds since 1970-01-01
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
    ForegroundGroup = 30,  // (set, group) -> the group that has the console, after setting it
    Kill = 31,             // (target, signal) -> 0; target as kill(2): a process, or a group
    SignalAction = 32,     // (signal, new_action, old_action) -> 0; either pointer may be null
    SignalMask = 33,       // (how, set) -> the previous blocked mask
    SignalReturn = 34,     // () -> resumes what a signal handler interrupted
    Pause = 35,            // () -> SyscallErrorInterrupted once a signal arrives
    TerminalSize = 36,     // () -> rows in bits 32-63, columns in bits 0-31
    TerminalMode = 37,     // (new_settings, old_settings) -> 0; either pointer may be null
    ThreadCreate = 38,     // (entry, first, second) -> the new thread's ID
    ThreadExit = 39,       // (value) -> does not return; ends only the calling thread
    ThreadJoin = 40,       // (thread, value) -> 0 once the thread ended; stores its value
    ThreadId = 41,         // () -> the calling thread's ID; 1 for the main thread
    FutexWait = 42,        // (address, expected, milliseconds) -> 0 once woken; 0 ms waits forever
    FutexWake = 43,        // (address, count) -> how many waiters it woke
    ThreadDetach = 44,     // (thread) -> 0; its slot is freed once it ends, without a join
    Rename = 45,           // (from, to) -> 0; replaces a file at to, within one filesystem
};

// Spawn flags. A child inherits every descriptor that is not close-on-exec and joins its
// parent's process group, and its group takes the console if the parent's group has it; a
// background child's never does. SpawnSetGroup puts the child in the group in bits 32-63 instead,
// or in a new group of its own when those are 0; the group must be the caller's or hold one of the
// caller's children. A session leader always leads a new group. A session leader's descendants form
// Ctrl+C jobs, and the leader itself is never interrupted.
constexpr uint64_t SpawnBackground = 1U << 0;
constexpr uint64_t SpawnSessionLeader = 1U << 1;
constexpr uint64_t SpawnSetGroup = 1U << 2;
constexpr uint64_t SpawnGroupShift = 32;
// Spawn's command line starts with the program: a name without '/' is a file in /system, and
// anything else a path, relative to the caller's working directory. Any regular file up to this
// size may be tried; one that is not a program fails with SyscallErrorNotExecutable, and a larger
// one with SyscallErrorNoMemory.
constexpr uint64_t MaximumProgramSize = 1024 * 1024;
// Shutdown actions.
constexpr uint64_t ShutdownPowerOff = 0;
constexpr uint64_t ShutdownRestart = 1;
// Wait flag: return SyscallErrorWouldBlock instead of blocking while the child runs.
constexpr uint64_t WaitNoHang = 1U << 0;
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
// initramfs.
constexpr uint64_t OpenRead = 1U << 0;
constexpr uint64_t OpenWrite = 1U << 1;
// With OpenWrite: create the file if it is missing, or empty it if it exists.
constexpr uint64_t OpenCreate = 1U << 2;
constexpr uint64_t OpenTruncate = 1U << 3;
// With OpenWrite: every write goes to the end of the file.
constexpr uint64_t OpenAppend = 1U << 4;
// The descriptor is not passed to children.
constexpr uint64_t OpenCloseOnExec = 1U << 5;
constexpr uint64_t MaximumPathLength = 64;
// Descriptors 0-2 start as the console, unless the parent passed other files there. The console
// is readable and writable, like a terminal: reading returns one line, ending in '\n', and only
// the foreground process may read. A new descriptor gets the lowest free number.
constexpr uint64_t StandardInput = 0;
constexpr uint64_t StandardOutput = 1;
constexpr uint64_t StandardError = 2;
constexpr uint64_t MaximumDescriptors = 32;
// Duplicate flags: replace exactly the target descriptor, as dup2 does, rather than taking the
// lowest free one at or above it; and mark the new descriptor close-on-exec.
constexpr uint64_t DuplicateExact = 1U << 0;
constexpr uint64_t DuplicateCloseOnExec = 1U << 1;
// Pipe flag: both new descriptors are close-on-exec.
constexpr uint64_t PipeCloseOnExec = 1U << 0;
// DescriptorFlags bits.
constexpr uint64_t DescriptorCloseOnExec = 1U << 0;

struct DirectoryEntry
{
    // The long name if the file has one, otherwise its 8.3 name, and a terminator.
    char name[64];
    bool directory;
    uint64_t size;
};

enum class FileType : uint32_t
{
    Regular = 1,
    Directory = 2,
    Console = 3,
    // Under /dev.
    Device = 4,
    Pipe = 5,
};

struct FileStatus
{
    uint64_t size;
    FileType type;
    // Files under /system and /proc cannot be changed.
    bool read_only;
};

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
// The console and pipes cannot seek.
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

constexpr bool IsSyscallError(uint64_t value)
{
    return value >= SyscallErrorBase;
}

// Signals, with Linux's numbers. The stop signals (19-22) are not supported, since processes
// cannot be stopped.
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

// The console's input settings. Canonical reads return one edited line at a time; otherwise a
// read returns once `minimum` bytes arrived, and `timeout`, in tenths of a second, limits the wait
// for the first byte (minimum 0) or between bytes. With TerminalSignals, Ctrl+C sends SIGINT
// instead of arriving as byte 3. Only the foreground group may change them, and the process that
// did keeps them until it changes them back or exits, which restores the defaults.
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
constexpr uint32_t TerminalDefaultFlags =
    TerminalCanonical | TerminalEcho | TerminalSignals | TerminalCarriageReturnToNewline;

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
