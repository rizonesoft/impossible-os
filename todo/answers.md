# Operator Answers

> Decisions made for the overnight runner. Each Q carries a stable id. Since 2026-11-07 the run decides for itself and records `A: (run decision, YYYY-MM-DD) <choice>. Reversal: <cost>.`, and it keeps going; only the halt class waits (unrecoverable data loss, credentials, money, shared infrastructure, a calculation that looks wrong). To veto a run decision or answer a waiting question, write `A: (operator, YYYY-MM-DD) <decision>`: an operator answer always wins, and the heal probe picks it up on the next pass.

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

A: (operator, 2026-11-07) (a) byte-compatible Windows encoded names. This cannot change once a handler ships, so it keeps compatibility open.

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

A: (operator, 2026-11-07) (a) task tombstone: pin the slot with its terminal state until every process handle closes.

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

A: (operator, 2026-11-07) (b) re-base `MM_KERNEL_PHYS_BASE` at or above `USER_PT_WINDOW_END` (0xA00000), keeping s3 one atomic flip. s7 may move it back once the ceiling is retired.

## Q4 (TODO-11 §16/§17/§19/§21/§23): may the run change the TEB layout, PEB layout and ring-3 entry frame?

`02-kernel-core/TODO-11` §16-§27 were deferred together on 2026-06-28 (`19672b49a`) as "operator-reserved", but the run applied that label itself, and each stamp's XREF pointed at its own section, so nothing could ever clear them and the oracle read the file as DONE. On 2026-11-07 the eight sections with no ABI question were reopened: §11, §18, §20, §22, §24, §25, §26 and §27. These five stay parked because they change what ring-3 sees:

- **§16 and §23:** move the TEB to a new VA band with two pages per thread. Today `TlsSlots` at 0x1480 falls outside the mapped page. The band itself needs a design change: a VA in 1-2 GiB lands in the kernel PD shared by every process.
- **§19:** fork's child TEB depends on §16.
- **§17:** move PEB version fields and others to true x64 offsets (0x118, 0x120). Native code using the current struct must rebuild; a real Win64 ntdll cannot read them today.
- **§21:** add a Win64 startup frame for PE32+ and define one for EIF.

Recommended: let the run do all five through the normal section pipeline, because the current layouts are defects, nothing outside this repo depends on them, and every section still gets a design review. Blocks these five sections only.

A: (operator, 2026-11-07) yes, all five sections (s16, s17, s19, s21, s23) through the normal section pipeline, each with its design review.

## Q5 (TODO-18 §2-§10): how does the kimage registry replace `exec_register_module`?

`02-kernel-core/TODO-18` s2 needs this before any registry code, and s3-s10 hang off s2. These stamps had no `awaiting-answer`, so this never reached the operator list. `s_modules[64]` in `src/kernel/exec.c` is the live registry, and the fatal Phase-1 self-registration (`boot_interrupts.c`) and the shipped crash-dump path (`TODO-27` s3) depend on it. Expensive to reverse.

Five answers are needed. The recommendations are from reading the code, not from a design review, and that review still runs when s2 starts.

- **Strategy: A or B?** A makes kimage the backing store and `exec_register_module` delegates to it. B makes kimage a superset and rewires every consumer. Recommended: A, because the fatal boot path and crash path keep working through the migration.
- **Extract a T17 LDR-list API from `exec.c` first?** Recommended: yes.
- **Per-process registry: a new `struct task` field, or PEB `Ldr` only?** Recommended: the `struct task` field, because ring 3 can write the PEB.
- **Does `kimage_init()` absorb the Phase-1 self-registration?** Recommended: not at first; move it once A has landed.
- **Add `SUBSYS_KIMAGE`?** Recommended: yes.

Blocks TODO-18 s2-s10 only.

A: (operator, 2026-11-07) strategy A: kimage is the backing store and `exec_register_module` delegates to it. Extract the LDR-list API from `exec.c` first. Per-process registry in a `struct task` field. `kimage_init()` does not absorb the Phase-1 self-registration at first; move it once A has landed. Add `SUBSYS_KIMAGE`.

## Q6 (TODO-20 §7, TODO-17 §13): what does an EIF import call actually compile to?

`specs/eif-format.md` rule 5 requires a callable stub for a skipped optional import, but `eif_dispatch_entry_t` is data-only (`{syscall_id, available}`), and nothing in user code reads the table at 0x8F0000. `TODO-20` §7 and `TODO-17` §13 each waited on the other for this, so nobody owned it, and `elf2eif` cannot produce a runnable image until it is settled.

