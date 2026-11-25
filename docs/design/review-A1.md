# A1 design: pin open files for the duration of a call (R2-02, R2-03, R5-01)

Status: design. Builds on `Files::HoldFramebuffer`/`Unpin` (src/kernel/files.cpp:1355-1379, 337-345) and the pin-then-`Unpin` pattern of `ReadPointer` (files.cpp:1116-1136).

## 1. Problem and summary

`Files::FindSocket` (files.cpp:1413), `FindPipe` (1091) and `FindMaster` (1103) give back the raw object after dropping `table_lock`, without taking a reference. If another thread closes the last descriptor, `DropReferenceLocked` (218-264) runs `Pipes::CloseEnd`, `Sockets::Close` or `PseudoTerminals::CloseMaster` while the call is still using the object, or is asleep on a wait queue inside it.

The fix has two parts:

1. **Hold.** Each lookup now takes a reference on the `OpenFile` (`references++`, as `HoldFramebuffer` does). A non-copyable RAII `Files::FileHold` gives it back. The real close and the free happen at the last reference.
2. **Revoke.** When the last *descriptor* naming an `OpenFile` closes while holds remain, `Files` revokes the object. Revoking sets a flag inside the object, under the object's own lock, and wakes its wait queue. A call that would otherwise wait then returns `SyscallErrorBadDescriptor` (EBADF). Revoking does not close anything: the peer sees EOF, EPIPE, hangup or FIN only when the last reference goes, which is as soon as the held call returns.

## 2. Files, types and signatures

### src/kernel/files.h
- Move `extern Sync::Mutex table_lock;` above the new class.
- Add:
  ```cpp
  // Keeps an open file, and the pipe, socket or pair behind it, from being closed or freed for
  // the length of one call, though another thread closes its descriptor meanwhile. Once the last
  // descriptor is closed, calls that would wait on it fail with SyscallErrorBadDescriptor; the
  // file closes when the last hold ends. Lives on the stack of one call: not copyable or movable.
  class FileHold
  {
  public:
      FileHold() = default;
      ~FileHold();                                   // Release()
      FileHold(const FileHold&) = delete;
      FileHold& operator=(const FileHold&) = delete;
      // Ends the hold early; the last reference closes the file here.
      void Release() EXCLUDES(table_lock);
      Network::Sockets::Socket* Socket() const;      // null unless FindSocket filled it
  private:
      friend struct HoldAccess;                      // files.cpp
      OpenFile* file = nullptr;
  };
  ```
- Change `uint64_t FindSocket(Table*, uint64_t descriptor, FileHold* hold, bool* nonblocking = nullptr);`. `hold` must be empty on entry. On error it stays empty. Rewrite the header comment: the socket stays allocated while the hold lasts.

### src/kernel/files.cpp
- `OpenFile`: add `uint32_t descriptors GUARDED_BY(table_lock);`, the number of slots naming the file across all tables. `references` is still descriptors plus holds plus pins; update its comment.
- Set `descriptors`: `NewFile` sets it to 1; the `console` initializer sets it to 1 (the console is never revoked or freed); `Inherit` (~925) and `DuplicateLocked` (~1549) add 1.
- `struct HoldAccess { static void Take(FileHold*, OpenFile*) REQUIRES(table_lock); static OpenFile* File(const FileHold&); };`. `Take` does `++references`.
- `FileHold::Release()` calls `Unpin(file)` and then sets `file = nullptr`. `FileHold::Socket()` returns `file->socket`; that field is set before the file is published and never changes, so reading it needs no lock.
- `void RevokeLocked(OpenFile* file) REQUIRES(table_lock)` switches on the kind: `Pipe` → `Pipes::Revoke(file->pipe, file->writable)`; `Socket` → `Network::Sockets::Revoke(file->socket)`; `PseudoTerminalMaster` → `PseudoTerminals::RevokeMaster(file->pair)`; other kinds: nothing.
- `DetachSlotLocked` (266): `if (--file->descriptors == 0 && file->references > 1) RevokeLocked(file);`, then `DropReferenceLocked` as before. Every way a descriptor goes away passes through here: `Close`, `CloseAll`, an exact `Duplicate` target, the close-on-exec pass at ~881, and the failed truncate at ~1078.
- Replace `FindPipe` and `FindMaster` with `HoldPipe(Table*, uint64_t, bool write_end, FileHold*, bool* nonblocking)` and `HoldMaster(Table*, uint64_t, bool write, FileHold*, bool* nonblocking)`, returning 0 or `SyscallErrorBadDescriptor`, in the anonymous namespace. `Files::Read`/`Write` (1141-1225) then call `Pipes::Read/Write(HoldAccess::File(hold)->pipe, …)` and `ReadMaster/WriteMaster(…->pair, …)` while the hold is in scope. Delete the false comment at 1087-1088.
- `FindSocket` fills the hold through `HoldAccess::Take` under `table_lock`.

