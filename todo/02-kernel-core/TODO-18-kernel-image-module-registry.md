---
schema_version: 1
id: kernel-image-module-registry
domain: 02-kernel-core
status: active
title: "TODO-18 -- Kernel Image & Module Registry"
---

# TODO-18 -- Kernel Image & Module Registry

> **Validated:** 2026-07-10 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Gap-audited:** 2026-07-10 | gap-audit + codex-gap-audit; 8 findings filed (module-loader ownership fix, atomic-publish transaction, address-space/lifetime-safe lookup, qualified symbol + duplicate-reject, unwind ownership split + dynamic/JIT unwind 💎, notify path-snapshot + ordering, unloaded-module tombstone ring 💎, unload quiescence state machine); reciprocals filed in T23 §6 / T29 §11; /proc-maps + retroactive-CI-revocation rejected to XREF (T10 / T19)

> **Goal:** Create the canonical loaded-image registry for the whole OS: kernel image, boot modules, drivers, kernel modules, user images, DLLs, EIF modules, PE sections, symbol tables, unwind metadata, code-integrity decisions, and provenance. Crash dumps, KD, stack walking, hot-patching, code integrity, ETW, and process introspection must all read the same source of truth.

> [!IMPORTANT]
> **Current state:** Crash dump TODO has module-enumeration helpers; binary loader TODO has LDR module list work; EIF has module registration. The canonical `.kmod` kernel-module loader is `D04 T05` (Kernel Module System, per CLAUDE.md); `D12 T03` (ELF Relocations & Kernel Module System) owns the ELF relocation engine that loader consumes -- these two must not each define a private loaded-module registry (both currently sketch incompatible `loaded_module_t`/`loaded_kmod_t` tables). These are separate plans. There is no central image registry object, no global address-to-image lookup, no canonical symbol/unwind provider, no provenance record, and no loader-independent image notification.

## Inputs

- [`src/kernel/exec.c`](../../src/kernel/exec.c)
- [`src/kernel/symtab.c`](../../src/kernel/symtab.c)
- [`src/kernel/pe.c`](../../src/kernel/pe.c)
- [`src/kernel/eif.c`](../../src/kernel/eif.c)
- → XREF: [`TODO-17-binary-system.md`](./TODO-17-binary-system.md) -- image loaders and LDR lists
- → XREF: [`TODO-23-exception-dispatch-seh.md`](./TODO-23-exception-dispatch-seh.md) -- unwind and stack walking
- → XREF: [`TODO-27-crash-dump-generation.md`](./TODO-27-crash-dump-generation.md) -- dump module streams
- → XREF: [`TODO-19-code-integrity-trust-policy.md`](./TODO-19-code-integrity-trust-policy.md) -- CI decisions stored here
- → XREF: [`04-drivers-hardware/TODO-05-kernel-module-system.md`](../04-drivers-hardware/TODO-05-kernel-module-system.md) -- canonical `.kmod` loader owner (must call `kimage_register`/`_unregister` + lookup/unload-lifetime APIs)
- → XREF: [`12-user-platform-sdk/TODO-03-elf-reloc-kernel-modules.md`](../12-user-platform-sdk/TODO-03-elf-reloc-kernel-modules.md) -- ELF relocation engine consumed by the D04 T05 loader (not a second registry)

## Outcome

- Every loaded executable image has one `KIMAGE_ENTRY`.
- Address-to-image and address-to-symbol lookup are fast and lock-safe.
- Crash dumps, KD, ETW, exception dispatch, CI, and process APIs enumerate the same image list.
- Image load/unload notifications are delivered through Executive callbacks and kernel notifications.
- Unwind metadata is registered once and reused by SEH, KD, and crash analyzer.
- Provenance includes source path, hash, signer, loader, trust decision, load time, and owning process/session.

## Implementation Order

| ⭐  | Order | Deliverable                             | Depends On        | Status |
| --- | :---: | --------------------------------------- | ----------------- | :----: |
| 💎  |   1   | `KIMAGE_ENTRY` data model               | --                |  [x]   |
| 💎  |   2   | Global and per-process image registries | T17, D04 T05      |  [/]   |
| 💎  |   3   | Address range index                     | Ex generic table  |  [/]   |
| 💎  |   4   | Symbol provider abstraction             | symtab            |  [/]   |
| 💎  |   5   | Unwind metadata registry                | T23               |  [/]   |
| 💎  |   6   | Loader integration                      | T17, T20, D04 T05 |  [/]   |
| ⭐  |   7   | Image notifications and callbacks       | T06, T16          |  [/]   |
| 💎  |   8   | KD/crash dump integration               | T27, T29          |  [/]   |
| ⭐  |   9   | Provenance, CI, and hotpatch metadata   | T19               |  [/]   |
| 💎  |  10   | Tests and consistency verifier          | §1..§9            |  [/]   |

