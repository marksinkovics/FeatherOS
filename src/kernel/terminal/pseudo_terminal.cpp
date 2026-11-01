#include "pseudo_terminal.h"

#include "kernel/sync/spinlock.h"
#include "kernel/terminal/terminal.h"
#include "user/syscall_abi.h"

namespace FeatherOS::PseudoTerminals
{

struct Pair
{
    // Set when the pair is claimed: GCC 14 leaves a constinit array of objects that point into
    // themselves zero-filled, so a constexpr constructor cannot set it.
    Terminals::Device device = {};
    Terminals::Terminal terminal;
    // Its read end is the master. Each slave descriptor holds a write end, so the master reads
    // end of file once they are all closed; until the first slave opens, the pair holds one.
    Pipes::Pipe* output = nullptr;
    uint64_t output_count = 0;
    uint32_t slaves = 0;
    bool in_use = false;
    bool master_open = false;
    bool locked = false;
    bool slave_opened = false;
    bool at_row_start = true;
};

namespace
{

constinit Sync::SpinLock pairs_lock;
constinit Pair pairs[User::MaximumPseudoTerminals];

Pair* Owner(void* context)
{
    return static_cast<Pair*>(context);
}

void Record(Pair* pair, const char* text, size_t length)
{
    if (length == 0)
    {
        return;
    }
    __atomic_add_fetch(&pair->output_count, length, __ATOMIC_RELAXED);
    const char last = text[length - 1];
    __atomic_store_n(&pair->at_row_start, last == '\n' || last == '\r', __ATOMIC_RELAXED);
}

// The terminal reaches its device only while it is not hung up, so the master is open and the
// pipe is there, or while a slave descriptor holds a write end.
void WriteWithoutWaiting(Pair* pair, const char* text, size_t length)
{
    // Echo that does not fit while the master is not reading is dropped.
    if (!User::IsSyscallError(Pipes::Write(pair->output, text, length, nullptr, true)))
    {
        Record(pair, text, length);
    }
}

void DeviceWrite(void* context, const char* text, size_t length)
{
    WriteWithoutWaiting(Owner(context), text, length);
}

uint64_t DeviceWriteOutput(void* context, const char* text, size_t length)
{
    Pair* pair = Owner(context);
    const uint64_t written =
        Pipes::Write(pair->output, text, length, Terminals::CallerInterrupted, false);
    // The master closed while the writer waited for room.
    if (written == User::SyscallErrorBrokenPipe)
    {
        return User::SyscallErrorInputOutput;
    }
    if (!User::IsSyscallError(written))
    {
        Record(pair, text, written);
    }
    return written;
}

void DeviceEraseCharacter(void* context)
{
    WriteWithoutWaiting(Owner(context), "\b \b", 3);
}

// The program on the master draws its own cursor.
void DeviceSetCursorVisible(void*, bool) {}

void DeviceResetModes(void* context)
{
    constexpr char Reset[] = "\x1b[?1049l\x1b[0m";
    WriteWithoutWaiting(Owner(context), Reset, sizeof(Reset) - 1);
}

uint64_t DeviceOutputCount(void* context)
{
    return __atomic_load_n(&Owner(context)->output_count, __ATOMIC_RELAXED);
}

void DeviceStartFreshRow(void* context)
{
    Pair* pair = Owner(context);
    if (!__atomic_load_n(&pair->at_row_start, __ATOMIC_RELAXED))
    {
        WriteWithoutWaiting(pair, "\n", 1);
    }
}

// Every program has let go of a pair once its master and slave descriptors are closed. The
// master's close hung the terminal up, so no session has it.
bool ReclaimableLocked(const Pair& pair)
{
    return pair.in_use && !pair.master_open && pair.slaves == 0;
}

// The pipe is freed with its last end, after the pair stops using it; a pair in use under the
// lock still has it.
void ReclaimLocked(Pair& pair)
{
    pair.output = nullptr;
    pair.in_use = false;
}

void AddSlaveLocked(Pair& pair)
{
    ++pair.slaves;
    if (pair.slave_opened)
    {
        Pipes::AddWriter(pair.output);
    }
    pair.slave_opened = true;
}

// No session can have the terminal of a pair that is not in use, so it can start over.
void Claim(Pair* pair, Pipes::Pipe* output)
{
    pair->device = {
        .context = pair,
        .write = DeviceWrite,
        .write_output = DeviceWriteOutput,
        .erase_character = DeviceEraseCharacter,
        .set_cursor_visible = DeviceSetCursorVisible,
        .reset_modes = DeviceResetModes,
        .output_count = DeviceOutputCount,
        .start_fresh_row = DeviceStartFreshRow,
        .poll = nullptr,
    };
    pair->terminal.Reset(&pair->device);
    pair->output = output;
    pair->output_count = 0;
    pair->slaves = 0;
    pair->in_use = true;
    pair->master_open = true;
    pair->locked = true;
    pair->slave_opened = false;
    pair->at_row_start = true;
}

} // namespace

uint64_t Create(Pair** created)
{
    Pipes::Pipe* output = Pipes::Create();
    if (output == nullptr)
    {
        return User::SyscallErrorNoMemory;
    }
    Pair* free_pair = nullptr;
    {
        Sync::SpinLockGuard guard(pairs_lock);
        for (Pair& pair : pairs)
        {
            if (!pair.in_use)
            {
                free_pair = &pair;
                Claim(free_pair, output);
                break;
            }
        }
    }
    if (free_pair == nullptr)
    {
        Pipes::CloseEnd(output, false);
        Pipes::CloseEnd(output, true);
        return User::SyscallErrorNoDevice;
    }
    *created = free_pair;
    return 0;
}

// Hung up before the master counts as closed, so a pair is never reclaimed while a session has
// its terminal.
void CloseMaster(Pair* pair)
{
    pair->terminal.HangUp();
    Pipes::Pipe* output;
    bool holds_writer = false;
    {
        Sync::SpinLockGuard guard(pairs_lock);
        pair->master_open = false;
        output = pair->output;
        if (ReclaimableLocked(*pair))
        {
            holds_writer = !pair->slave_opened;
            ReclaimLocked(*pair);
        }
    }
    Pipes::CloseEnd(output, false);
    if (holds_writer)
    {
        Pipes::CloseEnd(output, true);
    }
}

uint64_t OpenSlave(uint32_t number, Pair** opened)
{
    if (number >= User::MaximumPseudoTerminals)
    {
        return User::SyscallErrorNotFound;
    }
    Sync::SpinLockGuard guard(pairs_lock);
    Pair& pair = pairs[number];
    if (!pair.in_use)
    {
        return User::SyscallErrorNotFound;
    }
    if (pair.locked || !pair.master_open)
    {
        return User::SyscallErrorInputOutput;
    }
    AddSlaveLocked(pair);
    *opened = &pair;
    return 0;
}

Pair* AddSlave(Terminals::Terminal* terminal)
{
    Sync::SpinLockGuard guard(pairs_lock);
    for (Pair& pair : pairs)
    {
        if (pair.in_use && &pair.terminal == terminal)
        {
            AddSlaveLocked(pair);
            return &pair;
        }
    }
    return nullptr;
}

void CloseSlave(Pair* pair)
{
    Pipes::Pipe* output;
    {
        Sync::SpinLockGuard guard(pairs_lock);
        --pair->slaves;
        output = pair->output;
        if (ReclaimableLocked(*pair))
        {
            ReclaimLocked(*pair);
        }
    }
    Pipes::CloseEnd(output, true);
}

bool InUse(uint32_t number)
{
    Sync::SpinLockGuard guard(pairs_lock);
    return number < User::MaximumPseudoTerminals && pairs[number].in_use;
}

// Under the lock, so the pipe a reset writes to stays.
void ReleaseSettings(uint64_t owner)
{
    Sync::SpinLockGuard guard(pairs_lock);
    for (Pair& pair : pairs)
    {
        if (pair.in_use)
        {
            pair.terminal.ReleaseSettings(owner);
        }
    }
}

uint32_t Number(const Pair* pair)
{
    return static_cast<uint32_t>(pair - pairs);
}

Terminals::Terminal* TerminalOf(Pair* pair)
{
    return &pair->terminal;
}

void SetLocked(Pair* pair, bool locked)
{
    Sync::SpinLockGuard guard(pairs_lock);
    pair->locked = locked;
}

uint64_t ReadMaster(Pair* pair, void* buffer, uint64_t length, Pipes::InterruptCheck interrupted,
                    bool nonblocking)
{
    return Pipes::Read(pair->output, buffer, length, interrupted, nonblocking);
}

uint64_t WriteMaster(Pair* pair, const void* buffer, uint64_t length,
                     Pipes::InterruptCheck interrupted, bool nonblocking)
{
    const auto* bytes = static_cast<const char*>(buffer);
    uint64_t total = 0;
    while (total < length)
    {
        total += pair->terminal.ReceiveAvailable(bytes + total, length - total);
        if (total == length)
        {
            break;
        }
        if (nonblocking || (interrupted != nullptr && interrupted()))
        {
            if (total > 0)
            {
                return total;
            }
            return nonblocking ? User::SyscallErrorWouldBlock : User::SyscallErrorInterrupted;
        }
        pair->terminal.WaitForRoom();
    }
    return total;
}

int16_t PollMaster(Pair* pair)
{
    const int16_t output = Pipes::PollEvents(pair->output, false);
    return static_cast<int16_t>((output & (User::PollReadable | User::PollHangUp)) |
                                (pair->terminal.HasRoom() ? User::PollWritable : 0));
}

} // namespace FeatherOS::PseudoTerminals
