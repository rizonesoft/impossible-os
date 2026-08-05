---
schema_version: 1
id: warm-kernel-update-runtime
domain: 03-memory-concurrency
status: active
title: "TODO-11 -- Warm-Kernel-Update Runtime"
---

# TODO-11: Warm-Kernel-Update Runtime

## Goal

Ship the runtime machinery that uses the warm-kernel-update handoff ABI from [01-boot-platform/TODO-01 §14](../01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#14-warm-kernel-update-handoff-abi) to actually replace a running kernel image without bouncing user processes. The ABI section defines the contract (descriptor type, continuation flags, validator); this TODO owns the folio preservation, per-subsystem quiesce callbacks, scheduler drain, VFS writeback before handoff, and the kexec-equivalent jump into the new kernel image.

> [!IMPORTANT] Current state:
> The ABI surface shipped in 2026-04-24 via TODO-01 §14: `BOOT_PAYLOAD_WARM_UPDATE_STATE` descriptor, `BOOT_FLAG_WARM_UPDATE`, `BOOT_MMAP_WARM_UPDATE` discriminator, `BOOT_WARM_UPDATE_CONT_*` continuation flags, `boot_warm_update_consume()` validator. The incoming kernel can already decide accept-vs-cold-fallback at handoff time. What is missing: the outgoing kernel's producer path (stage memory, serialize subsystem state, jump) AND the incoming kernel's reattach path (splice preserved memory into PMM, restore subsystem state from the continuation blobs).

## Inputs

- [01-boot-platform/TODO-01 §14 Warm-Kernel-Update Handoff ABI](../01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#14-warm-kernel-update-handoff-abi): the ABI contract this runtime implements.
- [src/kernel/main/boot_warm_update.c](../../src/kernel/main/boot_warm_update.c): validator this runtime calls from `boot_hw.c` after `boot_payload_validate`.
- Linux 6.16 Kexec Handover (KHO) + Live Update Orchestrator (LUO) as the industry reference.

## Outcome

- Running kernel can replace itself with a new image from `/System32/kernel.exe` via a live-update syscall, preserving all user processes, file descriptors, and device state.
- End-to-end warm-update cycle under 500ms on QEMU WHPX (target parity with Linux KHO).
- Fail-closed on any subsystem quiesce failure: cold-reboot fallback with a structured diagnostic.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On        | Status |
| --- | :---: | ---------------------------------------- | ----------------- | :----: |
| 💎  |   1   | Outgoing-kernel staging area + folio preservation | TODO-04 pager     |  [ ]   |
| 💎  |   2   | Per-subsystem quiesce callback registry  | TODO-06 scheduler |  [ ]   |
| 💎  |   3   | VFS writeback + FD table serialize       | (none)            |  [ ]   |
| 💎  |   4   | Scheduler drain + thread freeze          | §2                |  [ ]   |
| 💎  |   5   | Kexec-equivalent jump into new kernel image | §1..§4            |  [ ]   |
| 💎  |   6   | Incoming-kernel reattach path (splice memory, restore) | D01 T01 §14       |  [ ]   |
| 💎  |   7   | Live-update syscall + `nt_live_update` SSDT entry | §5, §6            |  [ ]   |

> 💎 = parity work: Linux 6.16 KHO + LUO set the baseline for cloud/server kernel replacement without VM bounce.

---

## 1. Outgoing-Kernel Staging Area + Folio Preservation

Carve a preserved-memory region from the PMM, mark its pages as non-reclaimable in the boot_info `boot_mmap[]` under `BOOT_MMAP_WARM_UPDATE` or `EfiUnacceptedMemoryType`, populate a `BOOT_PAYLOAD_WARM_UPDATE_STATE` descriptor pointing at the region, and tell PMM that the folios belonging to user processes are to be preserved (not freed) at shutdown.

- [ ] Stub: to be filled when §1 lands.
- [ ] Commit: `"mm: warm-update outgoing-kernel folio preservation"`

**Test checkpoint:** outgoing kernel publishes a well-formed warm-update descriptor (validated by `boot_warm_update_consume()`) pointing at a page-aligned region containing at least one preserved user folio.

---

## 2. Per-Subsystem Quiesce Callback Registry

Register per-subsystem quiesce callbacks (VFS, net, block I/O, IPC, device drivers, scheduler) that the warm-update outgoing path invokes in a defined order so each subsystem can publish its continuation blob or veto the update with a failure reason. Depends on TODO-06's scheduler for safe callback dispatch.

- [ ] Stub: to be filled when §2 lands.
- [ ] Commit: `"mm: warm-update per-subsystem quiesce callback registry"`

**Test checkpoint:** a registered callback that returns a failure code aborts the warm-update sequence and the system remains live; all callbacks successful -> continuation blobs are collected and addressable by subsystem id.

---

## 3. VFS Writeback + FD Table Serialize

Flush dirty VFS pages to stable storage and serialize the per-task file-descriptor table (fd -> `struct file *` + offset + flags) into a continuation blob so the incoming kernel can reattach open files without user-visible disruption. Standalone work; no blocking dep.

- [ ] Stub: to be filled when §3 lands.
- [ ] Commit: `"vfs: warm-update writeback + FD table serialize"`

**Test checkpoint:** a process with open file descriptors + pending dirty pages survives a warm update and reads the same file bytes at the same offsets post-handoff.

---

## 4. Scheduler Drain + Thread Freeze

Drain per-CPU runqueues to a quiescent state and freeze all non-kernel threads at safe points (syscall boundary or uninterruptible sleep boundary; never mid-kernel-lock) so the kexec jump sees a known-thread-state snapshot. Depends on §2 so the scheduler quiesce callback can fire in the right order.

- [ ] Stub: to be filled when §4 lands.
- [ ] Commit: `"sched: warm-update drain + thread freeze"`

**Test checkpoint:** all non-kernel threads are frozen at a safe point (PC in syscall-entry trampoline or user-mode); no thread holds a kernel spinlock at handoff.

---

## 5. Kexec-Equivalent Jump Into New Kernel Image

Jump into the new kernel image in-place (no firmware reset) with a handoff descriptor pointing at the preserved-memory region + continuation blobs. Depends on §1..§4: staging area must exist, subsystems must be quiesced, VFS flushed, scheduler drained.

- [ ] Stub: to be filled when §5 lands.
- [ ] Commit: `"boot: warm-update kexec-equivalent jump"`

**Test checkpoint:** the outgoing kernel transfers control to the new kernel image via a verified jump vector; the new kernel's Phase 0 reads the BOOT_PAYLOAD_WARM_UPDATE_STATE descriptor and accepts the continuation.

---

## 6. Incoming-Kernel Reattach Path (Splice Memory, Restore)

Incoming kernel's Phase 0 reattach path: splice the preserved folios into the new PMM's reserved set (not freed), walk `BOOT_WARM_UPDATE_CONT_*` flags to drive per-subsystem restore callbacks, and fail-closed to cold init if any required continuation bit is unrecognized. Depends on TODO-01 §14 ABI contract (already shipped).

- [ ] Stub: to be filled when §6 lands.
- [ ] Commit: `"mm: warm-update incoming reattach + continuation restore"`

**Test checkpoint:** incoming kernel consumes the descriptor, preserved folios are addressable post-handoff without PMM re-allocation, per-subsystem restore callbacks run in dependency-correct order.

---

## 7. Live-Update Syscall + nt_live_update SSDT Entry

Syscall interface (`nt_live_update`) with an SSDT entry that user-space privileged callers can invoke to trigger a warm kernel update from a signed `C:\System32\kernel.exe`. Depends on §5 (jump) and §6 (reattach).

- [ ] Stub: to be filled when §7 lands.
- [ ] Commit: `"syscall: nt_live_update SSDT entry + privileged caller gate"`

**Test checkpoint:** a privileged user-mode caller invokes `nt_live_update` with a signed kernel image path; the kernel completes the warm-update cycle under 500ms on QEMU WHPX with a user process surviving across the handoff.

---

## OS Comparison

| ⭐  | Feature                      | 🪟 Win11                  | 🐧 Linux                     | 🚀 Impossible OS                |
| --- | ---------------------------- | ------------------------- | ---------------------------- | ------------------------------- |
| 💎  | Live kernel replacement      | ⚠️ Hot Patch (closed)      | ✅ 6.16 Kexec Handover + LUO | ⬜ runtime machinery (ABI done) |
| 💎  | User-process preservation    | ⚠️ Hot Patch scope limited | ✅ KHO preserves VMs         | ⬜ §1 folio preservation        |
| 💎  | Subsystem-state continuation | ❌                        | ✅ LUO continuation          | ⬜ §2 quiesce callback registry |

> Impossible OS starts with the ABI contract already in place (TODO-01 §14); this TODO builds the runtime that uses it.

## Unit Tests

> **Note:** Runtime tests will be wired as each implementation section lands. The ABI-surface validator is already covered by `src/kernel/test/test_boot_warm_update.c` (10 suites in `TEST_CAT_BOOT`).

## Verification

- [ ] Manual integration test: live-update cycle under QEMU WHPX preserves at least one running user process and its open file descriptors across the kernel replacement.
- [ ] `bash scripts/test.sh SUITE=mm` passes when §1..§7 land.
- [ ] `bash scripts/test-smoke.sh` passes on the new kernel image booted as warm-update recipient.

> **Test runner:** N/A (stub TODO; implementation tests will be wired per section).
