# Review fixes: area 1, the filesystem core

Branch `feat/review`, cut from `main` at `8b9ba14`. The findings are in `docs/reviews/2026-11-review.md` (IDs `R1-xx`), each with its failure scenario, evidence and verifier notes. This plan fixes all nine, most severe first. Drop a task by marking its finding `accepted: <reason>` in the review file and ticking the task here with "accepted".

## Rules for every task

- **Test first.** Write a host test in `src/fs/core/tests/` that reproduces the finding's failure scenario and fails on the current code. Use the existing crash harness (`crash_helpers.*`, `crash_scenarios.*`) where the finding needs a crash at a given write. Then fix the code until the test passes.
- **Not reproducible.** If the scenario cannot be produced after a real attempt, do not change the code. Mark the finding `false positive: <why>` in the review file and report it.
- **Record.** When the task is done, set the finding's status in the review file's table and in its detail section to `fixed (<commit>)`. The coordinator fills in the hash after committing.
- **Invariants.** Write order and crash behaviour follow `docs/design/M22-P6-s4.md` (invariants I11-I23) for ext, and CLAUDE.md "Known hard limits" for FAT16 ("a crash can lose the target, never the source").
- **Tests.** `build/unit-tests-ninja/featheros_unit_tests` does not hold the core's tests. They are in `featherdisk_core_tests`, run through ctest as `fs.core:*` (built by `./build.sh unit-test`). Smoke: `STORAGE` and `SHELL` on both architectures for every task, since the kernel builds the core.

## Model table

| Task | Model |
| --- | --- |
| F1, E1, E3 | Opus design (`docs/design/review-<task>.md`), then `sonnet`, effort high |
| F2, E2, E4, E5 | `sonnet`, effort high |

## Tasks

- [x] F1. FAT16 rename: R1-06 (rename to another spelling of the same entry deletes the source first) and R1-07 (a replacing rename frees the target before the new entry is known to fit). Both are in `Fat16::Rename` (`src/fs/core/fat/fat16.cpp:1700-1800`). The source must survive a crash or an error at every point; a failed rename must leave the target as it was.
- [x] F2. FAT16 directory scans: R1-08 (`FindFreeRun` writes past the 0x00 end marker) and R1-09 (`SlotSector` walks the cluster chain from the start for every entry, which is quadratic).
- [ ] E1. ext journal: R1-01, a metadata block freed and allocated again within one step is dropped at `EndStep`. Fix the pending-forget handling in `src/fs/core/ext/transaction.cpp` (and the allocator guard if the design chooses it) so I12 holds for blocks born in the running transaction.
- [ ] E2. ext journal: R1-02, `EndStep` fails with `OutOfMemory` when running and step counts add up past the layer maximum although the distinct blocks fit (`transaction.cpp:684`).
- [ ] E3. ext extents: R1-03, a failure after the extent tree is committed frees data blocks the tree already maps (`extent_write.cpp:772`, `AppendExtentBlock`).
- [ ] E4. ext truncate: R1-04, the tail of the new last block is zeroed in place before the truncation is committed (`write.cpp:1229`).
- [ ] E5. ext rename: R1-05, renaming a directory onto an empty directory in a parent at the link limit gives `Corrupt` and the error state instead of a clean link-limit error (`namespace.cpp:950`).

The full matrix runs after E5 (shared code: the core is built into the kernel).

## Part K: minor findings of areas 2-5 (planned, not started)

The user's choice (2026-11-25): run E1-E5 now; plan every minor finding of areas 2-5 as later tasks, and leave the seven important ones (R2-01, R2-02, R2-03, R3-01, R3-02, R3-03, R5-01) open in the review file for a later decision. Autopilot stops after E5 and does not start Part K until the user says so. The same rules apply (test first, or a QEMU case where no host test can reach the code; `false positive` when not reproducible). Models: `sonnet`, effort high; K2 and K3 are risky (concurrency, interrupt entry) and get an Opus design first.

- [ ] K1. Processes and files: R2-04 (`WaitSyscall` keeps a `ProcessRecord` pointer after dropping `process_lock`), R2-05 (a join after the main thread's `pthread_exit`), R2-06 (buffer checks reject untouched demand-grown stack pages), R2-07 (pipe read/write misses a signal between the check and blocking).
- [ ] K2. Scheduler and futex wake-ups: R3-04 (`WakeTask` drops the wake-up of a task between its signal check and blocking), R3-05 (`CurrentTask()` reads the per-CPU index and slot non-atomically), R3-06 (`Futex::Wait` loses a signal's wake-up). Shares a fix with R2-07.
- [ ] K3. Entry and stacks: R3-08 (AArch64 `aarch64_enter_user` loads SPSR/ELR with IRQs unmasked), R3-09 (the x86-64 signal frame copies uninitialised kernel stack bytes to user space), R3-07 (a kernel stack slot is lost when `AllocateKernelStack` fails part-way).
- [ ] K4. VFS and `/proc`: R2-08 (a mount point listed twice), R2-09 (renaming a directory that holds a mount point), R2-10 (`/proc/mounts` past 1024 bytes).
- [ ] K5. USB: R4-01 (USB 3 root port never reset on re-enumeration), R4-02 (recovery mask bit cleared again), R4-03 (`CRCR` written as two dwords in `AbortCommand`), R4-04 (HID reports longer than one packet).
- [ ] K6. Block devices: R4-05 (x86-64 virtio-blk legacy queue above 256 entries), R4-06 (a read-only virtio-blk device registered as writable), R4-07 (a dangling NVMe `Disk` pointer after revalidation).
- [ ] K7. Network: R5-02 (a closed receive window drops ACKs), R5-03 (zero-window probing never times out), R5-04 (the SYN adjustment fires again after sequence wrap), R5-05 (IPv4 accepts loopback or own-address sources from the wire), R5-06 (`getaddrinfo`'s UDP wait restarts on unrelated datagrams).

Part K changes the kernel, libc and drivers, so the full matrix runs after its last task.

## Run log

- F1: Opus design (`docs/design/review-F1.md`), sonnet (high), 0 fix rounds, one Sonnet review (no defects). Unit tests 1869/1869, STORAGE and SHELL on both arches (affected only; matrix after the last task). Mutations 3/3 (I1 by T2/T3, I3 by T5, I4 by T4). Departures: `PlanEntry` also refuses a taken plain 8.3 name with `Exists`; a crash during directory growth can leak one cluster (existing behaviour, tolerated in the growth test); `filesystem_tests.cpp` no longer allows a missing target after a FAT16 replacing rename; CLAUDE.md and `docs/posix.md` now say a FAT16 rename can leak or briefly share clusters until `fsck.fat`, and a failed rename leaves the target as it was. Not run: `fsck.fat` (not installed). Accepted limit: `SetParent` returning `Corrupt` after the swap write is untested.
- F2: sonnet (high), 0 fix rounds; 4 new tests failed first (read count 8000 at 32 clusters, 32384 at 64 before the fix); ctest 1873/1873, STORAGE and SHELL on both arches (affected only). Rule chosen: the first 0x00 slot ends the directory and every slot after it is free; a run that reaches it writes a new end marker after the run first, only when that slot holds stale bytes. Not checked against Linux vfat or mtools behaviour. Accepted limit: `ReadDirectory` still walks the chain from the first cluster on each call (a cookie seek).
