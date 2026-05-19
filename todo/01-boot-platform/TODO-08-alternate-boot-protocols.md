---
schema_version: 1
id: alternate-boot-protocols
domain: 01-boot-platform
status: active
title: "TODO-08 -- Alternate Boot Protocols & Compatibility Boundary"
---

# TODO-08 -- Alternate Boot Protocols & Compatibility Boundary

> **Goal:** Decide and enforce how non-primary boot protocols work. The tree still contains Multiboot2 structures and parser code, while the product documentation says UEFI/GPT/ESP is required. This TODO either brings alternate boot protocols to a defined compatibility level or fences them off so they cannot silently rot.

> [!IMPORTANT]
> **Current state:** `src/boot/multiboot2_header.asm`, `include/kernel/multiboot2.h`, `src/kernel/multiboot2_parse.c`, and `src/boot/grub.cfg` exist on disk. UEFI `BOOTX64.EFI` is the real boot path. There is no parity matrix, no CI boot for Multiboot2, no feature-complete fallback contract, and no explicit deprecation policy. Win11 ships UEFI-only on x86_64 (legacy BIOS deprecated as of 24H2); Linux distros routinely ship dual-protocol images with GRUB Multiboot2 + UEFI El Torito. The decision in §1 -- unsupported / diagnostic-only / compatible -- determines whether §2-§6 ship with code or whether §7 deletes the parser.

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

- Alternate boot protocols have explicit support level: unsupported, diagnostic-only, or compatible.
- If retained, Multiboot2 maps into canonical `boot_info` semantics with clear missing-feature behavior surfaced via TODO-01 §11 capability bits.
- If deprecated, build/docs/tests make unsupported paths impossible to mistake for product boot.
- Legacy BIOS, Limine/stivale, GRUB, and future protocols have a documented policy; explicit non-goals (Multiboot1, Linux boot protocol, EFI stub, kexec) are named so adjacent contributors don't try to ship them under TODO-08 scope.
- Alt-boot degradation is enumerable from userspace via `boot_info.alt_boot_degraded[]` + `NtQueryAltBootDegraded()` syscall; no serial-scrape needed.
- `compatible`-mode promotion gate explicitly tracks upstream GRUB/shim CVE backlog + SBAT/dbx/revocation status with a Secure Boot owner reciprocal XREF.

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On                | Status |
| --- | :---: | -------------------------------------------------- | ------------------------- | :----: |
| 💎  |   1   | Alternate boot protocol policy                     | T01 §8                    |  [/]   |
| 💎  |   2   | Multiboot2 feature parity audit                    | §1                        |  [ ]   |
| 💎  |   3   | Multiboot2-to-boot_info adapter                    | §2, T01 §11, T01 §12      |  [ ]   |
| 💎  |   4   | Unsupported-feature degradation matrix             | §2, §3                    |  [ ]   |
| 💎  |   5   | GRUB/Limine/legacy BIOS documentation              | §1                        |  [ ]   |
| ⭐  |   6   | Compatibility test images                          | §3                        |  [ ]   |
| ⭐  |   7   | Deprecation or promotion gate                      | §1--§6                    |  [ ]   |

> 💎 = parity -- Linux distros ship Multiboot2 + UEFI dual-boot; Win11 is UEFI-only since 24H2 (no Multiboot equivalent).
> ⭐ = exclusive -- §6 + §7 explicit policy gate (neither OS publishes a structured "supported / diagnostic / unsupported" taxonomy for alternate boot protocols).

---

## 1. Alternate Boot Protocol Policy

