#ifndef FEATHEROS_TERMINAL_TERMINAL_H
#define FEATHEROS_TERMINAL_TERMINAL_H

#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "user/process.h"
#include "user/syscall_abi.h"

#include <stddef.h>
#include <stdint.h>

// A terminal and its line discipline: the input queue, the mode programs set, the window size and
// the foreground process group. Input arrives through Receive; echo and program output leave
// through the terminal's Device. Requires the periodic timer and the scheduler for reads that
// wait.
namespace FeatherOS::Terminals
{

class Terminal;

constexpr char InterruptByte = '\x03';
constexpr char SuspendByte = '\x1a';
constexpr User::TerminalSettings DefaultSettings = {User::TerminalDefaultFlags, 1, 0, 0};
// The group of a reader the foreground group does not apply to: one whose controlling terminal
// this is not.
constexpr uint64_t AnyGroup = UINT64_MAX;

// A process as the session rules see it.
struct Caller
{
    uint64_t session;
    uint64_t group;
};

// What the terminal draws on. Each call is one unit that other output cannot interleave with.
struct Device
{
    void* context;
    // Text the terminal filtered (see Terminal::Write), plus the escape sequences it passes on.
    // Echo and other output that must not wait goes here.
    void (*write)(void* context, const char* text, size_t length);
    // Program output, which may wait for room. Returns length, or a SyscallError when the
    // writer has to stop. Null means write.
    uint64_t (*write_output)(void* context, const char* text, size_t length);
    // Blanks the character before the cursor and moves back onto it.
    void (*erase_character)(void* context);
    void (*set_cursor_visible)(void* context, bool visible);
    // Back to the main screen and normal video, after a program that changed them.
    void (*reset_modes)(void* context);
    // Grows with all output, so a reader can tell that output arrived meanwhile.
    uint64_t (*output_count)(void* context);
    // Ends a partly written row, so the next output starts at a row's beginning.
    void (*start_fresh_row)(void* context);
    // Fetches input that arrives without an interrupt; may be null.
    void (*poll)(void* context);
};

// How terminals reach the processes that use them; process.cpp provides these.
struct ProcessHooks
{
    // Ctrl+C or Ctrl+Z: sends signal to the group's job. False when nobody there takes it.
    bool (*interrupt_group)(uint64_t group, int signal);
    // A signal the calling program would take, so a blocking read gives up. Takes no locks.
    bool (*caller_interrupted)();
    // Sends signal to every member of the group.
    void (*signal_group)(uint64_t group, int signal);
    // terminal hung up while session had it: it is no longer the session's controlling terminal,
    // and the session's processes get SIGHUP.
    void (*hang_up_session)(Terminal* terminal, uint64_t session);
};

// What a background group gets when it reads its controlling terminal (signal SIGTTIN) or changes
// its mode or foreground group (SIGTTOU).
enum class BackgroundAccess : uint8_t
{
    Allowed,
    // signal goes to the caller's group, which stops it; the call starts again once continued.
    Signal,
    // SyscallErrorInputOutput.
    Fail,
};

// declined: the caller ignores or blocks signal. orphaned: nobody could continue its group.
BackgroundAccess DecideBackgroundAccess(int signal, bool declined, bool orphaned);

void SetProcessHooks(const ProcessHooks& hooks);
// The caller_interrupted hook, false without one.
bool CallerInterrupted();

class Terminal
{
public:
    constexpr explicit Terminal(const Device* device) : device(device) {}
    // Draws nowhere until Reset gives it a device.
    constexpr Terminal() = default;
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // Input as a keyboard sends it: a key's bytes are queued whole or, when the queue is full, not
    // at all. A lone Ctrl+C or Ctrl+Z becomes SIGINT or SIGTSTP for the foreground group while
    // signals are on and the group takes it; otherwise it is queued. It may run in interrupt
    // context.
    void Receive(const char* bytes, size_t length);
    // Input as a stream, such as a pty master's writes: queues the bytes that fit, each Ctrl+C or
    // Ctrl+Z treated as Receive treats a lone one. Returns how many it took.
    size_t ReceiveAvailable(const char* bytes, size_t length);
    // Returns once the input queue has room, or the task was woken, such as by a signal.
    void WaitForRoom();
    bool HasRoom();
    // Prints printable ASCII and newlines. Escape sequences, \r and \b pass through. Tab prints
    // as a space, bytes above 0x7F as '?', and other control bytes are dropped. Returns how many
    // bytes of text it took, or the device's error if it took none; SyscallErrorInputOutput once
    // hung up.
    uint64_t Write(const char* text, size_t length);
    // For output that bypassed Write, such as kernel messages: wakes readers watching for it.
    void NotifyOutput();