### src/kernel/pipe.h / pipe.cpp
- `Pipe`: add `bool read_revoked GUARDED_BY(lock); bool write_revoked GUARDED_BY(lock);`. `Create` zeroes them.
- Add `void Revoke(Pipe* pipe, bool write_end);`: sets the end's flag under `lock`, then outside the lock `WakeAll(write_end ? writable : readable)` and `Poll::Notify()`, the same shape as `CloseEnd`.
- `Read` loop: after the `Interrupted` check and before `WaitOn`, `if (pipe->read_revoked)` unlock and return `SyscallErrorBadDescriptor`.
- `Write` loop: same with `write_revoked`, returning `total > 0 ? total : SyscallErrorBadDescriptor`.
- Header docs gain the EBADF result.

### src/kernel/network/socket.h / socket.cpp
- `Socket`: add `bool revoked GUARDED_BY(socket_lock);`. `Create` zeroes it.
- Add `void Revoke(Socket* socket);`: under `socket_lock` sets `revoked`, `WakeAll(socket->waiters)`, `Poll::Notify()`.
- In each blocking loop, after `Interrupted` and before `WaitOn`: `ReceiveFrom` (~913) EBADF; `ReceiveStream` (~498) EBADF; `SendStream` (~449) `total != 0 ? total : EBADF`; `Accept` (~801) EBADF; `Connect` (~732) `connection->Abort(); PostTcpWorkLocked();` then EBADF, as its interrupt path.
- `Close` (583) is unchanged; it now runs only at the last reference.