## 1. `KIMAGE_ENTRY` Data Model

- [x] Define `kimage_entry_t` (832 B, offsets pinned) in `include/kernel/kimage.h`: canonical `kimage_format_t` (UNKNOWN=0, not EXEC_FMT_*) + embedded `EX_RUNDOWN_REF life` gate (avoids a check-then-increment unload race).
- [x] Provenance/identity fields: SHA-256 `hash[32]`, durable `identity[32]` (ELF build-id / PE CodeView / EIF build_id, survives into tombstones), `signer[64]`, `full_path[VFS_MAX_PATH]` + explicit length, `name[64]`.
- [x] Optional metadata handles (symbols, unwind_ranges, exports, imports, relocations, debug_info) as 0-until-populated hook points for §4/§5/§6; documented process-local, not durable across unload.
- [x] `kimage_type_t` role enum, independent from format: kernel, HAL/platform, boot module, driver, kmod, process, DLL, EIF module, synthetic/stub.
- [x] Record invariants documented in kimage.h (copy-safety of `life`/handles; bounded NUL-terminated strings); enforced by the §2 validator, tombstone copy-safety owned by §8.
- [x] Review adoptions: `aligned(64)` + alignof assert (records never straddle cache lines) and a `kimage_format_from_exec_fmt()` bridge (EXEC_FMT_* 0/1/2 -> KIMAGE 1/2/3, rejects unknown) so a raw field copy cannot silently corrupt format.
- [x] Commit: `"kernel: kimage -- KIMAGE_ENTRY data model"`

**Test checkpoint:** `sizeof(kimage_entry_t)` is 832 (compile-time `_Static_assert` + runtime assert); a valid populated entry round-trips every field with NUL-terminated in-capacity strings; the flags bitmask is single-bit non-overlapping; a zero-init entry classifies as UNKNOWN (not ELF); the embedded rundown gate acquires then refuses after rundown. Test on: QEMU WHPX + TCG; bare metal.
> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 6 KImage suites, 0 failures
>
> **Notes:**
> - Shipped `include/kernel/kimage.h` (`kimage_entry_t` 832 B cache-line-aligned, 4 enums, `KIMAGE_FLAG_*` bitmask, `kimage_format_from_exec_fmt` bridge, pinned asserts) + `src/kernel/test/test_kimage.c` (6 suites: flags, zero-init, lifetime gate, round-trip, format-conversion, alignment).
> - Header-only data model; tests registered in `test_runner.c`, run under `SUITE=exec`. Design + adversarial adoptions (rundown gate, canonical format, durable identity, VFS_MAX_PATH) are in the commit message.
> - Metadata handles / `life` gate / `identity` are 0-until-populated hook points for later sections; nothing populates the registry yet.
> - Canonical doc: the `include/kernel/kimage.h` record-invariants contract block.
> - Scope: §1 owns the data model only. §2 owns registries + the common validator; §4 symbols, §5 unwind, §8 tombstone copy-safety.
> **Verified:** 2026-07-10 | commit `99de36bc` | 6/6 items | build OK | tests 6 suites
> **Quality reviewed:** 2026-07-10 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 3M fixed | scope: kernel-code-quality

---

## 2. Global and Per-Process Image Registries

> [!IMPORTANT]
> **Blocked on an operator-reserved architectural decision** (kimage-supersedes-exec migration strategy). The existing `exec_register_module` / `loaded_module_t` registry (`s_modules[64]` + `s_module_lock`, `src/kernel/exec.c:35-618`) is already the de-facto registry, consumed by the shipped crash-dump path (TODO-27 §3) and the FATAL kernel Phase-1 self-registration (`boot_interrupts.c:253`). §2's atomic-publish transaction explicitly fixes `exec_register_module`, so §2 IS that migration; a second parallel kimage store would create the exact registry fork this TODO exists to eliminate. Four questions must be answered first: (A) kimage as the backing store that `exec_register_module` delegates to, vs (B) kimage superset with consumers rewired; is a T17 LDR-list API extracted from `exec.c:340` first; per-process registry as a new `struct task` field vs PEB->Ldr only; does `kimage_init()` absorb the fatal Phase-1 self-registration; add `SUBSYS_KIMAGE`?

