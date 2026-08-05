---
schema_version: 1
id: alternate-boot-protocols
domain: 01-boot-platform
status: done
title: "TODO-08 -- Alternate Boot Protocols & Compatibility Boundary"
---

# TODO-08 -- Alternate Boot Protocols & Compatibility Boundary

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Decide and enforce how non-primary boot protocols work. CLOSED with `unsupported` policy committed: UEFI/GPT/ESP is the only supported boot path; Multiboot2 / GRUB / Limine / legacy BIOS / Linux x86 boot protocol / EFI stub direct boot / kexec are explicit non-goals.

> [!IMPORTANT]
> **Closure state:** §7 deleted the Multiboot2 parser (`src/kernel/multiboot2_parse.c`), header (`include/kernel/multiboot2.h`), magic-check entry stub (`src/boot/entry.asm`), Multiboot2 header asm (`src/boot/multiboot2_header.asm`), and `src/boot/grub.cfg`. `g_boot_info` storage relocated to `src/kernel/main/boot_hw.c`. Linker script ENTRY changed to `kernel_main`. `BUILD_ALT_BOOT` Makefile var retained at default `off` for documentation continuity; no source code consumes the macro. §2-§6 (parity audit, adapter, degradation matrix, GRUB docs, compat test images) are `[~]` N/A under the chosen policy. Canonical policy doc: [`docs/boot/alt-boot.md`](../../docs/boot/alt-boot.md). Re-opening requires a new policy decision: restore deleted files from git history + add `BUILD_ALT_BOOT` gates.

## Inputs