- **A: keep the table data-only.** `elf2eif` emits thunks that check `available`, then return `STATUS_NOT_IMPLEMENTED` or issue `SYSCALL`. Rule 5 is rewritten; no layout change.
- **B: callable slots.** The entry layout changes and the kernel maps a read-only stub page into every process.

Recommended: A, because the on-disk format stays as it is, the kernel gains no user-executable page, and §9 can make the table read-only without touching the layout. This comes from reading the code, not a design review, and that review still runs when §7 starts. Blocks TODO-20 §7 and the import half of TODO-17 §13.

A: (operator, 2026-11-07) A: keep the table data-only; `elf2eif` emits thunks, and spec rule 5 is rewritten.

## Q7 (TODO-20 §11, TODO-19 §4, TODO-17 §17): may the run change the EIF signature block?

The spec'd block (`algo`, `sig_size`, `signature`) has no signer key, and CI anchors store only a key hash, so a verifier has nothing to run Ed25519 against. The proposal adds `pubkey[32]` and a key-id and bumps `EIF_VERSION`, with old images gated by version. No signed EIF exists yet, so nothing needs migrating, but this is a security-sensitive on-disk format, and until `eifsign`, the verifier and a compiled anchor exist it cannot be tested end to end.

Recommended: approve the amendment, landing it with the `TODO-17` §17 verifier rather than ahead of it, so the format and its first end-to-end test ship together. Blocks TODO-20 §11, TODO-19 §4 and §5 and the sections parked behind them.

A: (operator, 2026-11-07) approved: add `pubkey[32]` and a key id and bump `EIF_VERSION`, landing with the `TODO-17` s17 verifier rather than ahead of it.

## Q8 (TODO-21 §6/§7, TODO-06 §4): what is the authority for process capabilities?

`TODO-21` §6 specifies a per-process `capabilities` bitmask, but the shipped privilege check is the NT token (`SeSinglePrivilegeCheck`, `src/kernel/security/privileges.c`). A bitmask beside it is a second privilege authority, and a security model is expensive to reverse. `TODO-06` §4 also needs a `CAP_SCHED_RT` from this same model.

- **A: token only, with capability names as aliases for token LUIDs.** There is one authority, and impersonation keeps working. Drop-only needs a token that can lose privileges for good.
- **B: a denial-only ceiling ANDed with the token check.** The bitmask can only remove rights, which gives the drop-only `NtDropCapability` of §7 directly. There are two things to keep consistent, and the composition has to be defined per gate.

Recommended: B, because §7's drop-only rule and `child subset of parent` invariant are what the section exists to provide, and a ceiling cannot grant anything the token denies. This comes from reading the 2026-07-11 design review and the code, not a fresh review. Whichever is chosen, the mask needs atomic updates, an explicit child ceiling before ring-3 publication, and `CAP_SET_TIME` must also cover `KeSetSystemTimeEx`. Blocks `TODO-21` §6 and §7.

A: (operator, 2026-11-07) B: a denial-only ceiling ANDed with the token check, with the conditions the question lists: atomic mask updates, an explicit child ceiling before ring-3 publication, and `CAP_SET_TIME` also covering `KeSetSystemTimeEx`.

## Q9 (TODO-24 §6/§8/§9): how do syscalls with more than 6 arguments reach the kernel?

The ALPC ABI needs 8-11 arguments, but `SSDT_HANDLER` and `zw_dispatch` carry 6 words (`ssdt.h:30-31`). The aliases, full-width connect and accept, `ConnectPortEx`, `NtAlpcQueryInformationMessage` and the attribute-carrying send all wait on this, and it changes a kernel ABI.

- **A: widen the transport.** Handlers take more words, read from the user stack as Windows x64 callers already pass them. This needs `TODO-12` §3's stack-argument read and touches the handler signature.
- **B: a validated argument pack by pointer.** The 6-word handler stays, and only the wide syscalls take a pointer to a probed struct. Nothing else changes, but these syscalls no longer match the Windows calling shape.

Recommended: A, because Win32 is the native surface and `TODO-12` §3 already has to read stack arguments. This comes from reading the code, not a design review.

A: (operator, 2026-11-07) A: widen the transport; handlers read the extra words from the user stack as Windows x64 callers pass them.