- [ ] DECIDE the migration strategy (A backing-store-delegate vs B superset-rewire) and answer the 4 open questions in the callout above, before any registry storage code. -> XREF: TODO-27 §3 (existing crash-dump consumer of `exec_register_module`).
- [ ] Global registry owns kernel, boot, driver, and kmod images.
- [ ] Per-process registry owns main image and DLL/shared libraries.
- [ ] Expose lock-safe iteration API with rundown protection.
- [ ] PEB `Ldr` three-list shape (InLoadOrder / InMemoryOrder / InInitializationOrder) is owned by T17; consume its list APIs here, do not define a fourth list. -> XREF: TODO-17 §3 (LDR module list).
- [ ] Atomic publish transaction: stage every allocation (KIMAGE + three PEB lists + index nodes), commit membership in one step, reverse-roll-back on failure -- fixes the partial-publish divergence in `exec_register_module`.
- [ ] Fire load notifications only after commit; unload removes in symmetric reverse order. Test injected failure at every publish stage; assert membership + list order, not just counts.
- [ ] Common `kimage_entry_t` validator every producer calls before publish, enforcing the kimage.h record invariants (bounded `identity_len`, NUL-terminated in-capacity path/name/signer). Add the length/termination rejection tests here.
- [ ] Commit: `"kernel: kimage -- global and per-process registries"`

**Test checkpoint:** Registering N images and iterating returns exactly N under a spinlock/rundown; concurrent register/unregister on two CPUs leaves the count consistent. Serial log shows `"kimage: registered '<name>' (global|pid=<N>)"`. Test on: QEMU WHPX + TCG; bare metal.
> **Deferred:** 2026-07-10 | [architectural] §2 registries ARE the kimage-supersedes-exec migration; the strategy (backing-store-delegate vs superset-rewire) + 4 open questions touch the FATAL Phase-1 kernel self-registration and the shipped TODO-27 §3 crash-dump consumer (reason: operator-reserved, expensive-to-reverse boot/ABI refactor) -> XREF: TODO-27 §3 (crash-dump consumer of `exec_register_module`; DECIDE item at §2 line 85)

---

## 3. Address Range Index

- [ ] Two separate indexes: one kernel-VA index, one per-address-space (per-process) index. The same user VA can belong to different images in different processes.
- [ ] `image_lookup_by_address()` requires an address-space key `(asid/process, addr)` for user ranges; kernel ranges use the kernel index. No process-agnostic user lookup.
- [ ] Lifetime-safe lookup: return an acquired reference or a copied snapshot, never a bare entry pointer -- concurrent unload must not invalidate a result mid symbol/unwind walk. Support RCU-style lookup from any context including panic/NMI.
- [ ] Handle overlapping ranges as fatal loader bugs unless explicitly marked alias mapping.
- [ ] Cache last lookup per CPU keyed by address-space generation (invalidate on context switch / space teardown) for the stack-walk hot path.
- [ ] Commit: `"kernel: kimage -- address range index and lookup"`

**Test checkpoint:** `image_lookup_by_address(base)` and `(end-1)` return the entry; `(end)` and `(base-1)` return NULL; an overlapping insert is rejected/faults. Per-CPU cache hit on repeated lookup. Test on: QEMU WHPX + TCG; bare metal.
> **Deferred:** 2026-07-10 | [architectural] indexes the kimage registry (§2); blocked until the §2 migration strategy is decided (reason: downstream of §2) -> XREF: §2 (DECIDE item, line 85).

---

## 4. Symbol Provider Abstraction

- [ ] Providers: kernel symtab file, PE export/PDB-lite info, ELF symtab/dynsym, EIF metadata, synthetic stubs.
- [ ] API: `ksym_lookup_addr`, `ksym_lookup_name(image/namespace, name)`, `ksym_iterate_module`. Name lookup MUST take an image/namespace qualifier -- unqualified lookup can bind the wrong ring-0 function by incidental load order.
- [ ] Global-export uniqueness: reject a duplicate exported name at provider/module registration (Linux kallsyms/namespaces lesson); define weak/local/forwarded precedence and a deterministic ambiguity error.
- [ ] Return symbol name, displacement, source file/line when available.
- [ ] Mark stripped images clearly for crash output.
- [ ] Commit: `"kernel: kimage -- symbol provider abstraction"`

