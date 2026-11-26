# C1 design: no lost wake-ups between a waiter's check and its block (R2-07, R3-04, R3-06)

Status: design. Builds on `Scheduler::WakeTask` (src/kernel/scheduler.cpp:598-611), `WaitOn` (560-585), `SleepUntil` (613-629), `Wait` (457-488), and the stubs in `tests/unit/support/scheduler_stubs.{h,cpp}`.

## 1. Problem and decision

Every blocking call checks its condition (`CallerInterrupted()`, data available, `woken`), then calls `WaitOn` or `SleepUntil`, which take `scheduler_lock` and set the task Blocked or Sleeping. A signal sender (`SendSignalLocked`, process.cpp:619-656) runs `Signals::Send` and then `WakeTask` under `process_lock`, not under the waiter's lock. `WakeTask` readies only a Sleeping or Blocked task, so a target still Running, or Ready after a preemption, loses the wake-up. On SMP the waiter then sleeps with the signal pending, forever (pipe, futex, terminal, poll, sockets) or until its deadline (Sleep, Pause).

**Fix: a per-task wake token in the scheduler; no caller changes signature.**
- `Task` gets `bool wake_pending GUARDED_BY(scheduler_lock)`.
- `WakeTask` on a Sleeping or Blocked task: `MakeReady`, as now. On a Running or Ready task: `wake_pending = true` (Ready matters: a terminal read runs with interrupts on and can be preempted between `CallerInterrupted` and `WaitForInput`).
- The task's next block point consumes the token under `scheduler_lock`: `WaitOn` and `SleepUntil` clear it and return without blocking; `Wait` clears it when it is about to block (its `interrupted()` callback has just run under the same lock hold).

This is Linux's `set_current_state`/`try_to_wake_up` ordering moved into the block primitive: wake and block are serialized by `scheduler_lock`, so one always sees the other.

Rejected: checking `interrupted()` inside `WaitOn` under the lock. About 15 call sites would pass a callback, including waiters whose check sits outside any object lock (`Terminal::WaitForInput`, `Poll::WaitForChange`, `Console::WaitForEvents`, `SleepUntil` in `SleepSyscall`/`PauseSyscall`); it covers only signals; one caller that forgets reopens the hole.

**Stale tokens.** A token can be set when no wait follows (a signal to a task in user mode, an ignored SIGCHLD, a futex `Wake` racing a timeout). Each such `WakeTask` causes at most one spurious early return at the next block point, which the `WakeTask` contract already allows; every waiter loops (section 5). Consuming clears it, so nothing spins.

**Lock order:** unchanged. The token is touched only under `scheduler_lock`; `WakeTask` takes nothing else; `WaitOn` still takes `scheduler_lock` before releasing the caller's lock (A1's I5).

**Memory order:** from the lock. Each waker publishes its condition before `WakeTask` takes `scheduler_lock` (`Signals::Send` is a seq_cst `fetch_or`, futex sets `woken`, a child report is set under `process_lock`). If `WakeTask`'s section comes first, the waiter finds the token and its recheck sees the condition; if after, the waiter is already Blocked or Sleeping and is readied.

## 2. Files and changes

**src/kernel/scheduler.h:** no signature changes. Comments: `WaitOn` returns at the deadline, after a `WakeAll` of the queue, after a `WakeTask` of the caller (including one posted since it last blocked), or spuriously; callers loop and recheck. `WakeTask`: a task that has not blocked yet keeps the wake-up for its next `WaitOn`, `SleepUntil` or `Wait`; callers publish their condition first. `SleepUntil`: sleeps until the tick or a `WakeTask`, including an earlier one; callers that need the full time loop.