- [/] Decide supported state for Multiboot2: default `diagnostic` by inertia; decision-flip criteria for `off` vs `compatible` documented; final flip lands in §7 with symbol gating.
- [x] Decide supported state for Limine / stivale: default `rejected` per `docs/boot/alt-boot.md`; re-evaluation gate in §7.
- [x] Explicit non-goal table in `docs/boot/alt-boot.md`: Multiboot1, Linux x86 boot protocol / bzImage / zboot, EFI stub direct boot, kexec, legacy BIOS, Limine.
- [x] Document UEFI-only features in `docs/boot/alt-boot.md`: runtime services, Secure Boot, TPM, BootOrder, GOP, USB handoff, ESP integrity, UKI sig chain, A/B, recovery, hibernation, watchdog.
- [/] `BUILD_ALT_BOOT` plumbing: Makefile var + `$(error)` validation + `-D` propagated to CFLAGS + ASFLAGS; `build.sh` help documents passthrough. Symbol-presence assertion lands with §7.
- [x] Updated `docs/getting-started/index.md` Boot protocol section: UEFI-only stance + non-goals + `BUILD_ALT_BOOT` pointer.
- [ ] Commit: `"boot: alternate boot protocol policy"`

**Test checkpoint:** `BUILD_ALT_BOOT=diagnostic make` (default) builds OK; `BUILD_ALT_BOOT=off make` builds OK (today same kernel image -- gating deferred to §7); `BUILD_ALT_BOOT=bogus make` fails fast at Makefile parse with `$(error)`. `docs/boot/alt-boot.md` exists with UEFI-only statement + non-goals + `BUILD_ALT_BOOT` plumbing reference; `docs/getting-started/index.md` Boot protocol section points to it. Behavioral assertions about symbol presence and GRUB artifact shipment are §7 territory. Test on: host (Make parse + clean build).

> **Test runner:** N/A (policy + Makefile var + docs; no kernel-testable surface) | validation: `bash scripts/build.sh` (OK) + `BUILD_ALT_BOOT=bogus make` (fail-fast) + lint clean

> **Notes:**
> - What shipped: `BUILD_ALT_BOOT` Makefile var with `$(error)` validation + CFLAGS/ASFLAGS `-D` propagation; `docs/boot/alt-boot.md` canonical policy doc; `docs/getting-started/` Boot protocol section.
> - Structure / consumers: `docs/boot/alt-boot.md` is single-source-of-truth; cross-linked from getting-started + Makefile comment + TODO-08 §1.
> - Downstream effects: policy decision deferred (`[/]`); final flip to `off`/`compatible` lands in §7 with symbol gating. §2-§6 remain `[ ]` blocked on §1 decision.
> - Canonical doc: [`docs/boot/alt-boot.md`](../../docs/boot/alt-boot.md).
> - Scope boundary: §1 owns policy doc + `BUILD_ALT_BOOT` plumbing + non-goal contract. §7 owns symbol gating (entry.asm + multiboot2_parse.c + release assertion) that makes modes behaviorally distinct.

---

## 2. Multiboot2 Feature Parity Audit

- [ ] Compare every Multiboot2 information tag against `boot_info` fields; produce a parity table mapping each tag to its `boot_info` equivalent or marking it absent.
- [ ] Identify unavailable fields and whether the kernel can degrade gracefully (memory map, framebuffer, ACPI RSDP, modules, command line, boot device).
- [ ] Audit: framebuffer mode (Multiboot2 lacks GOP mode list), memory map (Multiboot2 type taxonomy vs UEFI), ACPI RSDP table, module list, command line, boot device identity.
- [ ] Audit Multiboot2 UEFI tags (`grub-mkrescue` UEFI mode): tag 7 EFI64 system table, tag 11/12 EFI image handle, tag 17 EFI memory map -- §3 / §4 must explicitly consume-or-document-as-degraded each.
- [ ] Record parity table in this TODO and in `docs/boot/boot-protocol.md` (the canonical living contract per T01 §8).
- [ ] Commit: `"boot: audit Multiboot2 parity"`

**Test checkpoint:** `docs/boot/boot-protocol.md` contains a "Multiboot2 parity" section with one row per `boot_info` field, listing tag-or-absent for each. Per-row coverage check: every `boot_info` field referenced in `boot-info-fields.md` appears with a Multiboot2 status in the parity table. Audit produces a list of N fields that have no Multiboot2 equivalent; this list feeds §3 + §4.