**Test checkpoint:** `ksym_lookup_addr(known_sym+4)` returns the symbol name with displacement 4; a stripped image reports `<no symbols>`; `ksym_lookup_name` round-trips. Test on: QEMU WHPX + TCG; bare metal.
> **Deferred:** 2026-07-10 | [architectural] symbol providers hang off kimage registry entries (§2); blocked until the §2 migration strategy is decided (reason: downstream of §2) -> XREF: §2 (DECIDE item, line 85).

---

## 5. Unwind Metadata Registry

- [ ] Ownership split: T18 owns unwind-metadata lifetime + a normalized sorted/indexed representation; T23 owns the unwind execution/lookup engine; T17 owns parsing image sections. -> XREF: TODO-23 §6 (table-based unwind).
- [ ] Register PE `.pdata`, EIF native unwind, and ELF `.eh_frame`. Translate `.eh_frame` to the normalized table at load time -- do not walk DWARF live. Lookup is bounded/indexed (ORC-style range-narrowing), NOT claimed O(1).
- [ ] Provide range lookup for the exception-dispatch unwind engine (T23).
- [ ] Validate unwind ranges are inside executable image sections.
- [ ] Include unwind table hash in image provenance.
- [ ] [/] Dynamic/JIT unwind (`RtlAddFunctionTable`) 💎: add/delete/callback APIs shipped in TODO-23 §6; the growable-table variant + T18 normalized storage remain. -> XREF: TODO-23 §6.
- [ ] Commit: `"kernel: kimage -- unwind metadata registry"`

**Test checkpoint:** An unwind range lookup for an address inside a registered image returns the `.pdata`/`.eh_frame` entry; a range outside executable sections is rejected at registration. Test on: QEMU WHPX + TCG; bare metal.
> **Deferred:** 2026-07-10 | [architectural] unwind metadata is registered against kimage entries (§2); blocked until the §2 migration strategy is decided (reason: downstream of §2) -> XREF: §2 (DECIDE item, line 85).

---

## 6. Loader Integration

- [ ] `exec_load`, PE loader, ELF loader, EIF loader, and kmod loader call `kimage_register`, mapping format via `kimage_format_from_exec_fmt()`.
- [ ] Set the TRUE format when EXEC_FMT is a WinDbg presentation lie: the ELF kernel registers EXEC_FMT_PE, so its KIMAGE record must be type=KERNEL, format=ELF (not the converted PE).
- [ ] Unload paths call `kimage_unregister` after rundown.
- [ ] Existing crash module enumeration migrates to this registry.
- [ ] Load failures must not leave partial image entries.
- [ ] Lock-free crash-safe module-range lookup: `exec_find_module_by_pc` takes `s_module_lock` (not NMI-safe), so crash-path PC validation can deadlock -- add an immutable module-range snapshot -> XREF: `TODO-23 §7` (`rtlp_is_code_pc`)
- [ ] Commit: `"kernel: kimage -- loader integration"`

**Test checkpoint:** Loading an EIF/PE image registers exactly one `KIMAGE_ENTRY`; a failed load leaves zero entries (atomic); the migrated crash-module enumeration returns the same set. Serial log shows `"kimage: registered '<name>'"` on load. Test on: QEMU WHPX + TCG; bare metal.
> **Deferred:** 2026-07-10 | [architectural] loaders call `kimage_register` (§2); the loader-migration IS the §2 decision (which loaders delegate/rewire) (reason: same operator decision as §2) -> XREF: §2 (DECIDE item, line 85).

---

## 7. Image Notifications and Callbacks

- [ ] Publish `ImageLoad` and `ImageUnload` through Executive callback object.
- [ ] Publish notification state through the crash-dump/diagnostics path (T27) for service/user consumers.
- [ ] Include CI decision and trust level.
- [ ] Payload carries a copied SNAPSHOT of `KIMAGE_ENTRY.full_path` + stable identity fields, never a live-recomputed path -- avoids the Windows `FullImageName` stale-buffer class of bug.
- [ ] Define the Impossible OS ordered sequence explicitly (process-create notify, then image-load notify, then user entrypoint) as our own contract -- Windows does not guarantee cross-callback ordering portably.
- [ ] Ensure callbacks run after registry insertion but before user entrypoint for user images; specify callback IRQL, reentrancy, and unregister-drain rules.
- [ ] Process-create/exit notifications are PRODUCED by the process-lifecycle owner; add the reciprocal producer item there. -> XREF: TODO-21 (process/thread create-exit notify producer).
- [ ] Commit: `"kernel: kimage -- image load/unload notifications"`