## Q10 (TODO-04 §10): where does the log-integrity verifier key live?

The design review found that keeping the key under `HKLM` proves nothing: an attacker who can rewrite the log can rewrite the key and every HMAC too. The section needs an anchor outside the mutable log domain, and this is a security-architecture choice no item in the tree makes.

- **A: TPM-sealed.** Strongest, and it ships (`01-boot-platform/TODO-13`), but machines without a TPM get no integrity chain.
- **B: protected UEFI NVRAM.** Works without a TPM, but anything that can write the variable can replace the key.

Recommended: A, with a logged warning and integrity disabled where no TPM exists, matching the item's WARN+disable wording. This comes from reading the TODO and the review note, not a new design review. Blocks TODO-04 §10.

A: (operator, 2026-11-07) A: TPM-sealed, with a logged warning and integrity disabled where no TPM exists.

## Q11 (TODO-04 §11): may the run define the ETW event-metadata wire ABI?

Today's scalar per-session masks cannot hold two providers with different policies, and events carry no provider or keyword. The recorded fix is a versioned metadata record, a per-session provider table, broadcast routing and two-phase registration. It extends the §7 syscall surface, which CLAUDE.md reserves as an ABI change. Nothing in the tree consumes the current shape outside the kernel's own tests, as far as the item text records; that was not re-checked here.

