---
schema_version: 1
id: unblocked-deferral-backfill
domain: 02-kernel-core
status: draft
title: "TODO-35 -- Unblocked-Deferral Backfill (2026-07-27 cohort)"
---

# TODO-35 -- Unblocked-Deferral Backfill (2026-07-27 cohort)

> **Goal:** Ship the work that was deferred waiting on another section, whose owner has since shipped, and which no
> mechanism was ever going to come back to. This is real OS capability -- fault-recoverable user-buffer hardening,
> registry hive durability, quota enforcement, ELF unwind registration -- that was invisible to the overnight runner
> and would have been declared complete without it.

> [!IMPORTANT]
> **This is a ONE-TIME cohort, closed to new entries.** It exists because there was no owner-side sweep when these
> items were parked. That gap is now fixed: `implement-todo-section` step 18 runs
> `stranded_deferrals.py --owner <todo> --section <N>` on every section ship and re-opens what that section freed,
> while the owning TODO is still open. A future stranded item goes to its real owner, NOT here. If you find yourself
> adding an item to this file, the sweep failed -- fix the sweep instead.

> [!WARNING]
> **The parked items in the source TODOs stay `[/]`.** Each gains a reciprocal XREF to its item here. Do NOT flip a
> source item to `[ ]` in place: the sequencer's oracle classifies on the Implementation Order ROW plus section stamps
> and never on checklist items, so an in-place flip inside a shipped section is invisible to the runner AND drops out
> of `stranded_deferrals.py` (which matches `- [/]` only) -- invisible to both nets. A source item flips to `[x]` only
> when its section here ships, which the owner sweep does automatically at that point.

## Inputs