**src/kernel/scheduler.cpp:**
- `struct Task` (25-51): `bool wake_pending GUARDED_BY(scheduler_lock);` ("A WakeTask that found the task running or ready; its next block point consumes it.").
- `ResetTask` (84-104): `task.wake_pending = false;`.
- `bool TakeWakePending(Task& task) REQUIRES(scheduler_lock)` in the anonymous namespace: returns and clears the flag.
- `WaitOn` (572-577): after `scheduler_lock.Lock()`, `const bool woken = TakeWakePending(*CurrentTask());`; if `woken` or the deadline passed, `scheduler_lock.Unlock(); return;` (the caller's lock is never released on this path, as with the existing deadline return).
- `SleepUntil` (620-626): block only if `!TakeWakePending(...) && Time::TickCount() < tick`.
- `Wait` (474): `CurrentTask()->wake_pending = false;` just before `state = Blocked`.
- `WakeTask` (606-610): Sleeping or Blocked → `MakeReady`; Running or Ready → `wake_pending = true`; Complete and Empty ignored.

**Fixed delays reachable from process context** (they do not loop; a signal already cuts them short, and stale tokens would more often; AHCI's `ResetHoldMs` COMRESET hold, ahci/port.cpp:292, runs on the caller's stack):
- `src/kernel/storage/kernel_environment.cpp` `Kernel::Sleep` (63-65): loop `SleepUntil(until)` while `TickCount() < until`.
- `src/kernel/usb/xhci/probe.cpp` `Pause` (92-95): the same loop.
- `Time::WaitForNextTick` (timer.cpp:131-145) unchanged: only boot and kernel tasks call it, and no `WakeTask` targets them.

**No change:** `pipe.cpp`, `futex.cpp`, `socket.cpp`, `terminal.cpp`, `poll.cpp`, `pointer.cpp`, `console.cpp`, `process.cpp`.

**Tests, smoke, docs:** stub token model (`tests/unit/support/scheduler_stubs.{h,cpp}`); `tests/unit/repro/wakeups_repro_tests.cpp`; new cases in `futex_tests.cpp`, `pipe_tests.cpp`, `terminal_tests.cpp`; `src/kernel/kernel_test.cpp` SCHEDULER and SMP checks and their success lines in `tools/smoke/cases/scheduler.py`, `smp.py`; `src/user/libctest.cpp` stress checks with `LIBCTEST_CHECKS` in `tools/smoke/cases/shell/programs.py`; `docs/development.md` "Scheduler And Processes" (the token and the loop rule; the "Signals" paragraph lists poll, sockets and futex waits); review statuses. No new matrix cases: SCHEDULER (15/16), shell_programs (21/22), SMP (23/24), and 25/26, 37/38 run libctest.

## 3. Invariants

- **I1.** A `WakeTask(T)` after T's last condition check and before T blocks makes that block return without sleeping (`WaitOn` at once, `SleepUntil` before its tick), whichever CPU each side runs on.
- **I2.** `wake_pending` is set only by `WakeTask`, only on a Running or Ready task, under `scheduler_lock`; a Blocked or Sleeping task is readied instead. So `wake_pending == false` whenever the state is Blocked or Sleeping.
- **I3.** The next block point (`WaitOn`, `SleepUntil`, or `Wait` when it blocks) consumes the token under `scheduler_lock`. One `WakeTask` causes at most one early return; a later unrelated wait never returns early because of it; nothing spins.
- **I4.** Every `WakeTask` caller publishes its condition first: `SendSignalLocked` (`Signals::Send`, process.cpp:638-650); `ThreadCreate` on an exiting process (2043-2047); `WakeThreadsLocked` (566-576, 592-601); `Futex::Wake` (`woken = true`, futex.cpp:124-125).
- **I5.** Every `WaitOn` caller loops and rechecks; every `SleepUntil` caller that needs its full time loops on the deadline.
- **I6.** Lock order unchanged (subsystem lock → `scheduler_lock`); no new callback under `scheduler_lock`.
- **I7.** `WaitOn` still takes `scheduler_lock` before releasing the caller's lock; its early returns never release it.
- **I8.** New tasks start without a token (`ResetTask`).

## 4. Edge cases

| Case | Result |
| --- | --- |
| Signal after the check in pipe or socket read/write, futex wait, pointer, framebuffer events, poll, terminal read | `WaitOn` returns, the loop sees the signal, EINTR (stop-only EINTR restarts as today) |
| Signal after `CallerInterrupted` in Sleep (R3-04) | `SleepUntil` returns at once; EINTR with `restart_argument` the time left |
| Pause | EINTR at once (was up to 1 s later, process.cpp:2355-2362) |
| SIGKILL from `ExitProcess` to a thread in a futex wait (R3-06) | EINTR, the thread ends, `WaitForThreads` completes |
| Signal to a task in user mode, ignored SIGCHLD | Token set; the next wait returns once spuriously, rechecks, blocks |
| Task preempted between check and block | Token kept, as for Running |
| Futex `Wake` racing a timeout | One spurious wake later |
| Token present and deadline passed | `WaitOn` returns, token consumed |
| Stopped process (`HoldWhileStopped`) | Unchanged; a stale token costs one extra loop |
| `Wait`, `WaitSyscall`, `ThreadJoin`, `WaitForThreads` | Unchanged results; token cleared on block |
| Kernel tasks and the boot task | Never targeted, no tokens |
| Driver delay in process context | Lasts its full time; a signal no longer cuts it short |

## 5. Waiters

All loop and recheck, so none changes beyond the two delays: `Pipes::Read`/`Write` (under `pipe->lock`), `Futex::Wait` (under `futex_lock`), socket `SendStream`/`ReceiveStream`/Connect/Accept/`ReceiveFrom` (under `socket_lock`), `RunTcpTask`, `Terminal::WaitForInput` via `EditLine`/`ReadBytes` (check outside the lock, interrupts on), `Terminal::WaitForRoom` via `WriteMaster`, `Pointer::Read`, `Console::WaitForEvents`, `Poll::WaitForChange` via `PollSyscall`, `SleepSyscall`, `PauseSyscall`, `HoldWhileStopped`, `Scheduler::Wait` users (`WaitSyscall`, `ThreadJoin`, `WaitForThreads`, kernel_runtime; `Wait` clears the token), `StopOtherProcesses`, `writeback.cpp`, the `WaitUntil`/`WaitForEvents` loops in drivers, mutex.h, volume_mutex, worker, USB manager, network device, interface, neighbor discovery, console status, kernel_test. Changed: `Kernel::Sleep` and xhci `Pause` (loop to the deadline).

## 6. Acceptance tests

**Stub model.** The stub tracks tasks sleeping inside `WaitOn` (while it releases the caller's lock around `on_wait`/`wait_for`) and a set of pending tokens, both under `wake_mutex`. `WakeTask(id)` still records into `woken_tasks`; a sleeping `id` gets `wake_generation++` and `notify_all`, otherwise a token. `WaitOn` first consumes a token for `current_task`: if found, `TestScheduler::early_wakes++` and return without unlocking, without `on_wait`, without sleeping. `Reset` clears tokens, sleeping counts and `early_wakes`. Existing futex tests (including "oldest first") must still pass, also under `unit-test-tsan`.

**Repro tests** (drop `.repro`, keep `[R2-07]`/`[R3-06]`): `SignalArrivesAfterFirstCheck` also calls `WakeTask(TestScheduler::current_task)` after setting `signal_pending`, as `SendSignalLocked` does; `CHECK(sleep_deadline == 0)` becomes `CHECK(TestScheduler::last_wait_deadline == 0)` (`sleep_deadline` is set only inside `on_wait`, which a correct fix never reaches); add `CHECK(TestScheduler::early_wakes == 1)`.
- **T1** [R2-07] pipe read: EINTR, `on_wait` never ran.
- **T2** [R2-07] full-pipe write: EINTR, `on_wait` never ran.
- **T3** [R3-06] futex wait: EINTR, `on_wait` never ran.

**New host tests:**
- **T4** [R2-07] stale token on a pipe read: `WakeTask(current_task)` before `Read`, no signal; `on_wait` writes 1 byte. Result 1, `wait_count == 2`, `early_wakes == 1`, `on_wait` ran once.
- **T5** [R3-06] stale token on a futex wait: `WakeTask` before `Wait`; `on_wait` calls `Wake`. `Wait` returns 0 only after the `Wake`; `early_wakes == 1`; `woken_tasks` holds both wakes.
- **T6** [R3-04] terminal read: the `caller_interrupted` hook returns false on its first call and posts a pending flag plus `WakeTask(current_task)`, then true. The read returns EINTR and `on_wait` never ran.

**Kernel tests, SCHEDULER mode** (`kernel_test.cpp` `RunBlockingTests`, boot CPU only, deterministic; failures Panic naming R3-04):
- **T7** A task calls `WakeTask(self)`, then `WaitOn(q, lock, now + 50)` with nobody to wake it: returns in < 50 ticks. Then `WaitOn(q, lock, now + 3)` takes ≥ 3 ticks.
- **T8** As T7 with `SleepUntil`.
- **T9** Ready task: the boot task creates T (Ready, not yet run), calls `WakeTask(T)`, then `Wait(T)`; T's `WaitOn(now + 50)` returns in < 50 ticks.
- **T10** A task calls `WakeTask(self)`, then `Wait` on a child that does `SleepUntil(+2)`, then `WaitOn(now + 3)`, which takes ≥ 3 ticks.
- **T11** A task blocks in `WaitOn` with no deadline; the boot task yields until it is Blocked, then `WakeTask`; the task finishes.
- Success line: `KERN/SCHEDULER: preemption, wait, exit, reap, blocking, early wake-ups, sleep, and priorities passed` (scheduler.py).

**Kernel test, SMP mode** (cross-CPU R3-04 path, deterministic):
- **T12** In `RunSmpTests`: kernel tasks W and S, preemption off, start through `SmpTask`'s barrier on different CPUs. 100 `WaitOn` rounds: W takes `test_lock` (IrqSave), sees atomic `condition == false`, publishes `phase = Checked(round)`, spins until `Sent(round)`; S sets `condition`, calls `WakeTask(W)` without `test_lock` (as `SendSignalLocked`), publishes `Sent`; W calls `WaitOn(q, test_lock, now + 100)` and must return with `TickCount() < deadline` and `condition == true`. Then 100 `SleepUntil(now + 100)` rounds, same handshake, no lock. Spins are bounded by iteration count (ticks stop when W holds the boot CPU with IRQs masked); a timeout Panics "partner did not run". Without the fix the first round Panics "R3-04: a wake-up posted before the block was lost". The SMP line gains `; wake-ups across CPUs kept` (smp.py).

**QEMU stress, SHELL smoke, both architectures:**
- **T13** `src/user/libctest.cpp` `CheckSignalWakeups()` after `CheckCloseWakesBlockedCalls()`. libctest starts itself in child modes (as `CheckAssert` does): `block-pipe` (`read` on its own empty pipe), `block-sleep` (`nanosleep` 10 s), `block-futex` (main locks a mutex, a thread blocks in `pthread_mutex_lock`, main yields a few times and returns, so exit SIGKILLs the thread from another CPU: R3-06). 16 rounds per mode; for pipe and sleep the parent sends SIGKILL after `round % 8` `sched_yield`s. Pass per round: `waitpid(WNOHANG)` reaps the child within 2 s (1 ms polls). On failure: a second SIGKILL, reap, `Check(false, "a SIGKILL ended every child blocked in a <pipe read|sleep|futex wait>")`. 3 new checks per architecture in `LIBCTEST_CHECKS`. Deterministic pass criterion; catching the race is probabilistic (T12 is the deterministic proof).

**Whole suite:**
- **T14** `tools/quality.sh check` (thread-safety analysis on `wake_pending`).
- **T15** Full unit suite including tsan; smoke `SHELL SCHEDULER SMP`.

| Invariant | Tests |
| --- | --- |
| I1 | T1-T3 (model), T6, T7, T8, T9, T12 (real, cross-CPU), T13 |
| I2 | T9 (Ready), T12 (Running), T11 (Blocked readied); clearing a token on a blocked task: review only |
| I3 | T7/T8 second half, T10, T4, T5 |
| I4 | review only (T13 indirectly) |
| I5 | T4, T5, T6 for pipe, futex, terminal; section 5 by review for the rest |
| I6 | T14 and review |
| I7 | `lock_held_during_wait` in T1-T3; A1's revoke tests; review |
| I8 | review |

## 7. Order of work

1. scheduler.cpp/.h. 2. Delay loops. 3. Stub token model, T1-T6, drop `.repro`. 4. Kernel tests T7-T12 and smoke lines. 5. libctest T13 and `LIBCTEST_CHECKS`. 6. Docs and review status. 7. Quality, unit tests, smoke.

## 8. Decisions (coordinator)

1. The repro tests' sender model also calls `WakeTask`, and the deadline check reads `last_wait_deadline`: the old assertion cannot pass under any fix.
2. `Kernel::Sleep` and xhci `Pause` loop to their deadline in C1.
3. The stress runs 16 rounds per mode (not 32), to keep SHELL runs short; T12 is the deterministic proof.
4. `SendSignalLocked` keeps calling `WakeTask` for ignored signals (one spurious wake per ignored SIGCHLD); out of scope.
5. The `KERN/SCHEDULER` and `KERN/SMP` success lines change as proposed.
