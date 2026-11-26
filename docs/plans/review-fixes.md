# Review fixes

Branch `feat/review`, cut from `main` at `8b9ba14`. The findings are in `docs/reviews/2026-11-review.md` (IDs `R<area>-<n>`), each with its failure scenario, evidence, verifier notes and, since the reproduction pass, its reproduction: a hidden failing test tagged `[.repro][<ID>]`, or a trace where the code is not in the host build. 41 findings: 4 fixed (F1, F2), 37 open (20 reproduced, 17 trace only, no false positives).

The tasks are grouped by root cause, most severe first. Drop a finding by marking it `accepted: <reason>` in the review file and noting it in the task.

## Rules for every task

- **Start from the reproduction.** A reproduced finding's test already fails on the current code: fix the code until it passes, then remove the `.repro` tag (keep the ID tag) so it runs as a regression test. A trace-only finding gets a test where the fix makes the code reachable (a host-built helper, a fake), else a QEMU case in `tools/smoke/` and `tools/matrix.py` on both architectures; if neither is possible, say so in the run log and rely on review.
- **Not reproducible after all.** Do not change the code; mark the finding `false positive: <why>` and report it.
- **Record.** Set each finding's status in the review file's table and detail section to `fixed (pending commit)`; the coordinator fills in the hash.
- **Invariants.** ext write order and crash behaviour follow `docs/design/M22-P6-s4.md` (I11-I23); the lock order is in `docs/development.md` ("Storage locking" and nearby sections).
- **Tests.** Kernel and library host tests: `build/unit-tests-ninja/featheros_unit_tests`. Filesystem core: `featherdisk_core_tests`, through ctest as `fs.core:*`. Both are built by `./build.sh unit-test`. Smoke modes per task are named below; `tools/check.sh --arch both --modes "<MODES>"` after the lint steps.

## Model table

| Task | Model |
| --- | --- |
| A1, A2, A3, C1 | Opus design (`docs/design/review-<task>.md`), then `sonnet`, effort high |
| B1 | the existing design `docs/design/review-E1.md`, then `sonnet`, effort high |
| everything else | `sonnet`, effort high |

## Done

- [x] F1. FAT16 rename keeps the source and the target: R1-06, R1-07 (`b25283e`).
- [x] F2. FAT16 end-of-directory marker and linear scans: R1-08, R1-09 (`922d2eb`).

## Part A: kernel memory safety (important)

Any user program can crash the kernel or use freed memory with these. Full matrix after this part.

- [x] A1. Pin open files for the duration of a call: R2-02, R5-01, R2-03 (all reproduced). `Files::FindSocket`, `FindPipe` and `FindMaster` return a raw object without a reference, so a `close` in another thread frees it under a running or blocked call. Give these lookups a hold that the call releases, as `HoldFramebuffer` already does, and make a blocked call on a closed object wake and return `EBADF` (or the POSIX result for that call). Smoke `SHELL NETWORK`.
- [x] A2. User copies against a concurrent unmap: R2-01 (x86-64 null write in ring 0), R3-01 (AArch64 use of a freed frame; its shootdown is empty). Both trace only. Copies must not race `ShrinkHeap`/`ReleaseStackSlot` of another thread: hold the process's memory lock (or a pin on the pages) across a copy, or make the copy fault-tolerant, and give AArch64 a real TLB shootdown before frames are freed. Needs a QEMU stress case (a read loop against an `sbrk(+4096)/sbrk(-4096)` loop in another thread, on both architectures). Smoke `SMP SHELL MEMORY`.
- [x] A3. User-mode entry and signal frames: R3-03 (reproduced: an unvalidated signal handler address in the `iretq` frame faults in ring 0), R3-02 (x86-64 `x86_64_enter_user` runs with interrupts on across `SetKernelStack` and `SWAPGS`), R3-08 (the AArch64 counterpart: SPSR/ELR loaded with IRQs unmasked), R3-09 (the x86-64 signal frame copies uninitialised kernel stack bytes to user space). Trace-only parts need a QEMU case where possible (e.g. a signal handler at a non-canonical address must kill only the program). Smoke `SHELL SMP INTERRUPT EXCEPTION`.

## Part B: filesystem core data integrity

