# GekkoAOT Native SDK graphics parity — v144

Date: 2026-09-25

## Goal

Keep `GEKKOAOT_NATIVE_OS` as a native PC runtime path, not as a guest-OS/HLE fallback, while preserving the observable semantics that retail GameCube code expects from Dolphin OS.

This work is clean-room-oriented: public decompilation projects and open runtimes are used to compare ABI, state transitions, ordering, and hardware-facing behavior. No Nintendo object code, libraries, assets, or near-verbatim proprietary implementation is imported into GekkoAOT.

## Reference set

Primary semantic references:

- `doldecomp/dolsdk2001` — early GameCube Dolphin SDK OS implementation.
- `doldecomp/dolsdk2004` — later GameCube Dolphin SDK OS implementation.
- `doldecomp/melee`, `doldecomp/sms`, `doldecomp/mkdd`, `zeldaret/tww` — retail-linked SDK variants and call-site behavior.
- `devkitPro/libogc` — independent open implementation used only as a cross-check.

Native/recomp architecture cross-checks:

- `999sian/melee-pc` — native PC port using Aurora and compatibility shims.
- `HexDump0/melee` — host-native OS/platform implementation patterns.
- `alphanu1/msg-twinsnakes-recomp` — useful evidence that host-entered guest callbacks must preserve complete architectural state; not used as SDK ground truth.
- `ExpansionPak/DolRecomp` — recompilation backend and CPU-state contract.

## P0 graphics finding: OSContext / FPU / paired-single ownership

The retail SDK has one physical FPU register file and a separate `__OSFPUContext` owner at low-memory address `0x800000D8`.

`OSSetCurrentContext` does **not** copy FPRs itself. It changes current-context bookkeeping and controls MSR[FP]. On hardware, the first floating-point/paired-single instruction in a context that does not own the FPU raises FP-unavailable. The SDK handler then saves the old owner's complete FPR/FPSCR/PS state and loads the new context if it has a valid saved payload.

Static native AOT cannot rely on a host CPU instruction generating the Gekko FP-unavailable exception. Before v144, NativeOS changed `OSContext` but the same live `CPUState::fpr[]` / `ps1[]` bank could remain exposed to a temporary callback. A GX/VI callback using floating point could therefore overwrite values belonging to the interrupted render thread.

This is directly graphics-relevant. Dolphin SDK GX token/finish interrupt handlers use the sequence:

1. `OSClearContext(&exceptionContext)`
2. `OSSetCurrentContext(&exceptionContext)`
3. call the game callback
4. `OSClearContext(&exceptionContext)`
5. `OSSetCurrentContext(interruptedContext)`

The v144 NativeOS barrier snapshots the outgoing context's host FP/PS state and restores the incoming context's saved payload/shadow at this exact boundary. It intentionally does **not** eagerly rewrite guest low-memory `0x800000D8`; guest-visible ownership remains lazy while the native host register file is isolated eagerly.

## P0 AOT finding: partial FP materialization at direct SDK hooks

The prior DolRecomp `context-capture` ABI forced nonvolatile FPR/PS registers 14–31 into `CPUState`. That is sufficient for ordinary PPC call preservation, but not for an asynchronous/lazy-FPU OSContext boundary where the complete architectural FP/PS register file belongs to the interrupted context.

v144 adds a `full-fp-context` manifest class. It materializes FPR0–31, PS1-0–31, FPSCR and the existing integer/control/GQR context inputs only for the SDK hooks that require it:

- `OSSetCurrentContext`
- `OSLoadFPUContext`
- `OSSaveFPUContext`
- `OSFillFPUContext`

Other scheduler hooks keep the lighter existing context ABI.

The module compatibility IDs are bumped so old AOT cache artifacts cannot silently reuse the pre-v144 ABI.

## Native FPU SDK entrypoints

NativeOS v144 has implementations for:

- `OSLoadFPUContext`
- `OSSaveFPUContext`
- `OSFillFPUContext`
- `OSGetStackPointer`
- `OSSwitchStack`

`OSSaveFPUContext` stores FPR0–31, FPSCR and paired-single PS1 lanes and marks `OS_CONTEXT_STATE_FPSAVED`.

`OSLoadFPUContext` follows the SDK validity bit: a context without `FPSAVED` is left untouched.