---

## 3. Multiboot2-to-boot_info Adapter

- [ ] Populate `boot_info.header` (magic, version, size) on the Multiboot2 path; same `boot_info_validate_header()` semantics apply.
- [ ] Map memory tags into `boot_info.mmap[]`, framebuffer tag into `boot_info.fb`, ACPI RSDP into `boot_info.acpi`, modules into `boot_info.modules`, cmdline into `boot_info.config.cmdline`.
- [ ] Modules security gap: Multiboot2 `module` tag has no signature chain (UKI coverage in T02 §16 is UEFI-only). Reject modules on alt-boot unless `allow_unsigned_modules=1`; surface `BOOT_CAP_UKI_SIGNED_MODULES` in `caps_degraded`.
- [ ] Set T01 §11 `caps_present` bits to mark UEFI-only features absent (no runtime services, no Secure Boot, no TPM, no BootOrder, no USB handoff, no UKI sig chain); set `caps_degraded` accordingly.
- [ ] Set T01 §12 `boot_path = ALT_BOOT`, `boot_reason = ALT_BOOT_PROTOCOL`, `boot_source_flags = MULTIBOOT2_HEADER`; populate `boot_fallback_depth = 0`.
- [ ] Reuse `boot_info_validate_addr()` and `boot_info_validate_header()` so the kernel-side checks stay path-agnostic.
- [ ] Commit: `"boot: adapt Multiboot2 into boot_info"`

**Test checkpoint:** Booting a GRUB Multiboot2 ISO produces a populated `boot_info` that passes `boot_info_validate_header()` AND `boot_info_validate_addr()`. `caps_present & BOOT_CAP_UEFI_RUNTIME == 0` AND `caps_degraded & BOOT_CAP_UEFI_RUNTIME != 0`. `boot_path == ALT_BOOT` reaches kernel. Memory map field count matches GRUB-reported tag.

---

## 4. Unsupported-Feature Degradation Matrix

- [ ] Define behavior for each missing UEFI feature: no UEFI variables, no runtime services, no Secure Boot, no TPM event log, no BootOrder, no USB pre-EBS handoff, no ESP integrity gate.
- [ ] LoaderXxx UEFI variables (T07 §15): alt-boot lacks `gRT->SetVariable`, set `boot_info.loader_vars_degraded = 1` and skip publish loop.
- [ ] Higher-level alt-boot losses (UKI sig chain / A/B / recovery / hibernation / watchdog): modules untrusted; A/B defaults to slot A; recovery via boot.conf only; hibernation refused; UEFI watchdog replaced or disabled.
- [ ] Reciprocal XREFs: T02 §16 (UKI), T21 (A/B), T22 (recovery), T26 (hibernation), T23 (watchdog), T07 §15 (LoaderXxx) -- back-link "alt-boot path loses this feature; degraded via T08 §4".
- [ ] Concrete alt-boot crash recovery flow: no UEFI variables -> T23 watchdog rollback cannot run; persistence via `boot.conf` + BlackBox JSONL (T07 §12); 3-crash threshold -> `boot_fatal(BOOT_ERR_ALT_BOOT_LOOP)` + serial recovery instructions.
- [ ] Mark affected subsystems degraded through the readiness oracle (`kernel_subsystem_set_ready` set with degraded flag); the oracle dump must show which subsystems are degraded due to alt-boot path.
- [ ] Show structured warning in VPD/serial: `[WARN] Alt-boot: <feature> unavailable -- <subsystem> degraded` once per missing feature.
- [ ] Structured degraded surface: `boot_info.alt_boot_degraded[N]` array + `alt_boot_degraded_count` + `NtQueryAltBootDegraded()` syscall so post-boot tools render the list (not serial-scrape).
- [ ] Security-cost note in `docs/boot/alt-boot.md`: `compatible` inherits upstream GRUB/shim CVE surface (BootHole CVE-2020-10713, shim CVE-2023-40547 family, 2025 SB bypasses) -- native UEFI/UKI remains canonical secure path.
- [ ] Prevent release-mode boot from continuing on unsupported protocol unless `boot.conf` has `allow_alt_boot=1` (default 0); halt with `boot_fatal()` otherwise.
- [ ] Commit: `"boot: alternate protocol degradation matrix"`

