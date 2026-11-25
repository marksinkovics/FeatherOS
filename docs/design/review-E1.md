# Design E1: a block freed and then written again as metadata in the same step (R1-01)

Finding: `docs/reviews/2026-11-review.md` R1-01. Base design: `docs/design/M22-P6-s4.md` section 6.4, invariants I11-I23.

## Problem

`NoteFreedBlock` (`src/fs/core/ext/transaction.cpp:562`) drops the block's step copy. When running or committed holds a copy, it also adds the block to `pending_forgets`, and nothing ever removes an entry from that set.

The allocator only masks the committed bitmap in frozen groups (`allocation.cpp:355-375`). So a block X born in the running transaction T and freed in step S is clear in both bitmaps and can be handed out again inside S. `AllocateTreeBlock`, whose goal is the inode group's first block, finds the lowest clear bit, which is X. `WriteTreeNode(X)` then puts the new node into the step layer.

`EndStep` (688-697) then runs `step.Drop(X)` and `running.Drop(X)` for every pending forget. The new node ends up in no layer while the committed parent index points at X. Reads of that part of the file fail with `Corrupt`, and after the commit the volume is corrupt on disk.

## Decision: option B, applied in `EndStep`

**The rule:** a pending forget gives way to a step copy written after the block's last free. In the `EndStep` loop:

```cpp
const std::uint64_t block = state.pending_forgets.At(index);
if (state.step.Find(block) != nullptr) { continue; } // written again after its last free
state.step.Drop(block); state.running.Drop(block); // ...revoke as today
```

**Why the test is exact:** `NoteFreedBlock` always drops the step copy at the time of the free. A step copy present at `EndStep` was therefore written after the latest free and is the block's current contents. `Absorb` puts it over the stale running copy.

**What happens to the revoke:** a revoke for X is created only when committed holds X, and that cannot happen for a re-allocated block:
1. The allocator hands out X only if X's bit is clear in the committed bitmap.
2. A committed copy exists only for a block that was live in a committed transaction.
3. A later committed free drops that copy at its commit (`transaction.cpp:1004-1006`).

So a kept block never carries a revoke in the same transaction, and replay never skips the new copy. This is jbd2's "cancel revoke on re-journal", reached here by construction.

**Guard:** `EndStep` adds a check in its "everything that can fail" section, before anything in the running transaction changes. A pending forget held by both step and committed makes it return `Error::Corrupt`; the normal step failure path rolls back and `NoteError` acts.

**Why not option A** (an allocator that skips blocks born and freed in T):
- It needs a new per-transaction set that grows with every forget, is rolled back on abort and cleared at commit.
- Every allocation in a frozen group would have to mask that set into its search bitmap.
- On a nearly full volume it adds `NoSpace` failures, each costing a commit and a retry.
- It leaves the `EndStep` defect in place for any other path that writes a block after freeing it in the same step.

Linux ext4 holds freed blocks until commit (closer to A) and needs jbd2's cancel-revoke too. Here the revoke side comes from the existing committed-bitmap guard, and option B is a two-line change where the bug is.

**Why in `EndStep` and not in `WriteMetadata`** (a `BlockSet::Remove`):
- No new `BlockSet` API (it has no erase today).
- No ordering trap: free, re-allocate, free again keeps the entry automatically.
- Re-use as data keeps the forget, because data never enters the step layer.

## Files

- `src/fs/core/ext/transaction.cpp`: `Status Ext2Filesystem::EndStep()` gets the guard and the `continue` (signature unchanged); rewrite the comment at the top of the file (lines 14-16).
- `docs/design/M22-P6-s4.md`: section 6.4 step 2 becomes "Each pending forget the step did not write again leaves step and running"; restate I11 and I12 as E-I4 below; add the new tests to the invariant table.
- `src/fs/core/tests/crash_scenarios.{h,cpp}`: `Scenario ReuseScenario(std::uint32_t block_size);` and a helper that builds the near-full leaf.
- `src/fs/core/tests/ext_transaction_tests.cpp`: T1, T3-T6. `src/fs/core/tests/ext_transaction_crash_tests.cpp`: T2.
- `docs/reviews/2026-11-review.md`: R1-01's status.

## Invariants

- **E-I1, the re-write wins:** a block the step holds at `EndStep` joins running with the step's bytes, even if it is in `pending_forgets`.
- **E-I2, the forget still applies otherwise:** a pending forget the step did not write again leaves step and running; if committed holds it, it gets a revoke (I11 as before).
- **E-I3, no revoke and copy together:** no transaction both revokes a block and logs a copy of it. A pending forget held by both step and committed fails the step with `Corrupt` before any change.
- **E-I4 (I12 restated), no early re-use of live blocks:** a block set in the committed bitmap and freed in T is not allocated before T commits. A block born in T may be re-used within T, in the same step or a later one.
- **E-I5, freed metadata never shadows data:** data written to a block freed in the same step is never overwritten by a layer or log copy (I22).
- **E-I6, abort leaves running unchanged:** after `AbortStep`, running, revokes and `freed_groups` are exactly as before the step.

## Write and commit order, and what a crash leaves

The scenario is one transaction T: build the file (its leaf X is born in T), then one `Write` that replaces X with a new leaf N, frees X, splits N and gets X back as the left half.

