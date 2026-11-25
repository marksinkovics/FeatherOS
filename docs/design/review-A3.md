# A3 design: user-mode entry and signal frames (R3-02, R3-03, R3-08, R3-09)

Status: design, from reading the code at `c2757ae` (A2 in).

## 1. What is wrong today

- **R3-03 (reproduced).** `Signals::SetAction` (`src/kernel/signals.cpp:121`) accepts any `handler > SignalIgnore` when the restorer is non-zero. x86-64 `EnterHandler` (`arch/x86_64/exceptions.cpp:402`) writes the handler into the `iretq` frame unchecked. On Intel, `iretq` to a non-canonical RIP raises #GP in ring 0 after `swapgs` (`exceptions.asm:206`), and the kernel dies. AArch64 `EnterHandler` (`arch/aarch64/exceptions.cpp:209`) lacks the check too, harmlessly (the fault is taken at EL0).
  - AArch64 `ReturnFromSignal` checks its own `UserAddressLimit = 1ULL << 48` (`aarch64/exceptions.cpp:116`) while `Process::UserAddressLimit` is `1 << 47`, so a doctored PC in `[2^47, 2^48)` fails only later, at EL0. x86 `exceptions.cpp:332` keeps a third copy of the constant.
- **R3-02.** `x86_64_enter_user` (`exceptions.asm:47-72`) runs with IF=1 across `call FeatherOS_x86_64_SetKernelStack` (reads the CPU index, then stores: a migration in between writes another CPU's `TSS.rsp0`) and across `swapgs; iretq` (an IRQ there enters with a kernel CS and the user GS base). The user RFLAGS comes from `pushfq`, which is why IF had to be 1. The common return path (`exceptions.asm:184-208`) is safe only if every handler returns with IF=0, which nothing enforces, and `GetUserReturnStack` writes `rsp0` there too.
- **R3-08.** `aarch64_enter_user` (`exceptions.S:42-62`) writes `SPSR_EL1`/`ELR_EL1` with IRQs unmasked. A second window: between `msr SPSel, #0` and `msr SPSel, #1` SP is the user's stack, so an IRQ there is taken on SP_EL0 and `aarch64_irq_common` pushes 272 bytes onto the user stack (or faults under PAN). The return paths (`exceptions.S:103-107`, `153-157`) also depend on handlers leaving IRQs masked. The unexplained AArch64 "EL1 jumped to a user address" panic in `docs/ideas.md:117` (ESR 0x8600000F, ELR = a user image address, `-cpu max`) matches this clobber.
- **R3-09.** x86 `EnterHandler` declares `SignalFrame frame;` (`exceptions.cpp:378`) uninitialised. Bytes 168-175 (padding before the `alignas(16)` FP state) and FXSAVE bytes 464-511 (frame offsets 640-687) are never written, and all 688 bytes go to the user stack. The copy already goes through A2's `CopyToCurrentUser` → `CopyToUser` → `Memory::UserCopy::CopyOut`; sigreturn reads through `CopyIn`.

## 2. Approach

1. **Check the handler where it enters the kernel.** `sigaction` refuses a handler (or, when a handler is set, a restorer) at or above the user address limit with `EINVAL`, as the `[R3-03]` test requires. The arch frame builders check again (a bad value gives SIGSEGV), and sigreturn uses the one shared limit. No check on every return: I1 lists every source of RIP/ELR/RFLAGS/SPSR/CS/SS, each validated, hardware-saved from user mode, or a kernel constant.
2. **Interrupts masked in the asm, not by handler discipline.** x86: `cli` as the first instruction of `x86_64_enter_user`, user RFLAGS forced to `0x202`, and `cli` right after `call FeatherOS_x86_64_ExceptionDispatch`, so `rsp0`, `swapgs` and `iretq` always run masked. AArch64: `msr daifset, #2` first in `aarch64_enter_user` and after each dispatch call, before `ELR`/`SPSR` are reloaded.
3. **Zero the whole frame** (`memset`) before filling it, on both arches.

Rejected: Linux's accept-then-SIGSEGV for R3-03 (the repro test pins rejection at `SetAction`; the frame-build check still gives SIGSEGV for any other path); an `iretq` fault fixup after `swapgs` (needs kernel-mode fault recovery the kernel lacks).

## 3. Files and changes

**`src/kernel/signals.h`:** `constexpr uint64_t HandlerAddressLimit = 0x0000800000000000;` (handlers and restorers must lie below the start of the kernel half on both architectures). The `SetAction` comment: `SyscallErrorInvalidArgument` also for a handler, or a handler's restorer, at or above it.

**`src/kernel/signals.cpp` `SetAction`:** add to the existing refusal, before `old_action` is written: `new_action->handler > SignalIgnore && (new_action->handler >= HandlerAddressLimit || new_action->restorer >= HandlerAddressLimit)`. `SIG_DFL`/`SIG_IGN` with any restorer still work. Signature unchanged.

**`src/kernel/process.h`:** after the arch `#if` block, `static_assert(Signals::HandlerAddressLimit == UserAddressLimit);`.

**`src/kernel/arch/x86_64/exceptions.cpp`:**
- Delete the local `UserAddressLimit` (l.332); use `Process::UserAddressLimit`.
- `EnterHandler`: first `if (decision.action.handler >= Process::UserAddressLimit) return false;` (the caller turns `false` into `TerminateForSignal(SIGSEGV)`); then `memset(&frame, 0, sizeof(frame));` before any field, `FloatingPoint::Save` after it.
- `static_assert`s: `offsetof(SignalFrame, instruction_pointer) == 144`, `flags == 152`, `stack_pointer == 160`, `floating_point == 176`, `sizeof(SignalFrame) == 688` ("hardening.cpp doctors this layout").
- `FeatherOS_x86_64_GetUserReturnStack`: when no exit is requested, `RestoreCurrentUserKernelStack()` before `return 0`; the asm now calls it with IF=0, so it is the one `rsp0` write before every `iretq`. Remove the redundant `RestoreCurrentUserKernelStack()` calls in `ExceptionDispatch` (l.497, 505, 513, 532) and `DeliverSignals` (l.429).
- `FeatherOS_x86_64_SetKernelStack`: unchanged; its contract (interrupts off) stated in the asm comment.

**`src/kernel/arch/x86_64/exceptions.asm`:**
- `x86_64_enter_user`: `cli` first (the `rsp0` store must reach this CPU's TSS, and an interrupt after `swapgs` would enter with the user GS base); `pushfq` becomes `push qword 0x202` (IF, IOPL 0, reserved bit 1).
- `x86_64_exception_common`: `cli` between `call FeatherOS_x86_64_ExceptionDispatch` and `call FeatherOS_x86_64_GetUserReturnStack`.

**`src/kernel/arch/aarch64/exceptions.cpp`:** delete the local `UserAddressLimit = 1ULL << 48`, use `Process::UserAddressLimit` in `ReturnFromSignal`. `EnterHandler`: the same handler check, then `memset(&frame, 0, sizeof(frame))`. `static_assert`s: `offsetof(SignalFrame, stack_pointer) == 264`, `program_counter == 272`, `pstate == 280`, `floating_point == 288`, `sizeof == 816`.

**`src/kernel/arch/aarch64/exceptions.S`:**
- `aarch64_enter_user`: `msr daifset, #2` first (an IRQ before `eret` would overwrite `SPSR_EL1` and `ELR_EL1`); `msr SPSel, #0; mov sp, x1; msr SPSel, #1` becomes `msr SP_EL0, x1` (SPSel is always 1 here), removing the window where SP is the user stack.
- `aarch64_exception_common`: `msr daifset, #2` after `bl FeatherOS_AArch64_ExceptionDispatch`.
- `aarch64_irq_common`: `msr daifset, #2` after `bl FeatherOS_AArch64_InterruptReturn`.
- `ReturnToKernel`'s EL1h return keeps the user's DAIF as today.

**`src/user/hardening.cpp`:**
- Per-arch `asm()` restorer stubs in the style of `src/libc/arch/*/signal_return.cpp`: x86 `hardening_doctor_restorer`: `lea rdi,[rsp-8]; call hardening_doctor_frame; mov eax,34; int 0x80; ud2` (at the restorer `rsp` is frame + 8); AArch64 `mov x0, sp; bl hardening_doctor_frame; mov x8,#34; svc #0; brk #0`.
- Landing stubs: x86 `pushfq; pop rdi; and rsp,-16; call CheckLanding`; AArch64 `mrs x0, nzcv; bl CheckLanding`.
- `hardening_doctor_frame` acts on a global mode, with the offsets asserted in the kernel. New `RunViolation` modes and checks per section 6; update the count in `Help`.

**Also:** `tests/unit/repro/arch_entry_repro_tests.cpp` `[.repro][R3-03]` → `[R3-03]`, named "R3-03 sigaction refuses a non-canonical handler address"; new cases in `tests/unit/kernel/process/signals_tests.cpp` (T2); `tools/smoke/cases/shell/limits.py` check count; `docs/posix.md` `signal.h`: `sigaction` fails with `EINVAL` for a handler or restorer outside user space (Linux accepts it and kills with `SIGSEGV` at delivery); `docs/development.md` l.277: the frame is zeroed first and the handler address checked; `docs/ideas.md:117`: note "likely R3-08, fixed by A3" on the AArch64 EL1 panic entry; review statuses.

## 4. Data layout

No new structures; frame layouts unchanged, now pinned by `static_assert`.

## 5. Invariants

- **I1.** Every value loaded into RIP (x86) or ELR_EL1 (AArch64) for a return to user mode is below `Process::UserAddressLimit` (2^47). Sources: hardware-saved frame from user mode (valid by construction); ELF entry (checked at `process.cpp:196`); thread entry (`process.cpp:1945`); signal handler (`SetAction` and `EnterHandler`); sigreturn PC (`ReturnFromSignal`, shared limit); syscall restart (`ip -= 2` / `elr -= 4` on a hardware-saved user PC).
- **I2.** The flags loaded for a user return cannot raise privilege. x86: CS=0x23 and SS=0x1b are constants or hardware-saved; RFLAGS is `0x202` at entry, hardware-saved afterwards, and a sigreturn changes only `UserFlags` (0x0CD5), so IOPL=0 and IF=1 and NT/TF/AC/VM/RF never come from a frame. AArch64: SPSR M is EL0t (0x3c0/0x340 at entry, hardware-saved after); a sigreturn changes only NZCV; only `ReturnToKernel` sets EL1h, with the kernel's own ELR.
- **I3 (x86).** IF=0 from the first instruction of `x86_64_enter_user` to its `iretq`, and from the return of `ExceptionDispatch` to the `iretq` of `x86_64_exception_common`. `TSS.rsp0` is written only with IF=0, on the CPU that executes the store.
- **I4 (AArch64).** IRQs masked from the first instruction of `aarch64_enter_user` to its `eret`, and from the return of each dispatch call to the `eret` of both common paths. SP never points at the user stack while at EL1.
- **I5.** No instruction between `swapgs` and `iretq` can fault for a program-controlled input (follows from I1, I2).
- **I6.** Every byte of a signal frame copied to user memory was written during this delivery (zero-filled first).
- **I7.** Signal frames move to and from user memory only through `Memory::UserCopy` (true since A2).
- **I8.** A bad value from a program ends only that program: `sigaction` with a handler or restorer ≥ limit → `EINVAL`; frame-build check fails, or sigreturn with a bad PC/SP → `ExitStatusSignalBase + SIGSEGV`; handler in the user half that is unmapped, misaligned or not executable → a user-mode fault, `ExitStatusFault`; privileged flags in a sigreturn frame → silently masked (like Linux's FIX_EFLAGS).

## 6. Acceptance tests

Deterministic and bounded; each child runs one signal and exits.

- **T1 (host).** `[R3-03]` passes without `.repro`.
- **T2 (host, `signals_tests.cpp`).** `SetAction` returns `SyscallErrorInvalidArgument` for handlers `0x0000800000000000`, `0xFFFF800000000000`, `UINT64_MAX`, and for a valid handler with restorer `0xFFFF800000000000`; after a refusal the action reads back `SignalDefault`; a handler at `HandlerAddressLimit - 4` with a valid restorer is accepted; `SignalIgnore` with restorer 0 and with a kernel restorer is accepted.
- **T3 (QEMU, hardening in-process).** The raw `SignalAction` syscall returns `SyscallErrorInvalidArgument` for a non-canonical handler, a kernel-half handler and a kernel restorer; libc `sigaction(SIGUSR1, {handler=0x0000800000000000})` returns -1 with `EINVAL`; `sigaction(SIGUSR1, nullptr, &now)` still gives `SIG_DFL`.
- **T4 (QEMU, child `hardening signal-unmapped-handler`).** Handler `0x00007FFFFFFFF000` (last canonical user page, unmapped) with the libc restorer, `kill(getpid(), SIGUSR1)`: `ChildTerminated` (`ExitStatusFault`), kernel alive, parent continues.
- **T5 (QEMU, child `hardening signal-return-kernel-pc`).** Saved PC doctored to `0x0000800000000000`: `status == ExitStatusSignalBase + SignalSegmentationFault` exactly. Today AArch64 gives `ExitStatusFault` (limit 2^48), so this catches the limit fix; x86 is pinned.
- **T6 (QEMU, child `hardening signal-return-flags`).** Saved PC doctored to the landing stub, saved SP to a static 16-byte-aligned stack (x86: `- 8`). x86: frame flags get IOPL=3, NT, TF, AC set, IF cleared, CF set; the landing's `pushfq` shows IOPL=0, NT=0, TF=0, AC=0, IF=1, CF=1 and the child exits 0 (an honoured TF would show as a #DB fault status). AArch64: pstate `0xF00003C5` (NZCV, DAIF, EL1h); the landing's `mrs nzcv` shows `0xF` and the child exits 0 (an honoured EL1h panics the kernel, user pages are PXN).
- **T7 (QEMU, child `hardening signal-frame-zeroed`).** x86: SIGUSR1's handler (SIGUSR2 in `sa_mask`) raises SIGUSR2; its restorer fills frame bytes 168-175 and 640-687 with 0xA5 before sigreturn; `ReturnFromSignal` copies that frame onto the kernel stack and SIGUSR2 is delivered at once from `DeliverSignals`; SIGUSR2's restorer checks the same bytes of its frame are zero and exits 0 or 1. Run T7 against the code without the `memset` to see whether it fails; if the two `frame` locals do not overlap, T7 stays as a check of the zero-fill contract and R3-09 counts as review-only. AArch64 (no gaps in its frame) runs `hardening signal-misaligned-handler` in that slot: handler = a real function + 2, expect `ChildTerminated` (PC alignment fault at EL0), keeping the check count equal.
- **T8 (matrix, at the end of Part A).** EXCEPTION, INTERRUPT, `shell_programs` (runs `hardening`), `smp_scheduling`, `shell_with_cpu_protections` (SMAP/PAN, `-cpu max`), `shell_release`, `user_mode_preemption`.

| Invariant | Caught by |
|---|---|
| I1 | T1, T2, T3 (sigaction side); T5 (sigreturn side, both arches). The `EnterHandler` check is unreachable once `sigaction` refuses: review only. |
| I2 | T6 (x86 fully; AArch64 M through the panic, NZCV directly). An honoured DAIF at EL0 cannot be seen from EL0: review only. |
| I3, I4 | Review only: TCG takes interrupts at translation-block boundaries and the windows are a few instructions; T8 shows nothing regressed. |
| I5 | Follows from I1/I2; the ring-0 #GP happens only on Intel hardware or KVM. Review beyond T5/T6. |
| I6 | T7 on x86 if it fails without the fix; otherwise review. AArch64 has no gaps: review. |
| I7 | Review, plus A2's tests. |
| I8 | T3-T7; a layout change breaks the build through the `static_assert`s. |

## 7. Decisions (coordinator)

1. `sigaction` gives `EINVAL` for a handler outside user space (the `[R3-03]` test requires it); documented as a deviation from Linux.
2. A kernel-half restorer is refused too, when a handler is set.
3. AArch64 sigreturn keeps masking non-EL0 or DAIF bits in pstate instead of SIGSEGV.
4. Out of scope, reported to the user as possible new findings: an NMI or #MC between `swapgs` and `iretq` still runs with the user GS base; padding in other structs copied to user memory (`stat`, old `sigaction`, socket structs) is not audited.
5. `user_mode_preemption` is covered by the matrix run at the end of Part A.
