---
schema_version: 1
id: eif-full-implementation
domain: 02-kernel-core
status: active
title: "TODO-20 -- EIF Full Implementation"
---

# TODO-20 -- EIF Full Implementation

> **Validated:** 2026-07-10 | validate-todo-file clean (structure / IO table / XREF / test wiring); fixed the metadata-vs-signature ordering claim + 11 stale XREF section numbers

> **Gap-audited:** 2026-07-10 | gap-audit + codex-gap-audit; 4H filed -- W^X page-sharing (§2), ASLR entropy/private-frames/PIC-soundness (§8), CET compat flag (§10 new), EIF signature-block ABI (§11 new, resolves the D02 T19 §4 signing-ownership cycle)

> [!IMPORTANT]
> **Decision pinned (2026-04-24):** EIF is the **native binary format** for Impossible OS user-mode and module code. EIF is **NOT** a replacement for the **kernel image format** -- the kernel itself stays **ELF indefinitely** (see `CLAUDE.md` → Toolchain → Kernel Binary Format). The kernel binary format is strictly internal to the UEFI bootloader handoff; nothing on the Win32 ABI surface or the EIF loader surface depends on it. Do not propose re-targeting the kernel to EIF or PE32+ under "unified format" reasoning -- the two layers are independently optimal. Distribution converters (`eif2pe`, `eif2elf`) exist for cross-OS user-app portability and are tracked in TODO-17 §13 as follow-ups.

> **Goal:** Complete the Executable Impossible Format (EIF) from its current basic loader to a production-quality native binary format. The basic loader (TODO-17 §5) validates headers, copies segments, and builds import dispatch tables. This TODO fills the gaps. Shipped + reviewed: range overlap validation, API version gating, metadata parsing, LZ4 decompression, CET flag reservation. Deferred with concrete owners: segment permission enforcement, ASLR, loader-owned module registration, optional import stubs, per-process isolation, signature-block ABI. When done, EIF is the fastest, most secure native binary format on any OS.

> [!IMPORTANT]
> **Current state:** `eif_load()` in `src/kernel/eif.c` validates the canonical section order + range overlap (§1), gates `api_version` (§4), parses the metadata section (§5), transparently LZ4-decompresses compressed segments (§6), and reserves the CET flag bits with fail-closed unknown-flag rejection (§10). The dispatch table at 0x8F0000 is still identity-mapped and SHARED across processes. Still missing (deferred, owner-tracked): segment R/W/X enforcement (§2), loader-owned module registration (§3), optional user-callable import stubs (§7), ASLR (§8), per-process dispatch isolation (§9), signature-block ABI (§11).
> EIF spec lives at `specs/eif-format.md`. Code signing is tracked separately in TODO-17 §17. The `elf2eif` converter is tracked in TODO-17 §13.

> [!NOTE]
> **Observability:** Checklists still cite `POST16(0xDE2x)` markers; the EIF path runs post-boot with `klog` available. Prefer matching `klog(..., "eif", ...)` strings for pass/fail when implementing, and treat POST16 lines here as legacy scaffolding unless a section is reclassified as true boot-phase work.

## Inputs