- [`src/boot/multiboot2_header.asm`](../../src/boot/multiboot2_header.asm)
- [`include/kernel/multiboot2.h`](../../include/kernel/multiboot2.h)
- [`src/kernel/multiboot2_parse.c`](../../src/kernel/multiboot2_parse.c)
- [`src/boot/grub.cfg`](../../src/boot/grub.cfg)
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §8` -- canonical boot protocol documentation + schema changelog
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §11` -- capability negotiation flags for missing UEFI features
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §12` -- boot-path provenance + reason codes for alt-boot attribution
- -> XREF: `TODO-06-boot-media-image-installer-handoff.md §4` -- ISO may expose only UEFI unless this TODO expands support
- -> XREF: `TODO-28-boot-validation-certification-matrix.md` -- if §1 promotes Multiboot2 to compatible, the matrix must add a Multiboot2 row
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §16` -- UKI signed-payload chain (UEFI-only; alt-boot loses module signature coverage per §3 / §4)
- -> XREF: `TODO-21-ab-boot-rollback.md` -- A/B selection (UEFI-only; alt-boot defaults to slot A per §4)
- -> XREF: `TODO-22-recovery-partition.md` -- recovery trigger (alt-boot lacks BootNext; boot.conf-only path per §4)
- -> XREF: `TODO-23-boot-watchdog.md` -- UEFI watchdog (alt-boot replaces or disables per §4)
- -> XREF: `TODO-26-hibernation-resume-fast-startup-handoff.md` -- hibernation resume refused on alt-boot (no UEFI variable for state) per §4
- -> XREF: `TODO-07-boot-entry-store-menu-policy.md §15` -- LoaderXxx UEFI variables: alt-boot path sets `boot_info.loader_vars_degraded = 1` (already v21 ABI) per §4
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §14` -- warm-kernel-update / KHO is the owner of kexec-style kernel-to-kernel transitions; alt-boot does NOT own this

## Outcome

- Closure committed: alternate boot protocols are `unsupported`. UEFI/GPT/ESP is the only supported boot path.
- Build / docs / tests cannot mistake an alternate path for product boot: parser + entry stub + header asm + `grub.cfg` are deleted; `make iso` errors with "no rule to make target"; kernel image contains zero `multiboot2_*` symbols.
- Explicit non-goals are named in [`docs/boot/alt-boot.md`](../../docs/boot/alt-boot.md): Multiboot2, GRUB, Limine/stivale, Multiboot1, Linux x86 boot protocol, EFI stub direct boot, kexec, legacy BIOS. Adjacent contributors cannot accidentally ship them without a new policy decision.
- `BUILD_ALT_BOOT` Makefile var retained at default `off` so a hypothetical future flip is a documented knob; no source code consumes the macro post-deletion.

## Implementation Order

| ⭐  | Order | Deliverable                            | Depends On           | Status |
| --- | :---: | -------------------------------------- | -------------------- | :----: |
| 💎  |   1   | Alternate boot protocol policy         | T01 §8               |  [x]   |
| 💎  |   2   | Multiboot2 feature parity audit        | §1                   |  [x]   |
| 💎  |   3   | Multiboot2-to-boot_info adapter        | §2, T01 §11, T01 §12 |  [x]   |
| 💎  |   4   | Unsupported-feature degradation matrix | §2, §3               |  [x]   |
| 💎  |   5   | GRUB/Limine/legacy BIOS documentation  | §1                   |  [x]   |
| ⭐  |   6   | Compatibility test images              | §3                   |  [x]   |
| ⭐  |   7   | Deprecation or promotion gate          | §1                   |  [x]   |

> 💎 = parity -- Linux distros ship Multiboot2 + UEFI dual-boot; Win11 is UEFI-only since 24H2 (no Multiboot equivalent).
> ⭐ = exclusive -- §6 + §7 explicit policy gate (neither OS publishes a structured "supported / diagnostic / unsupported" taxonomy for alternate boot protocols).
> `[x]` (§2-§6) = retired-closed -- §1 chose `unsupported` and §7 deleted the Multiboot2 parser/stub; these sections would have implemented / documented the retired path, so they are closed N/A (per-section RETIRED callouts + Verified retirement stamps note 0 items, no code surface).

---

## 1. Alternate Boot Protocol Policy

- [x] Decided supported state for Multiboot2: **unsupported**. §7 deleted the parser + entry stub + header asm + grub.cfg; `BUILD_ALT_BOOT` default flipped to `off`.
- [x] Decide supported state for Limine / stivale: default `rejected` per `docs/boot/alt-boot.md`; re-evaluation gate in §7.
- [x] Explicit non-goal table in `docs/boot/alt-boot.md`: Multiboot1, Linux x86 boot protocol / bzImage / zboot, EFI stub direct boot, kexec, legacy BIOS, Limine.
- [x] Document UEFI-only features in `docs/boot/alt-boot.md`: runtime services, Secure Boot, TPM, BootOrder, GOP, USB handoff, ESP integrity, UKI sig chain, A/B, recovery, hibernation, watchdog.
- [x] `BUILD_ALT_BOOT` plumbing: Makefile var + `$(error)` validation + `-D` to CFLAGS/ASFLAGS; default flipped to `off` post-§7; no source consumes the macro now that the parser is deleted.
- [x] Updated `docs/getting-started/index.md` Boot protocol section: UEFI-only stance + non-goals + `BUILD_ALT_BOOT` pointer.
- [x] Commit: `"boot: alternate boot protocol policy"` (commit `8117d6c7`)

**Test checkpoint:** `BUILD_ALT_BOOT=off make` (default post-§7) builds OK; `BUILD_ALT_BOOT=bogus make` fails fast at Makefile parse with `$(error)`. `docs/boot/alt-boot.md` documents `unsupported` as the committed state. Multiboot2 symbol absence (`nm build/kernel.exe | grep multiboot2` returns empty) is structural, not gated -- the parser was deleted in §7.

> **Test runner:** N/A (policy + Makefile var + docs; no kernel-testable surface) | validation: `bash scripts/build.sh` (OK) + `BUILD_ALT_BOOT=bogus make` (fail-fast) + lint clean

> **Notes:**
> - What shipped: `BUILD_ALT_BOOT` Makefile var with `$(error)` validation + CFLAGS/ASFLAGS `-D` propagation; `docs/boot/alt-boot.md` canonical policy doc; `docs/getting-started/` Boot protocol section.
> - Structure / consumers: `docs/boot/alt-boot.md` is single-source-of-truth; cross-linked from getting-started + Makefile comment + TODO-08 §1.
> - Downstream effects: §1 plumbing + non-goals shipped; §7 executed `unsupported` (deleted parser/stub/header/grub.cfg, flipped Makefile default to `off`, relocated `g_boot_info` to `boot_hw.c`).
> - Canonical doc: [`docs/boot/alt-boot.md`](../../docs/boot/alt-boot.md).
> - Scope boundary: §1 owned policy doc + `BUILD_ALT_BOOT` plumbing + non-goal contract. §7 retired the parser via deletion (no separate gating layer to maintain post-deletion).

> **Verified:** 2026-05-19 | commit `8117d6c7` (impl) + `479832ed` (commit-row flip) | 5/7 items | build OK | lint clean
> **Quality reviewed:** 2026-05-19 | Codex 4x (design + adversarial 2x + consistency + perf) | 3H+2M fixed, 0 open | scope: N/A (policy + Makefile var + docs; no kernel-testable surface)

---

## 2. Multiboot2 Feature Parity Audit

> RETIRED -- §1 chose `unsupported`. The Multiboot2 parser was deleted in §7; there is no path to audit. Rows below are kept as a historical record of the work that would have shipped under `diagnostic`/`compatible`.

- [~] Compare every Multiboot2 information tag against `boot_info` fields -- N/A.
- [~] Identify unavailable fields and degraded-graceful handling -- N/A.
- [~] Audit framebuffer / memory map / ACPI / module / cmdline / boot-device tags -- N/A.
- [~] Audit Multiboot2 UEFI tags (tag 7/11/12/17) -- N/A.
- [~] Record parity table in `docs/boot/boot-protocol.md` -- N/A.
- [~] Commit -- N/A.

**Test checkpoint:** N/A (section retired under `unsupported` policy).

> **Deferred:** [L] Multiboot2 feature parity audit retired -- §1 policy chose `unsupported`; §7 deleted the parser (8117d6c7) -> XREF: 01-boot-platform/TODO-08 §7 (item: "Chose `unsupported`: deleted 5 files" at line 182)

---

## 3. Multiboot2-to-boot_info Adapter

> RETIRED -- §1 chose `unsupported`. No adapter ships; `boot_info` is populated only by the UEFI bootloader.

- [~] Populate boot_info from Multiboot2 tags -- N/A.
- [~] Map mmap / framebuffer / ACPI / modules / cmdline -- N/A.
- [~] Modules security gap (`allow_unsigned_modules`) -- N/A.
- [~] Set `caps_present` / `caps_degraded` for missing UEFI features -- N/A.
- [~] Set `boot_path = ALT_BOOT` / `boot_reason = ALT_BOOT_PROTOCOL` -- N/A.
- [~] Reuse `boot_info_validate_*()` -- N/A.
- [~] Commit -- N/A.

**Test checkpoint:** N/A (section retired under `unsupported` policy).

> **Deferred:** [L] Multiboot2 to-boot_info adapter retired -- §1 policy chose `unsupported`; §7 deleted the parser (8117d6c7) -> XREF: 01-boot-platform/TODO-08 §7 (item: "Chose `unsupported`: deleted 5 files" at line 182)

---

## 4. Unsupported-Feature Degradation Matrix

> RETIRED -- §1 chose `unsupported`. There is no alt-boot code path to degrade; UEFI features are either present or the boot halts.

- [~] Define behavior for missing UEFI features -- N/A.
- [~] LoaderXxx + `loader_vars_degraded` wiring -- N/A.
- [~] Higher-level losses (UKI / A/B / recovery / hibernation / watchdog) -- N/A.
- [~] Reciprocal XREFs to T02 §16 / T21 / T22 / T26 / T23 / T07 §15 -- N/A (sister TODOs do not need back-links to a deleted path).
- [~] Alt-boot crash recovery flow / `BOOT_ERR_ALT_BOOT_LOOP` -- N/A.
- [~] Readiness oracle degraded flags -- N/A.
- [~] Serial `[WARN] Alt-boot:` lines -- N/A.
- [~] `boot_info.alt_boot_degraded[]` + `NtQueryAltBootDegraded()` syscall -- N/A.
- [~] `compatible`-mode security-cost note (GRUB/shim CVE inheritance) -- documented in `docs/boot/alt-boot.md` as historical / why-we-chose-unsupported context.
- [~] `boot.conf:allow_alt_boot` halt gate -- N/A.
- [~] Commit -- N/A.

**Test checkpoint:** N/A (section retired under `unsupported` policy).

> **Deferred:** [L] Multiboot2 unsupported-feature degradation matrix retired -- §1 policy chose `unsupported`; §7 deleted the parser (8117d6c7) -> XREF: 01-boot-platform/TODO-08 §7 (item: "Chose `unsupported`: deleted 5 files" at line 182)

---

## 5. GRUB / Limine / Legacy BIOS Documentation

> RETIRED -- §1 chose `unsupported`. The non-goal documentation that this section would have authored shipped via §1 into [`docs/boot/alt-boot.md`](../../docs/boot/alt-boot.md). All entry points listed below are documented there as explicit non-goals.

- [~] GRUB Multiboot2 usage doc -- N/A (no `grub-mkrescue` artifact ships).
- [~] Limine / stivale policy doc -- captured in `docs/boot/alt-boot.md` non-goal table.
- [~] Multiboot1 explicit rejection -- captured in `docs/boot/alt-boot.md`.
- [~] Linux x86 boot protocol non-goal -- captured in `docs/boot/alt-boot.md`.
- [~] EFI stub direct boot non-goal -- captured in `docs/boot/alt-boot.md`.
- [~] kexec non-goal -- captured in `docs/boot/alt-boot.md` with XREF to [warm-kernel-update](TODO-01-boot-protocol-abi-handoff.md#14-warm-kernel-update-handoff-abi).
- [~] Legacy BIOS unsupported statement -- captured in `docs/boot/alt-boot.md` + OS Comparison row.
- [~] `docs/development/vm-boot-testing.md` alt-boot QEMU commands -- N/A.
- [~] Commit -- N/A.

**Test checkpoint:** N/A (section retired; non-goal content lives in `docs/boot/alt-boot.md`).

> **Deferred:** [L] Multiboot2 GRUB/Limine/legacy BIOS docs retired -- §1 policy chose `unsupported`; §7 deleted the parser (8117d6c7) -> XREF: 01-boot-platform/TODO-08 §7 (item: "Chose `unsupported`: deleted 5 files" at line 182)

---

## 6. Compatibility Test Images

> RETIRED -- §1 chose `unsupported`. No GRUB Multiboot2 ISO is built; `scripts/test-alt-boot.sh` was never created.

- [~] Build GRUB Multiboot2 test ISO -- N/A.
- [~] QEMU GRUB ISO smoke -- N/A.
- [~] §4 degradation matrix assertions -- N/A.
- [~] CI gate / `scripts/test-alt-boot.sh` -- N/A.
- [~] TODO-06 §4 reciprocal XREF -- N/A.
- [~] Commit -- N/A.

**Test checkpoint:** N/A (section retired; the canonical UEFI ISO from TODO-06 §4 remains the only ISO artifact).

> **Deferred:** [L] Multiboot2 compatibility test images retired -- §1 policy chose `unsupported`; §7 deleted the parser (8117d6c7) -> XREF: 01-boot-platform/TODO-08 §7 (item: "Chose `unsupported`: deleted 5 files" at line 182)

---

## 7. Deprecation or Promotion Gate

- [x] Chose `unsupported`: deleted 5 files (parser + header + entry stub + grub.cfg + multiboot2.h); moved `g_boot_info` storage to `boot_hw.c`; linker.ld ENTRY -> `kernel_main`; Makefile `iso` target removed; `BUILD_ALT_BOOT` default = `off`.
- [~] `diagnostic` branch: N/A (policy chose `unsupported`).
- [~] `compatible` branch: N/A.
- [~] Compatible-mode ship gate: N/A.
- [~] Compatible-mode reciprocal XREF in TODO-02 §16: N/A.
- [x] Updated `docs/boot/alt-boot.md` to reflect `unsupported` closure status.
- [x] Commit: `"boot: settle alternate boot support level"` (commit `69397ff3`)

**Test checkpoint:** Repository state matches the chosen policy (`unsupported`): `find src/boot/ src/kernel/ include/kernel -name "multiboot2*"` returns empty (verified post-deletion); `nm build/kernel.exe | grep -i multiboot` returns empty (verified); `bash scripts/build.sh clean` produces `=== BUILD OK ===` (verified); `bash scripts/test-smoke.sh` reaches `SMOKE TEST PASSED` (verified, 2.770s KVM boot). INDEX.md TODO-08 line marks closed.

> **Test runner:** N/A (boot-path deletions; validation = clean build + smoke test) | smoke PASS (KVM 2.770s) + multiboot symbol absence verified via `nm`

> **Notes:**
> - What shipped: 5 file deletions + g_boot_info storage relocation to `boot_hw.c` + new BSP boot stack in C + linker.ld ENTRY -> `kernel_main` + Makefile iso target removed + `BUILD_ALT_BOOT` default = `off`.
> - How it integrates: UEFI bootloader resolves `kernel_main` via ELF symbol table (unaffected by ENTRY change); TSS.rsp0 points at `bsp_boot_stack` for ring 3 -> 0 transitions.
> - Downstream effects: closes TODO-08; alt-boot fenced off so no future contributor ships Multiboot2 / GRUB / Limine / legacy BIOS / EFI stub / kexec without a new policy decision.
> - Canonical doc: [`docs/boot/alt-boot.md`](../../docs/boot/alt-boot.md).
> - Scope boundary: §7 owns deletion + linker.ld + Makefile cleanup. `BOOT_PRODUCER_MULTIBOOT2 = 2` enum value kept reserved for ABI stability.

> **Verified:** 2026-05-20 | commit `69397ff3` | 7/7 items + 5 retired | build OK | smoke PASS (KVM 2.690s) + tooling 375/375 PASS + nm `multiboot2` empty
> **Quality reviewed:** 2026-05-20 | Codex 8x (design + adversarial 5x + consistency 2x + perf) | 2C+3H+5M fixed, 0 open | scope: boot-code-quality + kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                              | 🪟 Win11                    | 🐧 Linux                      | 🚀 Impossible OS                 |
| --- | ------------------------------------ | --------------------------- | ----------------------------- | -------------------------------- |
| 💎  | UEFI primary boot                    | ✅ UEFI/GPT only since 24H2 | ✅ UEFI is default            | ✅ UEFI is the canonical path    |
| 💎  | Legacy BIOS / CSM                    | ❌ removed in 24H2          | ⚠️ deprecated; distro-specific | ❌ unsupported -- §5 doc stance  |
| 💎  | Multiboot2 / GRUB protocol           | ❌ no equivalent            | ✅ widely shipped             | ❌ unsupported (parser deleted)  |
| 💎  | Limine / stivale protocol            | ❌ no equivalent            | ⚠️ niche distros               | ❌ unsupported per policy        |
| ⭐  | Explicit unsupported-path fence      | ⚠️ bootmgr-specific halts    | ⚠️ distro-specific             | ✅ deletion-as-enforcement       |
| ⭐  | Capability-bit degradation surfacing | ❌ silent feature absence   | ❌ silent feature absence     | ⚠️ (no alt-boot path exists)      |
| ⭐  | Higher-level alt-boot loss matrix    | ❌ undefined                | ❌ distro-specific            | ⚠️ (no alt-boot path exists)      |
| ⭐  | Userspace degraded-feature syscall   | ❌ no equivalent            | ❌ scrape dmesg               | ⚠️ (no alt-boot path exists)      |
| ⭐  | Compatible-mode CVE-tracking gate    | ❌ n/a (no alt-boot)        | ⚠️ per-distro shim/grub        | ⚠️ (no GRUB artifact ships)       |
| 💎  | Explicit alt-boot non-goals          | ⚠️ Microsoft-only path       | ⚠️ implicit per-distro         | ✅ `docs/boot/alt-boot.md` table |

> **Closure summary:** Impossible OS matches the Linux + Win11 UEFI/GPT floor and goes beyond on the "Explicit unsupported-path fence" + "Explicit alt-boot non-goals" rows: deletion-as-enforcement is structurally stronger than a build-flag gate (no code to drift), and the non-goal table documents adjacent rejections so future contributors do not re-discover the same decisions. The capability/loss/syscall rows are no longer applicable -- there is no alt-boot code path to surface degradation from.

---

## Unit Tests

> Closed under `unsupported` policy. The Multiboot2 parser was deleted, so there is no kernel-side parser code to unit-test. Validation today is the `nm build/kernel.exe | grep multiboot2` symbol-absence check + `make iso` no-rule check + boot smoke test (UEFI path still works). All retired test items below are kept as historical record of the test coverage that would have shipped under the `diagnostic` or `compatible` policy branches; under the chosen `unsupported` branch they are N/A.

- [~] `test_multiboot2_mmap_to_boot_info` -- N/A (parser deleted).
- [~] `test_multiboot2_missing_uefi_degraded` -- N/A (parser deleted).
- [~] `test_multiboot2_boot_path_attribution` -- N/A (parser deleted).
- [~] `test_alt_boot_policy_release_rejects` -- N/A (no alt-boot path to reject).
- [~] `test_alt_boot_policy_allow_continues` -- N/A (no alt-boot path to allow).
- [~] `test_alt_boot_loader_vars_degraded` -- N/A (no alt-boot path).
- [~] `test_alt_boot_degraded_array_populated` -- N/A (no alt-boot path).
- [~] `test_alt_boot_crash_loop_fatal` -- N/A (no alt-boot path).
- [~] `test_multiboot2_efi_tags_parsed` -- N/A (parser deleted).
- [~] Commit -- N/A.

---

## Verification

> Closure verification (§7 unsupported branch). Each item validated at commit time of §7.

- [x] QEMU UEFI primary boot regression check: `bash scripts/test-smoke.sh` -> `SMOKE TEST PASSED` (KVM 2.690s).
- [~] QEMU GRUB Multiboot2 path -- N/A (parser deleted; `make iso` errors "No rule to make target").
- [x] Canonical policy doc: [`docs/boot/alt-boot.md`](../../docs/boot/alt-boot.md) states `unsupported` as the committed state.
- [x] `nm build/kernel.exe | grep -i multiboot` returns empty (symbol absence is structural, not gated).
- [x] `find src/boot/ src/kernel/ include/kernel -name "multiboot2*"` returns empty (post-deletion).
- [x] No `GAP-ANALYSIS.md` references remain in the codebase or docs.
