# A2 design: user copies racing a concurrent unmap (R2-01, R3-01)

Status: design, from reading the code.

## 1. What is wrong today

- `src/kernel/process.cpp` has `UserByte` (l.853), `UserRangeWritable`/`UserRangeReadable` (866/882), `CopyFromUser`/`CopyToUser` (898/912), `UserPagesReadable` and `CopyFromUserPaged` (927/954), `CopyStringFromUser` (976) and `CopyEnvironmentFromUser` (~1030).
  - They look up a user page with `Memory::TranslateUserPage`, which takes no lock, then use the page through the direct map (`PhysicalBytes`) with no lock and no pin held.
  - `CopyToUser`/`CopyFromUser` look the page up again for each byte and use the result without checking it. If the page is gone, `UserByte` returns nullptr and the kernel writes to address 0 in ring 0. That is R2-01.
- `ShrinkHeap` (l.1287) and `ReleaseStackSlot` (l.1330) clear the page entries under `program.memory_lock`, drop the lock and call `FreeUnmappedFrames` (l.1271): `Memory::ShootDownUserMappings`, then `FreePageFrame`.
- On AArch64, `ShootDownUserMappings` is empty (`arch/aarch64/virtual_memory.cpp:491`). Its `InvalidatePage` (`dsb ishst; tlbi vaae1is; dsb ish; isb`) already removes stale user translations from every CPU, but nothing stops a direct-map pointer that a copier got just before the unmap, so the copier can use the frame after it is freed. That is R3-01.
- The real gap on both architectures is the kernel's own direct-map access, not the TLB.
- A fault in kernel mode cannot be recovered today (`ReportFatalException` on both architectures).

## 2. Approach: copy one page per hold of the memory lock

Every user copy works one page at a time: take `program.memory_lock`, look the page up, `memcpy` at most one page through the direct map, drop the lock. No direct-map pointer outlives the lock hold that produced it. An unmap happens either before a page's hold (the copy sees the page missing and returns `EFAULT`) or after it (nothing still points at the frame). The free always happens after the unmapper dropped the lock, so it is safe on both architectures whatever the TLB does.

Rejected:
- A fault-tolerant copy with an exception fixup table: every copy through user virtual addresses inside SMAP/PAN windows, a fixup table, kernel-mode fault recovery in both arch handlers and entries, and stack growth in the fault path (overlaps C3, R2-06). Much more code.
- Pinning frames: needs a per-frame reference count, which the allocator does not have.

Concerns:
- Long copies from disk: every caller already reads the file into a kernel buffer before copying to the user, so only a memcpy of at most 4 KiB runs with the lock held (interrupts off).
- Demand-grown stacks: the copy never faults (direct map). An untouched stack page still gives `EFAULT` (R2-06, task C3). The resolver may map a page under the lock, which is C3's hook.
- Deadlock: the lock is near a leaf (I9); shootdowns run with no spinlock held (I4), so a CPU spinning on `memory_lock` with interrupts off delays an x86 IPI acknowledgement only by bounded work.

## 3. Files, types and signatures

**New: `src/kernel/memory/user_copy.h` and `.cpp`** (namespace `FeatherOS::Memory::UserCopy`), shared code, also host-built.

```cpp
// Resolves one user page to its kernel (direct-map) address; nullptr when it is not mapped for the
// user, or not writable and write is set. Called with *Target::lock held: no sleep, no wait for
// another CPU, no kernel-heap allocation; it may map a page (C3's stack growth).
using ResolvePage = volatile uint8_t* (*)(void* context, uint64_t page_address, bool write);
struct Target { Sync::SpinLock* lock; void* context; ResolvePage resolve; };

bool RangeMapped(const Target&, uint64_t address, uint64_t size, bool write); // pre-check, per page
bool CopyIn(const Target&, uint64_t address, void* destination, size_t size);   // user -> kernel
bool CopyOut(const Target&, uint64_t address, const void* source, size_t size); // kernel -> user
// Copies the longest mapped prefix (at most capacity bytes); returns its length.
size_t CopyInPrefix(const Target&, uint64_t address, void* destination, size_t capacity);
// 0, User::SyscallErrorBadAddress, or User::SyscallErrorInvalidArgument (no terminator in capacity).
uint64_t CopyStringIn(const Target&, uint64_t address, char* buffer, size_t capacity);
```