No matrix after this part (user's decision, 2026-11-25): the host `fs.core` tests and STORAGE/SHELL smoke on both architectures verify each task, and the matrix after Part C covers the core in the kernel.

- [x] B1. ext journal: R1-01 (reproduced), a metadata block freed and written again within one step is dropped at `EndStep`. Implement `docs/design/review-E1.md`. Smoke `STORAGE SHELL`.
- [x] B2. ext extents: R1-03 (reproduced), a failure after the extent tree is committed frees blocks the tree already maps (`edit.committing` is false after a successful commit). Smoke `STORAGE`.
- [ ] B3. ext truncate: R1-04 (reproduced), the tail of the new last block is zeroed in place before the truncation commits. Smoke `STORAGE`.
- [ ] B4. ext rename: R1-05 (reproduced), a directory renamed onto an empty one in a parent at the link limit counts the parent's link twice and puts the volume in its error state. Smoke `STORAGE`.
- [ ] B5. ext journal: R1-02 (trace only, arithmetic shown), `EndStep` reserves running plus step counts instead of the distinct blocks. Smoke `STORAGE`.

## Part C: wake-ups and process lifetime

Full matrix after this part.

- [ ] C1. Lost wake-ups between the interrupted check and blocking: R2-07, R3-06 (reproduced), R3-04 (trace). One fix in `Scheduler::WaitOn` / `WakeTask` (record a wake-up for a task that is still running, or check `interrupted()` under the wait lock), used by pipes, futexes and the other waits. Smoke `SHELL SCHEDULER SMP`.
- [ ] C2. Scheduler per-CPU and stacks: R3-05 (`CurrentTask()` reads the per-CPU index and slot non-atomically), R3-07 (a kernel stack slot is lost when `AllocateKernelStack` fails part-way). Both trace only. Smoke `SCHEDULER SMP MEMORY`.
- [ ] C3. Process and thread lifetime: R2-04 (`WaitSyscall` keeps a `ProcessRecord` after dropping `process_lock`), R2-05 (a join after the main thread's `pthread_exit`), R2-06 (buffer checks reject untouched demand-grown stack pages). All trace only. Smoke `SHELL`.

## Part D: subsystems

Full matrix after this part, which is also the end of the plan.

- [ ] D1. VFS and `/proc`: R2-08 (a mount point listed twice), R2-09 (renaming a directory that holds a mount point), R2-10 (`/proc/mounts` past 1024 bytes). All reproduced. Smoke `SHELL STORAGE`.
- [ ] D2. USB: R4-01 (USB 3 root port not reset on re-enumeration), R4-02 (recovery mask bit cleared again), R4-03 (`CRCR` written as two dwords), R4-04 (HID reports longer than one packet). All reproduced. Smoke `INPUT USB_STORAGE`.
- [ ] D3. Block devices: R4-05 (legacy virtio queue above 256), R4-06 (a read-only virtio-blk disk registered as writable), R4-07 (a stale NVMe `Disk` pointer after revalidation). All trace only. Smoke `STORAGE NVME`.
- [ ] D4. Network: R5-02 (a closed receive window drops ACKs), R5-03 (zero-window probing never times out), R5-04 (the SYN adjustment after sequence wrap), all reproduced; R5-05 (IPv4 accepts loopback or own-address sources), R5-06 (`getaddrinfo`'s UDP wait restarts on stray datagrams), trace only. Smoke `NETWORK`.

## After the plan

- [ ] Z1. Draft M35, GUI foundations, in `docs/roadmap.md` as a planned milestone (user's request, 2026-11-25). Source: section 14 ("OS prerequisites") and phase G0 of `docs/design/GUI.md` on `feat/gui` (`git show feat/gui:docs/design/GUI.md`). Parts: `mmap` with anonymous and shared mappings plus a page limit that holds window surfaces; Unix-domain sockets, `socketpair`, `sendmsg`/`recvmsg` with descriptor passing; the compositor input path (a `Super` modifier, a raw mode for the `/dev/fb0` owner so Ctrl+C is not `SIGINT`, an event queue above 64, `EV_ABS` for QEMU's tablet; check the code first for what already exists). Note how it builds on A1's file holds and A2's per-page user copies. Docs only, `sonnet`; no matrix.

## Run log

- F1: Opus design (`docs/design/review-F1.md`), sonnet (high), 0 fix rounds, one Sonnet review (no defects). Unit tests 1869/1869, STORAGE and SHELL on both arches (affected only; matrix after the last task). Mutations 3/3 (I1 by T2/T3, I3 by T5, I4 by T4). Departures: `PlanEntry` also refuses a taken plain 8.3 name with `Exists`; a crash during directory growth can leak one cluster (existing behaviour, tolerated in the growth test); `filesystem_tests.cpp` no longer allows a missing target after a FAT16 replacing rename; CLAUDE.md and `docs/posix.md` now say a FAT16 rename can leak or briefly share clusters until `fsck.fat`, and a failed rename leaves the target as it was. Not run: `fsck.fat` (not installed). Accepted limit: `SetParent` returning `Corrupt` after the swap write is untested.
- F2: sonnet (high), 0 fix rounds; 4 new tests failed first (read count 8000 at 32 clusters, 32384 at 64 before the fix); ctest 1873/1873, STORAGE and SHELL on both arches (affected only). Rule chosen: the first 0x00 slot ends the directory and every slot after it is free; a run that reaches it writes a new end marker after the run first, only when that slot holds stale bytes. Not checked against Linux vfat or mtools behaviour. Accepted limit: `ReadDirectory` still walks the chain from the first cluster on each call (a cookie seek).
- Reproduction pass (2026-11-25, workflow, 8 Sonnet writers and 8 Sonnet runners, 1.4M tokens): 20 reproduced, 17 trace only, 0 false positives. The plan was regrouped by root cause; E1-E5 became B1-B5 and Part K became parts C and D, with the seven important kernel findings added as Part A.
- A1: Opus design (`docs/design/review-A1.md`), sonnet (high), 0 fix rounds, one Sonnet review (no defects). Unit tests 1890/1890, SHELL and NETWORK on both arches (affected only; matrix after Part A). Mutations 3/3 (I4 by T9/T10, I5 by T6, I1/I3 by T1/T4/T5). A blocked call on a revoked pipe, socket or pty master returns EBADF (Linux keeps it blocked); EINTR wins over EBADF. T6/T9/T10 hooks run in two waits so the close is seen before the data. Accepted limits: blocked `/dev/fb0` and `/dev/mouse` reads are not woken by `close`; `FindPseudoTerminal` (ioctls) still returns a raw `Pair*` (static array, no use-after-free); no mutation run on `Terminal::Reset` clearing `room_revoked`.
- A2: Opus design (`docs/design/review-A2.md`), sonnet (high), 0 fix rounds (one resume to finish skipped verification), one Sonnet review (no defects). User copies go one page per hold of the memory lock through `Memory::UserCopy`; AArch64 `ShootDownUserMappings` completes with `dsb ish`, no IPI. `[user_copy]` 8 cases (also under TSan), SMP SHELL MEMORY on both arches, SHELL with `QEMU_CPU=max` on x86-64 (affected only; matrix after Part A). Mutations 3/3 (I6 by T2/T5, I1 by the added T5b, I7 by T4 once the fake counted every call). TSan on macOS does not see races inside `memcpy`, so T6 cannot catch I1 there. Stress checks `copy-race-heap`/`copy-race-stack` in `hardening` (71 checks), run by the existing SHELL matrix cases. Accepted limits: a copy failing part-way leaves earlier pages copied and returns `EFAULT`; the environment block now accepts a double NUL before an unmapped page; untouched stack pages still `EFAULT` (R2-06, C3); a lost frame reuse on AArch64 QEMU would be silent (host T6/T5b and review only).
- A3: Opus design (`docs/design/review-A3.md`), sonnet (high), 0 fix rounds, one Sonnet review (no defects). `sigaction` refuses a handler or restorer at or above 2^47 (`EINVAL`, unlike Linux); one shared limit in both `EnterHandler`s and AArch64 `ReturnFromSignal` (was 2^48); frames zeroed before copy-out; interrupts masked in the asm from entry and from the end of each dispatch to `iretq`/`eret`; x86-64 writes `rsp0` once, in `GetUserReturnStack` with IF=0; AArch64 sets `SP_EL0` directly. Unit tests 1901/1901, SHELL SMP INTERRUPT EXCEPTION and USER_PREEMPTION on both arches, SHELL with `QEMU_CPU=max`; full matrix green (end of Part A). Mutations 4/4 (I1 by T1/T2 and by T5 on AArch64, I6 by T7, I2 by T6). `hardening` has 78 checks. Accepted limits: I3/I4 (the masked windows) review only; an NMI or #MC between `swapgs` and `iretq` still runs with the user GS base; padding in other structs copied to user memory not audited (both possible new findings). Carried: `featheros_unit_tests` run unfiltered in one process shows 63 `filesystem_tests` failures that pass under ctest (test isolation, not from A3).
- B1: existing design (`docs/design/review-E1.md`, "As built" added), sonnet (high), 0 fix rounds, one Sonnet review (no defects). `EndStep` keeps the step copy of a block freed and written again as metadata in the same step, with no revoke; M22-P6-s4 I11/I12 restated. Unit tests 1908/1908 (6 skipped), `[R1-01]` 7 cases (T2 crash cuts around flushes, 85 s), STORAGE and SHELL on both arches (affected only; Part B has no matrix). Mutations 3/3 (forget rule by T1/T2, revoke-and-log by T3, cancel-on-allocation by T6). Departures: the design's T1 geometry never split the leaf, so all tests use the reproduction's geometry; T2 cuts around flushes (every write took 14 min); T5 uses exact free-block counts instead of `LimitJournal`. Accepted limits: the new `Corrupt` guard is unreachable and untested; it checks only the current step, so a stale write to a block freed in an earlier step of the same transaction would not be caught (no caller does that).
- B2: sonnet (high), 0 fix rounds, one Sonnet review (no defects). The extent edit's flag (now `commit_tried`) stays set after a successful tree commit and is reset at the start of each operation and before each hole allocation. Unit tests 1911/1911, `[R1-03]` 3 cases (repro, depth-2 tree with the index or leaf free failing after the commit, pre-commit `NoSpace`), STORAGE on both arches (affected only). Mutations 2/2. Accepted limits: the per-hole reset in `WriteExtentFile` is not caught by a test; a failed commit, or a failed free of the replaced node after the commit, leaves that block allocated and unreferenced (a leak `e2fsck` reports, never a mapped free block).