`OSFillFPUContext` serializes the FP/PS payload but deliberately does not set the saved-state bit, matching the SDK routine's distinct contract.

The auto-resolver also recovers the tiny public `OSLoadFPUContext` / `OSSaveFPUContext` wrappers only inside an already-proven `OSSetCurrentContext` cluster. This avoids a global two-instruction-pattern match.

## Cache coherency: do not replace with no-ops

Dolphin SDK `DCFlushRange`, `DCStoreRange`, `DCInvalidateRange` and related routines operate in 32-byte cache-line steps. In GekkoAOT, the recompiled cache opcodes currently feed `HostRuntime::CacheControl`, which also notifies NativeGX about CPU-side memory-generation changes used by indexed vertex/texture paths.

Therefore v144 intentionally does **not** turn the cache SDK into generic host no-ops. A future native cache API must forward every affected line/range to the same NativeGX coherency source of truth before it can replace the guest-compiled SDK routine.

## Remaining graphics-priority SDK gaps

### P1 — `OSSetSwitchThreadCallback`

Retail `OSThread` calls the registered switch callback before publishing the new current thread. NativeOS currently owns scheduling but does not yet run this guest callback. It needs a safe host-to-compiled callback trampoline with complete asynchronous state preservation, not a direct C++ function pointer call.

### P1 — interrupt registration/masking closure

`__OSSetInterruptHandler`, `__OSGetInterruptHandler`, `__OSMaskInterrupts`, `__OSUnmaskInterrupts` and local/global mask state remain mostly guest SDK code. This is acceptable while guest interrupt dispatch remains authoritative, but a fully native SDK should eventually unify them with NativePI/PE/VI so callback ordering and acknowledgement have one owner.

### P1 — native cache API with GX invalidation bridge

Only move `DC*`/`IC*` routines native after adding a host service that exactly retains:

- 32-byte alignment/range expansion,
- store/flush/invalidate distinction,
- NativeGX invalidation notifications,
- `icbi` executable-identity invalidation,
- sync/isync ordering where observable.

### P1 — SDK revision profiles

There are real ABI/semantic differences between SDK revisions. Example: the restartable atomic sequence bounds in `OSLoadContext` differ between observed 2001 and 2004 implementations. Do not globally identify every title as “SDK 2004”; derive a small per-image profile from resolved function bodies/struct usage.

A second example is `OSThread`: the 2001 layout ends at `0x30C`, while the 2004 version adds `error` at `0x30C` and thread-specific pointers starting at `0x310`. NativeOS must not write those later fields until the linked SDK revision is proven.

### P1 — mutex priority inheritance

The retail mutex/thread implementation propagates effective priority through mutex wait chains. NativeOS queue ordering is already priority-aware but does not yet reproduce the complete inheritance graph. Wrong ordering can become a render/audio timing bug even when it does not corrupt pixels directly.

### P1 — alarm closure

The retail decrementer alarm callback also changes to a temporary `OSContext`, disables the scheduler around the handler, restores the interrupted context, reschedules, then loads it. The v144 context barrier fixes the FP/PS hazard even while alarms remain guest-AOT by default. A future fully native alarm path must keep that ordering.

### P2 — semaphores and thread-specific storage

`OSSemaphore*`, `OSSetThreadSpecific` / `OSGetThreadSpecific`, plus the 2004 `OSThread` extension remain candidates for revision-aware NativeOS coverage.

### P2 — stack/fiber helpers

`OSGetStackPointer` and `OSSwitchStack` are straightforward and are included in v144. `OSSwitchFiber` requires a native continuation/trampoline design before interception because it transfers control to a supplied guest PC and later returns to a different stack.

## Validation added

`tests/hardware/test_os_context_fpu.cpp` verifies:

1. temporary IRQ-style `OSContext` callbacks may overwrite every FPR/PS lane without corrupting the interrupted context;
2. a guest `FPSAVED` payload is authoritative when entering a context;
3. native `OSSaveFPUContext` + `OSLoadFPUContext` round-trip all 32 FPR/PS lanes and FPSCR;
4. NativeOS does not eagerly rewrite guest `0x800000D8` just to implement the host-side isolation barrier.

The existing OS message/priority test is also retained to catch scheduler regressions.