- Each page is handled inside `Sync::SpinLockGuard guard(*target.lock)`: resolve, then `memcpy` of `min(size left, room left in the page)`.
- Size 0 returns true without calling the resolver. A range that wraps past `UINT64_MAX` returns false.
- Add the `.cpp` to the kernel's sources and to `tests/unit/CMakeLists.txt` (next to `src/kernel/futex.cpp`).

**`src/kernel/process.h`:** `UserProgram::memory_lock` becomes `mutable Sync::SpinLock`; the copy helpers take `const ProcessRecord&`. Optionally put `stack_slots` under `GUARDED_BY(memory_lock)`.

**`src/kernel/process.cpp`:**
- Add `volatile uint8_t* ResolveUserPage(void* context, uint64_t page, bool write)` (`context` is `UserProgram*`: `Memory::TranslateUserPage`, the writable check, `PhysicalBytes`) and `Memory::UserCopy::Target CopyTarget(const ProcessRecord&)`.
- Delete `UserByte`, so nothing hands out a direct-map pointer to a user page.
- `CopyFromUser` → `CopyIn`, `CopyToUser` → `CopyOut`.
- `UserRangeWritable`/`UserRangeReadable`/`UserPagesReadable` → `RangeMapped`: early checks only (one lookup per page), so a bad buffer fails before file data is consumed.
- Remove `CopyFromUserPaged`; `PresentFramebuffer` (~l.3294/3307) uses `CopyIn`.
- `CopyStringFromUser` → `CopyStringIn`.
- `CopyEnvironmentFromUser` → `CopyInPrefix`, then the existing double-NUL scan on the kernel buffer; no end marker and a prefix shorter than the buffer gives `EFAULT`.
- `ReadUserWord` (futex) keeps calling `CopyFromUser`: `memory_lock` nests inside `futex_lock`.
- Unchanged: `CopyToCurrentUser` (l.3766, stack-growing loop then `CopyToUser`); `WriteProgramBytes` (l.231) and the arguments page (l.461), which write an address space no thread runs yet (I8); `SetUpThreadArea` (l.355), out of scope.

**`src/kernel/arch/aarch64/virtual_memory.cpp`, `ShootDownUserMappings`:** `{}` becomes `asm volatile("dsb ish" ::: "memory");`, with a comment: completing the broadcast invalidates `UnmapUserPage` issued means every processor has dropped the translation and finished the accesses that used it; kernel direct-map users are covered by the memory lock. It stays correct if invalidates are later batched without a per-page `dsb ish`.

**`src/kernel/arch/x86_64/virtual_memory.cpp`, `UnmapUserPage`:** the comment at ~l.446 ("enough while a process runs on one CPU at a time") is out of date; point it to `ShootDownUserMappings`.

**`src/kernel/memory/virtual_memory.h`:** the contract on `UnmapUserPage`/`ShootDownUserMappings` says the kernel reads and writes user pages only through `UserCopy`, holding the process's memory lock, so a frame may be freed after the shootdown once the unmapper has dropped the lock.

## 4. Data layout

No new structures. `Target` is three words on the stack; its lock is the existing `UserProgram::memory_lock`.

## 5. Invariants