- [`src/kernel/registry.c`](../../src/kernel/registry.c) -- `registry_load_hives()` (defined, ZERO callers), `hive_load`, `RegSaveKey`/`RegRestoreKey`, no SMP lock anywhere in the file
- [`src/kernel/main/boot_storage.c`](../../src/kernel/main/boot_storage.c) -- `vfs_init()` line 390, `partition_mount_filesystems()` line 403, `registry_init()` line 866; the comment at line 858 naming the unlocked-tree hazard
- [`src/kernel/cpu_security.c`](../../src/kernel/cpu_security.c) -- `copy_from_user()` line 398, fault-recoverable via a static exception table
- [`src/kernel/quota/quota_owner.c`](../../src/kernel/quota/quota_owner.c) -- `quota_charge_chain`
- [`src/kernel/pe.c`](../../src/kernel/pe.c) -- `loaded_module_t`, `s_kernel32_exports[]` / `s_ntdll_exports[]`
- [`src/kernel/eif.c`](../../src/kernel/eif.c) -- `eif_decompress_segment()` line 354 (the capability TODO-03's item still calls unimplemented)
- [`scripts/overnight/stranded_deferrals.py`](../../scripts/overnight/stranded_deferrals.py) -- the audit that found this cohort; `--gate` refuses fixpoint until each is dispositioned
- -> XREF: `02-kernel-core/TODO-14-registry-completion.md §2` -- source of 13 items; registry locking is arguably its scope, deliberately owned here so its file stays closed
- -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §9` -- source of the quota-enforcement items
- -> XREF: `02-kernel-core/TODO-23-exception-dispatch-seh.md §13` -- shipped the fault-recoverable usercopy four items waited on
- -> XREF: `02-kernel-core/TODO-17-binary-system.md §5` -- source of the module-identity and unwind items

## Outcome

- The registry tree is SMP-safe, and hive durability (journal, dual-log recovery, lazy writer) rests on that rather than on a boot-time mutation window.
- User-buffer paths that refused non-NULL callers because no fault-recoverable copy existed now accept them safely.
- Quota limits that were stored and returned but never enforced are enforced.
- ELF modules register unwind data symmetrically with PE, and module identity survives beyond a bare name.
- `stranded_deferrals.py --gate` passes, so the runner can reach fixpoint honestly.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| ⭐  |   1   | Registry tree SMP synchronization and hive durability | --         |  [ ]   |
| 💎  |   2   | Registry save/restore hive bodies and transactional load | §1         |  [ ]   |
| 💎  |   3   | Quota enforcement and job memory accounting | --         |  [ ]   |
| 💎  |   4   | Module identity and ELF unwind registration | --         |  [ ]   |
| ⭐  |   5   | User-buffer hardening on fault-recoverable usercopy | --         |  [ ]   |
| 💎  |   6   | Boot, entropy, config and SRM leftovers  | --         |  [ ]   |

> 💎 = parity -- Win11 and Linux both have these; we deferred them and the deferral went stale.
> ⭐ = exclusive -- the SMP-locked registry and the declared-safe user-buffer contract go beyond both.

---

## 1. Registry Tree SMP Synchronization and Hive Durability

`registry_load_hives()` exists at `registry.c:4245` with **zero callers**. Its parked items say the blocker is "needs C:", but the boot order disproves that: `vfs_init()` (line 390) then `partition_mount_filesystems()` (line 403) both run before `registry_init()` (line 866), and the registry block hard-fails if VFS is not ready. The volume has been mounted at that point for a long time; the item text is stale.

The real hazard is different and worse. `grep -c 'spin_lock\|SPINLOCK' src/kernel/registry.c` returns **0** -- the registry tree has no SMP synchronization at all. `boot_storage.c:858` names this: readiness is deliberately withheld until after population because the tree "has no SMP lock yet" and is mutated "while APs are already running". That mitigation covers a short population window. Hive load, journal writes, and a 5-second lazy-writer DPC extend that window enormously on a kernel that is SMP-from-day-one. Locking is therefore the first item, not an afterthought.

- [ ] Add SMP synchronization to the registry tree: a lock discipline covering key create/delete/enumerate and value set/query, with the lock order documented against the VFS and object-manager locks it nests with.
- [ ] Remove the `boot_storage.c` readiness-withholding workaround once the tree is locked, and publish `SUBSYS_REGISTRY` at its natural point; keep the panic-path gate honest.
- [ ] Wire `registry_load_hives()` into boot after `registry_populate_defaults()`; diagnose the GPF the original attempt hit rather than re-deferring on it.
- [ ] Per-hive flush status via `registry_flush_hive_checked()` so one dirty hive failing does not silently mask the others.
- [ ] Two alternating journal files `SYSTEM.hive.log1`/`log2` with sequence numbers, matching the Windows NT dual-log scheme.
- [ ] Journal write cycle: dirty pages + header to the active log, fsync, commit marker, fsync, then copy into the main hive.
- [ ] Journal recovery on mount: check both logs for a valid commit marker, higher sequence number wins, torn write loses.
- [ ] Crash-while-clearing safety: clearing one journal leaves the other holding prior good state.
- [ ] Fix `registry_load_hives` double-reads -- `hive_best_source` validates the full candidate, then the load re-reads it.
- [ ] Secure one-shot deletion across main + journal + `.bak` before cross-reboot absorb.
- [ ] Recovery tests: post-consume corruption must not reintroduce a consumed `ExternalEntropy` value.
- [ ] Lazy writer: call `hive_flush_incremental` from a timer DPC on a 5 s cadence, plus an explicit `RegFlushKey` path.
- [ ] Commit: `"registry: SMP-lock the tree and land hive durability"`

**Test checkpoint:** concurrent key/value mutation from two CPUs leaves the tree consistent under the new lock; a hive survives a simulated crash mid-journal-write and recovers to the last committed state; a torn commit marker is rejected in favour of the other log; the lazy writer flushes without holding the lock across I/O. Test on: QEMU TCG (timing-sensitive ordering), QEMU KVM, bare metal.

---

## 2. Registry Save/Restore Hive Bodies and Transactional Load

The privilege gates for these shipped in TODO-15 §8 (`SeSinglePrivilegeCheck` is live at `security/privileges.c:241`).
The hive I/O bodies behind them did not -- both entry points return an error today, so the privilege check guards a
function that cannot succeed. Recorded twice in the cohort, from TODO-14 and from TODO-15; it is one piece of work.

The `FileHandle`-taking variants (`NtSaveKey`/`NtRestoreKey`) stay OUT of scope: they need `FileHandle`->path
resolution, which does not exist in the tree. Those two source items are dispositioned `park`, not filed here.

- [ ] Implement the `RegSaveKey` hive-write body: serialize the subtree under the section-1 lock, write through the journal path, and return a real status instead of the current error.
- [ ] Implement the `RegRestoreKey` hive-read body with validation before any live mutation, so a malformed hive cannot half-apply.
- [ ] Make `hive_load` fully transactional: the validate pass (`apply=0`) already catches malformed input; extend it so a failure part-way through apply rolls back rather than leaving a partial tree.
- [ ] Commit: `"registry: implement save/restore hive bodies on a transactional load"`

**Test checkpoint:** save-then-restore round-trips a subtree with values of every type; a truncated hive is rejected by the validate pass with the live tree unmodified; a fault injected mid-apply leaves no partial subtree. Test on: QEMU TCG, QEMU KVM.

---

## 3. Quota Enforcement and Job Memory Accounting

`quota_charge_chain` is live at `quota/quota_owner.c` and TODO-21 §13 has shipped, so the accounting substrate these items waited on exists. What is missing is enforcement: working-set and pagefile limits are stored and returned but never acted on, and `JOBOBJECT_EXTENDED_LIMIT_INFORMATION`'s memory fields report 0 rather than real pool bytes -- a caller reading them gets a confident wrong answer.

- [ ] Per-process VM/commit counters so working-set and pagefile limits can be enforced rather than merely stored.
- [ ] Enforce the stored working-set and pagefile limits at the allocation path, returning the documented failure status on breach.
- [ ] Populate `JOBOBJECT_EXTENDED_LIMIT_INFORMATION` memory fields with real pool bytes instead of 0-means-no-limit.
- [ ] Quota inheritance: a child gets its own process block and SHARES the parent user block; teardown mirrors the job-detach path.
- [ ] Per-user caps wired to TODO-02 config: register a privileged `quota.user.<type>` tunable per type in `quota_config.c`.
- [ ] Targeted-cleanup seam: a cache-drain / log-trim / refuse-new-handles entry point so pressure has somewhere to act.
- [ ] Bring `quota_pressure_tick` under the 100 us single-DPC watchdog threshold: it is the sampler DPC and it OVERRUNS on every observed boot.
      Measured 2026-07-28 on a 2-CPU WHPX boot: `llvm-addr2line` resolves the repeat-offending DPC `0x219f80` to `quota_pressure_tick`
      (`src/kernel/quota/quota_pressure.c:1294`), and it ran 100-481 us against the 100 us threshold in `dpc.c`, continuously, for the
      whole capture. TODO-34 counted **567 `pressure tick overran` lines plus 1776 DPC-watchdog lines** in an earlier capture and recorded
      them as "owned elsewhere" -- no such owner existed anywhere in `todo/` until this item, so the perf defect had been characterised
      three times and assigned zero times. The sampler's own overrun report already carries partial suppression; the DPC watchdog's does
      not, so the real fix is making the tick fast enough rather than quieting either reporter.
      Split the work honestly: profile which of `sample` / `due` / `arm` / `drain` dominates (the overrun line already prints all four,
      and `drain` is frequently the largest), then either bound the per-tick drain batch or move the drain off the DPC path.
      -> XREF: `02-kernel-core/TODO-34-serial-log-signal-to-noise.md` (item: "Rate-limit the DPC-watchdog warning" -- the log-noise half)
- [ ] Commit: `"quota: enforce working-set and pagefile limits, report real job memory"`

**Test checkpoint:** a process exceeding its working-set limit is refused with the documented status; job memory fields report nonzero pool bytes matching the ledger; a child shares the parent user block and teardown leaves no leaked charge. Test on: QEMU TCG, QEMU KVM.

---

## 4. Module Identity and ELF Unwind Registration

TODO-20 §5 and TODO-23 §6 shipped. ELF modules still register no unwind data, so an exception unwinding through an ELF frame has nothing to consult while the PE path has `.pdata`. Module identity is still a bare `name`, which is not enough to match a module to its symbols.

- [ ] Extend module identity beyond `name`: image-ID storage for EIF `build_id`, PE CodeView GUID, and ELF `.note.gnu.build-id`.
- [ ] Register ELF `.eh_frame_hdr` unwind data symmetrically with PE `.pdata`: add the fields to `loaded_module_t` and parse `PT_GNU_EH_FRAME`.
- [ ] `NtQuerySection(SectionImageInformation)`: persist the PE optional-header fields (ImageBase, entry, stack sizes) on the section object so the query returns real values.
- [ ] ELF constructor/destructor ordering: call `DT_PREINIT_ARRAY` (main executable only) then `DT_INIT`/`DT_INIT_ARRAY` in dependency order.
- [ ] Commit: `"exec: register ELF unwind data and extend module identity"`

**Test checkpoint:** an exception unwinding through an ELF frame finds its FDE via `.eh_frame_hdr`; a module reports a build-ID distinct from its name; `NtQuerySection` returns the real ImageBase rather than a zero. Test on: QEMU TCG, QEMU KVM.

---

## 5. User-Buffer Hardening on Fault-Recoverable Usercopy

Four call sites refuse a legitimate non-NULL user buffer purely because no fault-recoverable copy existed when they were written. It exists: `copy_from_user()` at `cpu_security.c:398` recovers a `#PF` inside `__uaccess_copy_from` via a static exception table and returns -1 rather than bugchecking. Each of these is now a small, bounded change, and each currently presents as a capability gap to a caller.

- [ ] Boundary probe+copy for a non-NULL `Environment` in the environment-block path: copy the caller block into a terminated kernel snapshot, then operate on the snapshot.
- [ ] Apply the same probe+copy to `RtlQueryEnvironmentVariable_U`'s non-NULL `Environment`, which currently refuses it outright.
- [ ] `NtSetInformationProcess(ProcessMitigationPolicy)`: read the range-only-probed ring-3 buffer through the fault-recoverable path instead of deferring the class.
- [ ] Honor the client `SecurityQos` for RING-3 ALPC clients, copying the user attribute block through the recoverable path.
- [ ] Commit: `"kernel: accept non-NULL user buffers via fault-recoverable usercopy"`

**Test checkpoint:** each path accepts a valid user buffer and returns the documented error (never a bugcheck) for an unmapped one; a fault injected mid-copy leaves no partial kernel-side state. Test on: QEMU TCG (fault-path timing), QEMU KVM, bare metal.

---

## 6. Boot, Entropy, Config and SRM Leftovers

The tail of the cohort: each waited on a capability that is now present, verified at file:line during the 2026-07-27
triage. Grouped because none is large enough to carry a section alone, not because they share a subsystem.

- [ ] Harden `media_role_locate_blackbox_fs()` in `bootx64.c`: bind BlackBox by GPT name/GUID rather than FAT label alone.
- [ ] Admin external-entropy one-shot (Win11 `ExternalEntropy` parity): wire `entropy_external_consume()` to its registry-backed admin path.
- [ ] Canary seed sharing: feed `csprng_u64()` into `canary_init()`, which is live in `main.c`.
- [ ] Mint the klog session key at `klog_disk_enable()` via `csprng_fill()`, gated on `csprng_crypto_ok()` with skip+WARN when degraded.
- [ ] Wire the LZ4 codec into EIF compressed segments -- `eif_decompress_segment()` shipped at `eif.c:354`, so the item's "unimplemented" text is stale.
- [ ] Per-provider stack-trace flag for klog: capture frames via `RtlCaptureStackBackTrace` (live at `rtl/unwind.c:1266`) and symbolize.
- [ ] Complete the `SYSTEM_KERNEL_CONFIG_INFORMATION` set/write contract; the shared ABI struct already exists in `nt/sysconfig_info.h`.
- [ ] Enforce `SeSystemProfilePrivilege` on config writes using `SeSinglePrivilegeCheck` (live at `security/privileges.c:241`).
- [ ] Per-key provenance in the config query class: snapshot fields shipped, per-key provenance did not.
- [ ] `GetCommandLineW()` returns `PEB->ProcessParameters->CommandLine.Buffer` from the real PEB rather than a placeholder.
- [ ] Elevation-transition invocation: allow `NtCreateProcess` to raise child integrity level above parent under the SRM's rules.
- [ ] Spawn CSRSS as a `SYSTEM`-token process at kernel init Phase 3.
- [ ] Commit: `"kernel: land the unblocked boot, entropy, config and SRM deferrals"`

**Test checkpoint:** each item verified against its own subsystem's suite; the klog session key is present and distinct per boot; `GetCommandLineW` returns the real command line for a spawned process; config writes without the privilege are refused. Test on: QEMU TCG, QEMU KVM; bare metal for the boot-media item.

---

## OS Comparison

| ⭐  | Feature                              | 🪟 Win11             | 🐧 Linux                 | 🚀 Impossible OS                        |
| --- | ------------------------------------ | -------------------- | ------------------------ | --------------------------------------- |
| 💎  | Registry/config store SMP-safe       | ✅ CM lock hierarchy | ✅ per-subsystem locking | ⬜ §1 no lock in the tree today         |
| 💎  | Hive journal with dual-log recovery  | ✅ `.LOG1`/`.LOG2`   | ⚠️ no direct analogue     | ⬜ §1 matches the NT scheme             |
| 💎  | Working-set / commit limits enforced | ✅ enforced          | ✅ cgroups + rlimits     | ⚠️ §3 stored and returned, not enforced  |
| 💎  | ELF unwind data registered           | ⚠️ PE-only by design  | ✅ `.eh_frame_hdr`       | ⬜ §4 PE has `.pdata`, ELF has nothing  |
| ⭐  | Fault-recoverable usercopy contract  | ✅ SEH probe         | ✅ `copy_from_user`      | ✅ §5 shipped; callers not yet using it |

## Unit Tests

- [ ] Registry SMP: concurrent mutation from two CPUs leaves a consistent tree; lock order asserted against VFS and object-manager locks.
- [ ] Hive journal: commit-marker recovery picks the higher sequence number; a torn write is rejected; crash-while-clearing keeps prior good state.
- [ ] Quota: a working-set breach returns the documented status; job memory fields are nonzero and match the ledger; inheritance shares the user block.
- [ ] Unwind: an ELF frame resolves its FDE via `.eh_frame_hdr`; module identity reports a build-ID distinct from name.
- [ ] Usercopy: every §5 path returns the documented error for an unmapped buffer and never bugchecks.
- [ ] Pure helpers only -- no live boot infrastructure per [docs/infrastructure/test-policy.md](../../docs/infrastructure/test-policy.md).

## Verification

- [ ] `bash scripts/test.sh` green with the new suites registered in their existing categories.
- [ ] `bash scripts/test-smoke.sh` reports `SMOKE TEST PASSED` -- §1 touches boot-path registry wiring, so this is required, not advisory.
- [ ] `python3 scripts/overnight/stranded_deferrals.py --gate` exits 0 once every source item is re-pointed or dispositioned.
- [ ] Every source `[/]` item carries a reciprocal XREF to its item here, and none was flipped in place.
- [ ] Bare metal: boot with hive load wired, confirm the registry survives a power cut mid-write.

## History

**2026-07-27 -- filed from the stranded-deferral triage.** `stranded_deferrals.py` found 48 cross-TODO `[/]` items whose XREF owner had shipped both stamps, across 15 source files of which 14 were fully DONE -- so the sequencer's section oracle, which classifies on the Implementation Order row plus section stamps and never on checklist items, would never have revisited any of them. All 48 were triaged at file:line: **39 reopen** (38 distinct; the `RegSaveKey`/`RegRestoreKey` bodies were recorded from both TODO-14 and TODO-15), **8 park** with a verified-absent second blocker, **1 done**. Item text proved stale in both directions -- several items read "BLOCKED: X unimplemented" where X had long since shipped (`eif_decompress_segment`, the fault-recoverable usercopy), and the registry cluster's "needs C:" blocker was disproved by the boot order itself. The genuinely new finding was the unlocked registry tree, which is why §1 leads with SMP synchronization rather than the durability work that motivated the section.