**Test checkpoint:** Multiboot2 boot with `BUILD_ALT_BOOT=diagnostic` AND `boot.conf:allow_alt_boot=0` produces `boot_fatal()` with reason `BOOT_ERR_ALT_BOOT_REFUSED`. With `allow_alt_boot=1`, boot continues; serial shows N `[WARN] Alt-boot: <feature> unavailable` lines matching the §2 audit's "absent" list. Readiness oracle dump shows degraded subsystems with `degraded_reason = ALT_BOOT_PROTOCOL`.

---

## 5. GRUB / Limine / Legacy BIOS Documentation

- [ ] Document GRUB Multiboot2 usage with the chosen §1 policy state -- include `grub.cfg` template, `grub-mkrescue` invocation, and supported kernel + module load paths.
- [ ] Document Limine / stivale policy: default `rejected` per §1 (niche, UEFI+Multiboot2 cover dual-boot). Re-evaluation gate filed in §7 alongside Multiboot2 promotion.
- [ ] Document Multiboot1 explicit rejection (pre-GRUB-2.02; some old GRUB configs may still emit it).
- [ ] Document Linux x86 boot protocol / bzImage / zboot entry as explicit non-goal (Limine supports it; we don't -- Impossible OS uses its own ABI).
- [ ] Document EFI stub direct boot (CONFIG_EFI_STUB equivalent for `kernel.exe`) as explicit non-goal; UEFI boot path is always `BOOTX64.EFI` -> `kernel.exe`.
- [ ] Document kexec as explicit non-goal in TODO-08 scope; warm-kernel-update is owned by [TODO-01 §14](TODO-01-boot-protocol-abi-handoff.md) + [03-memory-concurrency/TODO-11](../03-memory-concurrency/TODO-11-warm-kernel-update-runtime.md).
- [ ] Document that legacy BIOS is unsupported on x86_64 unless a future TODO adds it; cite Win11 24H2's UEFI-only stance as the parity baseline.
- [ ] Update `docs/development/vm-boot-testing.md` (or create) with the alt-boot QEMU command lines and what to expect on serial.
- [ ] Commit: `"docs: alternate boot protocol boundary"`

**Test checkpoint:** `docs/boot/alt-boot.md` (or equivalent) exists with: §1 policy state quoted, GRUB Multiboot2 invocation working from copy-paste, Limine policy stance stated, legacy BIOS unsupported statement with future-TODO XREF. Every documented command runs without error on a clean dev machine.

---

## 6. Compatibility Test Images

- [ ] Build GRUB Multiboot2 test ISO via `grub-mkrescue` if §1 policy is `diagnostic` or `compatible`; skip if `unsupported`.
- [ ] Boot QEMU with the GRUB ISO; assert kernel reaches `Boot complete in` + `C:\>` on serial under the §3 adapter path.
- [ ] Assert missing UEFI-only features degrade per §4 matrix (warnings on serial, oracle reports degraded).
- [ ] CI gate: `scripts/test-tooling.sh` invokes `scripts/test-alt-boot.sh` keyed on `BUILD_ALT_BOOT`; SKIP on `unsupported`, PASS required on `diagnostic`/`compatible`. Release-artifact filter in `scripts/release/build-manifest.sh`.
- [ ] Reciprocal XREF in TODO-06 §4 (Hybrid ISO / El Torito): GRUB Multiboot2 ISO uses separate boot catalog from canonical UEFI ISO; back-link "alt-boot variant from T08 §6".
- [ ] Commit: `"test: alternate boot protocol image"`

**Test checkpoint:** `bash scripts/test-alt-boot.sh` builds the GRUB ISO (when applicable) and runs it under QEMU. Output matches policy: `unsupported` -> `[SKIP] alt-boot disabled by policy`; `diagnostic`/`compatible` -> serial shows `[BOOT] Multiboot2 path` + `Boot complete in` + the §4 degradation warnings + final `C:\>` prompt. Test exits 0 on success.

---

## 7. Deprecation or Promotion Gate

- [ ] If §1 chose `unsupported`: remove the parser source files, delete the multiboot2 header asm, delete `grub.cfg`, keep only the policy doc; update INDEX.md to mark TODO-08 as "decision-only" or close.
- [ ] If §1 chose `diagnostic`: keep parser code but compile-gate all references behind `BUILD_ALT_BOOT=diagnostic`; never ship in release artifacts (D15 T01 §2 raw image excludes alt-boot when policy is diagnostic).
- [ ] If §1 chose `compatible`: add the alt-boot path as a row in `TODO-28-boot-validation-certification-matrix.md` certification matrix; update D15 T01 to ship the GRUB ISO as a release artifact.
- [ ] **Compatible-mode ship gate**: before any GRUB artifact ships, run upstream GRUB CVE backlog scan, upstream shim CVE backlog scan, and SBAT/dbx/revocation status check; quarterly re-check while `compatible` ships.
- [ ] **Compatible-mode reciprocal XREF** in [`TODO-02 §16`](TODO-02-uefi-hardening-secureboot.md) confirming alt-boot variant is documented as a weaker trust-boundary; Secure Boot owner explicitly accepts the CVE inheritance.
- [ ] Update `docs/boot/alt-boot.md` to reflect closure status; do NOT reference `GAP-ANALYSIS.md` (deprecated artifact -- gap analysis lives per-TODO via `/gap-audit-todo`).
- [ ] Commit: `"boot: settle alternate boot support level"`

**Test checkpoint:** Repository state matches the §1 decision: `unsupported` -> `find src/boot/ src/kernel/ -name "multiboot2*"` returns empty (post-deletion); `diagnostic` -> the files exist but `nm build/kernel.exe | grep multiboot2` returns empty in default release builds; `compatible` -> TODO-28 certification matrix has a Multiboot2 row AND `bash scripts/test-alt-boot.sh` PASSes in CI. INDEX.md TODO-08 line reflects closure.

---

## OS Comparison

| ⭐  | Feature                              | 🪟 Win11                      | 🐧 Linux                       | 🚀 Impossible OS                 |
| --- | ------------------------------------ | ------------------------------ | ------------------------------ | --------------------------------- |
| 💎  | UEFI primary boot                    | ✅ UEFI/GPT only since 24H2   | ✅ UEFI is default             | ✅ UEFI is the canonical path    |
| 💎  | Legacy BIOS / CSM                    | ❌ removed in 24H2            | ⚠️ deprecated; distro-specific | ❌ unsupported -- §5 doc stance  |
| 💎  | Multiboot2 / GRUB protocol           | ❌ no equivalent              | ✅ widely shipped              | ⬜ §1 decides -- §3 adapter      |
| 💎  | Limine / stivale protocol            | ❌ no equivalent              | ⚠️ niche distros               | ⬜ §5 documents stance           |
| ⭐  | Explicit unsupported-path fence      | ⚠️ bootmgr-specific halts     | ⚠️ distro-specific             | ⬜ §1 + §4 + §7 structured gate  |
| ⭐  | Capability-bit degradation surfacing | ❌ silent feature absence     | ❌ silent feature absence      | ⬜ §3 via T01 §11 caps_degraded  |
| ⭐  | Higher-level alt-boot loss matrix    | ❌ undefined                  | ❌ distro-specific             | ⬜ §4 -- UKI/A-B/recovery/hib    |
| ⭐  | Userspace degraded-feature syscall   | ❌ no equivalent              | ❌ scrape dmesg                | ⬜ §4 `NtQueryAltBootDegraded`   |
| ⭐  | Compatible-mode CVE-tracking gate    | n/a (no alt-boot)             | ⚠️ per-distro shim/grub        | ⬜ §7 quarterly CVE + dbx scan   |
| 💎  | Explicit alt-boot non-goals          | ⚠️ Microsoft-only path        | ⚠️ implicit per-distro         | ⬜ §1 + §5 MB1/Linux/EFI/kexec   |

> **After parity items:** Impossible OS will match the Linux + Win11 floor on UEFI/GPT once §1 decides the alt-boot policy. The exclusive items push beyond: a structured "supported / diagnostic / unsupported" taxonomy with a build-flag gate (§1 + §7) and capability-bit-driven degradation surfacing (§3 via T01 §11) that surfaces missing features through the readiness oracle rather than silently degrading.

---

## Unit Tests

> Multiboot2 adapter is kernel-side parser code; tests under `TEST_CAT_BOOT`. Policy decisions in §1 and §7 produce no kernel-testable surface (build flags + docs); validation is via build outputs + boot smoke test.

- [ ] `test_multiboot2_mmap_to_boot_info` -- feed a synthetic Multiboot2 mmap tag, assert `boot_info.mmap_count` matches input AND every entry preserves `phys_addr`, `length`, `type` after type-taxonomy translation.
- [ ] `test_multiboot2_missing_uefi_degraded` -- run adapter without UEFI-only tags, assert `boot_info.caps_present & BOOT_CAP_UEFI_RUNTIME == 0` AND `caps_degraded & BOOT_CAP_UEFI_RUNTIME != 0`.
- [ ] `test_multiboot2_boot_path_attribution` -- assert `boot_info.boot_path == ALT_BOOT` AND `boot_reason == ALT_BOOT_PROTOCOL` AND `boot_source_flags & MULTIBOOT2_HEADER != 0`.
- [ ] `test_alt_boot_policy_release_rejects` -- with `BUILD_ALT_BOOT=diagnostic` AND `boot.conf:allow_alt_boot=0`, simulated alt-boot path triggers `boot_fatal()` with `BOOT_ERR_ALT_BOOT_REFUSED`.
- [ ] `test_alt_boot_policy_allow_continues` -- with `allow_alt_boot=1`, boot continues; readiness oracle reports degraded subsystems with `ALT_BOOT_PROTOCOL` reason.
- [ ] `test_alt_boot_loader_vars_degraded` -- alt-boot path sets `boot_info.loader_vars_degraded == 1`; LoaderXxx publish is skipped.
- [ ] `test_alt_boot_degraded_array_populated` -- `boot_info.alt_boot_degraded_count > 0` AND the array enumerates exactly the missing UEFI features per §4 matrix; `NtQueryAltBootDegraded()` returns the same list.
- [ ] `test_alt_boot_crash_loop_fatal` -- 3 simulated alt-boot crashes (BlackBox JSONL counter) trigger `boot_fatal(BOOT_ERR_ALT_BOOT_LOOP)` on the 4th attempt.
- [ ] `test_multiboot2_efi_tags_parsed` -- when Multiboot2 carries tag 7 / 11 / 12 / 17, the adapter populates the corresponding `boot_info` UEFI fields (or sets the explicit degraded flag if intentionally ignored).
- [ ] Commit: `"test: alternate boot protocol adapter + policy"`

---

## Verification

- [ ] QEMU UEFI remains the primary boot path (no regression).
- [ ] QEMU GRUB Multiboot2 path works per §1 selected policy (skipped if `unsupported`).
- [ ] Documentation clearly states support status under `docs/boot/alt-boot.md`.
- [ ] `BUILD_ALT_BOOT={off,diagnostic,compatible}` produces the documented build outputs (symbol presence, image size delta).
- [ ] No `GAP-ANALYSIS.md` references remain in the codebase or docs.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | host-side `bash scripts/test-alt-boot.sh` for integration | N suites, 0 failures