- **I1.** The kernel reads or writes a live process's memory only through `Memory::UserCopy`. A direct-map pointer derived from a user translation is used only inside the `memory_lock` hold that resolved it, never returned or stored.
- **I2.** Every unmap from a live address space (`ShrinkHeap`, `ReleaseStackSlot`) clears the entry under `memory_lock` (already true).
- **I3.** Order: clear the entry (under `memory_lock`), release `memory_lock`, `ShootDownUserMappings` completes on every CPU running the process, then `FreePageFrame`. x86-64: local `invlpg`, then an IPI to every CPU whose `active_root_physical_addresses` matches, waiting for each acknowledgement. AArch64: `tlbi vaae1is` + `dsb ish` in `InvalidatePage`, plus `dsb ish` in `ShootDownUserMappings`.
- **I4.** `ShootDownUserMappings` is never called with a spinlock held. Nothing under `memory_lock` waits for another CPU.
- **I5.** Under `memory_lock` the code only looks up a user page; calls `MapPage`/`UnmapUserPage`/`ProtectPage` (`page_table_lock`); calls the page allocator; `memcpy`s at most one page between a user frame and an always-mapped kernel buffer. No sleep, fault, logging, device I/O or kernel-heap allocation.
- **I6.** A copy looks each page up again under the lock. An unmapped page (or never mapped, or read-only for `CopyOut`) makes the copy return false and the syscall return `EFAULT`; earlier pages may already be copied.
- **I7.** Size 0 succeeds at any address. A wrapping range fails.
- **I8.** Address spaces no thread runs yet may be written without `memory_lock`. `DestroyAddressSpace` runs only after every thread is complete (thread lifetime: C3).
- **I9. Lock order.** `futex_lock`, `process_lock`, `table_lock` or an object's lock may be held when `memory_lock` is taken. Under `memory_lock` only `page_table_lock`, then the page allocator. No holder of `memory_lock` takes `futex_lock`, `process_lock`, `table_lock`, `scheduler_lock` or a volume lock. Documented in `docs/development.md` ("Threads", ~l.537, and next to "Lock annotations").

## 6. Edge cases