**Test checkpoint:** A registered callback fires on image load with the correct base/name/CI-decision, after insertion (lookup succeeds inside the callback) and before the user entrypoint. Unload fires the ImageUnload callback. Test on: QEMU WHPX + TCG; bare metal.
> **Deferred:** 2026-07-10 | [architectural] notifications fire after registry commit (§2); blocked until the §2 migration strategy is decided (reason: downstream of §2) -> XREF: §2 (DECIDE item, line 85).

---

## 8. KD/Crash Dump Integration

- [ ] KD module list reads global/per-process registry.
- [ ] Crash dumps emit module stream from `KIMAGE_ENTRY`.
- [ ] Panic screen "what failed" uses address-to-image lookup.
- [ ] Bounded recently-unloaded tombstone ring (parity 💎, Win `MmUnloadedDrivers`): identity + canonical path + ranges + build-id/hash + PID + unload time; panic-safe overwrite ring; consulted on a live-registry miss.
- [ ] Tombstone/snapshot is copy-SAFE: field-copy the durable fields and re-init a fresh gate; never raw-memcpy a live `kimage_entry_t` (its `life` is a live atomic; handles are process-local). Per the kimage.h copy-safety invariant.
- [ ] `dmpanalyze.exe` consumes same serialized format, including the unloaded ring.
- [ ] Serialize the tombstone ring as an unloaded-modules stream via the existing MDMP stream framework (TODO-27 §4, already shipped) -- T18 owns the ring; the dump writer emits it as extra module entries. -> XREF: TODO-27 §4.
- [ ] Reciprocal: KD exposes an unloaded-module (`lm`-style) query. -> XREF: TODO-29 §11 (module list; add unloaded query).
- [ ] Commit: `"kernel: kimage -- KD and crash dump integration"`

**Test checkpoint:** The crash-dump module stream lists every registered image once; the panic screen resolves a faulting RIP to `<image>+<offset>` via `image_lookup_by_address`. Test on: QEMU WHPX + TCG; bare metal.
> **Deferred:** 2026-07-10 | [architectural] KD/crash-dump reads the kimage registry (§2) + its address index (§3); blocked until the §2 migration strategy is decided (reason: downstream of §2) -> XREF: §2 (DECIDE item, line 85).

---

## 9. Provenance, CI, and Hotpatch Metadata

- [ ] Store load source: bootloader, VFS path, memory buffer, generated/stub.
- [ ] Store CI hash, signer, policy mode, decision, and audit ID.
- [ ] LIVE -> GOING unload state machine (refcount alone cannot prove no CPU runs image code): block new entry, pin via exported refs + callouts, drain callbacks/work/IRQs, wait an SMP grace period, remove indexes, THEN unmap.
- [ ] Test unload while another CPU is blocked inside module code and while callouts remain queued -- both must be refused until quiescent.
- [ ] Reserve hotpatch metadata fields only (patchable functions, original-bytes hash, active patch ID); a real hotpatch ENGINE is a separate concrete owner, not implied by these reserved fields.
- [ ] Retroactive CI re-check/revocation of an already-loaded image is a policy decision, not a registry baseline. -> XREF: TODO-19 (CI revocation policy).
- [ ] Commit: `"kernel: kimage -- provenance, CI, and hotpatch metadata"`

**Test checkpoint:** A registered image records its load source + CI decision + signer; unregister is refused (returns busy) while the refcount indicates active frames, and succeeds after rundown. Test on: QEMU WHPX + TCG; bare metal.
> **Deferred:** 2026-07-10 | [architectural] provenance/unload-quiescence extend the kimage entry lifecycle (§2); blocked until the §2 migration strategy is decided (reason: downstream of §2) -> XREF: §2 (DECIDE item, line 85).

---

## 10. Tests and Consistency Verifier

- [ ] Unit tests: register/unregister, overlap reject, addr lookup, symbol lookup, unwind lookup, per-process iteration.
- [ ] Boot verifier: kernel image, symtab, and loaded drivers all appear exactly once.
- [ ] Fuzz malformed image metadata; no registry leaks.
- [ ] Bulletproofing: PEB Ldr list count matches kernel per-process registry for each process.
- [ ] Commit: `"kernel: kimage -- tests and consistency verifier"`

**Test checkpoint:** All `test_kimage` assertions pass (register/unregister, overlap reject, addr/symbol/unwind lookup, per-process iteration); the boot verifier confirms kernel + symtab + drivers appear exactly once and the PEB-Ldr-vs-registry count matches per process. Test on: QEMU WHPX + TCG; bare metal.
> **Deferred:** 2026-07-10 | [architectural] the consistency verifier tests §2-§9 which are all blocked until the §2 migration strategy is decided (reason: downstream of §2) -> XREF: §2 (DECIDE item, line 85).