1. **During the step:** data for the holes goes home at once; the bitmap, descriptor, X, N, R and the inode go into the step layer. Crash: replay leaves the last committed state, where X and the new data blocks are free, so there is no stale fill.
2. **At `EndStep`:** X's new copy joins running (E-I1); nothing is written. A crash gives the same result as point 1.
3. **Commit:** the descriptor and the copies (X, N, R, the root inode, bitmap, group descriptor, superblock) go to the log, then a flush, the commit block and a second flush.
   - Crash before the commit block is durable: T is ignored, leaving the previous state.
   - Crash after it: replay writes X with the new left half; tree, bitmap and counts agree and `e2fsck -fn` passes. On the current code the root points at X, X is not in the log and its home holds zeros, so e2fsck reports a bad extent header.
4. **Checkpoint (K1-K4):** a crash in the middle keeps the log valid and replay rewrites the homes; a crash after `s_start` is cleared finds the homes already right.

## Edge cases

- **Abort of the step:** `AbortStep` clears the step and `pending_forgets`. Running keeps the old copy of X and the bitmap rolls back with the layers, so X belongs to the old leaf again (E-I6). If X had been re-used as data, its home was overwritten, but running still holds the leaf copy, which is logged and checkpointed over it, and every read looks in running first.
- **Commit between the free and the re-allocation:** commits happen only in `BeginStep` and `FailStep`, between steps, and S's `EndStep` has already dropped X from running. If X was born in T, it is free in T's committed state and needs no revoke. If X was live before T, its revoke is in T and it is re-used only in T+1, whose higher sequence makes replay apply the new copy.
- **Freed in an earlier step of the same transaction:** works today; X leaves running at that step's `EndStep` and the later copy is new (regression test T4).
- **Freed, re-used and freed again in one step:** the second free drops the step copy again, so the forget applies; if the last owner writes it again as metadata, it is kept.
- **Frozen groups:** masking is unchanged. X is clear in both bitmaps, which is why re-use happens; E-I4 limits re-use to blocks born in T.
- **Data blocks** never enter a layer. Data born and freed in T and re-used as data or metadata is safe today (no committed reference, no forget). Metadata re-used as data in the same step: the step holds no copy, so the forget still drops the stale running copy (E-I5), and no commit can happen inside a step, so the stale copy is never logged.
- **Revoke records:** covered by E-I3. `Footprint` still counts every pending forget as a revoke; that over-counts harmlessly (related to R1-02, not changed here).

## Acceptance tests

- **T1, reproduces the finding (fails on the current code).** For ext4 with 1 KiB blocks (leaf capacity 84) and with 4 KiB blocks (340), no Sync in between: create a file and write single blocks at logical 1, 3, 5, ... until leaf X holds capacity − 1 extents, then one `Write` covering logical 0..2 (filling holes 0 and 2). `ReadAll` must match the model before the Sync; on the current code it fails with `Corrupt`. Then Sync, unmount, remount, `ReadAll` again and `CheckClean` (`e2fsck -fn`). Precondition: a twin run that Syncs after the build finds X with `RunDebugfs` "ex", and the final tree's left-split leaf must be X, so the test cannot pass vacuously.
- **T2, crash at every write.** `ReuseScenario` = {`Together` (build + spanning write), `Put` into another hole, `Rm` the file, `Make` a new file}. `RunReference`, then for each cut in `CutsBetween(reference.mounted, reference.total, SIZE_MAX)`: `RunCrashed`, `ReplayBothWays` (our replay and e2fsck as the outside oracle), `RequireCommittedState`, `RequireNoStaleFill`, `RequireLossyRecovery`. Guarded by `FEATHERDISK_EXT_IMAGES` like the existing crash tests. Fails on the current code: the reference run already hits `Corrupt`, and replay after the commit fails e2fsck.
- **T3, no transaction both revokes and logs a block.** After the T1 and T2 reference runs, walk the log with the decoders the revoke test uses (`ext_transaction_tests.cpp:836`); in every transaction the revoked blocks and the tagged blocks must not overlap.
- **T4, free in one step, re-use in a later step of the same transaction.** As T1, but fill the two holes with two separate `Write` calls before the Sync. Passes before and after the fix; a regression test for E-I4.
- **T5, abort after the re-use.** T1's build, then `LimitJournal` lowers `hard` so the spanning write overflows alone: expect `NoSpace`, the file reads back exactly as built, and after a remount that lifts the limits, Sync and `CheckClean` pass.
- **T6, metadata re-used as data in the same step.** T1's build, then `FillVolume` so that after insert 1 the next hole's data run can only take X. Precondition: debugfs shows a data extent whose physical block is X. Sync, unmount (which checkpoints), remount: the data reads back right and `CheckClean` passes. Fails if the fix cancels forgets on allocation instead of on a metadata write.

## Invariant → tests that fail when it is broken

| Invariant | Tests |
| --- | --- |
| E-I1 | T1, T2 |
| E-I2 | T6, the existing test at `ext_transaction_tests.cpp:836` |
| E-I3 | T3, T2 (e2fsck replay) |
| E-I4 | the existing test at `ext_transaction_tests.cpp:939`, T4 |
| E-I5 | T6, the existing I22 tests (T9, T25 of the M22 design) |
| E-I6 | T5 |