| Case | Result |
|---|---|
| Copy reaches an unmapped page (never mapped, removed by a concurrent `sbrk(-)`, a joined thread's stack) | `EFAULT` |
| `CopyOut` to a read-only page | `EFAULT` |
| Address at or above the user limit, kernel address, wrapping range | `EFAULT` |
| Size 0, including at address 0 | success |
| `read` whose copy fails after `Files::Read` consumed data | `EFAULT` (unchanged, l.1735); pipe or terminal data already taken is lost |
| Early check passed, page then unmapped | `EFAULT` from the copy |
| String terminated in the mapped part, next page unmapped | success |
| String with no terminator before an unmapped page | `EFAULT` |
| String with no terminator within capacity | `EINVAL` (or `ENAMETOOLONG` where the path layer maps it today) |
| Untouched stack page (R2-06) | still `EFAULT`; C3 gives a resolver that calls `MapStackPageLocked` |
| Futex word unmapped while read | `EFAULT` from `Futex::Wait` |

## 7. Tests

Host tests in `tests/unit/kernel/memory/user_copy_tests.cpp`, with a fake address space of N pages backed by host arrays and a mapped/writable bit per page.

- **T1.** `CopyIn`/`CopyOut` give the right bytes inside one page, across two and three pages, at odd offsets.
- **T2.** Middle page unmapped: false; page 0 copied; resolver never called for page 2 and page 2 untouched.
- **T3.** `CopyOut` to a read-only page: false; `CopyIn` from it: true.
- **T4.** Size 0 at address 0: true, no resolver call. Wrapping range: false. `RangeMapped` calls the resolver once per page.
- **T5.** The fake resolver checks `target.lock->IsLocked()` on every call. A variant resolver unmaps page 1 while resolving page 0 (a `ShrinkHeap` between two holds): the copy returns false and never writes page 1.
- **T6** (`[concurrency]`, also `./build.sh unit-test-tsan`). Thread A loops `CopyIn`/`CopyOut` on page P. Thread B loops: lock, mark P unmapped, unlock; fill P's backing with poison (plain writes); lock, refill with the pattern, remap, unlock. `CopyIn` never returns true with poison in the data; TSan reports any access to P outside the lock.
- **T7.** `CopyInPrefix` returns the mapped prefix length. `CopyStringIn`: terminator found; terminator past an unmapped page (`EFAULT`); capacity full without a terminator (`EINVAL`).

QEMU stress, both architectures, 4 CPUs, in `src/user/hardening.cpp`:

- **T8. Copies racing a heap shrink.** Open `/dev/zero` and `/dev/null`, start the reader thread, align the break to a page, P = `sbrk(0)`. Main loops `sbrk(4096); sbrk(-4096)` up to 2000 times or 3 s. The reader loops until a stop flag: `read(zero, P+1024, 512)` and `write(null, P+1024, 512)`, and the same pair at `P-256, 512` (crossing from the mapped page below into P). Each result is 512, or -1 with `errno == EFAULT`; anything else is bad. The reader never touches P from user mode; no `malloc` in the loop; thread creation before the alignment. Pass: `bad == 0` and the loop finished.
- **T9. Copies racing a stack release.** 200 times: thread T publishes the address of a local 512-byte array and returns; the reader loops `write(null, addr, 512)`; main `pthread_join`s T (`ReleaseStackSlot`). Only `write`, so a reused slot is only read. Each result 512 or `EFAULT`.
- Both through the existing `Check()`. `tools/smoke/cases/shell/limits.py`: `"hardening: 69 checks passed"` → `71`. A panic ends the session through `PANIC_MARKER`.
- Matrix: no new entry; `limits` runs in the existing `*Both(21, "shell_programs")`, `*Both(25, "shell_with_cpu_protections", QEMU_CPU=max)` (SMAP/PAN) and `*Both(37, "shell_release")`, with `QEMU_CPU_COUNT = 4`. About 3 s per check.
- **T10.** The existing `hardening` check `freed-heap-threads` still passes on both architectures; smoke `SMP SHELL MEMORY` passes.

## 8. Which test fails when an invariant breaks

| Invariant | Caught by |
|---|---|
| I1 | T5, T6 for the helper; deleting `UserByte` makes the old pattern fail to compile; a new raw `PhysicalBytes` use in `process.cpp` only by review. T8/T9 catch the x86-64 null write only by chance. |
| I2 | Review (already true); T8 by chance. |
| I3, TLB side | T10 on both architectures. |
| I3, AArch64 free side | T6 (host). On QEMU, AArch64 reuse of a freed frame is silent; review. Removing the new `dsb ish` fails no test while `InvalidatePage` keeps its own: review only. |
| I4 | Review; a violation hangs T8/T9 (smoke timeout). |
| I5 | Review (thread-safety analysis cannot annotate the resolver pointer). |
| I6 | T2, T5, T8, T9. |
| I7 | T4, plus hardening's zero-length checks. |
| I8 | Review (C3). |
| I9 | Review and the lock-order paragraph; reversed order shows only as a hang. |

## 9. Docs

`docs/development.md`: "Threads" (l.537): copies take the memory lock one page at a time, AArch64's `ShootDownUserMappings` completes the broadcast invalidates; the lock order (I9); hardening count 69 → 71 (l.615); fix the stale "AArch64 needs nothing more" wording.

## 10. Decisions (coordinator)

1. **AArch64 shootdown.** A broadcast `tlbi vaae1is` completed by `dsb ish` is a full shootdown by the architecture; no IPI. The use-after-free was the direct-map access, which the memory lock covers.
2. **Stress test location.** In `hardening`, run by the existing SHELL matrix cases on both architectures (including SMAP/PAN), not a new program.
3. **Partial copies.** A copy failing part-way after data was consumed keeps returning `EFAULT` (unchanged), not a partial count as Linux does for `read`.