---

## OS Comparison

| ⭐  | Feature                    | 🪟 Win11                       | 🐧 Linux                   | 🚀 Impossible OS                     |
| --- | -------------------------- | ------------------------------ | -------------------------- | ------------------------------------ |
| 💎  | Loaded-image registry      | ✅ PsLoadedModuleList          | ✅ /proc/modules + vmap    | 🔄 §1-§2 KIMAGE_ENTRY registries     |
| 💎  | Address-to-image lookup    | ✅ KLDR range tables           | ✅ __module_address        | 🔄 §3 interval index + per-CPU cache |
| 💎  | Symbol resolution          | ✅ DbgHelp/PDB                 | ✅ kallsyms                | 🔄 §4 provider abstraction           |
| 💎  | Unwind metadata registry   | ✅ RtlLookupFunctionEntry      | ✅ ORC (kernel)            | 🔄 §5 normalized table; T23 executes |
| 💎  | Dynamic/JIT unwind         | ✅ RtlAddFunctionTable         | ⚠️ no kernel JIT unwind    | 🔄 §5 growable table; T23 add/delete |
| ⭐  | Format-agnostic image list | ⚠️ PE only                     | ⚠️ ELF only                | 🔄 §1-§6 PE + ELF + EIF + kmod       |
| 💎  | Image load/unload notify   | ✅ PsSetLoadImageNotifyRoutine | ⚠️ module notifier chain   | 🔄 §7 Executive callbacks            |
| 💎  | Crash-dump module stream   | ✅ MINIDUMP module list        | ✅ ELF core NT_FILE        | 🔄 §8 KIMAGE_ENTRY module stream     |
| 💎  | Recently-unloaded history  | ✅ MmUnloadedDrivers           | ❌ none                    | 🔄 §8 bounded tombstone ring         |
| ⭐  | Image provenance + CI      | ⚠️ CI.dll separate             | ⚠️ IMA/module sig separate | 🔄 §9 unified provenance record      |
| ⭐  | Hotpatch metadata slot     | ✅ hotpatch pointers           | ⚠️ livepatch separate      | ⬜ §9 reserved fields                |

> **After §1-§6:** a single loader-independent image registry (PE/ELF/EIF/kmod) with fast address/symbol/unwind lookup that every subsystem shares.
> **After §7-§9:** load notifications, crash-dump/KD module streams, and unified provenance/CI/hotpatch metadata -- one source of truth Windows and Linux split across several subsystems.

---

## Unit Tests

Create `src/kernel/test/test_kimage.c`, register via `test_register_kimage()` in `test_runner.c` under `TEST_CAT_EXEC` (the registry is populated by the exec/loader path). Concrete assertions:

- `test_kimage_entry_size`: `sizeof(KIMAGE_ENTRY)` == static-assert value; type/format enums distinct.
- `test_kimage_register_iterate`: register 3 images, iterate returns 3; unregister 1, iterate returns 2.
- `test_kimage_addr_lookup`: `image_lookup_by_address(base)` and `(end-1)` return the entry; `(end)`/`(base-1)` return NULL.
- `test_kimage_overlap_reject`: inserting an overlapping `[base,end)` returns an error (no silent replace).
- `test_kimage_symbol_lookup`: `ksym_lookup_addr(sym+4)` returns name + displacement 4; stripped image returns `<no symbols>`.
- `test_kimage_unwind_lookup`: unwind range lookup inside a registered image returns its entry; out-of-section range rejected.
- `test_kimage_per_process_iterate`: per-process registry count matches inserted DLL count.

> **Note:** these tests use in-memory `KIMAGE_ENTRY` fixtures and pure registry helpers; they MUST NOT call live loaders (`exec_load`, `pe_load`, `eif_load`) or boot infrastructure (per the test-side-effect policy).

---

## Verification

- [ ] `bash scripts/build.sh` shows `=== BUILD OK ===`.
- [ ] `bash scripts/test.sh SUITE=exec` passes with the new `test_kimage` assertions.
- [ ] Boot verifier: serial log confirms kernel image + symtab + each loaded driver appears exactly once in the registry, and the PEB Ldr count matches the per-process registry.
- [ ] `bash scripts/test-smoke.sh` shows `SMOKE TEST PASSED` (loader-path changes).

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | N suites, 0 failures