Recommended: approve it, keeping the existing channel call as the legacy path the item already describes. Blocks TODO-04 §11 and the section behind it (line 428's stamp).

A: (operator, 2026-11-07) approved, keeping the existing channel call as the legacy path.

## Q12 (TODO-13 §15/§10): what may set the sticky recovery trigger and reset a trusted baseline?

Nothing in the tree sets `recovery_trigger`: `boot_sticky.c` reads and clears it, `boot_audit.c` preserves or clears it, and user-mode writes are denied. The enrollment gate refuses at `sticky-trigger-clear` (`tpm_enroll_gate.c:158`), so no baseline can be enrolled end to end. A trusted baseline reset needs the same authority.

- **A: physical presence.** A key choice at a boot console, built on `boot_confirm_wait_keys()`. No new key material, but a machine with no console has no path.
- **B: signed authorization.** The TODO-13 §23 headless authority also admits the trigger. Works headless, but only after an integrator provisions a key.
- **A plus B.** Both paths, with both gates to review.

Recommended: A first, because it needs no provisioning and matches the local-confirmation wording in §10; B can follow. This comes from reading the code and TODO text, not a design review. Blocks the producer, the one-shot consume and the baseline reset; the rest of §15 and §10 is open.

A: (operator, 2026-11-07) A first: physical presence at a boot console. Signed authorization may follow as separate work.

## Q13 (TODO-13 §9): how does user mode read attestation evidence, and who may?

The report exports to `X:\Diag\attestation.json` today, with no query API. This is new ABI plus an access-control choice.

- **A: an `NtQuerySystemInformation` class** restricted to administrators.
- **B: a new dedicated syscall.**

Recommended: A, since it adds no SSDT entry. I did not check how the existing classes gate access. Blocks one item in §9.

A: (operator, 2026-11-07) A: an `NtQuerySystemInformation` class restricted to administrators. The section must confirm how existing classes gate access.

## Q14 (TODO-13 §9/§13): which remote-attestation protocol does the platform target?

A verifier-side challenge does not exist in the tree, so `TPM2_MakeCredential` / `TPM2_ActivateCredential` has no peer and the "remote-attestation placeholder" has nothing to be a placeholder for. No candidate protocol was researched for this question.

Recommended: leave both parked until a platform service needs remote attestation, since no consumer exists. Blocks the placeholder in §9 and credential activation in §13.

A: (operator, 2026-11-07) leave both parked until a platform service needs remote attestation.

## Q15 (TODO-24 §5): must MarkGood be gated on the durable health-record write?

`append_health_record`'s return is discarded at `boot_health_check.c:629`, and the next boot can consume `ImpossibleOS-MarkGood` and delete the boot counter, which cannot be undone. The honest-write retrofit is open regardless; this asks only whether `X:\Boot\health.jsonl` is authoritative for the mark-good decision.

- **A: gate it.** No durable record, no MarkGood. A full or failing log volume could then roll a healthy OS back.
- **B: decouple it.** MarkGood follows the health verdict, and a record-write failure is logged as a failure. A healthy boot can leave no durable evidence.

Recommended: B, because a diagnostics volume should not decide which OS slot runs. `git stash@{2}` holds earlier WIP labelled as this decision; I did not read its diff. Blocks one item in §5.

A: (operator, 2026-11-07) B: decouple it. MarkGood follows the health verdict, and a record-write failure is logged as a failure.

## Q16 (00-infrastructure/TODO-04 §56): does a degraded test reap stop the boot?

After a degraded reap, `s_reap_degraded` stops the test plan, but `boot_phase3` still spawns `cmd.exe` into a slot an unfinished `task_fork` may hold. The review rated this [high]; the path needs a forker preempted across about 400 ms of yields, so it is a forward-looking SMP guard, not an observed failure.

- **A: fatal from `test_usermode_run()`.** The machine halts before the shell.
- **B: `task_create_internal` fails closed while poisoned.** A test flag then changes scheduler behaviour system-wide.

Recommended: A, since B reaches every task creation. I did not check whether the plan runs on non-test boots, which decides how much A costs. Blocks one item in §56.

A: (operator, 2026-11-07) A: fatal from `test_usermode_run()`. The launcher is built only under `KERNEL_TESTS`, so a release boot is unaffected.

## Q17 (00-infrastructure/TODO-04 §62): may a record whose lease census never completes be destroyed?

Such a record is held forever and announced on every pass. Converging it means destroying a record the pass could not classify, which §46, §53 and §62 exist to avoid.

- **A: keep holding and reporting.** Disk can stay held, visibly.
- **B: destroy after N consecutive held passes.** Disk is reclaimed, and a live lease can lose its record.

Recommended: A until a real run shows the hold costing disk; `UTEST_CENSUS_BUDGET` already removed the common cause. Blocks one item in §62.

A: (operator, 2026-11-07) A: keep holding and reporting. Revisit only if a real run shows the hold costing disk.

## Q18 (02-kernel-core/TODO-13 §8): does the platform ship language packs, and who owns them?

`NtGetMUIRegistryInfo` returns a fixed blob and `NtIsUILanguageComitted` has no installed-vs-active distinction, because no language-pack or MUI-install subsystem exists and no TODO under `todo/` owns one. The review rated the contract gap [H].

- **A: file an owner now.** A new section in a platform-services TODO, with both syscalls waiting on it.
- **B: one-language contract.** Implement the real Flags-driven multi-string with a single installed language, and file packs only when a second language is planned.

Recommended: B, since the wire contract can be correct without a pack system. I did not read the Flags semantics for this answer. Blocks one item in §8.

A: (operator, 2026-11-07) B: one-language contract. Implement the real Flags-driven multi-string with a single installed language, and file packs when a second language is planned.

## Q19 (01-boot-platform/TODO-15 §9/§13): does Tier 2 boot progress survive the splash spec?

`docs/design/shell.md` says the boot splash is logo plus spinner, with "No text unless boot fails". §9 renders `STAGE_NAME +NNNms` at `postbars=1` with a history scroll, and §10 and §13 follow from it. Neither carries a `**Design deviation:**` line, and the spec wins by rule.

- **A: drop Tier 2's text.** Keep the spec's splash, and leave stage detail to the Tier 1 diagnostic screen. §9's text items and §13's post-splash display are retired.
- **B: opt-in deviation.** `postbars=1` is a diagnostic mode, so record a deviation and extend the spec to allow it.

Recommended: A, since Tier 1 already serves diagnostics and the end-user splash stays as specified. The park's other condition, proven bare-metal reliability, is an operator judgment either way.

A: (operator, 2026-11-07) A: drop Tier 2's text. s9's text items and s13's post-splash display are retired, and stage detail stays on the Tier 1 diagnostic screen.

## Q20 (01-boot-platform/TODO-01 §23, TODO-20 §4, TODO-27 §3, TODO-29 §8): may the run add named `boot_info` fields and bump `BOOT_INFO_VERSION`?

These four sections each record an operator-gated ABI change, but none had a question here, so nobody had been asked. All four want an additive named field. CLAUDE.md reserves ABI changes to the operator, and a mismatched BOOTX64.EFI and kernel.exe halt with "boot_info: bad header".

- **A: standing approval for additive fields.** One version bump per change, both headers and the producer in one commit, an `F()` row in `dump-fields.inc`, and the 4-leg smoke matrix green before it ships. Removing or reordering a field still comes to you.
- **B: per-change approval.** Each section stays parked until you approve that field.

Recommended: A. `scripts/build.sh` rebuilds both binaries and three drift layers already catch a mirror mismatch. The cost is that a version bump strands any separately deployed loader, which only matters once one exists outside this repo.

A: (operator, 2026-11-07) A: standing approval for additive named fields, on the listed conditions. Removing or reordering a field still comes to the operator.

## Q21 (01-boot-platform/TODO-20 §3/§4/§5): what shape does the xHCI DMA handoff take?

§1 allocates DCBAA, command ring, event ring, ERST and scratchpad as disjoint pages, so one contiguous descriptor cannot cover them. Bootloader enumeration and true zero-delay handover wait on this, and the 500 ms Intel routing cost stays until it lands.

- **A: contiguous arena.** Allocate all the DMA state from one run, and publish one descriptor.
- **B: scatter/gather.** Keep the disjoint pages, and publish a variable-length list.

Recommended: A, since one range is simpler to reserve and validate. INFERRED from the item text and not measured; the arena's size with per-device rings is the thing to check first.

A: (operator, 2026-11-07) A: contiguous arena. Measure the arena's size with per-device rings first.

## Q22 (01-boot-platform/TODO-01 §22): what happens when the handoff pointer disagrees with the compiled base?

`boot_phase0` accepts a pointer that passes the envelope checks even if it is not the compiled `0x10000`, then reserves and validates a region nothing wrote.

- **A: refuse.** Fail closed on the earliest boot path. A third-party or experimental loader that places the struct elsewhere is rejected.
- **B: thread the address.** Carry the validated actual address through every consumer.

Recommended: A, because the fixed address is already the documented contract and this repo ships the only loader. B touches every consumer to support a loader that does not exist.

A: (operator, 2026-11-07) A: refuse, failing closed on the earliest boot path.

## Q23 (01-boot-platform/TODO-01 §23): what does integrity mean for the `boot_info` body?

Validation covers the 8-byte header only, so a corrupted body reaches every consumer. `serial_select_port` narrowed itself to four legacy COM bases to compensate.

- **A: loader-produced checksum over `header.size` bytes**, in a named field, checked before any field is read.
- **B: extend the payload-descriptor `checksum`** to cover the scalar body.
- **C: per-field validity discipline** in each consumer.

Recommended: A. It detects corruption, not forgery: the kernel holds no key at that stage, so a MAC would add nothing here. It needs Q20. `_reserved` is already refused as a home for the digest because an unnamed field is undetected drift.

A: (operator, 2026-11-07) A: a loader-produced checksum over `header.size` bytes in a named field, under the Q20 approval.

## Q24 (01-boot-platform/TODO-27 §7): may the platform write dbx updates to NVRAM?

An authenticated dbx apply cannot be undone, and a wrong one can stop the machine booting. The section also waits on the release-signing primitive in `15-installer-release/TODO-01 §5`, which your answer does not change.

- **A: never apply; report only.** Ship the freshness check, and leave updates to firmware tools.
- **B: apply with a brick-safety preflight**, once the signed baseline exists.

Recommended: A for now. Reversing it later costs only the write path, whereas a bad write can cost the machine. B stays open once there is bare-metal evidence to back the preflight.

A: (operator, 2026-11-07) A: never apply; report only. Ship the freshness check and leave updates to firmware tools.

## Q25 (02-kernel-core/TODO-02 §3): does registry policy get its own Phase-2 effective-policy object?

The §3 merge items say "operator-reserved architecture decision with no TODO owner", so shipping §4 would not release them. §2's `kernel_config_t` is an immutable Phase 0 snapshot, and registry policy arrives later.

- **A: separate Phase-2 effective-policy object.** The Phase 0 snapshot stays immutable, and §3 can land before §4 reads control sets.
- **B: wait for §4.** Merge only after control-set selection exists, keeping §3 after §4.

Recommended: A, since it leaves the shipped immutability guarantee alone. INFERRED from the stamps; I did not read the original design discussion, so veto it if that settled otherwise.

A: (operator, 2026-11-07) A: a separate Phase-2 effective-policy object.

## Q26 (01-boot-platform/TODO-02 §12): how should `HKLM\SYSTEM\SecureBoot\ShimCA` report a MOK-dev chain?

`scripts/extract-shim-ca.sh` recognises only Microsoft CA subjects, so a valid self-built shim publishes `0xFFFFFFFF`, the parse-failure sentinel, and audit consumers read an error for a valid trust mode.

- **A: add a distinct MOK-dev value** alongside the Microsoft ones, and keep the sentinel for real parse failures.
- **B: add a separate trust-mode value** and leave `ShimCA` Microsoft-only.

Recommended: A, one value with no new key for consumers to join. This registry value is the ABI the item parks on.

A: (operator, 2026-11-07) A: a distinct MOK-dev value, keeping the sentinel for real parse failures.

## Q27 (01-boot-platform/TODO-02 §20): what anchors verification of the MS-signed shim?

`sbverify --list` reads a signature table without validating it, so a tampered image with plausible metadata passes both the smoke test and `scripts/sign-efi.sh` `shim_ca_gate`. This was a high finding on 2026-08-13.

- **A: pin the Microsoft UEFI CA certificate** in the repo and verify cryptographically in both places.
- **B: approved-hash allowlist** extending `shim/SHA256SUMS`.

Recommended: B for now. It is the cheaper in-repo half the item names, and it needs no certificate choice while the MS-signed shim is about two years out. Moving to A later costs one script change in each place. Re-pinning a 2023-signed shim stays yours either way.

A: (operator, 2026-11-07) B: approved-hash allowlist extending `shim/SHA256SUMS`.

## Q28 (01-boot-platform/TODO-25 §5): how is a network-boot manifest signed and verified?

The section's stamp calls this user-reserved, and it gates every other §5 item, but it had no question here. `TODO-28`'s network-boot suite waits on it too.

- **A: firmware PKCS7.** The loader asks firmware to verify.
- **B: in-bootloader X.509.** The loader carries its own parser and trust store.
- **C: Secure-Boot-signed PE wrap.** The manifest travels inside a signed PE, and firmware's image verification does the work.

Recommended: C, because the loader gains no certificate parser and revocation follows the existing dbx path. The cost is that it verifies only with Secure Boot on, so a machine without it needs a stated policy. INFERRED from the item text, with no firmware survey behind it; if I recall correctly the PKCS7 protocol is optional in UEFI, which would count against A, and that is unverified.

A: (operator, 2026-11-07) C: Secure-Boot-signed PE wrap. With Secure Boot off, network boot refuses by default; an explicit `boot.conf` setting may allow it for lab use, and that boot is logged as unverified.

## Q29 (02-kernel-core/TODO-10 §30): what signs the release-flavor CI attestation?

An in-tree `kernel.link-trace.sha` can be rewritten by anything that can write the build tree, and `build-image.sh` repeats about 678 compiles because nothing tamper-resistant lets it skip them.

- **A: cosign keyless (OIDC).** CI identity signs, and there is no long-lived key to store, but it adds a tool dependency and writes that identity to a public log.
- **B: keyed identity.** A key in the vault signs, with no external log, and you hold the key.

Recommended: A. The repository is public, so the logged identity reveals nothing new, and there is no key to lose. Local builds would still recompute, since they cannot sign.

A: (operator, 2026-11-07) A: cosign keyless (OIDC).

## Q30 (01-boot-platform/TODO-29 §13): how does the `boot_history` NVRAM blob grow from 8 to 32 entries?

An old blob already lives in firmware variables on any machine that has booted this OS.

- **A: tail-append.** Grow the variable in place and keep existing offsets, so an older loader can still read the first 8.
- **B: version bump and migrate.** Cleaner layout, but a rollback to an older loader cannot read the migrated blob.

Recommended: A, because the A/B rollback path can land on an older loader. It is low priority ([L]), and real firmware's variable-size limits are unmeasured here, so bare metal should confirm 32 entries fit.

A: (operator, 2026-11-07) A: tail-append, keeping existing offsets. Bare metal must confirm 32 entries fit.

## Q31 (00-infrastructure/TODO-06 §11): should Check 7 delegate symbol resolution to clangd through lsp-bridge?

§10 measured `textDocument/documentSymbol` matching the local parse wherever both resolve, at about 0.1s cold start, with no compile database. Delegating would make `clangd` a hard dependency of the pre-commit lint on every contributor host and of `todo-graph.yml` in CI, which installs no clangd today.

- **A: keep the local resolver as the gate.** `resolve_symbol.py` stays the only resolver Check 7 and the identity gate read; lsp-bridge stays an interactive oracle for symbol questions.
- **B: delegate.** Gains the refs the local parse cannot see, but adds a toolchain dependency to every commit, and the identity gate's pinned `(path, start, end)` tuples would then shift with the installed clangd version.

A: (run decision, 2026-11-08) A: keep the local resolver. The coverage gap this was meant to close has since been closed locally (521 of 1,750 resolved on 2026-11-08, up from 57; the 520 floor was proved at 0 lost / 0 moved), and a version-dependent resolver would make the byte-exact identity gate non-reproducible across hosts. Reversal: low; `resolve_symbol()` is the single seam, so an opt-in clangd backend behind an env flag is one function plus a CI install step.

## Q32 (01-boot-platform/TODO-07 §21): should the loader keep writing `LoaderConfigTimeout` and `LoaderEntryDefault` as a report of its own effective timeout and default?

The loader wrote both every boot (NON_VOLATILE), and on a rejected store deleted `LoaderEntryDefault`. In the systemd Boot Loader Interface both are persistent settings that `bootctl set-timeout` / `bootctl set-default` write for the loader to honour; this loader does not honour either (LoaderFeatures bits 0 and 2 are not advertised), and on a machine shared with systemd-boot they belong to that loader.

- **A: stop writing them.** No one else's setting is overwritten or deleted. Cost: `bootctl status` no longer shows this loader's effective timeout and default (systemd-boot does not report them that way either). Values older builds wrote stay until something replaces them; this loader does not read them.
- **B: keep writing them volatile.** A volatile write is refused while a persistent copy exists, so it would have to delete the user's setting first: configuration loss.

A: (run decision, 2026-11-08) A: stop writing them. Reversal cost: restore two calls in `boot_loader_vars_publish()` plus their tests; no data is lost by A. §24's `d` key becomes the writer of `LoaderEntryDefault` when it is honoured, and then bit 2 is advertised.

## Q33 (01-boot-platform/TODO-07 §23): how does a rejected boot entry store get back to its own entries?

A rejected `bootentries.json` booted one synthesized fallback entry, and nothing kept a copy to recover from. Options: (A) a last-known-good shadow the loader keeps beside the store; (B) an A/B pair of stores; (C) regenerate the store from install data.

- **A: last-known-good shadow.** The loader copies a store it accepted to `bootentries.lkg.json` (through `bootentries.lkg.tmp`), only when the bytes changed, and boots the copy when the primary is present but rejected. The primary is never rewritten by the loader; `bootcfg restore` copies the shadow back on request. Cost: one extra file read per boot, a write only when the store changes, and a recovered boot may run an older configuration (the notice says so).
- **B: A/B stores.** Needs every writer (bootcfg, a future live editor) to alternate slots and a generation counter to pick between them; more format surface for the same protection.
- **C: regeneration.** No source of truth exists in the loader: the seed lives only in host-side `bootcfg.py`, and the result would be a default store, not the user's.

A: (run decision, 2026-11-08) A: last-known-good shadow, with an absent primary NOT consulting the shadow (deleting the store resets to the fallback) and restoration explicit rather than automatic (automatic restore would overwrite the evidence and persist a possibly stale policy). Reversal cost: remove `store_shadow_maintain`/`store_shadow_recover` and their two call sites in `bootx64.c`, the pure plan in `boot_entries_parser.c`, `bootcfg restore` and the smoke leg; the shadow files left on ESPs are inert without the code.

## Q34 (01-boot-platform/TODO-07 §24): where does a saved default (`LoaderEntryDefault`) rank, and how is it reported?

The menu's `d` key and `bootctl set-default` both write `LoaderEntryDefault`. Options for its rank: (A) below a one-shot and a tagged `BootCurrent` firmware option, above the lowest `sort_key`; (B) above the tagged `BootCurrent` option too, with the one-shot split out of the `BootCurrent` channel; (C) only as a menu highlight, never an automatic choice. Options for its report: (1) a new selection reason `SAVED_DEFAULT`; (2) `STORE_DEFAULT` plus a loader-side flag.

- **A, 2:** matches systemd-boot (one-shot, then `LoaderEntryDefault`, then configuration). A tagged firmware option is a per-option binding an administrator made with efibootmgr, and nothing in this tree writes one, so it keeps its rank; when it would hide a newly saved default, the `d` notice says so. Reporting `STORE_DEFAULT` keeps the handoff readable by an older kernel: `boot_decision.c` refuses a reason above `BOOT_SELECTION_REASON_MAX`, so a new reason would halt a rolled-back kernel booted by this loader. The serial policy line carries `via=saved-default`.
- **B:** a saved default would override a binding the firmware made for that one option.
- **C:** not the systemd semantic, and `bootctl set-default` would do nothing.
- A value an older build of this loader left as a per-boot report is honoured like any other: it names the entry that was the default when written, and `d` on that entry clears it. Only development machines ran those builds.

A: (run decision, 2026-11-09) A with report 2. Reversal cost: moving the ladder step is a few lines in `boot_policy_decide` plus its tests; adding a distinct reason later needs the kernel to advertise that it accepts it first (a capability bit), then the reason, the audit event mapping and the decoders in `boot_audit.c` and `bootx64.c`.

## Q35 (01-boot-platform/TODO-07 §25): how do `LoaderConfigTimeout`, an entry's `cmdline` and the `e` editor rank against what the loader already had?

Before this section the countdown came from the one-shot, else the default entry's `timeout_override`, else 5 s, and an entry's payload `cmdline` was validated but never used (the kernel always got boot.conf's `cmdline=`). Options for the timeout: (A) one-shot, then `LoaderConfigTimeout`, then `timeout_override`, then 5 s; (B) `timeout_override` above `LoaderConfigTimeout`. Options for the command line: (1) the edit, else a non-empty entry `cmdline`, else boot.conf; (2) boot.conf always wins, the entry only fills an empty boot.conf value.

- **A:** matches systemd-boot, where the EFI variable outranks the loader configuration and the one-shot outranks both; `bootctl set-timeout` and the `t`/`T` keys then do what they say. `timeout_override` stays as the store's own default for machines nobody has configured. `menu-disabled` is kept distinct from `menu-hidden` (it also ignores F11), and no key ever writes it, so a key press can never lock the keyboard out of the menu.
- **B:** a store entry would silently defeat `bootctl set-timeout`.
- **1:** an entry's `cmdline` is the systemd Type 1 `options` line, which is the entry's whole point; boot.conf remains the default for entries that set none (the seed store's entries set `""`, so nothing changes on a default install). The kernel is told which source won through `boot_config.cmdline_source` (offset 28, the old reserved bytes, no version bump; an older loader leaves 0, which means boot.conf). The editor is per entry (`allow_editor`), for split and safe entries only; a UKI's options are part of its signed image and are refused.
- **2:** the entry field would stay dead weight, and the editor would need its own precedence rule.
- A `LoaderConfigTimeout` an older build of this loader left as its per-boot report (Q32) is now honoured as a setting. Only development machines ran those builds, and `t`/`T` or `bootctl set-timeout` replace it.

A: (run decision, 2026-11-09) A with 1. Reversal cost: the timeout rank is one ordered pair of calls in `boot_menu_timeout_resolve()` plus its test; dropping the entry `cmdline` is one branch in `boot_policy_apply_cmdline()` in `bootx64.c`; `cmdline_source` can stay as a reported field either way. No data is lost by either choice.

## Q36 (01-boot-platform/TODO-10 §7): where do the absent-vs-failed driver results come from, and where does the degraded mask live?

- **A:** reuse TODO-14 §12's design-reviewed `enum boot_probe_result` (`include/kernel/boot_probe.h`: OK 0, ABSENT -1, FAILED -2) by shipping that section first from its preserved diff, then build TODO-10 §7 on it. The degraded set moves out of the loader handoff into a kernel-owned 64-bit word in `boot_init.c`, because `boot_info.degraded_mask` is kernel-written only, has 2 free bits for the 7 new slots, and sits in a packed block where widening shifts every later offset.
- **B:** invent a separate driver-result enum inside §7 and widen `boot_info.degraded_mask` with a `BOOT_INFO_VERSION` bump.
- **1:** a failed init records the degraded bit and leaves the subsystem NOT ready; only a usable fallback is "degraded and ready" (design review H1).
- **2:** the BSP owns AHCI's record on the async SKIP path, so a worker that never returns still appears in the summary (design review H2).

A: (run decision, 2026-11-09) A with 1 and 2. Reversal cost: the mask accessor is one function and five readers; restoring a handoff field is a rename in `boot_info.h`, the mirror and `dump-fields.inc`. Two enums for one convention would have needed a later merge across every driver.
