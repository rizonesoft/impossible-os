# Operator Answers

> Operator-reserved decisions surfaced by the overnight runner. Each Q carries a
> stable id. Answer inline with `A: (operator, YYYY-MM-DD) <decision>` and the
> runner's heal probe will pick it up on the next pass and unblock the deferred
> section. Low-risk conservative defaults may appear as `A: (proposed default)`
> for the operator to confirm or veto.

## Q1 (TODO-16 §8): WNF_STATE_NAME compat level -- ABI-defining, cannot change post-ship

The Native WNF syscall surface (`NtCreateWnfStateName` / `NtQueryWnfStateData` /
`NtUpdateWnfStateData` / `NtSubscribeWnfStateChange` / ...) must commit a
`WNF_STATE_NAME` wire encoding before the handlers ship. Two options:

- **(a) Byte-compatible:** the real Windows 64-bit encoded/XOR-obfuscated
  `WNF_STATE_NAME` scheme, so genuine Windows WNF constants (e.g.
  `WNF_SHEL_DESKTOP_APPLICATION_STARTED`) round-trip unchanged. Maximizes Win32
  binary compatibility (North Star), but pins us to Microsoft's undocumented
  obfuscation constants.
- **(b) Syscall-arg-shape only:** KNF maps opaque 64-bit names to internal OB
  paths (`\Notifications\<category>\<name>`). Simpler, fully ours, but a Windows
  binary passing a real `WNF_STATE_NAME` constant would not resolve.

This is ABI-defining and cannot change once a handler ships. Blocking TODO-16 §8.

A: (pending)

## Q2 (TODO-21 §15/§18): PROCESS_OBJECT terminal-state ownership under slot reuse

POSIX reaping (§15) needs a process to stay observable after exit until reaped,
but `PROCESS_OBJECT` (`include/kernel/ob/ob_process.h:17`) holds a raw
`struct task *` into the static `tasks[]` array. Once TODO-06 §357 adds
slot-reuse-after-drain, an old NT process handle could observe a DIFFERENT
process in the reused slot (an NT handle can outlive the POSIX reaper). Where
does terminal state (exit_status + signaled bit) live so it survives reuse?

- **(a) Task tombstone:** pin a non-reusable slot/tombstone carrying terminal
  state until every process handle closes; slot returns to the free pool only
  then. Least code change to the reaping design; costs one slot per
  handle-referenced zombie (bounded by open handles).
- **(b) Move terminal state into the refcounted `PROCESS_OBJECT`:** exit_status
  + signaled bit live on the OB object (refcounted by handles), the `tasks[]`
  slot frees immediately at reap. Cleaner NT-handle lifetime, but decouples
  process identity from the task slot (larger change to `wait_is_ready`,
  `NtWaitForSingleObject`, `NtQueryInformationProcess`).
- **(c) Generation-stable task id:** pair (slot, generation) everywhere a
  `struct task *`/pid is cached; a stale handle detects a generation mismatch.
  Composes with TODO-06 §357 monotonic PID but touches every task lookup.

Blocks TODO-21 §15 (and §18). Depends on TODO-06 §357 slot-reuse landing first.

A: (proposed default) (a) task tombstone -- least invasive, keeps §15's reaping
design intact and bounds the extra slots by open handle count; revisit if the
per-handle slot cost proves too high under the TASK_MAX ceiling.

## Q3 (TODO-33 §3): Higher-half kernel jump vs user-frame relocation -- sequencing

`codex-design-review` returned **NO-SHIP** on §3 (Linker VMA/LMA split + higher-half
jump). The blocker is a physical collision the pinned plan did not resolve. The
kernel image is 6.99 MiB (`__kernel_start` 0x100000 .. `__kernel_end` 0x7fe000).
Moving the LMA to `MM_KERNEL_PHYS_BASE` 0x200000 (forced 2 MiB alignment for a
PS=1 PDE) puts the physical image at [0x200000, 0x8fe000), which OVERLAPS
`USER_PT_WINDOW` [0x800000, 0xa00000]. User processes still identity-map user VMA
0x800000 -> phys 0x800000 and exec writes the launcher ELF + stack there directly,
so §3 would overwrite the kernel `.bss` tail + PMM bitmap; the smoke test reaches
`cmd.exe`, so gating userspace also fails §3's ship checkpoint. A 6.99 MiB image
does not fit 2 MiB-aligned below 0x800000 (only 6 MiB is free there). This is a
circular dependency: §3 (jump) is blocked on user frames not aliasing the kernel
image, but user-frame relocation lives in §6/§7 which build ON the high kernel.

- **(a) Relocate user frames first:** pull §6's "user low private" (unique physical
  pages per process, user VMA no longer identity-mapped to 0x800000) AHEAD of §3,
  so the launcher/user pages never touch phys 0x200000-0x8fe000. Breaks the cycle
  cleanly and is on the roadmap anyway, but re-sequences §6 before §3 and is a
  substantial change to the exec/user-mapping model (today all user pages share
  identity frames -- CLAUDE.md, `vmm_create_user_pml4`, `USER_PT_WINDOW`).
- **(b) Re-base the kernel physically above the user window:** set
  `MM_KERNEL_PHYS_BASE` (memmap.h SSOT) to >= `USER_PT_WINDOW_END` (0xa00000) so the
  image never overlaps. Minimal §3-local change and endorsed by the design review
  ("select a kernel physical range wholly outside USER_PT_WINDOW"), but edits a
  pinned SSOT constant with a documented rationale (lowest 2 MiB base above the AP
  envelope, max low RAM) and derives `MM_KERNEL_IMAGE_BASE` off it; §7 could move it
  back once the ceiling/user base is retired.
- **(c) Shrink the image below 6 MiB** so it fits [0x200000, 0x800000): not viable --
  the kernel is 6.99 MiB and growing; this is the exact pressure the relocation
  exists to relieve.

Blocks TODO-33 §3-§8 (and, transitively via the un-retired ceiling, the whole
kernel-code queue: TODO-22 §23-§25, TODO-23 §2-§16, and every later kernel TODO
that adds `.text`). Non-kernel/userland work is unaffected and continues.

A: (proposed default) (b) re-base `MM_KERNEL_PHYS_BASE` above `USER_PT_WINDOW` --
smallest, design-review-endorsed, keeps §3 an atomic flip and defers the larger
user-frame-model change to §6 where it is already owned; revisit the base in §7.
Deferred to the operator because it edits the pinned address-space SSOT and picks
the relocation ordering for the whole kernel roadmap.
