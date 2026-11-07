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

## Q4 (TODO-11 §16/§17/§19/§21/§23): may the run change the TEB layout, PEB layout and ring-3 entry frame?

`02-kernel-core/TODO-11` §16-§27 were deferred together on 2026-06-28 (`19672b49a`) as "operator-reserved", but the run applied that label itself, and each stamp's XREF pointed at its own section, so nothing could ever clear them and the oracle read the file as DONE. On 2026-11-07 the eight sections with no ABI question were reopened: §11, §18, §20, §22, §24, §25, §26 and §27. These five stay parked because they change what ring-3 sees:

- **§16 and §23:** move the TEB to a new VA band with two pages per thread. Today `TlsSlots` at 0x1480 falls outside the mapped page. The band itself needs a design change: a VA in 1-2 GiB lands in the kernel PD shared by every process.
- **§19:** fork's child TEB depends on §16.
- **§17:** move PEB version fields and others to true x64 offsets (0x118, 0x120). Native code using the current struct must rebuild; a real Win64 ntdll cannot read them today.
- **§21:** add a Win64 startup frame for PE32+ and define one for EIF.

Recommended: let the run do all five through the normal section pipeline, because the current layouts are defects, nothing outside this repo depends on them, and every section still gets a design review. Blocks these five sections only.

## Q5 (TODO-18 §2-§10): how does the kimage registry replace `exec_register_module`?

`02-kernel-core/TODO-18` s2 needs this before any registry code, and s3-s10 hang off s2. These stamps had no `awaiting-answer`, so this never reached the operator list. `s_modules[64]` in `src/kernel/exec.c` is the live registry, and the fatal Phase-1 self-registration (`boot_interrupts.c`) and the shipped crash-dump path (`TODO-27` s3) depend on it. Expensive to reverse.

Five answers are needed. The recommendations are from reading the code, not from a design review, and that review still runs when s2 starts.

- **Strategy: A or B?** A makes kimage the backing store and `exec_register_module` delegates to it. B makes kimage a superset and rewires every consumer. Recommended: A, because the fatal boot path and crash path keep working through the migration.
- **Extract a T17 LDR-list API from `exec.c` first?** Recommended: yes.
- **Per-process registry: a new `struct task` field, or PEB `Ldr` only?** Recommended: the `struct task` field, because ring 3 can write the PEB.
- **Does `kimage_init()` absorb the Phase-1 self-registration?** Recommended: not at first; move it once A has landed.
- **Add `SUBSYS_KIMAGE`?** Recommended: yes.

Blocks TODO-18 s2-s10 only.

## Q6 (TODO-20 §7, TODO-17 §13): what does an EIF import call actually compile to?

`specs/eif-format.md` rule 5 requires a callable stub for a skipped optional import, but `eif_dispatch_entry_t` is data-only (`{syscall_id, available}`), and nothing in user code reads the table at 0x8F0000. `TODO-20` §7 and `TODO-17` §13 each waited on the other for this, so nobody owned it, and `elf2eif` cannot produce a runnable image until it is settled.

- **A: keep the table data-only.** `elf2eif` emits thunks that check `available`, then return `STATUS_NOT_IMPLEMENTED` or issue `SYSCALL`. Rule 5 is rewritten; no layout change.
- **B: callable slots.** The entry layout changes and the kernel maps a read-only stub page into every process.

Recommended: A, because the on-disk format stays as it is, the kernel gains no user-executable page, and §9 can make the table read-only without touching the layout. This comes from reading the code, not a design review, and that review still runs when §7 starts. Blocks TODO-20 §7 and the import half of TODO-17 §13.

## Q7 (TODO-20 §11, TODO-19 §4, TODO-17 §17): may the run change the EIF signature block?

The spec'd block (`algo`, `sig_size`, `signature`) has no signer key, and CI anchors store only a key hash, so a verifier has nothing to run Ed25519 against. The proposal adds `pubkey[32]` and a key-id and bumps `EIF_VERSION`, with old images gated by version. No signed EIF exists yet, so nothing needs migrating, but this is a security-sensitive on-disk format, and until `eifsign`, the verifier and a compiled anchor exist it cannot be tested end to end.

Recommended: approve the amendment, landing it with the `TODO-17` §17 verifier rather than ahead of it, so the format and its first end-to-end test ship together. Blocks TODO-20 §11, TODO-19 §4 and the sections parked behind it.

## Q8 (TODO-21 §6/§7, TODO-06 §4): what is the authority for process capabilities?

`TODO-21` §6 specifies a per-process `capabilities` bitmask, but the shipped privilege check is the NT token (`SeSinglePrivilegeCheck`, `src/kernel/security/privileges.c`). A bitmask beside it is a second privilege authority, and a security model is expensive to reverse. `TODO-06` §4 also needs a `CAP_SCHED_RT` from this same model.

- **A: token only, with capability names as aliases for token LUIDs.** There is one authority, and impersonation keeps working. Drop-only needs a token that can lose privileges for good.
- **B: a denial-only ceiling ANDed with the token check.** The bitmask can only remove rights, which gives the drop-only `NtDropCapability` of §7 directly. There are two things to keep consistent, and the composition has to be defined per gate.

Recommended: B, because §7's drop-only rule and `child subset of parent` invariant are what the section exists to provide, and a ceiling cannot grant anything the token denies. This comes from reading the 2026-07-11 design review and the code, not a fresh review. Whichever is chosen, the mask needs atomic updates, an explicit child ceiling before ring-3 publication, and `CAP_SET_TIME` must also cover `KeSetSystemTimeEx`. Blocks `TODO-21` §6 and §7.

## Q9 (TODO-24 §6/§8/§9): how do syscalls with more than 6 arguments reach the kernel?

The ALPC ABI needs 8-11 arguments, but `SSDT_HANDLER` and `zw_dispatch` carry 6 words (`ssdt.h:30-31`). The aliases, full-width connect and accept, `ConnectPortEx`, `NtAlpcQueryInformationMessage` and the attribute-carrying send all wait on this, and it changes a kernel ABI.

- **A: widen the transport.** Handlers take more words, read from the user stack as Windows x64 callers already pass them. This needs `TODO-12` §3's stack-argument read and touches the handler signature.
- **B: a validated argument pack by pointer.** The 6-word handler stays, and only the wide syscalls take a pointer to a probed struct. Nothing else changes, but these syscalls no longer match the Windows calling shape.

Recommended: A, because Win32 is the native surface and `TODO-12` §3 already has to read stack arguments. This comes from reading the code, not a design review.