- [`src/kernel/eif.c`](../../src/kernel/eif.c) -- current EIF loader (basic segments + imports)
- [`include/kernel/eif.h`](../../include/kernel/eif.h) -- EIF structures and constants
- [`specs/eif-format.md`](../../specs/eif-format.md) -- normative EIF specification
- [`src/kernel/exec.c`](../../src/kernel/exec.c) -- exec dispatcher, module registration API
- [`include/kernel/exec.h`](../../include/kernel/exec.h) -- `loaded_module_t`, `exec_register_module()`
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) -- `vmm_map_page()`, `vmm_protect_range()`
- [`src/kernel/mm/pmm.c`](../../src/kernel/mm/pmm.c) -- `pmm_alloc_frame()`
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- current EIF-specific module registration in `task_exec()` to remove in section 3
- -> XREF: `TODO-17-binary-system.md §5` -- EIF kernel loader (FOUNDATION, complete)
- -> XREF: `TODO-17-binary-system.md §13` -- `elf2eif` converter and producer-side emission for metadata, `api_version`, PIC `load_base=0`, compressed segments, and the chosen optional-import call ABI (COMPLEMENT, not started)
- -> XREF: `TODO-17-binary-system.md §17` -- EIF code signing (COMPLEMENT, not started)
- -> XREF: `TODO-17-binary-system.md §15` -- ASLR for all formats (COMPLEMENT, not started)
- -> XREF: `TODO-17-binary-system.md §6` -- module list registration (FOUNDATION, partial)
- -> XREF: `TODO-03-kernel-libraries.md §3` -- LZ4 block decompressor (FOUNDATION for §6 compressed segments; consumed via `lz4_decompress`, §6 now [x])
- -> XREF: `TODO-03-kernel-libraries.md §5` -- kernel CSPRNG for `load_base=0` ASLR in §8 (FOUNDATION, not started)
- -> XREF: `TODO-12-native-api-ssdt.md §4` -- SSDT service table (FOUNDATION, partial)
- -> XREF: `01-boot-platform/TODO-10-bare-metal-hardening.md §8` -- per-process PML4 base already implemented; §9 consumes it as a foundation
- -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3` -- unique per-process user frames are still required before §9 can be complete

## Outcome

- EIF segments mapped with correct R/W/X permissions via PTE flags (NX on data, RO on rodata).
- EIF binaries with `load_base=0` get ASLR-randomized load addresses.
- `api_version` field checked against running OS API version; rejects too-new binaries.
- Metadata section parsed (§5): `"name"` reaches `loaded_module_t` for the crash registry on first load; `"version"`/`"author"`/`"min_os"`/`"build_id"` are logged at load time (module-record STORAGE of build_id deferred -> `TODO-17-binary-system.md §6`).
- LZ4-compressed segments (`EIF_FLAG_COMPRESSED`) transparently decompressed at load time.
- File range overlap validation per spec normative rule 2.
- Every loaded EIF registered via `exec_register_module()` for crash dumps and debugger.
- Optional imports resolved to deterministic `STATUS_NOT_IMPLEMENTED` stubs.
- Per-process dispatch table properly isolated when per-process page tables exist.
- Loader-visible hardening story aligned with PE/ELF practice: W^X segments, read-only import/dispatch surfaces where Linux would use RELRO, documented PIC/ASLR behavior versus PE `.reloc` / ELF PIE.

## Implementation Order

| ⭐  | Order | Deliverable                                 | Depends On                 | Status |
| --- | :---: | ------------------------------------------- | -------------------------- | :----: |
| ⭐  |   1   | Range overlap validation (normative rule 2) | --                         |  [x]   |
| 💎  |   2   | Segment permission enforcement (R/W/X PTE)  | §1                         |  [/]   |
| ⭐  |   3   | Module registration for EIF                 | T17 §6                     |  [/]   |
| ⭐  |   4   | API version gating                          | §1                         |  [x]   |
| ⭐  |   5   | Metadata section parser                     | §1                         |  [x]   |
| ⭐  |   6   | LZ4 compressed segments                     | T03 §3                     |  [x]   |
| ⭐  |   7   | Optional import stubs                       | §3, T17 §4, T17 §13        |  [/]   |
| ⭐  |   8   | EIF ASLR (load_base=0 randomization)        | §2, T17 §15, T03 §5        |  [/]   |
| ⭐  |   9   | Per-process dispatch table isolation        | §3, D01 T10 §8, D03 T01 §3 |  [/]   |
| 💎  |  10   | CET compatibility flag (format reservation) | --                         |  [/]   |
| 💎  |  11   | EIF signature-block ABI (signer key)        | D02 T19 §4                 |  [/]   |

> 💎 = parity -- matches a capability Windows PE and Linux ELF both have.
> ⭐ = exclusive -- Impossible OS native format superiority.

---

## 1. Range Overlap Validation (Normative Rule 2)

The spec mandates `segment_offset < import_offset < metadata_offset < signature_offset` (metadata ALWAYS precedes the signature so the signature covers it, per `specs/eif-format.md` Normative Rules 2-3) and that all ranges must be non-overlapping. Shipped: `eif_validate()` enforces the canonical order + non-overlap, and `eif_load` enforces per-segment data non-overlap.

- [x] `eif_validate()` enforces canonical ordering via an ascending cursor: segment table -> import table -> metadata -> signature, each starting at/after the previous end, skipping absent (0) regions (`src/kernel/eif.c`).
- [x] Per-segment file-data non-overlap: an O(n) monotonic walk in `eif_load` rejects a segment whose `[file_offset, file_size)` precedes the previous end (spec rule 7).
- [x] Segment data confined to the canonical region `[tables_end, data_limit)` (after both tables, before metadata/signature) -- data cannot overlap the tables/metadata/signature.
- [x] Rejection returns 0 -> `exec_load_fmt` maps to `ENOEXEC`; each reject emits a descriptive `klog(..., "eif", ...)` (e.g. "Segment table out of canonical order").
- [x] Commit: `"kernel: eif -- normative range overlap validation"` (already shipped)

**Test checkpoint:** An EIF whose segment file ranges overlap is rejected (`"Segment %u file range overlaps previous segment"`); an EIF with `import_offset` before the segment-table end is rejected (`"Import table out of canonical order"`); a valid ordered EIF still loads. Covered by `test_eif_reject_out_of_order_sections` + `test_eif_reject_segment_data_over_table` in `test_exec.c` (TEST_CAT_EXEC). EIF is post-boot (no POST16). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | EIF range-overlap suites, 0 failures
>
> **Notes:**
> - Shipped (prior EIF hardening, this reconciliation only flips the checkboxes): `eif_validate()` canonical-order cursor (`src/kernel/eif.c`) + `eif_load` per-segment vaddr/file non-overlap walk + segment-data-in-canonical-region check.
> - All range math is u64-widened + overflow-safe (check-before-subtract); absent (0) regions are skipped; rejects via `return 0` -> `ENOEXEC` with `klog(..., "eif", ...)`.
> - Tests: `test_eif_reject_out_of_order_sections` + `test_eif_reject_segment_data_over_table` (test_exec.c, TEST_CAT_EXEC).
> - Canonical doc: `specs/eif-format.md` Normative Rules 2, 6, 7.
> - Scope: §1 owns file-range ordering/overlap; the finer segment-data-vs-metadata overlap tie-in rides with signing (§11 + TODO-17 §17); VA-range/dispatch-table guards are in `eif_load` (§9 owns per-process isolation).
>
> **Verified:** 2026-07-10 | commit `e360c6c3` | 4/4 items | build OK | tests 712/712 (exec; 4 EIF range suites)
> **Accepted:** [L] concurrent-exec race on the shared dispatch table + identity-mapped user range (documented in `eif.c`) -> owned by per-process isolation. -> XREF: 02-kernel-core/TODO-20 §9 (item: "Map at EIF_DISPATCH_TABLE_ADDR in the per-process PML4" at line 269)
> **Quality reviewed:** 2026-07-10 | Codex 3x (adversarial, consistency, perf) + kernel-quality-auditor | 1M+1L fixed, 2L accepted-XREF | scope: kernel-code-quality

---

## 2. Segment Permission Enforcement (R/W/X via PTE)

The loader copies segment data but does not set page permissions. Segments should have NX on data and RO on rodata/text. The PTE-enforcement core is architecturally BLOCKED (see the callout + Deferred stamp); the W+X-reject + page-align VALIDATION half is self-contained and implementable now.

> [!WARNING]
> **Architectural blocker (found in the §2 exploration, corrects the old note):** `vmm_protect_range` does NOT split 2 MiB huge pages (returns -1); use `vmm_set_ro`/`vmm_set_nx` (RMW + auto-split). BUT those + `vmm_split_huge_page` are all hardcoded to `kernel_pml4` (T17 §5 "per-process PML4 support deferred"), and `vmm_create_user_pml4` rebuilds the user PT as Present|Writable (no NX). So perms set from `eif_load` are INVISIBLE to the per-process PML4 in CR3. The ELF loader has the identical dead-enforcement gap (`elf.c` `nx_stack`/`relro` are unused). Needs a new per-process-PML4 protect API first.

- [/] Per-segment PTE permission enforcement (NX/RO/exec-not-writable). BLOCKED: `vmm_set_ro`/`vmm_set_nx` are kernel_pml4-only (T17 §5), so perms set here are invisible to the per-process PML4 the task runs under.
- [ ] Prereq: add a per-process-PML4 protect API to vmm.c (`vmm_set_ro_pml4(pml4_phys,virt,size)` / `vmm_set_nx_pml4` + pml4-aware huge-page split); shared with the ELF loader. -> XREF: T17 §5.
- [ ] Prereq: thread per-segment R/W/X metadata from `eif_load` to `task_exec` (an `eif_exec_wrapper`; `eif_load` returns only the entry) + apply perms on the per-process cr3 after User bits. -> XREF: §9.
- [ ] Prereq: SMP TLB shootdown for the RMW protect path (`vmm_pte_rmw` is single-CPU-only; `task_exec` runs post-SMP). -> XREF: D03 T07 §2.
- [ ] Reject W+X segments (`EIF_SEG_WRITE | EIF_SEG_EXEC`) in `eif_validate` -- self-contained policy check, no PTE writes.
- [ ] Require page-aligned `seg->vaddr` in `eif_validate` so no two differently-flagged segments share a 4K page (the W^X page-granularity precondition; gap-audit). Self-contained.
- [ ] Commit: `"kernel: eif -- segment permission enforcement via PTE flags"`

**Test checkpoint:** (self-contained half, implementable now) a W+X segment (`EIF_SEG_WRITE|EIF_SEG_EXEC`) is rejected; a non-page-aligned `seg->vaddr` is rejected. (PTE-enforcement half, deferred) `.text` executable-not-writable + `.data` writable-not-executable once the per-process-PML4 protect API lands. EIF is post-boot (no POST16). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
>
> **Deferred:** [H] Segment PTE permission enforcement is architecturally blocked: `vmm_set_ro`/`vmm_set_nx` are kernel_pml4-only (T17 §5 "per-process PML4 support deferred"), so perms set from the loader are invisible to the per-process PML4 in CR3. Needs a new per-process-PML4 protect API + segment-metadata threading + SMP shootdown -- shared with the ELF loader, coupled to §9. The W+X-reject + page-align validation half is self-contained + still open. -> XREF: 02-kernel-core/TODO-17 §5 (item: "Implement `vmm_protect(virt, new_flags)` + `vmm_protect_range(addr, size, flags)`" -- extend with a per-process-PML4 variant); 02-kernel-core/TODO-20 §9 (item: "Map at EIF_DISPATCH_TABLE_ADDR in the per-process PML4" at line 222)

---

## 3. Module Registration for EIF

Every loaded binary must be registered in the crash registry and module list. EIF modules ARE registered today via the generic `task_exec` path; the metadata-name refinement shipped in §5 (`task.c` prefers the parsed name on first load). The remaining refinement -- loader-owned registration with the accurate load range + replace-on-re-exec -- is this §3's own open refactor.

- [/] Build `loaded_module_t`: DONE generically in `task_exec` with `format=EXEC_FMT_EIF` + task name -- but with the whole USER_ELF range, not the accurate load range, and not the metadata `"name"`.
- [ ] Register from `eif_load()` with the ACTUAL load range (lowest..highest segment vaddr), replacing the generic `task_exec` path -- a refactor (`eif_load` returns only the entry, so needs an ABI change or header re-parse).
- [ ] Replace-on-re-exec: `task_exec` skips re-registration when a module already covers the entry point, so a re-exec keeps the stale name/range (all formats). Add same-base replace keyed by image ownership.
- [x] Name from the metadata `"name"` key -- shipped in §5 (`task.c` prefers `eif_parse_metadata` name on first load). -> XREF: §5.
- [ ] Commit: `"kernel: eif -- module registration via exec_register_module"`

**Test checkpoint:** After loading an EIF, `exec_find_module_by_pc(entry_va)` returns the EIF module with `EXEC_FMT_EIF` (already true via the generic path); once the loader-owned refactor lands, `base_address` is the actual load base + `size_of_image` the segment span. EIF is post-boot (no POST16). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
>
> **Deferred:** [M] Loader-owned EIF module registration (accurate load range + replace-on-re-exec) is a refactor owned by this §3. The crash-registry need is ALREADY met: the generic `task_exec` registration sets format=EXEC_FMT_EIF and (via §5) the metadata `"name"` on first load. Open work: register from `eif_load` with the true load range instead of the whole USER_ELF span, and same-base replace so a re-exec updates the stale name/range (all formats). -> XREF: 02-kernel-core/TODO-20 §3 (items: "Register from `eif_load()` with the ACTUAL load range" + "Replace-on-re-exec").

---

## 4. API Version Gating

The `api_version` field is the minimum OS API version a binary requires. `eif_validate()` now rejects binaries requiring a newer API than the loader provides.

- [x] `EIF_CURRENT_API_VERSION 1` in `include/kernel/eif.h`.
- [x] `eif_validate()` rejects `hdr->api_version > EIF_CURRENT_API_VERSION` with a descriptive `klog` ("binary requires API version %u, OS provides %u"); v0 (unset) and <= current stay compatible.
- [x] Accept path is silent inside `eif_load`'s timed region (a klog there would inflate the `<10 us` load budget); the aggregate `eif_load` success record reports the load.
- [x] Commit: `"kernel: eif -- API version gating"`

**Test checkpoint:** an EIF with `api_version = EIF_CURRENT_API_VERSION + 1` is rejected; `api_version` 0 or 1 passes the gate. Covered by `test_eif_reject_api_version_too_new` (test_exec.c, TEST_CAT_EXEC). EIF is post-boot (no POST16). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | EIF api-version suite, 0 failures
>
> **Notes:**
> - Shipped: `EIF_CURRENT_API_VERSION` (`include/kernel/eif.h`) + an api-version gate in `eif_validate()` (`src/kernel/eif.c`) rejecting too-new binaries; 1 new `TEST_CAT_EXEC` test.
> - Runs at EIF load-validation time (post-boot, klog); v0 and <= current accepted, `> current` rejected fail-closed. Design-review approved the plain `>` gate (no v0 special-case).
> - Closes the TODO-20 §1 review Accepted-XREF (api_version was unenforced -> now owned + done here).
> - Canonical doc: `specs/eif-format.md` Normative Rule 4.
> - Scope: §4 gates the header `api_version` only; per-import capability/version negotiation is not an EIF concern.
> - Review: Codex 3x all clean; 1 LOW fixed (removed accept-path klog inside the timed load region); re-adversarial N/A (log-only fix, no logic change).
>
> **Verified:** 2026-07-10 | commit `63640f25` | 4/4 items | build OK | tests 713+16 PASS
> **Quality reviewed:** 2026-07-10 | Codex 3x (adversarial, consistency, perf) | 0H+0M+1L fixed | scope: kernel-code-quality

---

## 5. Metadata Section Parser

The spec defines a key-value metadata section (name, version, author, icon, min_os). Currently the `metadata_offset` field is parsed but the section is never read.

- [x] `eif_metadata_t` in `eif.h`: `name[64]`/`version[32]`/`author[64]`/`min_os[16]` + `build_id[32]`/`build_id_len` (raw-byte id). In-memory decode target, not an on-disk struct.
- [x] `eif_parse_metadata(data, size, hdr, out)` in `eif.c`: pure (no klog), walks `key_len+key+val_len+val` records, maps known keys, skips unknown (forward-compat), terminates on `key_len==0` or range end.
- [x] Remaining-length bounds check before every length read (`remaining>=4`, then `declared<=remaining`) -- no cursor-addition overflow; 1-3 trailing bytes are malformed.
- [x] `EIF_MAX_METADATA_RECORDS` (64) cap: reject a record flood before mutation (parser-DoS guard; mirrors the segment/import count caps).
- [x] Parse in `eif_load` validation phase: a malformed record rejects before the segment copy touches user memory; `name/version/author/min_os` logged after `end_ns` (klog-timing discipline).
- [x] `build_id` key logged (hex prefix) after `end_ns`; module-identity STORAGE deferred -> XREF: `TODO-17-binary-system.md §6` (item: "Extend module identity beyond `name`" at line 243).
- [x] `loaded_module_t.name` uses the EIF metadata name on first load (`task.c`), falling back to the task name -- satisfies the §3 "name from metadata" item.
- [x] Commit: `"kernel: eif -- metadata section parser"`

**Test checkpoint:** `eif_parse_metadata` populates name/version/author from a well-formed section; a value length past the range end is rejected; `metadata_offset=0` yields an empty struct and success; >`EIF_MAX_METADATA_RECORDS` records is rejected; a `build_id` record captures the raw bytes + length. 5 `TEST_CAT_EXEC` tests (parser is pure -- no `eif_load` mutation, no user-memory writes). EIF is post-boot (no POST16). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | EIF metadata suite, 0 failures
>
> **Notes:**
> - Shipped: `eif_parse_metadata()` + `eif_metadata_t` (`eif.c`/`eif.h`), a bounded key-value decoder for the EIF metadata section (name/version/author/min_os/build_id); 5 new `TEST_CAT_EXEC` tests.
> - Runs in `eif_load`'s validation phase (rejects malformed metadata before the mutation phase); success log deferred past `end_ns`; `task.c` uses the metadata name for `loaded_module_t.name` on first load.
> - Hardening from design review: `EIF_MAX_METADATA_RECORDS` cap (record-flood DoS), remaining-length checks (no cursor overflow), build_id given an explicit output path.
> - Canonical doc: `specs/eif-format.md` "Metadata Section".
> - Scope: parser + logs + first-load naming. build_id module-identity STORAGE -> `TODO-17-binary-system.md §6`; loader-owned registration + replace-on-re-exec renaming -> this TODO §3.
>
> **Verified:** 2026-07-10 | commit `ff583f8f` | 7/7 items | build OK | tests 726+16 PASS
> **Accepted:** [H] build_id/version/author/min_os decoded but never reach `loaded_module_t` (crash/debug id unreachable post-load) -> XREF: 02-kernel-core/TODO-17 §6 (item: "Extend module identity beyond `name`" at line 243)
> **Quality reviewed:** 2026-07-10 | Codex 6x (design, adversarial, re-adversarial, consistency, perf) | 4M fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 6. LZ4 Compressed Segments

The spec reserves `EIF_FLAG_COMPRESSED` for LZ4-compressed segment data. This enables smaller binaries on disk with transparent decompression at load time.

- [x] Uses the in-kernel `lz4_decompress` (`src/kernel/lz4.c`, `LZ4_decompress_safe`); the old unconditional compressed-reject in `eif_load` is removed.
- [x] `EIF_FLAG_COMPRESSED` per-file flag: `file_size`=compressed length, `mem_size`=exact decompressed extent (no BSS). Permits `file_size > mem_size`; `file_size==0`=pure-BSS; compressed + `mem_size==0` rejected.
- [x] `eif_decompress_segment()` + one `pmm_alloc_contiguous` scratch PREFLIGHTS every compressed stream in validation (verify == `mem_size`), so a corrupt stream rejects before any user write; scratch freed on all paths.
- [x] Mutation phase re-decodes each compressed segment directly into the user range (deterministic, guaranteed after preflight); `specs/eif-format.md` normative rule 9 documents the ABI.
- [x] Commit: `"kernel: eif -- LZ4 compressed segment loading"`

**Test checkpoint:** `eif_decompress_segment` round-trips a valid LZ4 stream to exact size+bytes; a corrupt stream, a size-mismatched stream, and `dst_cap < expect` are each rejected; `src_size==0` is pure-BSS (ok iff `expect==0`). 5 `TEST_CAT_EXEC` tests (pure -- no `eif_load` mutation). A full compressed image loading+executing is serial-log validated on WHPX (needs the `0x800000` write the unit harness avoids). EIF is post-boot (no POST16). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | EIF decompression suite, 0 failures
>
> **Notes:**
> - Shipped: `eif_decompress_segment()` + compressed-segment preflight/decode in `eif_load` (`eif.c`), flag-conditional segment ABI, LZ4 via `lz4_decompress`; 5 new `TEST_CAT_EXEC` tests.
> - Preflight (one reusable `pmm_alloc_contiguous` scratch) decompresses+verifies every compressed segment in validation, keeping the mutation phase infallible; mutation re-decodes deterministically into the user range.
> - Design review (2 HIGH adopted): fallible-decompress-in-mutation broke atomicity -> preflight; the `file_size<=mem_size` BSS rule rejected valid compressed segments -> flag-conditional ABI.
> - Canonical doc: `specs/eif-format.md` normative rule 9 (compressed segments).
> - Scope: per-file LZ4 segment compression. Streaming/partial decode not needed (image fits the user range); signed+compressed ordering is a signature concern owned by §11.
>
> **Verified:** 2026-07-10 | commit `d0e2d587` | 5/5 items | build OK | tests 735+16 PASS
> **Accepted:** [M] no producer emits `EIF_FLAG_COMPRESSED` yet (loader-shipped-before-producer) -> XREF: 02-kernel-core/TODO-17 §13 (item: "CLI `--api-version`/`--pic`/metadata (lz4 `EIF_FLAG_COMPRESSED` ... deferred)" at line 390)
> **Quality reviewed:** 2026-07-10 | Codex 6x (design, adversarial, re-adversarial, consistency, perf) | 2H+1L fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 7. Optional Import Stubs

Spec normative rule 5: skipped optional imports MUST have their dispatch table entry set to a deterministic stub returning `STATUS_NOT_IMPLEMENTED`.

- [ ] ABI prerequisite: reconcile `specs/eif-format.md` rule 5, `include/kernel/eif.h` `eif_dispatch_entry_t`, and `TODO-17-binary-system.md` §13 before implementation; the current contract is data-only (`{syscall_id, available}`), not callable stub slots
- [ ] If the ABI stays data-only: have `elf2eif` emit deterministic user thunks for optional imports that translate `available=0` into `STATUS_NOT_IMPLEMENTED` (-> XREF: `TODO-17-binary-system.md §13`)
- [ ] If the ABI changes to callable stub slots: update the EIF spec and `eif_dispatch_entry_t` layout first (-> XREF: `TODO-17-binary-system.md §4` -- EIF format spec, the ABI source of truth)
- [ ] In the dispatch table write pass: for optional imports where `available=0`, set the dispatch entry to point to a kernel-provided stub function that returns `STATUS_NOT_IMPLEMENTED` (0xC0000002)
- [ ] The stub address should be a well-known kernel page mapped read-only into user space
- [ ] User code calling an unavailable optional import gets a clean `STATUS_NOT_IMPLEMENTED` return instead of a crash
- [ ] Commit: `"kernel: eif -- optional import stub generation"`

**Test checkpoint:** An EIF importing an unregistered optional SSDT entry gets `STATUS_NOT_IMPLEMENTED` (0xC0000002) return value, not a crash. Serial log shows `"eif: optional import 0x%x stubbed"`. EIF is post-boot (no POST16). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Deferred:** [M] Optional-import STUB slots (spec rule 5) blocked: the EIF dispatch table is data-only (`{syscall_id, available}`; user code checks availability before SYSCALL), and a callable stub needs the unsettled EIF import-CALL ABI plus a per-process read-only user stub page -> XREF: 02-kernel-core/TODO-17 §13 (item: "Generate import table -> the required-import CALL ABI" at line 388); per-process user mapping -> this TODO §9.

---

## 8. EIF ASLR (load_base=0 Randomization)

When `load_base=0`, the EIF binary is position-independent and should be loaded at a randomized address for security. This integrates with the broader ASLR work in TODO-17 §15.

- [ ] Depends on: kernel CSPRNG in `TODO-03-kernel-libraries.md` §5 (until [x] there, this section stays blocked)
- [ ] If `load_base == 0`: generate a random base address using the kernel CSPRNG (-> XREF: `TODO-03-kernel-libraries.md` §5 Monocypher/RDRAND)
- [ ] Random base must be page-aligned, within the user address range, and not overlapping existing mappings
- [ ] Update all segment vaddr calculations to use the randomized base
- [ ] Keep the dispatch table VA fixed at `EIF_DISPATCH_TABLE_ADDR`; only the image base moves here, while §9 changes the physical backing and protection model later
- [ ] Real entropy requires process-PRIVATE image frames: randomizing over the shared identity map gives ~0 entropy. Block meaningful ASLR on §9 per-process frames. -> XREF: §9, D03 T01 §3.
- [ ] Require a measured entropy floor (base varies over >= N address bits across many loads), not just "different each time".
- [ ] Soundness assumes `elf2eif` emits true -fpic with no absolute data/fn pointers; absolute-pointer binaries need the v1.1 relocation table -- test PIC binaries with internal vtables across many bases.
- [ ] Log the randomized base: `"eif: ASLR base=0x%x"`
- [ ] Commit: `"kernel: eif -- ASLR for position-independent binaries"`

**Test checkpoint:** Over many loads of the same PIC EIF binary (`load_base=0`), the base varies over a measured entropy floor (not just "different twice"); a PIC binary carrying internal vtables/function-pointers executes correctly across many randomized bases. Non-PIC EIF (`load_base!=0`) loads at its preferred address. Serial: `"eif: ASLR base=0x%x"`. EIF is post-boot (no POST16). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Deferred:** [M] EIF ASLR blocked: randomizing `load_base` over the shared identity-mapped user range gives ~0 real entropy -- meaningful ASLR needs process-PRIVATE image frames (§9) plus the kernel CSPRNG. Base-randomization mechanics also require `elf2eif` true-PIC emission -> XREF: this TODO §9 (per-process frames); `03-memory-concurrency/TODO-01 §3` (per-process physical isolation); `TODO-03-kernel-libraries.md §5` (CSPRNG).

---

## 9. Per-Process Dispatch Table Isolation

Currently the EIF dispatch table at 0x8F0000 is identity-mapped and shared. When per-process page tables exist, each process should have its own dispatch table backed by unique physical pages.

- [ ] Foundation check: consume `01-boot-platform/TODO-10-bare-metal-hardening.md §8` per-process PML4 base instead of the shared kernel PML4
- [ ] Blocker: wait for `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3` per-process physical isolation so the dispatch table pages are backed by unique frames per process
- [ ] Allocate physical frames for the dispatch table pages via `pmm_alloc_frame()`
- [ ] Map at EIF_DISPATCH_TABLE_ADDR in the per-process PML4 (not kernel PML4)
- [ ] Write dispatch entries into the per-process pages
- [ ] Set pages as User + Read-Only (dispatch table should not be writable by user code)
- [ ] Commit: `"kernel: eif -- per-process dispatch table isolation"`

**Test checkpoint:** With per-process page tables enabled, two EIF processes with different import tables each observe isolated physical backing for the dispatch slot at `0x8F0000` (no cross-process bleed through the shared kernel map). A user-mode write attempt to the dispatch table pages faults. Serial log shows a clear per-process map message (exact substring documented in the implementation PR). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Deferred:** [M] Per-process dispatch-table isolation blocked on per-process page tables: the table at `0x8F0000` is identity-mapped in the shared kernel PML4 today, so unique per-process physical backing + User+RO protection need a per-process PML4 -> XREF: `01-boot-platform/TODO-10 §8` (per-process PML4 base); `03-memory-concurrency/TODO-01 §3` (per-process physical isolation).

---

## 10. CET Compatibility Flag (Format Reservation)

Both Win11 (`IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT`) and Linux (`.note.gnu.property` IBT/SHSTK) declare CET support per-binary. EIF reserves two independent bits (IBT + SHSTK, matching the Linux split). Reserving previously-unused flag bits is cheap and needs no `EIF_VERSION` bump. Enforcement waits for ring-3 CET; the consumer is now owned by `TODO-10-kernel-security-hardening.md §10` (per-process `U_CET`/`PL3_SSP`), which programs supervisor `S_CET` for ring 0 today.

- [x] Reserve two independent CET bits `EIF_FLAG_CET_IBT` (bit 6) + `EIF_FLAG_CET_SHSTK` (bit 7) in `eif.h`; documented in `specs/eif-format.md` rule 10 as reserved + unenforced until ring-3 CET.
- [x] `EIF_FLAG_KNOWN_MASK` (bits 0-7) + `eif_flags_known()`; `eif_validate()` fail-closed-rejects any header flag outside the mask (`ENOEXEC`). No `EIF_VERSION` bump (previously-unused bits).
- [/] Propagate CET intent into per-process metadata -- DEFERRED: no ring-3 CET consumer exists (speculative storage) -> XREF: `TODO-10-kernel-security-hardening.md §10` (item: "Ring-3/per-process CET: consume per-binary CET flags").
- [x] Filed the ring-3 CET ENFORCEMENT owner gap concretely -> XREF: `TODO-10-kernel-security-hardening.md §10` (consumes `EIF_FLAG_CET_IBT`/`_SHSTK` to program per-process `U_CET`/`PL3_SSP`).
- [x] Commit: `"kernel: eif -- reserve CET compatibility header flag"`

**Test checkpoint:** `eif_flags_known()` accepts the CET bits (and combinations) and rejects any bit outside `EIF_FLAG_KNOWN_MASK`; an otherwise-valid EIF carrying an unknown flag bit is rejected by `eif_load` before the segment copy (`ENOEXEC`). Enforcement is out of scope (ring-3 CET unowned). 3 `TEST_CAT_EXEC` tests. EIF is post-boot (no POST16). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | EIF flag-validation suite, 0 failures
>
> **Notes:**
> - Shipped: `EIF_FLAG_CET_IBT`/`_SHSTK` (bits 6/7) + `EIF_FLAG_KNOWN_MASK` + `eif_flags_known()` (`eif.h`/`eif.c`); `eif_validate` fail-closed-rejects unknown flag bits; `specs/eif-format.md` rule 10; 3 `TEST_CAT_EXEC` tests.
> - Format reservation only -- the CET bits are parsed/accepted but UNENFORCED; enforcement (per-process `U_CET`/`PL3_SSP`) is owned by `TODO-10-kernel-security-hardening.md §10`.
> - Design review (1 HIGH adopted): a single generic CET_COMPAT bit conflates IBT (forward-edge) and SHSTK (backward-edge) -> two independent bits matching the Linux `.note.gnu.property` split.
> - Canonical doc: `specs/eif-format.md` normative rule 10.
> - Scope: reserves + validates the flags. Propagation + ring-3 enforcement -> `TODO-10-kernel-security-hardening.md §10`.
>
> **Verified:** 2026-07-10 | commit `95864f42` | 3/4 items | build OK | tests 743+16 PASS
> **Accepted:** [L] no produce/enforce-time verifier confirms the ENDBR64 / shadow-stack-clean guarantee before a CET bit is trusted (unverified trust chain) -> XREF: 02-kernel-core/TODO-10 §10 (item: "Ring-3/per-process CET: consume per-binary CET flags")
> **Quality reviewed:** 2026-07-10 | Codex 6x (design, adversarial, re-adversarial, consistency, perf) | 1H+2M fixed, 1L accepted-XREF | scope: kernel-code-quality

---

## 11. EIF Signature-Block ABI (Signer Key Delivery)

The spec'd signature block (`algo`, `sig_size`, `signature` over `[0, signature_offset)`) carries no signer pubkey or key-id, and CI trust anchors store only a pubkey hash -- so a verifier has no pubkey to run Ed25519 against. TODO-19 §4 deferred this ABI decision here (TODO-20 owns the EIF format); the verify implementation is TODO-17 §17. This section owns the FORMAT amendment that unblocks both -- resolving the current signing-ownership cycle.

- [ ] Amend `specs/eif-format.md` signature block to carry signer `pubkey[32]` (+ key-id) alongside `algo`/`sig_size`/`signature`; map `algo` to `ci_sig_alg_t` (Ed25519; reject RSA = unsupported).
- [ ] Bump `EIF_VERSION` for the signature-block layout change; gate old vs new by version.
- [ ] Verifier contract: `ci_crypto_verify(ED25519, [0,sig_off), sig, pubkey)` AND the pubkey hash must match a trusted CI anchor tier. -> XREF: D02 T19 §3, D02 T19 §4.
- [ ] Commit: `"kernel: eif -- signature-block ABI (signer key delivery)"`

**Test checkpoint:** the amended signature block round-trips through `elf2eif` + the loader; a signed EIF with a pubkey whose hash matches a trusted anchor verifies, and one with an untrusted pubkey is rejected. (Verify impl lands in TODO-17 §17.) Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Deferred:** [M] Operator-reserved: this is a security-sensitive EIF signature-block ABI + `EIF_VERSION` bump with NO end-to-end validation path today -- the producer (`eifsign`/`elf2eif` signing) and the verify impl are both deferred, and CI trust anchors do not exist yet, so ratifying the on-disk signature format unilaterally in an unattended run is inappropriate. Needs operator sign-off -> XREF: `TODO-17-binary-system.md §17` (verify impl + `eifsign` producer); `TODO-19-code-integrity-trust-policy.md §4` (trusted CI anchor tiers, item: "Embedded signature validation").

---

## OS Comparison

| ⭐  | Feature                 | 🪟 Win11                 | 🐧 Linux                 | 🚀 Impossible OS                                        |
| --- | ----------------------- | ------------------------ | ------------------------ | ------------------------------------------------------- |
| 💎  | Segment RWX pages       | ✅ PE section chars      | ✅ ELF p_flags           | ⬜ §2                                                   |
| 💎  | ASLR                    | ✅ HighEntropyVA + reloc | ✅ PIE + mmap ASLR       | ⬜ §8 T17 §15                                           |
| 💎  | RELRO import table RO   | ⚠️ often partial RELRO   | ✅ full RELRO w/-z now   | ⬜ §2 §7 §9                                             |
| 💎  | Code signing            | ✅ Authenticode pipeline | ✅ IMA / module sig      | ⬜ §11 ABI + T17 §17                                    |
| 💎  | CET compat flag         | ✅ CETCOMPAT             | ✅ .note IBT/SHSTK       | ⚠️ §10 IBT+SHSTK reserved+validated; enforce D02T10 §10 |
| ⭐  | Fast load path          | ❌ slow IAT fixups       | ❌ slow PLT/GOT          | ✅ T17 §5                                               |
| ⭐  | Integer SSDT imports    | ❌ name-based imports    | ❌ dynamic string sym    | ✅ T17 §5                                               |
| ⭐  | API version gate        | ⚠️ subsystem version     | ❌ no ELF equivalent     | ✅ §4                                                   |
| ⭐  | Built-in metadata       | ⚠️ RT_VERSION resource   | ⚠️ .note / build-id      | ✅ §5                                                   |
| ⭐  | LZ4 compressed segments | ❌ not in PE load        | ❌ not standard ELF      | ✅ §6                                                   |
| ⭐  | Range overlap checks    | ⚠️ loader partial checks | ⚠️ partial loader checks | ✅ §1                                                   |
| ⭐  | Optional import stub    | ❌ delay-load thunks     | ❌ weak sym may be NULL  | ⬜ §7                                                   |

> **Parity gaps:** 💎/⬜ rows are owned by §1-§11 or `TODO-17-binary-system.md` §17. **Shipped+reviewed:** overlap (§1), API gate (§4), metadata (§5), LZ4 (§6), CET flags (§10). **Deferred with concrete owners:** W^X permissions (§2), loader-owned module registration (§3), optional-import stubs (§7), ASLR (§8), per-process dispatch (§9), signature-block ABI (§11).

## Unit Tests

> Wire into `test_runner_init()` via `test_register_eif()` -- register in `src/kernel/test/test_runner.c`.
> Tests run with `debug=1` or `test=1` in boot.conf. Use `TEST_CAT_EXEC` category.

- [ ] Create `src/kernel/test/test_eif.c` with:
  - Overlap validation: overlapping segment data ranges rejected
  - Overlap validation: valid non-overlapping EIF accepted
  - API version: `api_version=1` accepted, `api_version=99` rejected
  - Metadata parser: valid key-value pairs parsed correctly, `name` field populated, optional `build_id` logged or surfaced once module identity storage lands
  - Metadata parser: truncated metadata (key_len past EOF) rejected
  - Metadata parser: `metadata_offset=0` accepted (no metadata)
  - Struct sizes: `sizeof(eif_header_t)==64`, `sizeof(eif_segment_t)==32`, `sizeof(eif_import_t)==8`
  - Module registration: after `eif_load`, `exec_find_module_by_pc(entry)` finds module with `EXEC_FMT_EIF`
  - Optional imports: reserve a real assertion or `TEST_PENDING` case for the chosen §7 ABI path so unavailable optional imports return `STATUS_NOT_IMPLEMENTED` without crashing
  - Compressed segments: reserve a test for >4 KiB compressed input so the implementation does not use `kmalloc()` past repo limits
- [ ] Move existing EIF tests from `test_exec.c` into `test_eif.c` for consolidation
- [ ] Register in `test_runner_init()`: `test_register_eif()`
- [ ] Commit: `"test: add EIF full implementation test suite"`

**Test checkpoint:** `bash scripts/test.sh SUITE=exec` -- all `test_eif_*` cases PASS; `tail -1 build/build.log` is `=== BUILD OK ===`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] EIF binary loads with correct segment permissions (R-X text, RW- data)
- [ ] EIF with `api_version` > current is rejected cleanly
- [ ] EIF with metadata shows parsed name/version in serial log
- [ ] EIF module appears in crash registry via `exec_find_module_by_pc`
- [ ] Two PIC EIF loads (`load_base=0`) get different ASLR base addresses
- [ ] Optional import returns `STATUS_NOT_IMPLEMENTED`, not crash
- [ ] All POST16 codes appear in correct order on serial output (where still used); otherwise verify the same scenarios via documented `klog("eif", ...)` substrings from each section
- [ ] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal

**Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec)