    // A read for the program in group: one line in canonical mode, ending in '\n', of which the
    // part that does not fit stays for the next read; otherwise as the minimum and timeout say.
    // Returns the byte count, zero once hung up, SyscallErrorNotForeground outside the foreground
    // group, or SyscallErrorInterrupted when a signal arrived before any byte.
    uint64_t Read(uint64_t group, char* buffer, size_t capacity);
    // Shows prompt, then edits a line until Enter, regardless of the mode's canonical flag.
    // buffer receives at most capacity - 1 characters and a terminator. Returns the line length,
    // the same errors as Read, or SyscallErrorInputOutput once hung up.
    uint64_t ReadLine(uint64_t group, const char* prompt, char* buffer, size_t capacity);
    // Readable for the foreground group: a line kept from a canonical read, or queued input.
    bool HasInput(uint64_t group);

    User::TerminalSettings Settings();
    // owner keeps settings other than the defaults until it sets the defaults or ReleaseSettings
    // restores them, together with the device's cursor and video modes.
    void SetSettings(const User::TerminalSettings& settings, uint64_t owner);
    void ReleaseSettings(uint64_t owner);

    void Size(uint32_t* rows, uint32_t* columns);
    // A change sends SIGWINCH to the foreground group.
    void SetSize(uint32_t rows, uint32_t columns);

    // Zero while no group has the terminal. Changes, and those of the session, must be
    // serialized by the caller, which checks the groups that exist.
    uint64_t ForegroundGroup();
    void SetForegroundGroup(uint64_t group);
    // True for AnyGroup.
    bool InForeground(uint64_t group);

    // The session this is the controlling terminal of, or zero.
    uint64_t Session();
    // Becomes session's controlling terminal with group in the foreground, unless a session
    // already has it or it hung up.
    bool Acquire(uint64_t session, uint64_t group);
    // Once session's leader has exited, no session or group has the terminal.
    void Release(uint64_t session);
    // The group a read or a mode change by caller is checked against: its own on its controlling
    // terminal, otherwise AnyGroup.
    uint64_t AccessGroup(const Caller& caller);
    // tcgetpgrp: the foreground group, or SyscallErrorNotTerminal unless this is caller's
    // controlling terminal.
    uint64_t ForegroundGroupFor(const Caller& caller);
    // Back to the state of a new terminal drawing on new_device, for one that is reused. No
    // session may have it.
    void Reset(const Device* new_device);
    // The other side is gone for good, as when a pty master closes: input is dropped, reads end,
    // writes fail, the device is no longer drawn on, and no session or group has the terminal.
    void HangUp();
    bool HungUp();

    // tcsetpgrp: on caller's controlling terminal, to a group of its session
    // (SyscallErrorNotPermitted otherwise). A caller outside the foreground group is first held
    // to DecideBackgroundAccess. Returns 0 or the error.
    uint64_t ChangeForegroundGroup(const Caller& caller, uint64_t group, bool group_in_session);

private:
    static constexpr uint32_t QueueCapacity = 256;
    static constexpr uint32_t DefaultRows = 24;
    static constexpr uint32_t DefaultColumns = 80;

    // Line editing has no use for the cursor and function keys, so their escape sequences are
    // skipped.
    enum class SequenceState : uint8_t
    {
        None,
        Escape,
        Control,
    };

    const Device* device = nullptr;
    Sync::SpinLock lock;
    Scheduler::WaitQueue readers;
    Scheduler::WaitQueue room;
    uint32_t room_waiters GUARDED_BY(lock) = 0;
    char queue[QueueCapacity] GUARDED_BY(lock) = {};
    uint32_t queue_head GUARDED_BY(lock) = 0;
    uint32_t queue_tail GUARDED_BY(lock) = 0;
    uint32_t queue_size GUARDED_BY(lock) = 0;
    User::TerminalSettings settings GUARDED_BY(lock) = DefaultSettings;
    uint64_t settings_owner GUARDED_BY(lock) = 0;
    uint32_t rows GUARDED_BY(lock) = DefaultRows;
    uint32_t columns GUARDED_BY(lock) = DefaultColumns;
    uint64_t foreground_group GUARDED_BY(lock) = 0;
    uint64_t session GUARDED_BY(lock) = 0;
    bool hung_up GUARDED_BY(lock) = false;
    // Readers waiting with TerminalWakeOnOutput.
    uint32_t output_watchers GUARDED_BY(lock) = 0;
    // The rest of a canonical line that a read did not take, with its '\n'.
    char line[User::MaximumCommandLineLength] GUARDED_BY(lock) = {};
    uint32_t line_length GUARDED_BY(lock) = 0;
    uint32_t line_offset GUARDED_BY(lock) = 0;

    void Poll();
    bool TryReadByte(char* byte);
    void WaitForInput(uint64_t deadline_tick);
    void QueueLocked(char byte) REQUIRES(lock);
    // 0, or the device's error.
    uint64_t Output(const char* text, size_t length);
    void DeliverJobSignal(char byte);
    static bool IsJobSignalByte(char byte);
    void Echo(const char* text);
    void ShowPrompt(const char* prompt);
    // Returns false when interrupted before Enter.
    bool EditLine(const char* prompt, char* buffer, size_t capacity, size_t* length);
    uint64_t ReadBytes(char* buffer, size_t capacity);
    static bool SkipSequence(SequenceState* state, char byte);
};

} // namespace FeatherOS::Terminals

#endif
