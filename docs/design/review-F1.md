# Design F1: FAT16 rename keeps the source safe (R1-06) and keeps the target on error (R1-07)

Findings: `docs/reviews/2026-11-review.md` R1-06 and R1-07. Scope: `Fat16Filesystem::Rename` in `src/fs/core/fat/fat16.cpp` and the small helpers it needs. Nothing else changes.

## 1. Root cause

- **R1-06.** With `same_entry` true, Rename calls `MarkDeleted(source)` and only then `CreateEntry`. A crash or an error between the two leaves no entry for the file, and its chain leaks.
- **R1-07.** When a rename replaces the target, `RemoveEntry(existing)` (mark deleted, then `FreeChain`) runs before anything checks that the new name's slot run or alias can be had. A `NoSpace` or `Exists` error therefore destroys the target.

The fix has three parts:
1. Every check that can fail without an I/O error runs before the first write.
2. A respelling rewrites the entry where it already sits, so its cluster reference never moves.
3. A replacing rename swaps the source's data into the target's short entry with one sector write, then cleans up.

## 2. Changes (all in fat16.cpp)

**`struct Found`.** Add `Name name;`, the name `ScanNames` showed: the long name, or else the short one. `FindEntry` fills it in. The no-op check uses it.

**Split `CreateEntry`** into `PlanEntry` followed by `WriteEntryRun`. Its behaviour does not change.

```cpp
struct EntryPlan { ShortName short_name; std::uint8_t case_flags; std::uint32_t long_entries; Slot first; };
// Validates the name, picks the short name or a free BASIS~N alias (ShortNameTaken), and finds the
// free run with FindFreeRun, growing a subdirectory if needed. Its only writes are directory growth.
Status PlanEntry(std::uint32_t directory, std::string_view name, EntryPlan& plan);
// Writes the plan's long-name slots, highest sequence first, then its short entry.
Status WriteEntryRun(const EntryPlan& plan, std::string_view name, std::uint8_t attributes,
                     std::uint32_t first_cluster, Slot& entry_slot, const std::uint8_t* model);
```

Factor the long-name loop into `Status WriteLongEntries(std::uint32_t directory, std::uint32_t first_index, std::string_view name, std::uint32_t count, std::uint8_t checksum)`. It reuses `EncodeLongEntry`, `Checksum` and `WriteRawEntry`.

**Respelling plan.**

```cpp
struct Respelling { bool in_place; std::uint32_t long_entries; ShortName short_name;
                    std::uint8_t case_flags; EntryPlan moved; };
Status PlanRespelling(const Found& entry, std::string_view name, Respelling& plan);
Status Respell(const Found& entry, std::string_view name, const Respelling& plan,
               const std::uint8_t* short_bytes /* attributes, times, cluster, size */);
```

The respelled entry keeps its short name S:
- If `ToShortName(name)` gives exactly S, the entry stays short-only with the new case flags (`long_entries = 0`).
- Otherwise it gets `LongEntryCount(name)` long-name slots, checksummed against S, and case flags 0.

No alias search is needed, so the in-place path cannot fail with `Exists`, and it never makes a second entry with the same short name.