### src/kernel/terminal/pseudo_terminal.h / .cpp and terminal.h / terminal.cpp
- Add `void PseudoTerminals::RevokeMaster(Pair* pair);`: reads `pair->output` under `pairs_lock`, releases it, then `Pipes::Revoke(output, false)` (the master's read end) and `pair->terminal.RevokeRoomWaits()`.
- `Terminal`: add `bool room_revoked GUARDED_BY(lock) = false;` and `void RevokeRoomWaits();` (sets the flag under `lock`, `WakeAll(room)`).
- `bool WaitForRoom();` returns false without waiting once `room_revoked` is set, checked under `lock` in the same critical section as the `WaitOn`.
- `Terminal::Reset` (448) clears `room_revoked`.
- `WriteMaster` (pseudo_terminal.cpp:333-336): `if (!pair->terminal.WaitForRoom()) return total > 0 ? total : SyscallErrorBadDescriptor;`.

### src/kernel/process.cpp
Every `FindSocket` caller (1696, 1767, 2514, 2528, 2562, 2585, 2600, 2609, 2648, 2656, 2802, 2818) declares `Files::FileHold hold;` and passes `hold.Socket()` on, so the hold lasts to the end of the syscall, including `ReceiveSocketData`/`SendSocketData` and, in `AcceptSyscall`, the call to `AddSocket`. `FramebufferHold` stays as it is.

### Tests and docs
- `tests/unit/kernel/network/socket_tests.cpp` (492-550): move to the new `FindSocket` signature.
- `docs/development.md` "Filesystem locking" item 3: descriptor operations on pipes, sockets and pty masters hold the file (`FileHold`), the revoke rule, and the lock order in I9.
- `docs/posix.md` `close` entry: what a thread blocked on the closed descriptor gets.
- `docs/reviews/2026-11-review.md`: R2-02, R2-03 and R5-01 to `fixed (pending commit)`.

## 3. Data layout

| State | Where | Guard |
| --- | --- | --- |
| hold count = `references - descriptors` | `OpenFile::references`, `OpenFile::descriptors` | `table_lock` |
| pipe end revoked | `Pipe::read_revoked/write_revoked` | `Pipe::lock` |
| socket revoked | `Socket::revoked` | `socket_lock` |
| pty master write waits revoked | `Terminal::room_revoked` | `Terminal::lock` |
| pty master read revoked | the output pipe's `read_revoked` | its `Pipe::lock` |

`OpenFile` has no revoked flag of its own: `descriptors == 0` is that state. A pipe end, a socket and a pty master each have exactly one `OpenFile`, so a flag in the object means the same as one per `OpenFile`.

## 4. Invariants

- **I1.** `references = descriptors + holds + pins`; `descriptors` counts the slots in all tables. Both change only under `table_lock`. The console is exempt.
- **I2.** Holds and pins are taken only through a slot lookup under `table_lock`, so once `descriptors == 0` no new hold appears and `references` only falls.
- **I3.** The kind-specific close (`Pipes::CloseEnd`, `Sockets::Close`, `CloseMaster`) and the `Memory::Deallocate` of the `OpenFile` run exactly once, in `DropReferenceLocked` when `references` reaches 0: the later of the last descriptor close and the last hold release.
- **I4.** `RevokeLocked` runs exactly once per `OpenFile`, in `DetachSlotLocked`, when `descriptors` reaches 0 while `references > 1`. It neither frees nor changes what the other end sees.
- **I5.** A revoke flag is set under the object's lock and the queue is woken afterwards. Every blocking loop tests the flag under that same lock immediately before `WaitOn`. `WaitOn` takes `scheduler_lock` before it releases the object lock (scheduler.cpp:571-582), so the wake-up cannot be lost.
- **I6.** A call returns EBADF only where it would otherwise wait. Order of checks in every loop: (1) the call can complete (data, EOF, EPIPE, a ready connection, connection state); (2) non-blocking or deadline passed: EAGAIN; (3) interrupted: EINTR; (4) revoked: EBADF; (5) wait. A write that has made progress returns its count.
- **I7.** A revoke flag is never cleared during the object's life. The only reset is `Terminal::Reset` when a reclaimed pair is claimed again, after `CloseMaster`.
- **I8.** The other end sees EOF, EPIPE, a broken connection or a hangup only once the last reference is gone (I3), not at revoke.
- **I9.** Lock order: `table_lock` → {`Pipe::lock` | `socket_lock` | `pairs_lock`, then `Terminal::lock`} → `scheduler_lock`. `FileHold::Release` excludes `table_lock` and runs with no spinlock and no `process_lock` held, because the last release of a master runs `CloseMaster` → `HangUp` → `process_lock`.
- **I10.** A `FileHold` is a stack object of one call: not copyable or movable, never stored in a kernel object.
- **I11.** `pair->output` is non-null while the master's `OpenFile` exists; `master_open` stays true until `CloseMaster`, which now runs only at the last reference. This removes the `Pipes::Read(nullptr)` path in R2-03.

## 5. Results per edge case

| Case | Result |
| --- | --- |
| Close of the only descriptor while read/write/recv/send/accept/connect is blocked | EBADF; EINTR if a signal is also deliverable; the normal result if the call can complete (I6). |
| Close of one of several descriptors (dup) | No effect (Linux). |
| OpenFile shared by fork/Spawn (`Inherit`), closed in the parent | No effect while the child's descriptor remains. |
| Close of the last of several descriptors | Revoke at that close. |
| `dup2` onto the blocked descriptor, or close-on-exec | Same as a close. |
| Non-blocking call racing the close (bind, listen, name, shutdown, sockopts, UDP sendto) | Completes on the still-allocated object; the close takes effect when it returns. |
| Close during `poll` | POLLNVAL for that descriptor (rescanned under `table_lock`, woken by the revoke's `Poll::Notify`). Unchanged. |
| Process exit with a blocked thread | SIGKILL gives EINTR; `WaitForThreads` runs before `CloseAll`. No use-after-free either way. |
| Blocked framebuffer events or `/dev/mouse` read | Already pinned; not woken by close (unchanged). |
| TCP socket | Close, orphan, FIN or RST and the free are deferred to the last reference. |

## 6. Acceptance tests

Unless noted, in `tests/unit/repro/process_lifetime_repro_tests.cpp` with the existing `OtherThread` hook and ASan `ReleasesAtLeast`.

Existing tests (remove `.repro`, keep the ID tag):
- **T1** R2-03 pipe read: not freed; result `== 0` (EOF: the writer's last reference closed).
- **T2** R2-03 pipe write: not freed; result `SyscallErrorBrokenPipe`.
- **T3** R2-03 pty master read: not freed; result `0`.
- **T4** R5-01: not freed; result `SyscallErrorInterrupted` (EINTR wins over EBADF, I6). `DatagramSocket` holds a `Files::FileHold` and uses `hold.Socket()`.
- **T5** R2-02, extended: not freed at `Close`, and `ReleasesAtLeast(SocketQueueBytes, [&]{ fixture.hold.Release(); })` is true.

New tests:
- **T6** [R2-03] Pipe read blocked; the hook closes only the read descriptor, no interrupt. Inside the hook `Files::PollEvents(writer)` has no `PollError` yet. Read returns EBADF; afterwards `Files::Write(writer)` returns EPIPE.
- **T7** [R2-03] Full pipe, write blocked; the hook closes the write descriptor. EBADF; the reader then drains `Capacity` bytes and gets 0.
- **T8** [R2-03] Empty pipe, write of `Capacity + AtomicSize`; the hook closes the write descriptor. Result `Capacity`.
- **T9** [R2-03] `Duplicate(reader)` first; the hook closes the original and writes 1 byte. Read returns 1.
- **T10** [R2-03] `Inherit` into a child table; the hook closes the parent's reader and writes 1 byte. Read returns 1. `CloseAll` the child after.
- **T11** [R2-03] dup; the hook closes both. EBADF.
- **T12** [R5-01] UDP `ReceiveFrom` through a hold; the hook closes the descriptor, no interrupt. EBADF; `hold.Release()` then frees `SocketQueueBytes`.
- **T13** [R5-01] TCP `Accept` blocked on a listening socket; the hook closes the descriptor. EBADF. Uses the stubs of `tcp_socket_tests.cpp`; if they cannot drive it, say so in the run log.
- **T14** [R2-03] pty master read; the hook closes only the master. Inside the hook `PseudoTerminals::InUse(n)` is true and the terminal is not hung up. EBADF. After the call and closing the slave, `InUse(n)` is false.
- **T14b** Opening `/dev/ptmx` again reclaims the same pair and a master write works (`room_revoked` cleared).
- **T15** [R2-03] Fill the slave's input queue through a non-blocking `WriteMaster`, then a blocking write; the hook closes the master. EBADF, or the bytes already taken.
- **T16** [R2-02] `FindSocket` hold, `Close`, then `Files::PollEvents(descriptor) == PollInvalid` and `FindSocket` returns EBADF with the hold left empty.
- **T17** clang-tidy thread-safety analysis passes (`tools/quality.sh check`).
- **T18** QEMU, both architectures: two checks in `src/user/libctest.cpp`, expected lines in `tools/smoke/cases/shell/programs.py`: a thread blocked in `read` on a pipe while main sleeps ~50 ms and closes the fd gets -1/EBADF; the same with `recvfrom` on a UDP socket bound to 127.0.0.1.
- **T19** Smoke `SHELL NETWORK` on both architectures.

| Invariant | Tests |
| --- | --- |
| I1 / I3 | T1-T5, T12; T5 also fails on a leak |
| I2 | T16 |
| I4 | T9, T10 (too early); T6, T11 (too late: hangs) |
| I5 | T6, T7, T12, T13, T15 |
| I6 | T4, T8, T1/T2 |
| I7 | T14b |
| I8 | T6, T14, T7 |
| I9 | T17 and review |
| I10 | the type, T17 |
| I11 | T3, T14 |

## 7. Order of work

1. Counters and `FileHold`, internal `HoldPipe`/`HoldMaster`. T1-T3, T5 pass.
2. `Pipes::Revoke`, `Sockets::Revoke`, `RevokeMaster`, the `Terminal` room revoke, `RevokeLocked`. T6-T16 pass.
3. process.cpp and `socket_tests.cpp` to `FileHold`.
4. Remove the `.repro` tags.
5. libctest checks, docs, review status, smoke.

C1 (lost wake-ups) does not block this: I5 relies only on `WaitOn` registering before it unlocks.

## 8. Decisions (coordinator)

- **Q1 EBADF on wake.** Kept, as the plan asks. Linux instead keeps the call blocked; POSIX leaves it unspecified. Documented in `docs/posix.md`.
- **Q2 precedence.** EINTR before EBADF.
- **Q3 framebuffer and `/dev/mouse` reads.** Not woken by close; left as an accepted limit.
- **Q4 `FindPseudoTerminal`.** Ioctl paths keep the raw `Pair*` (static array, not a use-after-free); out of A1, accepted limit.
- **Q5 test changes.** Tightening the repro tests' result checks and moving the fixture to `FileHold` is fine.
