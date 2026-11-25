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

- [ ] F1. FAT16 rename: R1-06 (rename to another spelling of the same entry deletes the source first) and R1-07 (a replacing rename frees the target before the new entry is known to fit). Both are in `Fat16::Rename` (`src/fs/core/fat/fat16.cpp:1700-1800`). The source must survive a crash or an error at every point; a failed rename must leave the target as it was.
- [ ] F2. FAT16 directory scans: R1-08 (`FindFreeRun` writes past the 0x00 end marker) and R1-09 (`SlotSector` walks the cluster chain from the start for every entry, which is quadratic).
- [ ] E1. ext journal: R1-01, a metadata block freed and allocated again within one step is dropped at `EndStep`. Fix the pending-forget handling in `src/fs/core/ext/transaction.cpp` (and the allocator guard if the design chooses it) so I12 holds for blocks born in the running transaction.
- [ ] E2. ext journal: R1-02, `EndStep` fails with `OutOfMemory` when running and step counts add up past the layer maximum although the distinct blocks fit (`transaction.cpp:684`).
- [ ] E3. ext extents: R1-03, a failure after the extent tree is committed frees data blocks the tree already maps (`extent_write.cpp:772`, `AppendExtentBlock`).
- [ ] E4. ext truncate: R1-04, the tail of the new last block is zeroed in place before the truncation is committed (`write.cpp:1229`).
- [ ] E5. ext rename: R1-05, renaming a directory onto an empty directory in a parent at the link limit gives `Corrupt` and the error state instead of a clean link-limit error (`namespace.cpp:950`).

The full matrix runs after the last task (shared code: the core is built into the kernel).

## Run log
