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

- [ ] A1. Pin open files for the duration of a call: R2-02, R5-01, R2-03 (all reproduced). `Files::FindSocket`, `FindPipe` and `FindMaster` return a raw object without a reference, so a `close` in another thread frees it under a running or blocked call. Give these lookups a hold that the call releases, as `HoldFramebuffer` already does, and make a blocked call on a closed object wake and return `EBADF` (or the POSIX result for that call). Smoke `SHELL NETWORK`.
- [ ] A2. User copies against a concurrent unmap: R2-01 (x86-64 null write in ring 0), R3-01 (AArch64 use of a freed frame; its shootdown is empty). Both trace only. Copies must not race `ShrinkHeap`/`ReleaseStackSlot` of another thread: hold the process's memory lock (or a pin on the pages) across a copy, or make the copy fault-tolerant, and give AArch64 a real TLB shootdown before frames are freed. Needs a QEMU stress case (a read loop against an `sbrk(+4096)/sbrk(-4096)` loop in another thread, on both architectures). Smoke `SMP SHELL MEMORY`.
- [ ] A3. User-mode entry and signal frames: R3-03 (reproduced: an unvalidated signal handler address in the `iretq` frame faults in ring 0), R3-02 (x86-64 `x86_64_enter_user` runs with interrupts on across `SetKernelStack` and `SWAPGS`), R3-08 (the AArch64 counterpart: SPSR/ELR loaded with IRQs unmasked), R3-09 (the x86-64 signal frame copies uninitialised kernel stack bytes to user space). Trace-only parts need a QEMU case where possible (e.g. a signal handler at a non-canonical address must kill only the program). Smoke `SHELL SMP INTERRUPT EXCEPTION`.

## Part B: filesystem core data integrity

Full matrix after this part (the core is built into the kernel).

- [ ] B1. ext journal: R1-01 (reproduced), a metadata block freed and written again within one step is dropped at `EndStep`. Implement `docs/design/review-E1.md`. Smoke `STORAGE SHELL`.
- [ ] B2. ext extents: R1-03 (reproduced), a failure after the extent tree is committed frees blocks the tree already maps (`edit.committing` is false after a successful commit). Smoke `STORAGE`.
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

## Run log

- F1: Opus design (`docs/design/review-F1.md`), sonnet (high), 0 fix rounds, one Sonnet review (no defects). Unit tests 1869/1869, STORAGE and SHELL on both arches (affected only; matrix after the last task). Mutations 3/3 (I1 by T2/T3, I3 by T5, I4 by T4). Departures: `PlanEntry` also refuses a taken plain 8.3 name with `Exists`; a crash during directory growth can leak one cluster (existing behaviour, tolerated in the growth test); `filesystem_tests.cpp` no longer allows a missing target after a FAT16 replacing rename; CLAUDE.md and `docs/posix.md` now say a FAT16 rename can leak or briefly share clusters until `fsck.fat`, and a failed rename leaves the target as it was. Not run: `fsck.fat` (not installed). Accepted limit: `SetParent` returning `Corrupt` after the swap write is untested.
- F2: sonnet (high), 0 fix rounds; 4 new tests failed first (read count 8000 at 32 clusters, 32384 at 64 before the fix); ctest 1873/1873, STORAGE and SHELL on both arches (affected only). Rule chosen: the first 0x00 slot ends the directory and every slot after it is free; a run that reaches it writes a new end marker after the run first, only when that slot holds stale bytes. Not checked against Linux vfat or mtools behaviour. Accepted limit: `ReadDirectory` still walks the chain from the first cluster on each call (a cookie seek).
- Reproduction pass (2026-11-25, workflow, 8 Sonnet writers and 8 Sonnet runners, 1.4M tokens): 20 reproduced, 17 trace only, 0 false positives. The plan was regrouped by root cause; E1-E5 became B1-B5 and Part K became parts C and D, with the seven important kernel findings added as Part A.