Whether the new name fits in place:
- The entry occupies slots `[f, s]`: f is `first_slot`, s is the short slot, and the old long-name count is k = s − f.
- The new name needs k' long-name slots, ending at s − 1.
- It fits when s − k' ≥ 0 and every slot in `[s−k', f−1]` reads `DeletedEntry`.
- If it does not fit, `PlanRespelling` calls `PlanEntry` for the moved entry: a fresh run and a fresh `~N` alias, which differs from S because S is still taken. This can return `NoSpace` or `Exists`, before any write.

**`Respell` write order, in place:**
1. `MarkDeleted(dir, f, s−1)`, only when k > 0.
2. `WriteLongEntries(dir, s−k', ...)`.
3. Write the short slot with S, the new case flags and `short_bytes` (one `WriteRawEntry`).

Deleting the old slots first matters. The new long-name slots carry the same checksum as the old ones, so overwriting old parts in place could join new and old parts into a valid but garbled name. With slot s−1 deleted first, no chain is complete until the new part 1 is written. `LongNameReader` restarts a chain at each `LastLongEntry`, so leftover old parts are ignored.

**`Respell` when moved:** `WriteEntryRun(plan.moved, ...)`, then `MarkDeleted(old run)`. This is order (a) with a run reserved up front.

**`Rename`.** The checks stay as they are and stay first (dot names, `NameTooLong`, `InvalidArgument`, `OutsideOf`, kind mismatch, `NotEmpty`). Then it branches by case (section 4).

## 3. Invariants

- **I1. The source survives.** At every crash point and every error return, a live short entry points at the source's first cluster with its size, and the source's chain stays allocated. Its visible name is the old name, the new name, or (only in the window of order (b) in place) its short name S.
- **I2. Errors before any write.** Every non-I/O error (`NotFound`, `InvalidArgument`, `NameTooLong`, `NotEmpty`, `IsDirectory`, `NotDirectory`, `NoSpace`, `Exists`) is returned before the first write. The one exception is directory growth during planning, which adds a zeroed cluster to the end of the directory and changes no entry.
- **I3. The target is untouched on error.** A rename that returns an error leaves the target's entry bytes, long-name slots and chain as they were. The one exception is an I/O error at or after the swap write C1, when the rename has already taken effect.
- **I4. At most one shared chain.** After any crash point, at most two live short entries reference the same cluster, and both point to the source's chain with the same data. The target's chain is never referenced twice, and no freed cluster is referenced. Without a journal this shared state cannot be avoided when the reference moves between sectors: the only alternative is a moment with zero references, which would break I1.
- **I5. The target is never half-lost.** After a crash, the target's name names either the target's old data or the source's data. The target's chain is still referenced, or unreferenced but allocated (lost clusters), or freed.
- **I6. No-op.** Renaming onto the identical name (same entry, and `found.name == to_name` byte for byte) returns success with zero writes.

## 4. Write orders and crash states

Wn is the write that completes the step. A sector write is atomic. `WriteFatEntry` writes every FAT copy in turn, so a crash between copies leaves them different (general, not specific to rename).

### (a) Plain rename to a new name (no target)

- **P:** `PlanEntry(to_dir, to_name)`. Errors: `NoSpace`, `Exists`. Nothing has changed.
- **A1..Ak:** new long-name slots. Crash: orphan long-name slots before a free slot, ignored by readers; fsck.fat reports orphaned long-name parts.
- **A(k+1):** new short entry with the source's cluster and model bytes. Crash: the old and new names both name the source's chain (the I4 case); fsck.fat reports the two files sharing clusters.
- **A(k+2):** only for a directory moved to another directory, `SetParent` (the `..` entry). Crash before it: `..` still points at the old parent while both entries exist.
- **A(k+3)...:** `MarkDeleted(old run)`, short slot last. `MarkDeleted` writes slots in ascending order and the short slot is the highest, so the old name stays whole until its last write.

This is the current order with planning made explicit.

### (b) Same-entry respelling (R1-06)

- **P:** `PlanRespelling`. Errors: `NoSpace`, `Exists`, only when the name has to move.
- **In place:**
  - B1: delete the old long-name slots. Crash: the file is visible under S; data and slot intact.
  - B2..B(k'+1): new long-name parts. Crash: an incomplete chain, so the file is still visible under S.
  - B(k'+2): short slot. Crash after it: the new name.

  The cluster reference never moves, so I4 holds trivially. The file's slot, and with it its `NodeId`, stays the same.
- **Moved:** order (a) within one directory. The two entries name the same data, with names that differ only in case.

### (c) Replacing rename (R1-07)

**P.** All checks pass. Then:
- Build `swapped`: the target's short entry, with bytes 11 and 13..31 (attributes, times, cluster, size) taken from the source's short entry. Name bytes 0..10 and case byte 12 stay the target's.
- If `to_name` differs from the target's `found.name`, call `PlanRespelling(target, to_name)`. It may reserve a fresh run.

Errors return here with nothing changed (I2, I3).

**Writes:**
- **C1, swap:** one `WriteRawEntry` puts `swapped` into the target's short slot. Crash: the target's name now names the source's data; the source's old entry also names it (I4); the target's chain is lost clusters (I5). fsck.fat reports the shared clusters and the lost clusters.
- **C2:** `SetParent`, only for a directory moved to another directory.
- **C3:** `MarkDeleted(source run)`. Crash: only lost clusters remain.
- **C4:** `FreeChain(target cluster)`, skipped when the cluster is 0. Crash midway: the rest of the chain stays lost.
- **C5:** when a respelling was planned, `Respell(target slot, to_name, plan, swapped)`, the writes of (b).

C5 can no longer fail for space: its run was reserved in P, and C1..C4 only free slots. The driver runs one change at a time, as today.

## 5. Edge cases

| Case | Result |
| --- | --- |
| The new name has to move and no run fits (root full, subdirectory at its 65536-entry cap, or disk full) | `NoSpace`, nothing changed |
| All `~1..~9999` aliases taken (move path only) | `Exists`, nothing changed |
| Target is a non-empty directory | `NotEmpty`, nothing changed |
| Kinds differ | `IsDirectory` / `NotDirectory` |
| Source and target in different directories | (a) or (c), planned in `to_dir`; `same_entry` cannot hold across directories |
| Renaming a directory | `..` is updated only when it moves to another directory (A(k+2)/C2); a respelling never touches `..` |
| Directory moved into its own subtree | `InvalidArgument` (`OutsideOf`) |
| Identical name | no-op, zero writes (I6) |
| Case-only change, or the source's own alias (`README~1.TXT`) | (b); the alias is S itself, so the result is short-only and the long-name slots are deleted |
| I/O error after the first write | `Io`; I1 holds, and I3 holds before C1 |

Update the comments at the old lines 1737 and 1757 to describe the new orders. The CLAUDE.md contract ("a crash can lose the target, never the source") now holds as written.

## 6. Tests

New file `src/fs/core/tests/fat_rename_crash_tests.cpp`, plus additions to `fat_tests.cpp` and `fat_mtools_tests.cpp`.

**Harness.** A local one, since `crash_helpers` is ext-only.
- Reference run: on a copy of a prepared `Fat16Image` (or the mtools image), count the device's writes during `Rename`, giving N.
- For each cut in 0..N: copy the base, `FailWritesAfter(before + cut)`, mount, run `Rename` (it returns `Io` or success), drop the mount, remount the copy read-only and run the oracles.
- **In-core oracle:** a new helper `FatOwners(MemoryDevice&)` in `fat_image.cpp`. It walks every directory, follows the FAT, and returns each cluster's owners, the lost clusters and the live short names per directory.
- **Outside oracle** (in the `FEATHERDISK_MTOOLS_IMAGE` build): `mtype` reads the expected contents and `mshowfat` lists each file's clusters. When found, also run `fsck.fat -n`: add an optional `find_program(FEATHERDISK_FSCK_FAT fsck.fat)` to `src/fs/core/CMakeLists.txt` and skip it when absent.

**Tests:**
- **T1, (a) crash at every write.** The source's data is reachable under the old or new name; the only shared clusters are the source's chain; at most orphan long-name slots. Checked with `mtype`/`mshowfat`. Covers I1, I4.
- **T2, (b) in place, crash at every write.** Cases `readme.txt`→`Readme.txt` (k 0→1), `A much longer name.txt`→`a MUCH longer name.txt`, and `Readme.txt`→`README~1.TXT`. The file is always visible under the old name, the new name or S; no cluster has two owners at any cut; the short slot index never changes. Covers I1, I4. On the current code it fails: the file is missing after the first write.
- **T3, (b) moved, crash at every write.** The slot before the entry is live and the new name needs more slots. Same checks as T1. Covers I1, I4.
- **T4, (c) crash at every write.** A file onto a file, an empty directory onto an empty directory, and a move to another directory. At each cut, the target's name holds the old or the new data, the source's data is reachable, and the target's chain is owned by at most one entry. Run once with a respelling that fits and once with one that moves. Covers I1, I4, I5.
- **T5, R1-07 reproduction.** A full root directory with no room to grow, holding the target and the source, where the replacing rename needs a long-name run that cannot be had: expect `NoSpace`, the target's entry bytes identical, its chain allocated, and zero writes. On the current code the target is destroyed. Covers I2, I3.
- **T6, R1-06 reproduction.** In a full root, `readme.txt`→`Readme.txt` where in place does not fit: `NoSpace` and the device unchanged. Where it fits: success. Covers I1, I2.
- **T7, errors before writes.** For `NotEmpty`, `IsDirectory`, `NotDirectory`, `Exists` (aliases exhausted) and moving a directory into itself, the write count is unchanged. Covers I2, I3.
- **T8, identical name.** `Rename(x, x)` makes zero writes. Covers I6.
- **T9, I/O error at each write index.** `FailWritesAfter` on a live mount, then reads on that same mount: for (a)-(c) the source's data is readable; before C1 the target is unchanged. Covers I1, I3.
- **T10, mtools round trip.** A case-change rename, a replacing rename with a mixed-case name and a directory moved to another directory: `mdir` shows the new spellings and `mtype` reads the data back; `fsck.fat -n` is clean when installed. Covers I4 (no sharing in the final state).
- **Existing tests.** The replace cases in `fat_tests.cpp` (around lines 399 and 440, the `B.TXT` spelling after a replace) still pass.

**Invariant → tests that fail when it is broken:** I1 → T1, T2, T3, T9. I2 → T5, T6, T7. I3 → T5, T7, T9. I4 → T1-T4, T10. I5 → T4. I6 → T8.
