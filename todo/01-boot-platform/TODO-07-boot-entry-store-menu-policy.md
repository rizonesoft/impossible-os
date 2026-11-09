---
schema_version: 1
id: boot-entry-store-menu-policy
domain: 01-boot-platform
status: active
title: "TODO-07 -- Boot Entry Store, Menu & Policy"
---

# TODO-07 -- Boot Entry Store, Menu & Policy

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Provide a complete boot-entry model: multiple OS entries, multiple entry kinds (split-kernel, UKI, chainload, network, hibernation resume), A/B slots, recovery, safe mode, test mode, BootNext one-shot reading, deterministic OS-side policy merge, a user-visible boot menu, per-entry health-gated mark-good, OS-visible loader UEFI variables, and idempotent first-install seeding.

> [!IMPORTANT]
> **Current state:** `boot.conf` is parsed into a single `boot_config`; UEFI `BootOrder` / `BootCurrent` / `BootNext` are read for diagnostics ([`TODO-05 §6`](TODO-05-boot-device-discovery.md) shipped); `BOOTX64.UKI.efi` ships as a Unified Kernel Image with whole-chain Secure Boot signature ([`TODO-02 §11`](TODO-02-uefi-hardening-secureboot.md), [`TODO-02 §16`](TODO-02-uefi-hardening-secureboot.md) both shipped); [`TODO-27 §1`](TODO-27-uefi-advanced.md) has a deferred multi-OS chainload contributor that consumes this store. There is no BCD-style entry store, no menu UI beyond firmware selection, no OS-side policy merge, no entry-kind discriminator (kernel-path is implicit), no per-entry health gate, no OS-visible LoaderXxx UEFI variables, no first-install bootstrap, and selection reason is not persisted. The bootloader sees `BootCurrent` (the firmware-selected `Boot####`) but has no mapping to internal entry ids.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- -> XREF: [`TODO-05-boot-device-discovery.md §6`](TODO-05-boot-device-discovery.md) -- UEFI boot variable read (shipped); §3 firmware-provenance reads it
- -> XREF: [`TODO-21-ab-boot-rollback.md §3, §5`](TODO-21-ab-boot-rollback.md) -- A/B slot selection (§3) feeds §9; slot-level `mark_boot_successful` (§5) is the floor §14 layers on top of
- -> XREF: [`TODO-22-recovery-partition.md §1, §7`](TODO-22-recovery-partition.md) -- recovery partition (§1) feeds §9 + §16; recovery UI (§7) consumes §12 audit
- -> XREF: [`TODO-15-visual-post-display.md §3, §4`](TODO-15-visual-post-display.md) -- micro-font + pre-splash renderer used by §6 menu
- -> XREF: [`TODO-10-bare-metal-hardening.md §7`](TODO-10-bare-metal-hardening.md) -- graceful-degradation flags feed §8
- -> XREF: [`TODO-06-boot-media-image-installer-handoff.md §1`](TODO-06-boot-media-image-installer-handoff.md) -- artifact manifest feeds §10 + §16 installer entry
- -> XREF: [`TODO-02-uefi-hardening-secureboot.md §11, §16`](TODO-02-uefi-hardening-secureboot.md) -- UKI artifact + signed payload PE sections (shipped) define §13 `kind: uki`
- -> XREF: [`TODO-25-network-pxe-http-boot.md §7`](TODO-25-network-pxe-http-boot.md) -- network fallback consumes §13 `kind: network`
- -> XREF: [`TODO-26-hibernation-resume-fast-startup-handoff.md §3, §5`](TODO-26-hibernation-resume-fast-startup-handoff.md) -- resume eligibility + handoff consume §13 `kind: resume`
- -> XREF: [`TODO-13-tpm-measured-boot-attestation.md §9`](TODO-13-tpm-measured-boot-attestation.md) -- attestation export carries §3 selected entry id
- -> XREF: [`TODO-11-interrupt-timer-arch.md §1`](TODO-11-interrupt-timer-arch.md) -- pre-APIC clock source for §6 menu countdown (current §6 uses `gBS->Stall` pre-EBS; UTS HAL applies once §6 lands the post-EBS path)
- -> XREF: [`TODO-18-usb-hid-keyboard-mouse.md`](TODO-18-usb-hid-keyboard-mouse.md) -- USB HID handoff for §6 menu input (firmware `SIMPLE_TEXT_INPUT` is the first-class path; HID layered on later)
- -> XREF: [`TODO-27-uefi-advanced.md §1`](TODO-27-uefi-advanced.md) -- UEFI multi-OS chainload contributor consumes §13 `kind: chainload`
- -> XREF: [`../02-kernel-core/TODO-02-kernel-configuration-policy.md §1, §4, §5, §10`](../02-kernel-core/TODO-02-kernel-configuration-policy.md) -- kernel policy merge, control-set, safe-mode, acceptance ledger feed §14

## Outcome

- Boot entries are structured, validated, schema-versioned, CRC-checksummed, and editable by OS tools.
- Multiple entry kinds (split-kernel, UKI, chainload, network, resume, recovery, installer, safe, diagnostics, test) are first-class with per-kind validators.
- Boot menu can select normal, safe, recovery, previous-kernel, test, installer, network, resume, and chainload entries; GOP graphical and serial-text paths are equal-status.
- Firmware-side `BootCurrent` / `BootNext` provenance is read separately from OS-side internal entry selection; the two layers compose without conflation.
- BootCurrent-to-internal-id mapping is explicit; unknown `Boot####` -> fallback entry with logged reason.
- A/B rollback, watchdog rollback, recovery requests, and OS-side defaults compose deterministically with documented precedence.
- Bootloader records selected entry id, selection reason, and rejected/skipped entries in `boot_info`; per-boot history journals durably to BlackBox JSONL (NVRAM is exceptional-only -- no per-boot writes).
- Per-entry health gate ensures only post-health-check boots mark "good"; failed updates never retire the previous-kernel entry.
- Userspace observes the live entry list, capability bitmap, selected entry, and one-shot overrides via systemd-boot-compatible `LoaderXxx` UEFI variables.
- Demote-not-drop UX keeps bad entries visible with `last_failure_reason` labels.
- Bootstrap path idempotently seeds default + recovery + installer entries from offline tooling.
- Per-entry mutation audit (add/remove/reorder/edit) is journalled with requester + prior-state.

## Implementation Order

| ⭐  | Order | Deliverable                                                         | Depends On                              | Status |
| --- | :---: | ------------------------------------------------------------------- | --------------------------------------- | :----: |
| 💎  |   1   | Boot entry file format                                              | --                                      |  [x]   |
| 💎  |   2   | Boot entry parser and validator                                     | §1                                      |  [x]   |
| 💎  |   3   | Boot policy merge order (firmware vs OS layer split)                | §2, T01 §1, T05 §6                      |  [x]   |
| 💎  |   4   | ESP store read + Boot#### OptionalData + policy wiring              | §3                                      |  [x]   |
| 💎  |   5   | Crash-tolerant counter protocol + v19 ABI validator                 | §3, §4                                  |  [x]   |
| 💎  |   6   | Boot menu renderer + countdown + input infrastructure               | §4, T15 §3, T15 §4                      |  [x]   |
| 💎  |   7   | Boot menu indicators + hotkeys + hide_when_alone                    | §6                                      |  [x]   |
| 💎  |   8   | Safe mode, test mode, and diagnostics entries                       | §3, T10 §7                              |  [/]   |
| 💎  |   9   | A/B and recovery entry integration (DEFERRED)                       | §3, §4, T21 §1+§3+§4, T22 §1+§2         |  [/]   |
| 💎  |  10   | Previous-kernel and known-good entries (DEFERRED)                   | §2, §14, T06 §1, D15 T03 §6 (updater)   |  [/]   |
| 💎  |  11   | Boot entry editor tooling                                           | §1, §2                                  |  [/]   |
| ⭐  |  12   | Policy audit (BlackBox primary, NVRAM exceptional)                  | §3, §4, §9                              |  [x]   |
| 💎  |  13   | Entry kinds: split, UKI, chainload, network, resume                 | §1, §2, T02 §11, T26 §3, T25 §7, T27 §1 |  [/]   |
| 💎  |  14   | Per-entry health-gated mark-good                                    | §3, §5, §9, T21 §5, D02 T02 §10         |  [/]   |
| 💎  |  15   | OS-visible loader UEFI variables                                    | §3, §4, §9, §13                         |  [x]   |
| 💎  |  16   | Bootstrap and first-install entry seeding                           | §1, §9, §13, T22 §1, T06 §1             |  [/]   |
| 💎  |  17   | Boot entry tests                                                    | §1-§16                                  |  [/]   |
| 💎  |  18   | systemd BLI parity (BLS display order, one-shot, loader timestamps) | §6, §15                                 |  [x]   |
| 💎  |  19   | Repeated top-level keys defeat the entries-array bound              | §2                                      |  [x]   |
| 💎  |  20   | `crc32` header located by string match, not structurally            | §1, §19                                 |  [x]   |
| 💎  |  21   | Published loader variables need an explicit set-or-clear outcome    | §15, §19                                |  [x]   |
| 💎  |  22   | Store size cap is untested on both sides of the 16 KiB boundary     | §2, §20                                 |  [x]   |
| 💎  |  23   | A rejected store has no repair path, only a one-entry fallback      | §1, §20                                 |  [x]   |
| 💎  |  24   | Boot menu help, power and saved-default keys                        | §6, §7, §15                             |  [x]   |
| 💎  |  25   | Menu timeout keys and one-off edit                                  | §21, §24                                |  [x]   |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.

---

## 1. Boot Entry File Format

Define the on-ESP entry store schema. The file format is the contract every later section reads or writes; getting it stable first means parser, kinds, and tooling can develop in parallel. Per-kind field details live in §10; this section pins the discriminator + envelope.

- [x] Define `\EFI\ImpossibleOS\bootentries.json` (JSON-only; INI fallback dropped during design review for YAGNI).
- [x] Define entry envelope: `id`, `title`, `kind` (10-value enum), `flags` (5 bits), `timeout_override`, `sort_key`, `machine_id`, `policy_tags`. Full schema in `docs/boot/boot-entry-schema.md`.
- [x] Pin per-kind payload field-name table in `docs/boot/boot-entry-schema.md` so §10 has a stable schema to validate against.
- [x] Header carries `schema_version` + CRC-32 over canonical-form entries payload (corruption-only; signed-store is Branch B). Spec in `docs/boot/boot-entry-schema.md`.
- [x] In-firmware fallback pinned: missing/invalid store synthesizes one entry matching current load path (uki under UKI, split with hardcoded `\EFI\ImpossibleOS\kernel.exe` otherwise).
- [x] Document the JSON schema and per-kind field tables in `docs/boot/boot-entry-schema.md`.
- [x] Forward-compat policy: stable 0..99, vendor 100..199 + reserved >=200 skip-with-warn; string-vs-numeric asymmetry documented in `docs/boot/boot-entry-schema.md`.
- [x] Commit: `"boot: define boot entry store format"`

**Test checkpoint:** `bootentries.json` round-trips through host-side validator (`tools/boot-entry-validate`); CRC mismatch rejected with reason; pre-read 16 KiB cap enforced; NaN/Infinity rejected; per-kind type / value / GUID / UKI-prefix violations rejected; ESP-path grammar enforced; kebab-case id; URL hostname + userinfo cross-validated. 78-case mutator harness covers envelope + store + per-kind + forward-compat + path-safety + non-finite + size + URL paths. Test on: host validator (`python3 tools/boot-entry-validate/test_validate.py`) + QEMU TCG smoke (build + smoke test pass with header in tree).

> **Note:** No kernel test surface for §1 -- host-side validator covers schema validation; kernel-side parser tests land in §2.

> **Test runner:** N/A (host-side schema + validator only) | validation: `python3 tools/boot-entry-validate/test_validate.py` (78 cases, 0 failures)

> **Notes:**
> - What shipped: `include/boot/boot_entries.h`, `docs/boot/boot-entry-schema.md`, `tools/boot-entry-validate/` (validator + 57-case harness + README), `resources/boot/bootentries-example.json` (CRC `0x62275CFE`).
> - How it integrates: `validate.py` is the host gate (exit 0/1/2; `--emit-crc` recompute mode); test harness uses in-process CRC via `importlib` so intentionally-invalid fixtures get correct envelopes.
> - Downstream effects: parser/per-kind/bootstrap TODOs consume header types + kind enum + per-kind tables + `--offline --seed` shape. Codex 3-pass adoptions (12 findings) in commit message.
> - Canonical doc: [`docs/boot/boot-entry-schema.md`](../../docs/boot/boot-entry-schema.md).
> - Scope boundary: §1 owns schema + canonical doc + host validator; §2 owns bootloader parser + C-side canonicalization; §10 owns per-kind loaders + UKI prefix runtime check; signed-store (Ed25519) is a Branch B follow-up.

> **Verified:** 2026-05-08 | commit `767ea206` | 7/7 items | build OK | tests 78/78 PASS
> **Quality reviewed:** 2026-05-08 | Codex 7x (design + test-coverage + adversarial + re-adversarial + adversarial-impl + consistency + perf) | 1C+7H+10M+2L fixed, 0 open | scope: boot-code-quality

---

## 2. Boot Entry Parser and Validator

Bootloader-side parser. Must be heap-free (UEFI pre-EBS), bounded, and reject hostile input without halting boot.

- [x] Iterative tokenizer + state-machine value walker (no recursion; depth cap `BOOT_ENTRIES_MAX_PARSE_DEPTH=8`); fixed `boot_entry_envelope_t entries[64]` arena per the size cap. Pure C, no UEFI types.
- [x] Validate envelope: schema_version == 1, 16 KiB size cap, CRC verification via byte-level scheme (per amended §1), unique IDs, kebab-case id, length caps, required envelope field set.
- [x] Reject `kind: chainload` without `trusted_chainload` flag when Secure Boot is on (path-escape gate). Per-kind path-prefix enforcement is owned by §10's per-kind validators.
- [x] Per-kind hook table deferred to §10 per design review; §2 stores `payload_offset` + `payload_length` byte-range slice so §10 can re-tokenize without re-walking the file.
- [x] On parse failure: log via callback, return `reject_code` in `boot_entries_parse_result_t`; `boot_entries_synthesize_fallback()` produces the fallback envelope. `boot_info` wiring deferred to §3.
- [x] Build integration: `boot_entries_parser.o` added to UEFI Makefile OBJS + root Makefile `$(UEFI_EFI)` prerequisites.
- [x] Commit: `"boot: parse boot entry store"`

**Test checkpoint:** 19-case kernel-side mutator harness in `src/kernel/test/test_boot_entry_parser.c` covers valid minimal, bad schema_version, empty entries, missing payload, bad CRC, unknown string-kind skipped, stable-numeric-kind unknown rejected, duplicate id, non-kebab id, malformed payload value, chainload Secure Boot gate, depth-bomb (no crash), trailing garbage after root, bad string escape (`\q`), flags trailing comma, entry-object trailing comma, top-level trailing comma, fallback UKI/split synth. Build OK; smoke test passes (boot complete in 2.48s); lint clean. Test on: kernel test runner via `scripts\debug\kernel\run-boot-tests.bat` + QEMU TCG smoke.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 19 suites, 0 failures

> **Notes:**
> - What shipped: `include/boot/boot_entries_parser.h` (API), `src/boot/uefi/boot_entries_parser.c` (~640 lines: CRC + tokenizer + state-machine walker + validator + fallback), `src/kernel/test/test_boot_entry_parser.c` (19 cases via #include).
> - How it integrates: pure-C / freestanding / no UEFI types / no allocs. Linkable from bootloader (efi_main call deferred to §3) and kernel test binary (#include of source).
> - Downstream effects: §3 consumes `boot_entries_parse_result_t` for boot_info plumbing; §10 consumes payload byte slices for per-kind validation. Codex 2-pass adoptions (7 findings) in commit message.
> - Canonical doc: [`docs/boot/boot-entry-schema.md`](../../docs/boot/boot-entry-schema.md).
> - Scope boundary: §2 owns envelope walk + CRC + path-escape + fallback synth. §3 owns boot_info plumbing. §10 owns per-kind validators. Signed stores remain Branch B.

> **Verified:** 2026-05-08 | commit `3cbeaf55` | 7/7 items | build OK | smoke PASS (KVM 2.48s)
> **Quality reviewed:** 2026-05-08 | Codex 10x (design + adversarial 3x + re-adversarial + adversarial-impl + test-coverage + consistency + perf 2x) | 6H+10M fixed, 0 open | scope: boot-code-quality

---

## 3. Boot Policy Merge Order

Two distinct layers, deterministically composed. **Firmware layer**: UEFI `BootCurrent` / `BootNext` / `BootOrder` are read-only inputs -- the firmware has already deleted `BootNext` before our bootloader runs (UEFI 2.10 spec section 3.1.5), so we observe the *result* via `BootCurrent`, never override it. **OS layer**: once an Impossible OS `Boot####` has been selected by firmware, this section picks the internal entry id from the store. Win11's BCD merge and Linux's per-loader rules conflate these layers, which makes precedence opaque -- documenting the split is where Impossible OS earns ⭐. Live bootloader wiring (ESP read, counter mutation, `boot_info` populate) is owned by §4 + §5; this section ships only the pure-C ladder + ABI surface.

- [x] Firmware-layer provenance: documented view over existing `boot_info.uefi_boot_*` fields per Codex design review; no duplicate aggregate added. Mapping in [`docs/boot/boot-policy.md`](../../docs/boot/boot-policy.md).
- [x] OS-layer precedence ladder `boot_policy_decide()` in `src/boot/uefi/boot_policy.c`: hotkey > watchdog > A/B > recovery > store default (BootCurrent OptionalData hint) > fallback.
- [x] boot_info schema v18 -> v19 adds `selected_entry_id[64]` + `selection_reason` (new 9-value enum) + `rejected_entries[64]` + `rejected_entry_count` + `rejected_entry_overflow`. Mirror + manifest + doc-coverage all updated.
- [x] Parser widened: envelope retains `sort_key` / `machine_id` / `policy_tags[4][24]` / `policy_tag_overflow` / `timeout_override`. Closes Codex design review High finding (parser was discarding ladder inputs).
- [x] BLS-style counter filename `<entry-id>+<L>-<D>` parser + formatter ship in the policy module. Storage: zero-byte files under `\EFI\ImpossibleOS\counters\`. The original spec assumed atomic FAT32 rename; §5 design review caught that UEFI `EFI_FILE_PROTOCOL.SetInfo` rename is NOT power-fail-atomic on FAT32, so §5 ships the crash-tolerant write-new + Flush() + Close(success) + delete-old protocol instead. NOT in bootentries.json. NOT NVRAM.
- [x] Document precedence ladder + 6 worked examples in [`docs/boot/boot-policy.md`](../../docs/boot/boot-policy.md).
- [x] Commit: `"boot: define boot entry policy precedence"`

**Test checkpoint:** 27 ladder + counter unit tests in `src/kernel/test/test_boot_policy.c` cover the 6 worked examples plus BLS filename round-trip / two-digit-done / bad-grammar rejection / boundary caps + NULL guards / kind_skipped + hidden + empty-local-machine-id rejects / A/B slot indexing / multi-signal priority (watchdog beats recovery) / empty-store fallback / 7 regression tests (watchdog-only-peak no-fallthrough, NULL counters with stale count, path-escape per-entry demote, zeroed inputs no auto-A/B, empty machine_id wildcard, counters_overflow fail-closed, invoked_via_uki -> kind=uki fallback). Plus 1 new parser fixture in `test_boot_entry_parser.c` for empty-machine_id acceptance. Build OK; lint clean. Live bootloader wiring (ESP read, OptionalData parse, counter scan, decision -> boot_info) is split out into `§4` and `§5` per the design review. Test on: kernel test runner; full end-to-end QEMU coverage lands when §4 + §5 ship.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 28 suites added (47 total in TEST_CAT_BOOT), 0 failures

> **Notes:**
> - What shipped: `boot_policy.{h,c}` (~370 lines: 6-priority ladder + filter + BLS counter grammar), parser widened (4 retained fields), boot_info v19 with selection ABI, `test_boot_policy.c` (27 cases).
> - How it integrates: pure-C / freestanding / no UEFI types / no allocs. Cross-include pattern same as `boot_entries_parser.c`; live bootloader wiring lives in §4 (ESP read + policy invocation) and §5 (counter protocol + ABI validator).
> - Downstream effects: feeds `§6` (menu) / `§12` (audit) / `§14` (mark-good); closes Codex design High where envelope discarded ladder inputs. Adoptions in commit message.
> - Canonical doc: [`docs/boot/boot-policy.md`](../../docs/boot/boot-policy.md).
> - Scope boundary: §3 owns the pure-C ladder + selection ABI + counter filename grammar. §4 + §5 own bootloader-side ESP / NVRAM I/O + Phase-0 ABI validator.

> **Verified:** 2026-05-08 | commit `39b9feee` | 6/6 items | build OK | smoke PASS (KVM 2.60s)
> **Quality reviewed:** 2026-05-08 | Codex 8x (design + adversarial 2x + re-adversarial 2x + test-coverage + consistency + perf) | 8H+14M fixed, 0 open | scope: boot-code-quality

---


## 4. ESP Store Read + Boot#### OptionalData + Policy Wiring

Wire the §3 ladder into the live boot path. Pre-EBS only -- counter mutation (§5) and `gRT->SetVariable` calls happen here while Boot Services + Runtime Services are still callable. Integration point: AFTER `boot_conf_load` and AFTER the GOP/SecureBoot read (§6), BEFORE `load_kernel()` at `src/boot/uefi/bootx64.c:~9023` -- never post-EBS, since counter mutation needs Boot Services. Sanity cap matches `BOOT_ENTRIES_MAX_TOTAL_BYTES` (16 KiB); larger files are treated as INVALID stores per the schema fallback contract -- the loader synthesizes the in-firmware fallback envelope rather than halting (a hostile or corrupt ESP cannot stop the machine just by inflating the policy file). Path-changing kinds (recovery/diagnostics/network/resume) update `boot_info.boot_path` + `boot_info.boot_reason` to the matching enum values; otherwise leave the existing values alone. `FALLBACK_STORE_INVALID` is the only ladder reason where the caller still synthesizes the fallback envelope itself (the ladder leaves `selected` empty per §3 contract).

- [x] Read `\EFI\ImpossibleOS\bootentries.json` via size-probe + `AllocatePool`; pass to `boot_entries_parse()`. Oversize (>16 KiB) and short reads return EFI_NOT_FOUND so the ladder synthesizes the fallback envelope.
- [x] Parse `Boot####.OptionalData` (`IPOS\x01` tag + kebab-case id) via `gRT->GetVariable`; populate `bootcurrent_entry_id` + `bootcurrent_known`. Bounded walk + kebab grammar gate.
- [x] Invoke `boot_policy_decide()` post-parse, pre-`load_kernel`; copy decision into `boot_info` v19 fields. `supported_kinds_mask` is mode-locked: UKI launches admit only `{UKI}`, split launches admit only `{SPLIT}` (the bootloader cannot switch modes from a policy decision); later sections widen.
- [x] Map path-changing kinds to `boot_info.boot_path` + `boot_info.boot_reason` per the kind -> path table. Pure mapping helper applied in the post-EBS populate block; `_Static_assert`s pin raw constants to kernel enums.
- [x] Synthesize fallback envelope on `BOOT_SELECTION_FALLBACK_STORE_INVALID`. Covers parser-reject AND heap-exhaustion paths.
- [x] Commit: `"boot: read ESP store + wire boot policy decide into bootloader"`

**Test checkpoint:** Crafted `bootentries.json` boots through the ladder; serial shows `[BOOT] policy: selection_reason=N (...)` and the chosen entry id; `kind=recovery` flips `boot_info.boot_path` to `BOOT_PATH_RECOVERY`. Missing/corrupt store falls back without halting. Smoke test verified missing-store flow: serial shows `selection_reason=8 (FALLBACK_STORE_INVALID) selected="" kind=0` (empty selected_entry_id is the documented v19 ABI sentinel; the synthesized "fallback" envelope is only the load-time object), smoke PASS in 2.46s on KVM. Test on: QEMU WHPX + TCG; VirtualBox; bare metal once §6 menu ships.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 7 new boot-policy suites added, 0 failures

> **Notes:**
> - What shipped: `boot_policy_invoke()` + 3 helpers in `bootx64.c`, pure mapping `boot_policy_kind_to_path()` in `boot_policy.c`, `supported_kinds_mask` input field, 4 POST16 codes, 9 unit tests.
> - How it integrates: pre-EBS slot in `efi_main` between `init_gop` and `load_kernel`; post-EBS kind override in the boot-decision populate block (after media-role switch); smoke test green on KVM.
> - Downstream effects: live producer for boot_info v19 selection ABI; kernel-side Phase-0 validator + counter scan ship in §5. Codex review adoptions in commit message.
> - Canonical doc: [`docs/boot/boot-policy.md`](../../docs/boot/boot-policy.md).
> - Scope boundary: §4 owns ESP read / OD parse / ladder invoke / boot_info v19 fields / kind override. §5 owns counter scan + ABI validator. §8 / §9 / §13 / §16 each widen `supported_kinds_mask`.

> **Verified:** 2026-05-08 | commit `a24bc44f` | 6/6 items | build OK | smoke PASS (KVM 2.46s)
> **Quality reviewed:** 2026-05-08 | Codex 8x (adversarial + consistency + perf + re-adversarial 5x) | 1H+7M fixed, 0 open | scope: boot-code-quality

---

## 5. Crash-Tolerant Counter Protocol + v19 ABI Validator

UEFI `EFI_FILE_PROTOCOL.SetInfo` rename is **not** power-fail-atomic on FAT32 -- LFN entries can span multiple directory entries, and a reset mid-rename can leave torn names, duplicates, or orphaned old names (Codex design review 2026-05-08 H finding). Counter persistence has to assume the rename can fail at any byte and design for that. The crash-tolerant protocol is: (1) `Open(new_filename, CREATE)`, (2) `Flush()` + `Close()` -- BOTH must return EFI_SUCCESS for the replacement to be considered durable, (3) only on confirmed durability, `Open(old_filename) + Delete()`. If either Flush or Close fails, step 3 is skipped and the old file is preserved (next-boot scan dedupes). A reset between (1) and (3) leaves both files; the conservative scan resolves duplicates with **lowest** `tries_left` + **highest** `tries_done` (worst-case demotion -- never silently ignore an apparent exhaustion record). This section also lands the kernel-side Phase-0 validator for the v19 selection ABI that §4 starts producing: rejects out-of-range `selection_reason` / `rejected_entries` / non-NUL-terminated id; treats `UNSET` as the explicit "no decision available" sentinel until §4 ships, then flips to fatal.

- [x] Counter scan in `\EFI\ImpossibleOS\counters\`; cap at `BOOT_ENTRIES_MAX_ENTRIES`; overflow -> `counters_overflow=1`. Implemented as `policy_scan_counters()` in `bootx64.c` with conservative duplicate resolution at scan time.
- [x] Conservative duplicate resolution: lowest `tries_left` + highest `tries_done` wins. Implemented in `counters_insert()` helper; covers torn-rename worst-case.
- [x] Crash-tolerant decrement: write-new + Flush() + Close(success) + delete-old (NOT atomic rename). Implemented as `policy_counter_decrement()` in `bootx64.c`; auto-mkdirs the `counters/` directory on first decrement; logs + skips on read-only ESP.
- [x] First-boot bootstrap: missing counter file for chosen entry -> `Open(CREATE)` `<id>+2-1` (BLS 3-try semantic, NOT `+0-1` -- design review caught that `+0-1` would brick single-entry installs before mark-good ships).
- [x] Phase-0 v19 ABI validator in `src/kernel/main/boot_decision.c` rejecting out-of-range fields. Rules 9-11 added: `selection_reason` UNSET-fatal + range; `selected_entry_id` NUL-terminated + STORE_INVALID empty-sentinel; `rejected_entries` integrity (count, overflow, NUL-term, NONE-rejected).
- [x] Update `docs/boot/boot-policy.md` section 4 + remove "atomic FAT32 rename" wording in `include/boot/boot_policy.h`.
- [x] Commit: `"boot: crash-tolerant counter protocol + v19 ABI validator"`

**Test checkpoint:** Pre-populated counter dir with torn duplicate (`foo+1-3` and `foo+0-4`) -> ladder treats `foo` as `tries_left=0` (worst-case demotion via `counters_insert()` dedupe). Decrement creates new + deletes old. First-boot bootstrap creates `<id>+2-1` (BLS 3-try). Phase-0 validator rejects synthetic out-of-range `selection_reason`, UNSET reason, non-NUL-terminated `selected_entry_id`, empty-id-with-non-STORE_INVALID, OOR `rejected_entry_count`, non-boolean `rejected_entry_overflow`, NONE-sentinel rejected reason, non-NUL-terminated rejected id. Build OK; smoke PASS (KVM 2.42s) -- counter dir absent on test ESP so scan returns 0 (correct happy-path for first boot). Test on: QEMU TCG (FS deterministic); bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 10 new boot-decision suites (9 v19 validator + 1 torn-duplicate counter), 0 failures

> **Notes:**
> - What shipped: `policy_scan_counters()` + `policy_counter_decrement()` helpers in `bootx64.c`, public `boot_policy_counter_dedup_insert()` (tri-state: +1 append / 0 merge / -1 cap-reject) in `boot_policy.c`, 3 v19 validator rules (9-11) + 3 error codes in `boot_decision.{c,h}`, `boot/boot_policy.h` consumed by kernel for shared ABI, EFI_FILE_INFO + EFI_FILE_DIRECTORY in `efi.h`, 2 POST16 codes (0xB0B4 required + 0xB0B5 optional).
> - How it integrates: `boot_policy_invoke()` calls scan before ladder, decrement after entry pick (skipped on FALLBACK_*; durability gate Flush+Close before delete-old; scan stops on cap-overflow); kernel validator runs Phase 0 after `boot_caps_validate`; smoke test green.
> - Downstream effects: closes the v19 ABI loop (§4 producer + §5 validator); per-entry tries-tracking is durable across boots. Codex review adoptions in commit message.
> - Canonical doc: [`docs/boot/boot-policy.md`](../../docs/boot/boot-policy.md) section 4.
> - Scope boundary: §5 owns counter persistence + v19 validator. mark-good (deletes counter on success) is §14; menu UX for demoted entries is §6 / §9.

> **Verified:** 2026-05-08 | commit `1d86c05c` | 7/7 items | build OK | smoke PASS (KVM 2.43s)
> **Quality reviewed:** 2026-05-08 | Codex 13x (design + adversarial 4x + test-coverage + consistency 3x + perf 2x + re-adversarial 5x) | 2H+11M fixed, 0 open | scope: boot-code-quality + kernel-code-quality

---

## 6. Boot Menu Renderer + Countdown + Input Infrastructure

Pre-EBS user-visible selector. GOP for graphical, UEFI text protocol + serial mirror for headless, both first-class -- a kiosk host must be selectable without keyboard. Reuse the existing `bsod_aa_char` + `fb_pack_rgb` infrastructure in `bootx64.c` (Selawik AA font already shipped); fall back to `gST->ConOut->OutputString` when GOP is absent or `FrameBufferBase == 0`. Countdown via `gBS->Stall(50000)` ticks (50ms granularity); default 5s; per-entry `timeout_override` from the parsed envelope wins when set. Input via `gST->ConIn->WaitForEvent` + `ReadKeyStroke`: `Up/Down` move, `Home/End` jump, `Enter` select, `Esc` boot the highlighted entry without further interaction. **NEW** `bootloader_secureboot_active()` helper reads `SecureBoot` + `SetupMode` + `AuditMode` UEFI variables BEFORE the menu renders -- `boot_info.secure_boot_enabled` is kernel-populated post-EBS and cannot be trusted pre-EBS (Codex design review 2026-05-08 H finding). The new helper feeds both the §7 indicator and `boot_policy_inputs.secure_boot_active`. Forced-selection paths (hotkey override, watchdog, A/B, recovery) skip the interactive menu entirely and proceed straight to handoff -- no zero-countdown UX surprise.

- [x] GOP renderer using `bsod_aa_char` + `fb_pack_rgb`; UEFI text protocol fallback; serial mirror via `serial_early_print`. Implemented as `boot_menu_render()` in `bootx64.c`; clears framebuffer + renders Selawik AA title + entry rows + countdown footer; ConOut path uses `gST->ConOut->OutputString` with prefix `>` for highlight.
- [x] `bootloader_secureboot_active()` helper -- shipped in §4 (commit a24bc44f), consumed by §7 indicator + `boot_policy_inputs.secure_boot_active`. §6 simply uses the existing helper; no new code here.
- [x] `gBS->Stall(50000)` 50ms-tick countdown; default 5s; per-entry `timeout_override` wins; any key cancels. Implemented in `boot_menu_run()`. Watchdog reset every tick (Codex caught: 600s timeout would trip 60s firmware watchdog without it). Effective timeout capped at `BOOT_MENU_TIMEOUT_CAP_S=60s`.
- [x] `gST->ConIn` `ReadKeyStroke` navigation: Up/Down/Home/End/Enter/Esc. Scan codes per UEFI 2.10 spec Table 12.4 added as `EFI_SCAN_*` constants in `efi.h`.
- [x] Candidate list filter via `boot_policy_menu_collect()`: HIDDEN flag + KIND_SKIPPED-in-rejected only. Demote-not-drop reasons (TRIES_EXHAUSTED, FAILED_HEALTH) stay visible per §9 contract.
- [x] `boot_policy_menu_should_show()` skips UI on forced-selection reasons; only soft reasons (STORE_DEFAULT, BOOTNEXT_HINT, UNKNOWN_BOOTCURRENT) render the menu.
- [x] `timeout_override == 0` honored as immediate auto-boot (kiosk path; UI suppressed). Schema range stays 0..600.
- [x] Watchdog refresh throttled to 10s intervals inside `boot_menu_run()` (firmware WD is 60s; per-tick refresh was wasteful).
- [x] Dirty-rectangle repaint: `boot_menu_render()` draws the menu once, then repaints only the rows a highlight move touched and the footer when its text changes
  - `boot_menu_repaint_plan()` in `src/boot/uefi/boot_policy.c` compares what is on screen with what should be; the drawn view is a stack local in `boot_menu_run()`, so no loader static needs resetting.
  - Row and footer bands are cleared and repainted on their own (`boot_menu_paint_row`, `boot_menu_paint_footer`). The ConOut fallback reprints only on a highlight move, not once a second.
- [x] Each line's ink sits inside its own band: the geometry lives in `include/boot/boot_menu_layout.h` and `boot_menu_text_py()` derives the text origin from the atlas ink box
  - Before this the rows were drawn about 24 px below their highlight bars, so each bar sat above its own text and the next row's bar covered it. Found by the adversarial review of this item.
  - Badge width is measured with `bsod_aa_string_width()` instead of an 11 px/char guess; `test_boot_menu_ink_in_bands` checks the title, all 16 rows and the footer against the shipped atlases.
- [x] The menu fits small GOP modes and keeps text inside the highlight bar
  - `boot_menu_fit()` sizes the list to the framebuffer (9 rows at 640x480, 12 at 800x600) with the footer right below it; `boot_menu_scroll_top()` scrolls a longer list so the selection stays visible, and the footer then reads "Entry N of M".
  - `boot_menu_fit_text()` cuts a badge or title to the bar's right edge and ends it in "..."; a 63-character title of W's measured 1448 px against an 800 px bar before this. Both found by the round-2 adversarial review.
  - Tests: `test_boot_menu_fit_small_modes`, `test_boot_menu_fit_text`, and scroll and plan cases in `test_menu_repaint_plan`.
- [x] The countdown rounds up (`boot_menu_seconds_left`), skips its final 0 repaint, and once a key stops it the footer reads "Use arrows + Enter to boot." instead of "Auto-boot in 0s"
- [x] `SMOKE_MENU=1` smoke fixture (the `store menu` matrix leg): a two-entry store with a 2 s countdown, so a real boot draws the menu, repaints it and auto-boots the default; no leg reached the menu before
- [x] Review fix: a repaint clears only the highlight bar, and a full paint no longer clears every row twice (Codex perf, [medium])
  - `boot_menu_paint_row` fills the bar rectangle once in the row's final colour and skips the fill for plain rows during a full paint; `boot_menu_paint_footer` clears the same rectangle. On a 3840-wide mode a two-row move was 983,040 bytes of clear and a 16-row first paint 7.8 MB of redundant writes.
  - `test_boot_menu_glyph_overhang_in_bar` pins that atlas ink cannot leave the bar (measured overhang 2 px left, 1 px right, against 8 px margins). `boot_menu_gop_ok` also requires a width wider than the text column, so a zero-width mode takes the ConOut fallback (boot auditor, [low]).
- [x] PageUp / PageDown move a screen and 1-9 highlight that entry, via the pure `boot_menu_nav()` in `boot_policy.c` (parity review: systemd-boot has both, and a scrolling list needs paging)
  - `boot_menu_nav_applies()` decides which keys cancel the countdown, so a digit naming no entry (9 on a two-entry menu) leaves auto-boot running (re-adversarial, [medium]). Help, power and saved-default keys are owned by §24.
- [x] Commit: `"boot: menu renderer + countdown + input infrastructure"`

**Test checkpoint:** Menu renders 4+ entries on QEMU WHPX (GOP); countdown decrements 5..0 then auto-selects; arrow keys move + cancel countdown; Enter boots the highlighted entry. Serial mirror shows the same lines on TCG (no GOP). `bootloader_secureboot_active()` returns 1 with SecureBoot=1 + SetupMode=0; 0 otherwise. Smoke test verified menu-skip path: serial shows `[BOOT] menu: skipped (no viable candidates)` on FALLBACK_STORE_INVALID with no bootentries.json, smoke PASS in 2.55s on KVM. Test on: QEMU WHPX (GOP), TCG (serial fallback), VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 16 menu-helper suites, 0 failures | `SMOKE_MENU=1 bash scripts/test-smoke.sh` boots through the menu

> **Notes:**
> - What shipped: `boot_menu_render` + `boot_menu_run` + pure helpers `boot_policy_menu_should_show` + `boot_policy_menu_collect` + `boot_menu_repaint_plan` + `boot_menu_seconds_left` + `boot_menu_scroll_top`; band geometry, `boot_menu_fit` and `boot_menu_fit_text` in `boot_menu_layout.h`; `EFI_SCAN_*` constants; POST16 0xB0B6.
> - How it integrates: `boot_policy_invoke()` calls collect -> should_show -> menu_run after ladder, before counter decrement; `timeout_override==0` skips UI; user-pick flips reason to HOTKEY.
> - Downstream effects: live menu-render path, repainted per band; serial mirror gives kiosk/headless operators visibility on selection; the `store menu` smoke leg boots through it.
> - Canonical doc: [`docs/boot/boot-policy.md`](../../docs/boot/boot-policy.md) (menu UX) + [`docs/boot/boot-menu.md`](../../docs/boot/boot-menu.md) (§7 indicator legend lands there).
> - Scope boundary: §6 owns renderer + countdown + input + filter; indicators + hotkeys + `hide_when_alone` are §7; demote-not-drop visual is §9.

> **Verified:** 2026-11-08 | commit `db82cdb25` + review fixes | all items | build OK | SUITE=boot 7483+17 | `store menu` smoke PASS | matrix 12/12
> **Quality reviewed:** 2026-11-08 | Codex 10x (2 adversarial, consistency, 2 perf, 5 re-adversarial) + boot-quality-auditor + parity pass | 1H+4M+1L fixed, 1M rejected (ConOut past 16: candidates capped at 16, `bootx64.c:7779`), 0 open | scope: boot-code-quality

---

## 7. Boot Menu Indicators, Hotkeys, hide_when_alone

Layer indicators + hotkeys + single-entry skip on top of the menu renderer. F10 firmware setup uses the narrow `OsIndications` BOOT_TO_FW_UI path, gated on `OsIndicationsSupported`; capsule-trigger bits stay banned per the UEFI hardening doctrine. `BOOT_ENTRY_FLAG_HIDE_WHEN_ALONE` already lives at bit 3 (the §7 draft saying "bit 5" was stale before ship); parser already accepts the `hide_when_alone` name. Indicators are ASCII-only (`[SB]`, `[REC]`, `[NET]`, `[FAIL]`) because the Selawik AA atlas covers 0x20-0x7E and Windows serial mojibakes multi-byte UTF-8; `[MB]` reserved for the TPM event-log producer. F8 ships as recognition + audit log; kernel-side safe-mode wire-up (boot_info.safe_mode_request, supported_kinds_mask widening for SAFE) owned by §8.

- [x] Per-entry indicators in `boot_menu_indicators()`: `[SB]` / `[REC]` / `[NET]` / `[FAIL]`. `[MB]` reserved for TPM event-log; A/B slot indicator deferred to §9.
- [x] Hotkey table in `boot_menu_run()`: F8 (safe-mode signal), F10 (firmware setup), F11 (force-show via pre-menu probe), Esc (boot highlighted), Enter (boot selected).
- [x] F10 narrow path in `boot_menu_enter_fw_setup()`: gated on `OsIndicationsSupported & BOOT_TO_FW_UI`; OR-in only that bit; ResetSystem ONLY after SetVariable returns EFI_SUCCESS, otherwise stay in menu.
- [x] F11 force-show via `boot_menu_probe_f11()` BEFORE `boot_policy_menu_should_show()` so the operator can override forced-selection paths.
- [x] `BOOT_ENTRY_FLAG_HIDE_WHEN_ALONE` already at bit 3 in `boot_entries.h` v1 mask; parser accept-list already accepts the name.
- [x] Single-entry store with `hide_when_alone` bypasses menu UI; without flag the lone-entry menu still renders briefly for hotkey access.
- [x] Hotkey table + indicator legend + `hide_when_alone` semantics in `docs/boot/boot-menu.md`; cross-linked from `boot-policy.md` + `boot-entry-schema.md`.
- [x] F8 kernel wire-up (boot_info.safe_mode_request field + supported_kinds_mask widening for SAFE) owned by §8; §7 ships F8 recognition + audit log only.
- [x] Commit: `"boot: menu indicators + hotkeys + hide_when_alone"`

**Test checkpoint:** Boot a multi-entry store -> recovery row renders `[REC]` ahead of title. SecureBoot=1 -> all rows render `[SB]`. F10 with `BOOT_TO_FW_UI` supported -> reboots into setup; without support -> serial logs `OsIndicationsSupported absent` and menu continues. F8 -> serial logs `safe-mode requested (kernel wire-up pending)`. Single-entry + `hide_when_alone` -> serial logs `1 candidate + hide_when_alone -- skipping`; without flag the menu renders. F11 buffered during handoff -> menu renders even on forced-selection paths. Test on: QEMU WHPX + TCG; bare metal.

> **Test runner:** N/A (pre-EBS UEFI bootloader code -- no kernel-testable mirror) | validation: serial log on QEMU WHPX + TCG + bare metal per Test checkpoint above

> **Notes:**
> - What shipped: `boot_menu_indicators` + `boot_menu_probe_f11` + `boot_menu_enter_fw_setup` helpers; `BOOT_MENU_HOTKEY_*` codes; `boot_menu_run` extended with hotkey return + decision/sb context + allow_skip_when_alone.
> - How it integrates: `boot_policy_invoke()` probes F11, computes hide_when_alone, calls `boot_menu_run`, dispatches F10 -> `boot_menu_enter_fw_setup` (gated on SetVariable success).
> - Downstream effects: live menu UX surface -- indicators visible, hotkeys active, F10 reboots into firmware setup, hide_when_alone honored.
> - Canonical doc: [`docs/boot/boot-menu.md`](../../docs/boot/boot-menu.md) (legend + hotkey table + F10 narrow path).
> - Scope boundary: §7 owns indicators + hotkeys + hide_when_alone; F8 kernel wire-up + SAFE kind admission belong to §8; A/B slot indicator + last-failure label belong to §9; `[MB]` measured-boot indicator belongs to the measured-boot TODO domain.

> **Verified:** 2026-05-09 | commit `d7d1cff6` (impl) + post-review fixup | 9/9 items | build OK | smoke PASS (KVM 2.59s)
> **Quality reviewed:** 2026-05-09 | Codex 9x (design + adversarial + 5 re-adversarial + consistency + perf) | 2H+9M fixed | scope: boot-code-quality

---

## 8. Safe Mode, Test Mode, and Diagnostics Entries

Convert today's ad-hoc `boot.conf` booleans into structured entries with explicit flags. SAFE ships now via post-policy `boot_config` materialization; TEST + DIAGNOSTICS need per-kind payload parsing plus an early-policy-read for DIAGNOSTICS' verbose POST. Both deferred to §13 entry-kind dispatch.

- [x] `kind: safe` admitted via `supported_kinds_mask` widen; post-policy materialization sets `boot_config.boot_mode = 1`. `safe_mode_subset` payload parsing deferred to §13.
- [x] F8 hotkey wires safe-mode intent into `boot_config.boot_mode = 1` independent of selected envelope kind (closes §7's deferred F8 wire-up).
- [/] `kind: test` admission + `test_suite` payload parsing deferred to §13 (per-kind handler reads payload; without it every test entry would run all categories).
  - Blocker owner: -> XREF: 01-boot-platform/TODO-07 §13 (item: "Entry kinds: split, UKI, chainload, network, resume"). Still `[/]` as of 2026-09-03: the per-kind handler table that would read the payload object has not shipped.
- [/] `kind: diagnostics` admission deferred to §13 + an early-policy-read redesign (`boot_config.verbose` is consumed pre-policy).
  - Blocker owner: -> XREF: 01-boot-platform/TODO-07 §13 (item: "Entry kinds: split, UKI, chainload, network, resume"). Also needs the early-policy-read redesign, because `boot_config.verbose` is consumed before policy is read.
- [x] On SAFE selection, `boot_config.boot_mode` is materialized post-policy pre-EBS; existing kernel consumers read the field unchanged.
- [x] KUSD `SafeBootMode` consumer mirrors `boot_config.boot_mode` per [02-kernel-core TODO-02](../02-kernel-core/TODO-02-kernel-configuration-policy.md#5-safe-mode-policy-and-effective-safe-mode); §8 only changes the producer.
- [x] `supported_kinds_mask` widened to include KIND_SAFE only; TEST + DIAGNOSTICS stay filtered until §13.
- [x] Commit: `"boot: structured safe/test/diagnostic entries"`

**Test checkpoint:** Selecting `kind: safe` yields `boot_config.boot_mode == 1` and serial shows `[BOOT] policy: kind=SAFE -- boot_config.boot_mode=1 materialized`. F8 on any highlighted entry yields `[BOOT] menu: F8 -- boot_config.boot_mode=1 (safe-mode override)`. KUSD `SafeBootMode` non-zero post-boot once kernel consumers wire up. TEST + DIAGNOSTICS entries currently rejected with `BOOT_REJECT_REASON_KIND_UNAVAILABLE` (§13 owns admission). Test on: QEMU WHPX + TCG; bare metal.

> **Test runner:** N/A (pre-EBS materialization in bootloader; kernel-side observers owned by TODO-10 + 02-kernel-core TODO-02) | validation: serial log on QEMU WHPX + TCG + bare metal per Test checkpoint

> **Notes:**
> - What shipped: `supported_kinds_mask |= KIND_SAFE`; SAFE materialization in `boot_policy_invoke()` after menu pick; F8 hotkey wires `boot_config.boot_mode = 1` independent of kind.
> - How it integrates: both writes target `g_boot_info_ptr->config.boot_mode`, the same field consumed by the existing boot.conf parser; F8 path runs before SAFE materialization so F8 + SAFE combinations are idempotent.
> - Downstream effects: closes §7's deferred F8 wire-up; unblocks TODO-10 + 02-kernel-core TODO-02 KUSD SafeBootMode consumers.
> - Canonical doc: [`docs/boot/boot-policy.md`](../../docs/boot/boot-policy.md) (per-kind dispatch) + [`docs/boot/boot-menu.md`](../../docs/boot/boot-menu.md) (F8 row).
> - Scope boundary: §8 owns SAFE materialization + F8 wire-up + KIND_SAFE admission. TEST + DIAGNOSTICS payload parsing is §13. KUSD SafeBootMode population is 02-kernel-core TODO-02 §5.

> **Verified:** 2026-05-09 | commit `9fb38d53` (impl) + post-review fixup | 6/8 items + 2 deferred to §13 | build OK | smoke PASS (KVM)
> **Deferred:** [M] `kind: test` admission + test_suite payload parsing -> XREF: 01-boot-platform/TODO-07 §13 (item: "Entry kinds: split, UKI, chainload, network, resume" -- per-kind handler table reads payload object)
> **Deferred:** [M] `kind: diagnostics` admission + verbose POST early-policy-read -> XREF: 01-boot-platform/TODO-07 §13 (item: "Entry kinds: split, UKI, chainload, network, resume" -- diagnostics needs early-policy-read pass before pre-policy verbose decision)
> **Quality reviewed:** 2026-05-09 | Codex 5x (design + adversarial + re-adversarial + consistency + perf) | 1H+3M fixed, 1M rejected (SAFE payload validation -- §8 scope does not depend on payload fields, payload subset is §13 scope) | scope: boot-code-quality

---

## 9. A/B and Recovery Entry Integration

Generate entries from runtime state; the user sees a single coherent menu instead of two parallel mechanisms. Demote-not-drop on failure. **Section deferred 2026-05-09**: the runtime-state producers (TODO-21 A/B slot metadata + TODO-22 recovery partition) are not yet shipped; §9 has nothing to integrate until they land. See blocker XREFs per item below; re-enter §9 after the prerequisites land.

- [/] Generate entries for slot A, slot B, recovery, fallback. Blocked: TODO-21 §1+§3, TODO-22 §1.
  - Blocker owner: -> XREF: 01-boot-platform/TODO-21 §1 (item: "Choose storage: UEFI NVRAM ... vs GPT metadata partition" at line 5) + 01-boot-platform/TODO-22 §1 (item: "GPT layout: EFI (64 MiB) + Slot A ..." at line 5). Both still `[/]` as of 2026-09-03.
- [/] Merge A/B success/failure counters into entry labels. Producers ready (TODO-21 §1 metadata + §4 tries + §6 `boot_info.ab_slot_tries`/`ab_slot_flags`); the label-merge is §9 work when this section re-enters.
  - Blocker owner: -> XREF: 01-boot-platform/TODO-21 §1 (item: "Choose storage: UEFI NVRAM ... vs GPT metadata partition" at line 5). The producers exist; what is missing is this section re-entering to merge the counters into the label, so the park is on §9 scheduling rather than on data.
- [/] Auto-select recovery on double-fail with `BOOT_SELECTION_FALLBACK_ALL_PATHS_BAD`. Blocked: TODO-21 §4 + TODO-22 §2; sentinel enum value lands when TODO-22 §2 ships.
  - Blocker owner: -> XREF: 01-boot-platform/TODO-22 §1 (item: "GPT layout: EFI (64 MiB) + Slot A ..." at line 5). The `BOOT_SELECTION_FALLBACK_ALL_PATHS_BAD` sentinel enum value lands with TODO-22 §2.
- [/] Demote-not-drop visual (greyed-out + `last_failure_reason` label). Partially unblocked: §6+§8 keep TRIES_EXHAUSTED rows visible; §7 adds `[FAIL]` indicator. The greyed style + label string remains §9 scope.
  - Blocker owner: -> XREF: 01-boot-platform/TODO-21 §1 (item: "Choose storage: UEFI NVRAM ... vs GPT metadata partition" at line 5). Partially unblocked already: §6 and §8 keep TRIES_EXHAUSTED rows visible and §7 adds the `[FAIL]` indicator, so only the greyed style and the label string remain §9 scope.
- [/] Display rollback reason in menu line. Partially unblocked: TODO-21 §6 ships the rollback-reason source (`boot_info.ab_select_reason`/`ab_from_slot`) + the VPD display already; the menu-line rendering remains §9 scope.
  - Blocker owner: -> XREF: 01-boot-platform/TODO-21 §1 (item: "Choose storage: UEFI NVRAM ... vs GPT metadata partition" at line 5). TODO-21 §6 shipped the rollback-reason source and the VPD display; only the menu-line rendering remains §9 scope.
- [/] Widen `supported_kinds_mask` to include `BOOT_ENTRY_KIND_RECOVERY`. Blocked: TODO-22 §2 (recovery load path).
  - Blocker owner: -> XREF: 01-boot-platform/TODO-22 §1 (item: "GPT layout: EFI (64 MiB) + Slot A ..." at line 5). The recovery load path has to exist before the mask can honestly admit `BOOT_ENTRY_KIND_RECOVERY`.
- [/] Commit: `"boot: integrate A/B and recovery entries"`
  - PARKED with the rest of §9; this is the section commit and closes when the six items above do. Blocker owner: -> XREF: 01-boot-platform/TODO-21 §1 (item: "Choose storage: UEFI NVRAM ... vs GPT metadata partition" at line 5).

**Test checkpoint:** Corrupt slot A kernel; reboot 3 times; menu shows `Slot A (try 3/3, kernel CRC fail)` greyed out and auto-selects slot B. Both slots fail -> recovery selected without user input. Recovery partition corrupt + both slots fail -> fallback default with ALL_PATHS_BAD reason. Test on: QEMU TCG (deterministic); bare metal. **Cannot run today** -- requires TODO-21 + TODO-22 producers.

> **Test runner:** N/A (deferred -- blocked on unshipped producers) | validation: §9 Test checkpoint once TODO-21 + TODO-22 land
> **Notes:**
> - **Status:** deferred 2026-05-09 (re-filed `[/]` 2026-06-13); all items blocked on unshipped runtime-state producers, nothing to integrate until they land.
> - **Blockers:** A/B slot metadata (`01-boot-platform/TODO-21` §1/§3/§4) + recovery partition (`01-boot-platform/TODO-22` §1/§2), both NEEDS_WORK.
> - **Partial:** the demote-not-drop greyed-out visual + label is the only §9-owned piece; §6/§8 keep TRIES_EXHAUSTED rows visible and §7 adds the `[FAIL]` indicator.
> - **Scope boundary:** §9 owns the menu integration of A/B + recovery entries; TODO-21/TODO-22 own the runtime state it reads.
> **Verified:** 2026-06-13 | 0/6 items (all blocked) | build N/A | scope: section deferred -- all items blocked on unshipped producers, no code shippable this pass
> **Deferred:** [M] A/B + recovery menu integration blocked on the runtime-state producers -> XREF: 01-boot-platform/TODO-21 §1 (item: "Choose storage: UEFI NVRAM ... vs GPT metadata partition" at line 5) + 01-boot-platform/TODO-22 §1 (item: "GPT layout: EFI (64 MiB) + Slot A ..." at line 5)

---

## 10. Previous-Kernel and Known-Good Entries

After an update, the previous kernel stays available until the new kernel is confirmed good. Health gate (§14) is the gating signal, not a wall-clock timer. **Section deferred 2026-05-09**: the producers it integrates (§14 health gate + the post-update insertion hook owned by the updater in 15-installer-release) are not yet shipped; without them §10's items have nothing to integrate. Same shape as §9. See blocker XREFs per item below; re-enter §10 after the prerequisites land.

- [/] Persist last-known-good kernel path + manifest digest + slot id in the entry store. Blocked: §14 (kernel-good signal) + 15-installer-release TODO-03 §6 (updater that writes the persisted state).
  - Blocker owner: -> XREF: 15-installer-release/TODO-03 §6 (item: "`scripts/make-delta.sh`" at line 7). Also gated on §14 supplying the kernel-good signal.
- [/] Insert "Previous kernel (known-good)" entry post-update; remove ONLY after §14 marks new kernel good. Blocked: §14 + updater hook.
  - Blocker owner: -> XREF: 15-installer-release/TODO-03 §6 (item: "`scripts/make-delta.sh`" at line 7). Removal is gated on §14 marking the new kernel good, so the entry lifecycle needs both halves.
- [/] Interlock with TODO-13 measured boot + TODO-06 §1 manifest digest. TODO-06 §1 is shipped; TODO-13 §9 (attestation export) consumes the selected entry id but the digest-equality check still depends on the updater wiring.
  - Blocker owner: -> XREF: 15-installer-release/TODO-03 §6 (item: "`scripts/make-delta.sh`" at line 7). TODO-06 §1 shipped; the digest-equality check still waits on the updater wiring, and TODO-13 §9 only consumes the selected entry id.
- [/] Failed-health or panic auto-promotes previous-kernel; `last_failure_reason` set on the new entry. Blocked: §14 (health-gate failure source) + a panic-counter persistence path (no current owner).
  - Blocker owner: -> XREF: 02-kernel-core/TODO-30 §7 (item: "Track crash/hang counts by bucket across boots") -- that item owns cross-boot crash and hang counts, which is the panic-counter persistence this needs; it was recorded as ownerless before 2026-09-03. Also gated on §14 for the health-gate failure source.
- [/] Manifest-missing fallback -> `BOOT_REJECT_REASON_MANIFEST_UNREADABLE`. Blocked: enum value not yet added; manifest read in bootloader is partial (TODO-06 §7 manifest verification is `[/]`).
  - Blocker owner: -> XREF: 01-boot-platform/TODO-06 §7 (item: "manifest verification"). The `BOOT_REJECT_REASON_MANIFEST_UNREADABLE` enum value is not added yet and the bootloader-side manifest read is still partial.
- [/] Commit: `"boot: previous-kernel known-good entries"`
  - PARKED with the rest of §10; this is the section commit and closes when the five items above do. Blocker owner: -> XREF: 15-installer-release/TODO-03 §6 (item: "`scripts/make-delta.sh`" at line 7).

**Test checkpoint:** Apply update -> reboot -> menu shows new + previous. Boot new + health gate passes -> reboot -> previous-kernel entry retired. Boot new and panic 3x -> previous-kernel entry auto-selected. Manifest deleted post-update -> previous-kernel stays around. Test on: QEMU TCG; bare metal. **Cannot run today** -- requires §14 + the updater + a panic-counter producer.

> **Test runner:** N/A (deferred -- blocked on the updater + panic-counter producers) | validation: §10 Test checkpoint once they land
> **Notes:**
> - **Status:** deferred 2026-05-09 (re-filed `[/]` 2026-06-13); items blocked on the post-update insertion hook + a panic-counter producer. §14 health gate is shipped but its consumers here are not.
> - **Blockers:** the installer updater that writes persisted known-good state (`15-installer-release/TODO-03`, exact hook section TBD) + a panic-counter persistence path with no current owner.
> - **Partial:** §14 (health gate) + TODO-06 §1 (manifest digest) are shipped; the digest-equality + entry-insertion still need the updater wiring.
> - **Scope boundary:** §10 owns the previous-kernel/known-good menu entries; the installer updater + §14 own the good/bad signal it gates on.
> **Verified:** 2026-06-13 | 0/6 items (all blocked) | build N/A | scope: section deferred -- blocked on the updater + a panic-counter producer, no code shippable this pass
> **Deferred:** [M] previous-kernel known-good entries blocked on producers -> XREF: 15-installer-release/TODO-03 §6 (item: "`scripts/make-delta.sh`" at line 7 -- the precise post-update known-good-writer hook needs a concrete owner item filed there) + an ownerless panic-counter persistence path (needs a tracked owner before §10 can proceed)

---

## 11. Boot Entry Editor Tooling

Offline editor so operators are not stuck hex-editing the JSON. Host-side Python ships now (offline + CI/release path); native user-mode binary deferred until user-mode runtime supports it.

- [x] `tools/bootcfg/bootcfg.py` host CLI subcommands: list / add / remove / set-default / emit-seed. Live-boot subcommands (set-bootnext-hint, set-oneshot, dump-history) deferred to a future user-mode binary.
- [/] Live-boot `bootcfg` native binary: set-bootnext-hint, set-oneshot, dump-history
  - BLOCKED on 02-kernel-core/TODO-12 §3 (item: "Extend INT 0x2E + SYSCALL entry to read stack arguments 5-6+"). Both ring-3 entry paths pass arg5 as 0 (`syscall_entry.asm:100`, `syscall.c:1320`), so `NtSetSystemEnvironmentValueEx` gets Attributes 0 and UEFI deletes the variable instead of writing it.
  - Checked 2026-11-08: reads work (the Ex query's arg5 is the optional attributes out-pointer), but every subcommand that matters writes `LoaderEntryOneShot` or `BootNext`, so the binary waits for the write path rather than shipping read-only and being redone.
- [x] Offline-only mode (operates on a path argument); CI + release tooling consume it directly.
- [x] Every write goes through `tools/boot-entry-validate/validate.py`; invalid stores never touch disk. Atomic CoW: tempfile + fsync + os.replace + parent-dir fsync.
- [x] `set-default` rewrites sort_keys (not just array order); the policy ladder picks lowest sort_key.
- [x] `scripts/release/build-image.sh` invokes `bootcfg.py emit-seed` to populate the staged ESP; idempotent re-run preserves the deterministic-image guarantee.
- [x] Command surface + atomic-write protocol + deferred-subcommand list documented in [`docs/boot/bootcfg.md`](../../docs/boot/bootcfg.md).
- [x] Validator gap closed: `MAX_SORT_KEY_LEN=63`, `MAX_POLICY_TAGS=4`, `MAX_POLICY_TAG_LEN=23` enforced so bootcfg cannot persist parser-rejecting stores.
- [x] Commit: `"tools: boot entry editor"`

**Test checkpoint:** `python3 tools/bootcfg/bootcfg.py list <path>` shows current entries; `set-default <id>` rewrites sort_keys so the named id wins the policy ladder's lowest-sort_key tie-break. `emit-seed` is byte-idempotent. `add` of an entry with an over-cap sort_key, oversize policy_tags array, or path-escape kernel path fails before persisting. Atomic write leaves no `.new` tempfiles on success or rejection. Test on: 19 self-contained tests in `tools/bootcfg/test_bootcfg.py` + smoke build with `scripts/release/build-image.sh`.

> **Test runner:** `python3 tools/bootcfg/test_bootcfg.py` | 19 cases, 0 failures (mirrors `tools/boot-entry-validate/test_validate.py` pattern, no pytest dep)

> **Notes:**
> - What shipped: `tools/bootcfg/bootcfg.py` (5 subcommands), `tools/bootcfg/test_bootcfg.py` (19 cases), `docs/boot/bootcfg.md`, validator length-cap parity (3 new constants), `build-image.sh` emit-seed integration.
> - How it integrates: bootcfg imports validate.py for schema + CRC + validate_store; never duplicates schema constants. Atomic CoW = tempfile + fsync + os.replace + parent-dir fsync. emit-seed wired into release-image staging.
> - Downstream effects: deterministic build-image now seeds the boot store; CI consumers gain validated edit primitives; live-boot bootcfg is the only piece left for the editor-tooling story.
> - Canonical doc: [`docs/boot/bootcfg.md`](../../docs/boot/bootcfg.md).
> - Scope boundary: §11 owns the offline editor + the validator length-cap parity. Live-boot UEFI-variable subcommands (BootNext / one-shot / history) belong to a future user-mode binary; the §12 audit log owns the history side.

> **Verified:** 2026-05-09 | commit `01cbc213` (impl) + post-review fixup | 7/8 items + 1 deferred | build OK | tests 25/25 + 78/78 PASS
> **Notes:** Reopened 2026-11-07, was Deferred on its own `[x]` item, which hid the live-boot binary from every pass.
> **Quality reviewed:** 2026-05-09 | Codex 5x (design + adversarial + 2 re-adversarial + consistency + perf) | 2H+5M fixed | scope: N/A (host-side Python tool)
> **Deferred:** live-boot `bootcfg.exe` (ring-3 firmware-variable writes arrive with Attributes 0) -> XREF: 02-kernel-core/TODO-12 §3 (item: "Extend INT 0x2E + SYSCALL entry to read stack arguments 5-6+")

---

## 12. Policy Audit Trail and Rollback Reason Codes

Per-decision audit. Disk-first (BlackBox JSONL) -- NVRAM is exceptional-only because per-boot writes wear flash and BLS rationale (`https://uapi-group.org/specifications/specs/boot_loader_specification/#why-not-use-efi-variables-for-storing-the-boot-counter`) explicitly rejects EFI vars for high-frequency state. The bootloader is read-only on the sticky var; the kernel acks consumed triggers post-publish so a reset between observe and ack leaves the trigger pending (correct sticky semantic). ⭐ vs Win11 (event log only) and Linux (journalctl scattered).

- [x] Audit event-code enum + 256-byte `boot_sticky_record` (magic + version + size + CRC, offsets pinned) in `include/boot/boot_audit_codes.h`.
- [x] Per-boot record -> `X:\Boot\history.jsonl` (append-only, 4 MiB rotated) via `boot_audit_publish()` in `src/kernel/main/boot_audit.c`. Schema in [`docs/boot/boot-history-schema.md`](../../docs/boot/boot-history-schema.md).
- [x] `ImpossibleOS-BootSticky` (256-byte) consumed read-only by `src/boot/uefi/boot_sticky.c`; surfaced through boot_info v20; kernel acks consumed triggers post-publish via `uefi_var_set`.
- [x] `tools/bootcfg/bootcfg.py --mutation-log <path>` appends JSONL on every successful mutation; failed mutations emit no record. Live-boot path lands at `X:\Boot\mutations.jsonl` when the deferred user-mode bootcfg binary ships.
- [x] NVRAM-read failure -> `boot_info.audit_degraded=1` + `BOOT_AUDIT_EVENT_AUDIT_DEGRADED` + serial WARN; ack failure -> WARN + trigger persists (idempotent retry).
- [x] [`TODO-22 §7`](TODO-22-recovery-partition.md) recovery UI and TODO-14 boot diagnostics consume both JSONL streams (passive contract; no live wire-up needed today).
- [x] Commit: `"boot: audit boot entry policy decisions"`

**Test checkpoint:** Force watchdog rollback -> NVRAM sticky bit set + BlackBox JSONL gains a record with `event=WATCHDOG_ROLLBACK`; subsequent boots see the sticky flag and auto-recover. Per-boot records always go to BlackBox; NVRAM-quota-full leaves `audit_degraded=1` without blocking boot. `bootcfg --mutation-log <path>` round-trips a JSONL record on every successful mutation; failed mutations leave no record. Test on: QEMU WHPX + TCG; bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 13 boot_audit suites + bootcfg `python3 tools/bootcfg/test_bootcfg.py` (32 cases incl. 7 new mutation-log cases, 0 failures)

> **Notes:**
> - What shipped: audit codes header, sticky reader, JSONL publisher + selection-reason-gated ack, boot_info v20, dual-file boot_seq, bootcfg `--mutation-log`, 22 kernel + 7 bootcfg tests.
> - How it integrates: pre-policy sticky read feeds `inputs->recovery_requested` / `watchdog_rollback`; Phase-3 publish + `uefi_var_set` clears triggers only when `selection_reason` matches (reset between -> trigger persists).
> - Downstream effects: TODO-14 + [`TODO-22 §7`](TODO-22-recovery-partition.md) consume both JSONL streams when shipped; CI passes `--mutation-log` to stage the disk trail.
> - Canonical doc: [`docs/boot/boot-history-schema.md`](../../docs/boot/boot-history-schema.md).
> - Scope boundary: §12 owns codes + sticky + JSONL + mutation log. §14 flips `last_outcome` (deferred). [`TODO-23`](TODO-23-boot-watchdog.md) produces `watchdog_rollback_request`.

> **Verified:** 2026-05-10 | commit `38517fcc` | 7/7 items | build OK | smoke PASS (KVM 2.41s, seq=1) | tests 32/32 bootcfg + 22 boot_audit
> **Notes:** Reopened 2026-11-07, was Deferred: [L] Audit dual-write-failure dedup harness. The stamp named this section's own item, and that item recorded no external blocker.
> **Quality reviewed:** 2026-05-10 | Codex 12x (design + adversarial 2x + test-coverage + re-adversarial 5x + adversarial-impl 4x + consistency + perf) | 7H+5M+0L fixed, 0 open, 2L deferred-XREF | scope: kernel-code-quality + boot-code-quality

---

## 13. Entry Kinds: Split, UKI, Chainload, Network, Resume

The §1 envelope only carries the discriminator; this section owns the per-kind validators, payload-loading paths, and the integration with neighboring TODOs that produce/consume each kind. Without this section, the schema is a paper exercise -- §1 supports a flag-based "kernel + initrd" model that cannot represent the UKI artifact already shipped in [`TODO-02 §11`](TODO-02-uefi-hardening-secureboot.md), or the resume / network / chainload cases owned by neighboring TODOs.

- [x] `kind: split` validator (kernel required + ASCII + allowed-prefix + no `..`; cmdline / root / initrd[] optional). Authoritative loader: SOLE candidate, refuses fallback to ambient on miss.
- [x] `kind: uki` schema-enforced validator: payload-absence accepted; payload-present requires `uki_path` (ASCII + Linux/ImpossibleOS prefix + no traversal), optional `profile` 0..15, all other keys rejected as smuggled overrides.
- [/] `kind: chainload` stub validator (`REJ_NOT_SUPPORTED`). LoadImage/StartImage runtime path -> [`TODO-27 §1`](TODO-27-uefi-advanced.md).
- [/] `kind: network` stub validator. HTTP/TFTP runtime + url+digest fields -> [`TODO-25 §7`](TODO-25-network-pxe-http-boot.md).
- [/] `kind: resume` stub validator. Hibernation snapshot runtime -> [`TODO-26 §3, §5`](TODO-26-hibernation-resume-fast-startup-handoff.md).
- [x] `kind: safe` accepts SPLIT shape; `recovery`/`installer`/`diagnostics`/`test` stub-reject pending production rules in §8 + §9 + §16.
- [x] Per-kind dispatch via `boot_entry_kind_validate()` in `include/boot/boot_entry_kind.h` + `src/boot/uefi/boot_entry_kind.c`.
- [x] `boot_policy_invoke()` validates AFTER menu+SAFE, BEFORE counter decrement; reject -> `FALLBACK_NO_VIABLE`; accept -> stashed decoded drives `load_kernel()` SOLE-candidate path.
- [x] Per-kind fields documented in [`docs/boot/boot-entry-schema.md`](../../docs/boot/boot-entry-schema.md) section 4.1.
- [/] Widen `supported_kinds_mask` in `boot_policy_invoke()` to include CHAINLOAD / NETWORK / RESUME once their per-kind handlers land. Today only SPLIT/UKI/SAFE are admitted.
- [x] Commit: `"boot: per-entry-kind validators and load paths"`

**Test checkpoint:** Each kind has matching parser-fixture tests (valid + 2 rejection cases). Selecting a `kind: uki` entry sets `BOOT_FLAG_INVOKED_VIA_UKI` and reaches the kernel without disk-side cmdline override. Selecting a `kind: chainload` entry under Secure Boot without `trusted_chainload` is rejected. `kind: resume` digest mismatch falls through to the next priority entry per §3. SPLIT-with-validated-kernel-path: load_kernel opens that exact path or fails (no ambient fallback); SPLIT-with-rejected-payload: ladder demotes to `FALLBACK_NO_VIABLE`. Test on: QEMU WHPX + TCG (UKI), TCG (network sim, chainload), bare metal once §4 menu ships.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 32 boot_entry_kind suites, 0 failures

> **Notes:**
> - What shipped: `boot_entry_kind.{h,c}` (validators + decoded union), bootx64.c post-menu dispatch + authoritative SPLIT/SAFE load_kernel path, 32 kernel tests.
> - How it integrates: validate runs after menu + SAFE materialization, before counter decrement; reject -> FALLBACK_NO_VIABLE (counter skips, audit retains id); accept -> decoded stash + SOLE-candidate load.
> - Downstream effects: TODO-27 / TODO-25 / TODO-26 own the runtime paths; §13 ships the validator dispatch + decoded ABI they plug into.
> - Canonical doc: [`docs/boot/boot-entry-schema.md`](../../docs/boot/boot-entry-schema.md) section 4.
> - Scope boundary: §13 owns validators + SPLIT loader wiring + decoded ABI. supported_kinds_mask widening for CHAINLOAD/NETWORK/RESUME stays deferred until owner sections ship runtime paths.

> **Verified:** 2026-05-10 | commit `e5abf3a1` (impl) + post-review fixup | 7/10 items + 3 deferred to owner sections | build OK | tests 32/32 boot_entry_kind PASS
> **Quality reviewed:** 2026-05-10 | Codex 11x (design + adversarial 8x + consistency + perf + re-adversarial) | 1C+15H+8M+0L fixed, 0 open | scope: boot-code-quality + kernel-code-quality

---

## 14. Per-Entry Health-Gated Mark-Good

Bootloader marks try-down (decrement `tries_left`); userspace marks good ONLY after a configurable health check passes. Layered ABOVE [`TODO-21 §5`](TODO-21-ab-boot-rollback.md) (slot-level `mark_boot_successful`) and [`02-kernel-core/TODO-02 §10`](../02-kernel-core/TODO-02-kernel-configuration-policy.md) (boot acceptance ledger) -- those ship the foundations; this section adds entry-level gating + configurable health checks. Linux parity (systemd-bless-boot.service after `boot-complete.target`, plus `systemd-boot-check-no-failures.service` and Fedora greenboot's required-vs-wanted model). Without this gate, a boot that reaches the kernel but fails required services / safe-mode policy / resume validation can retire the previous-kernel entry and poison rollback.

- [x] Helper API `mark_entry_successful(entry_id)` writes `ImpossibleOS-MarkGood` (NV+BS+RT) with the state-bound triple from `CurBootCtr`; bootloader consume deletes the matching counter file pre-EBS.
- [x] Health-check registry in `boot_health_check.{h,c}`: required + wanted; each returns `OK` / `SOFT_FAIL` / `HARD_FAIL` / `SKIPPED`. Cap 12; duplicate-name reject; pre-scheduler registration.
- [x] Default required checks: `desktop_ready` (`SUBSYS_DESKTOP` ready), `no_boot_err` (klog ring scan for `boot`/`BOOT` exact + `boot_*` lowercase; reporter tags excluded), `no_panic` (trivially OK since `panic_screen` halts).
- [/] Default wanted checks: `x_mountable` shipped (`vfs_is_mounted('X')`); `network_reachable` + `no_service_crash_60s` registered as `SKIPPED` -- no NIC stack and no service supervisor yet. Follow-up items below.
- [x] `boot_health_check_run()` fires in `boot_phase3` after `boot_audit_publish`; PASS -> `mark_entry_successful`; required SOFT/HARD/SKIPPED -> INDETERMINATE. Single-shot. Writes JSONL to `X:\Boot\health.jsonl` (4 MiB rotation).
- [x] Per-entry `health_check_subset` envelope field (up to 8 printable-ASCII names, 23 chars each). Bootloader writes `ImpossibleOS-HealthSubset` (BS+RT); kernel intersects with registry. Absent -> full default set.
- [x] `bootcfg health <path>` host subcommand tabulates the last JSONL record (partial-line tolerant).
- [x] Documented in [`docs/boot/boot-health.md`](../../docs/boot/boot-health.md); schema field in [`docs/boot/boot-entry-schema.md`](../../docs/boot/boot-entry-schema.md).
- [x] Reserved-name guard in `NtSetSystemEnvironmentValueEx` blocks user-mode writes to the four kernel-owned vars (MarkGood, CurBootCtr, HealthSubset, BootSticky).
- [x] Attribute binding on every handoff read: MarkGood == NV+BS+RT; CurBootCtr + HealthSubset == BS+RT only.
- [x] Commit: `"boot: per-entry health-gated mark-good"`

Follow-ups (wanted checks depend on producers outside §14's scope):

- [/] Re-enable `network_reachable` wanted check once a NIC stack provides reachability state. Owner: TODO-25 (network) when basic L3 lands; check currently returns `SKIPPED`.
- [/] Re-enable `no_service_crash_60s` wanted check once a service supervisor lands. Owner: future service-supervisor TODO; check currently returns `SKIPPED`.
- [/] Wire TODO-21 §5 slot-level `mark_boot_successful()` call into `mark_entry_successful()` when TODO-21 §5 ships. Today it is a documented XREF hook in `boot_health_check.c`.

**Test checkpoint:** Boot reaches desktop, all required checks pass -> entry marked good (mark-good UEFI var written; next bootloader run deletes the counter file). Boot reaches desktop but kernel logged `LOG_ERROR` from a `boot_*` subsystem -> `no_boot_err` returns HARD_FAIL, gate aggregates INDETERMINATE, no mark-good var written, tries_left stays decremented for next-boot retry. `network_reachable` and `no_service_crash_60s` return SKIPPED today (wanted), never block the gate. Smoke (no bootentries.json on test ESP) confirms gate fires + reports PASS + correctly skips mark-good because no CurBootCtr is written by the bootloader on the fallback path. Test on: QEMU WHPX + TCG; bare metal once a real bootentries.json with health_check_subset is present.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 36 boot_health_check suites + 8 health_check_subset parser fixture suites, 0 failures (host validator 86/86, bootcfg 32/32)

> **Notes:**
> - What shipped: `boot_health_handoff.h` cross-boot ABI, `boot_health_check.{h,c}` kernel registry + gate, bootloader handoff helpers, `health_check_subset` schema + parser + validator, `bootcfg health`, reserved-name guard.
> - How it integrates: gate runs after `boot_audit_publish`; PASS writes state-bound MarkGood; next bootloader's consume deletes the counter file matching the triple. Fail-soft on every uefi_var / vfs call.
> - Downstream effects: closes the per-entry confirmation half of §5's counter protocol; unblocks §10 retirement once its updater hook lands. Codex 3-pass adoptions in commit message.
> - Canonical doc: [`docs/boot/boot-health.md`](../../docs/boot/boot-health.md).
> - Scope boundary: §14 owns entry-level gate + handoff + override transport. TODO-21 §5 (slot-level) + 02-kernel-core/TODO-02 §10 (acceptance ledger) remain unshipped foundations.

> **Verified:** 2026-05-15 | 11/11 items + 3 deferred follow-ups | build OK | smoke PASS (KVM 2.45s, gate fires PASS) | tests: 36 boot_health_check + 8 health_check_subset parser + 86 host validator + 32 bootcfg
> **Quality reviewed:** 2026-05-15 | Codex 8x (design + test-coverage + adversarial 2x + consistency + perf + re-adversarial 2x) | 1H+5M fixed, 0 open | scope: boot-code-quality + kernel-code-quality

---

## 15. OS-Visible Loader UEFI Variables

Publish the live entry list, capability bitmap, selected entry, and one-shot overrides as systemd-boot-compatible `LoaderXxx` UEFI variables under vendor UUID `4a67b082-0a4c-41cf-b6c7-440b29bb8c4f` so userspace tooling (and ported third-party tools like `bootctl`-equivalents) does not need to re-parse the on-disk store. Without this, dynamic entries (A/B-generated, recovery-generated, network-discovered, chainload) are invisible to userspace until the on-disk store is regenerated. Linux parity (`https://systemd.io/BOOT_LOADER_INTERFACE/`).

- [x] Publish the read-only `Loader*` vars after policy decision + counter decrement, before kernel handoff.
  - Since §21: 10 of them, volatile and set or cleared every boot, via `boot_loader_vars_publish()` in `loader_vars.c`, driven by `loader_vars_report()` in `bootx64.c`.
- [x] Consume + delete `LoaderEntryOneShot` + `LoaderConfigTimeoutOneShot`; EntryOneShot overlays `bootcurrent_entry_id`; TimeoutOneShot overrides the menu countdown with a pre-multiply overflow guard (3600s cap).
  - Since §21 the consumer is the pure `boot_loader_one_shot_consume()` in `loader_vars.c`: a value is honoured only when well formed and its delete succeeded.
- [x] `LoaderFeatures` uses canonical systemd-boot bit positions (bit 1, 3, 4 advertised). Impossible OS extensions in separate `ImpossibleOSLoaderFeaturesExt` var under our vendor GUID.
- [x] Every published-variable write goes through one adapter, `loader_vars_fw_set()` in `bootx64.c` (since §21); any refusal flips `boot_info.loader_vars_degraded = 1` and boot continues.
  - One-shot deletes are checked too, in `boot_loader_one_shot_consume()`; a refused one is not honoured and also marks the boot degraded.
- [x] Compatibility documented in [`docs/boot/loader-vars.md`](../../docs/boot/loader-vars.md): bit positions, one-shot protocol, Linux tool Just-Works statement, security model.
- [x] SMBIOS Type 1 UUID extraction via pure walker in [`include/boot/boot_smbios_parse.h`](../../include/boot/boot_smbios_parse.h); anchor + length + checksum validated; ConfigurationTable NULL-guarded; feeds `inputs->local_machine_id`.
- [x] `boot_info` v21 ABI bump (mirror + manifest + doc-coverage allowlist): adds `loader_vars_degraded` + `_loader_vars_pad[7]`.
- [x] Reserved-name guard in `NtSetSystemEnvironmentValueEx` extended to refuse user-mode writes to `ImpossibleOSLoaderFeaturesExt`.
- [x] Commit: `"boot: OS-visible loader UEFI variables"`

**Test checkpoint:** Post-boot, `efivar -l | grep Loader` (or equivalent) shows all read-only vars; values match `boot_info`. Setting `LoaderEntryOneShot` to a known entry id then rebooting causes that entry to boot once, then revert to the store default. `LoaderFeatures` advertises the bits implemented. NVRAM-quota-full harness sets `loader_vars_degraded` without aborting boot. Smoke (QEMU OVMF) confirms SMBIOS Type 1 walker handles the "not-specified sentinel" cleanly (QEMU UUID is all-zero); kernel-side SMBIOS UUID test exercises the pure walker + RFC 4122 formatter via 11 unit tests in `test_smbios_parse.c`. Test on: QEMU WHPX + TCG; bare metal once a real GPT ESP and SMBIOS UUID are present.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 11 smbios_parse suites added, 0 failures (live-UEFI integration tests deferred -- forbidden per test policy; validation via smoke + bare metal)

> **Notes:**
> - What shipped: `boot_smbios_parse.h` pure walker + UUID formatter; bootloader LoaderXxx publish + one-shot consume + SMBIOS extraction; `boot_info` v21; `docs/boot/loader-vars.md`; 11 unit tests; reserved-name guard extended.
> - How it integrates: `boot_policy_invoke()` consumes one-shot vars before bootcurrent resolution; SMBIOS UUID feeds `inputs->local_machine_id`; LoaderXxx published after decision + counter decrement, before EBS. Fail-soft on every SetVariable.
> - Downstream effects: `bootctl status` / `systemctl reboot --boot-loader-entry` work unmodified; machine_id-pinned entries become selectable. Codex design + adversarial adoptions in commit message.
> - Canonical doc: [`docs/boot/loader-vars.md`](../../docs/boot/loader-vars.md).
> - Scope boundary: §15 owns systemd-boot interface + SMBIOS UUID extraction. Kernel-side `Loader*` reads + sysinfo CLI integration belong to a future user-mode tool TODO.

> **Verified:** 2026-05-15 | 9/9 items | build OK | smoke PASS (KVM 2.42s, SMBIOS Type 1 walker fires, loader-vars publish path silent on success) | tests: 11 smbios_parse + existing suites unchanged
> **Quality reviewed:** 2026-05-15 | Codex 5x (design + adversarial 2x + consistency + perf + re-adversarial) | 1H+5M fixed, 0 open | scope: boot-code-quality + kernel-code-quality

---

## 16. Bootstrap and First-Install Entry Seeding

Who creates the FIRST default entry on a freshly-installed system? Who creates the recovery entry on the first boot after the recovery partition first appears? This is missing today: §11 covers list/add/remove and §9 generates from runtime state, but the bootstrap path is implicit. Idempotent offline seeding closes the gap.

- [x] `bootcfg.py emit-seed` produces byte-identical 3-entry store; ids deterministic from `(machine_id, kind, slot)` (wildcard machine_id -> `slot-a` / `slot-b` / `recovery`).
- [/] Release-image-build integration shipped via `scripts/release/build-image.sh`; dedicated installer release script pending -> XREF: [`../15-installer-release/TODO-01-release-artifacts.md`](../15-installer-release/TODO-01-release-artifacts.md).
- [/] First-boot self-seed (synthesize 3-entry default on missing store). Blocked: [`TODO-22 §1`](TODO-22-recovery-partition.md) + [`TODO-21 §2`](TODO-21-ab-boot-rollback.md).
- [x] Image build path: `scripts/release/build-image.sh` ships the 3-entry seed; `kernel` path matches staged location per staged-vs-seeded contract.
- [/] Widen `supported_kinds_mask` to include INSTALLER once distinct installer-image load path exists AND first-install seeding is complete. Blocked: no distinct load path today.
- [/] Root-aware `load_kernel()`: honor a SPLIT entry's `payload.root` so a `slot-b` entry loads from Slot B (needs a bootloader IXFS reader) -> XREF: [`TODO-21 §3`](TODO-21-ab-boot-rollback.md). Blocked: no IXFS reader yet.
- [/] Flip the seeded `slot-b` entry inactive->active in `bootcfg.py` `_seed_store()` once root-aware `load_kernel` lands so the menu's Slot B entry is selectable -> XREF: [`TODO-21 §3`](TODO-21-ab-boot-rollback.md).
- [x] Documented bootstrap order + ownership boundary in [`docs/boot/bootstrap.md`](../../docs/boot/bootstrap.md).
- [x] Commit: `"boot: bootstrap and first-install entry seeding"`

**Test checkpoint:** Run `bootcfg.py emit-seed` twice on the same mounted ESP; resulting `bootentries.json` is byte-identical (CRC `0x69430B34`, 1040 bytes, 3 entries). Validator accepts the seed. Mutation log of two consecutive `emit-seed` runs records identical `new_crc` on both with the second carrying `prior_crc == new_crc`. First-boot self-seed scenario cannot run today -- requires TODO-22 §1 producer. Test on: host (offline) + QEMU smoke (bootloader synthesizes 1-entry fallback on missing seed -- existing path).

> **Test runner:** `python3 tools/bootcfg/test_bootcfg.py` | 33 cases (added `emit-seed: 3-entry default`); host validator `python3 tools/boot-entry-validate/test_validate.py` | 86 cases, 0 failures

> **Notes:**
> - What shipped: 3-entry `_seed_store()` in `tools/bootcfg/bootcfg.py` (slot-a active + slot-b inactive + recovery active-but-filtered); new `docs/boot/bootstrap.md`.
> - How it integrates: release-image build already invokes `bootcfg.py emit-seed`; bootloader filters slot-b (NOT_ACTIVE) and recovery (KIND_UNAVAILABLE); ladder picks slot-a by sort_key.
> - Downstream effects: closes bootstrap gap, fixes latent §11 staged-path bug; TODO-21 §3 and TODO-22 §2 each flip-to-live with single-line follow-ups.
> - Canonical doc: [`docs/boot/bootstrap.md`](../../docs/boot/bootstrap.md).
> - Scope boundary: §16 owns host-side seed + bootstrap doc + image-build integration. Self-seed + INSTALLER widening + installer-release script are deferred to TODO-22 §1, TODO-21 §3, and 15-installer-release.

> **Verified:** 2026-05-19 | commit `17ee74f9` | 5/7 items | build OK | tests 33/33 bootcfg + 86/86 validator | lint clean
> **Quality reviewed:** 2026-05-19 | Codex 4x (design + adversarial + consistency + perf) | 1C+2H+3M fixed, 0 open | scope: N/A (host-side Python tool + docs)

---

## 17. Boot Entry Tests

Umbrella aggregation: per-section coverage shipped throughout §1-§16; this section maps that coverage and lists blocked / deferred live-boot scenarios. No new test files land here.

> **Note:** Several items below reference a "live-boot integration harness" -- a framework that boots an image in QEMU/WHPX/bare-metal with seeded `bootentries.json` + simulated UEFI variable state, asserts kernel/serial output, and tears down. This harness does not yet have a current owner; it is scope-gap-protocol Branch C work (new TODO file in `00-infrastructure` or successor) that landed when the QEMU smoke test gained richer assertion capability beyond `Boot complete in` + `C:\>`. Until that owner is filed, the items below citing the harness stay `[ ]`/`[/]` with the gap explicit.

- [x] Parser fixture tests (envelope + per-kind + invalid cases): covered by the host validator suite (§1), kernel parser suite (§2), and entry-kind suite (§13). See each section's Test runner line for current case counts.
- [x] QEMU boot menu timeout/default-selection scenario: `SMOKE_MENU=1` (timeout) and `SMOKE_MENU=keys` (QEMU monitor `sendkey`) in `scripts/test-smoke.sh`, both matrix legs
  - Timeout leg: the 2 s countdown auto-boots `smoke-entry`. Keys leg: Down moves to `smoke-alt`, the leg waits past the 4 s deadline (fails if the menu boots on its own), then Enter boots `smoke-alt` with reason HOTKEY.
  - Both legs assert the kernel received the entry: `boot_decision_validate()` now logs `entry=` and `selection=` at its success return, after every rule, so a rejected decision never reads as accepted.
  - Control: dropping `counting_down = 0` in `boot_menu_run()` fails the keys leg with "the menu booted on its own after a keypress".
- [x] Boot fixture whose policy default sits on a non-zero menu row: the §24 `SMOKE_MENU=default` then `SMOKE_MENU=saved` legs (`scripts/test-smoke.sh`, both in the smoke matrix).
  - The guest seeds the variable itself, so no host NVRAM writer (`virt-fw-vars`) is needed: the default leg presses `d` on row 1 (`smoke-alt`), which writes `LoaderEntryDefault`, and the saved leg reuses that variable store.
  - The saved leg requires `[BOOT] menu: 2 entries, selected="smoke-alt"` on open and the kernel to receive `smoke-alt` with reason STORE_DEFAULT, so "highlight the policy default" is now distinguished from "highlight the first row".
  - Control: pinning the menu's initial highlight to row 0 (`boot_menu_run()`) fails the saved leg with "boot_policy did not report selection_reason=1 (STORE_DEFAULT) for smoke-alt".
- [x] Firmware-vs-OS layer separation scenario: every valid-store smoke leg boots through OVMF's own untagged `Boot####` option and must fall through to the store's default.
  - `scripts/test-smoke.sh` requires `[BOOT] BootCurrent=0x....` and the policy line `selection_reason=... selected="<entry>" kind=0 rejected=0`, so a firmware hint that named an entry, or a recorded reject, fails the leg.
  - Control: making `read_boot_optionaldata_id()` report the option as tagged for `smoke-alt` fails the `store menu` leg with `BOOTNEXT_HINT selected="smoke-alt"`.
  - A tagged option naming an entry the store lacks (reject recorded, then fall-through) stays covered by the §4 unit tests: no fixture can tag OVMF's option without a host NVRAM writer.
- [/] A/B rollback entry-selection scenario (1/2/3-fail). Blocked: [`TODO-21 §1`](TODO-21-ab-boot-rollback.md), [`TODO-21 §3`](TODO-21-ab-boot-rollback.md), [`TODO-21 §4`](TODO-21-ab-boot-rollback.md).
  - Blocker owner: -> XREF: 01-boot-platform/TODO-21 §1 (item: "Choose storage: UEFI NVRAM ... vs GPT metadata partition" at line 5). The scenario needs real 1/2/3-fail counter state from the A/B producers.
- [/] Recovery auto-select + ALL_PATHS_BAD scenario. Blocked: [`TODO-22 §1`](TODO-22-recovery-partition.md), [`TODO-22 §2`](TODO-22-recovery-partition.md).
  - Blocker owner: -> XREF: 01-boot-platform/TODO-22 §1 (item: "GPT layout: EFI (64 MiB) + Slot A ..." at line 5). The scenario needs the recovery load path and the ALL_PATHS_BAD sentinel.
- [x] Per-entry health-gate scenario: the mark-good loop runs across two real boots (`store default` then `store saved` in the smoke matrix).
  - Every valid-store leg waits for the kernel's `health: entry marked good (id=<booted entry> ...)`, which lands just after the prompt, as a pass marker.
  - The saved leg carries the first boot's `\EFI\ImpossibleOS\counters` onto its rebuilt fixture disk, as a second boot of the same machine has them, and must log `MarkGood consumed (id=smoke-alt, deleted 1 counter file(s))`.
  - Before this the loader discarded that mark silently (no counter directory on the fresh disk). `policy_consume_mark_good_var()` and `health_read_mark_good()` now log every discard.
  - The saved leg also requires `smoke-alt+2-1 (first-boot bootstrap)`, proving the counter is gone. Review found the sweep counted a refused `Delete()` (`EFI_WARN_DELETE_FAILURE`) as deleted; it now counts only `EFI_SUCCESS` and logs refusals.
  - Controls: skipping the consume fails the saved leg; a refused delete fails it (counter left at `+1-2`); a gate that never passes fails the valid-store leg on its 30 s timeout.
- [/] Entry-kind dispatch: SPLIT/UKI/SAFE covered via §13; chainload -> [`T27 §1`](TODO-27-uefi-advanced.md), network -> [`T25 §7`](TODO-25-network-pxe-http-boot.md), resume -> [`T26 §3`](TODO-26-hibernation-resume-fast-startup-handoff.md).
- [x] Loader UEFI variable scenario: the kernel reads the published variables back after boot (`loader_vars_readback()` in `src/kernel/main/loader_vars_readback.c`, Phase 3) and checks them against `boot_info`.
  - `LoaderInfo` must be a non-empty NUL-terminated BS+RT string; `LoaderEntrySelected` must name `boot_info.selected_entry_id`, or be absent when nothing was selected. Pure classifiers tested in `test_uefi_boot.c`; the live check runs in the boot suite.
  - Every valid-store smoke leg requires `loader-vars: readback matches boot_info (selected=<entry>)`.
  - Its first run found the loader writing every string variable without its NUL, against the Boot Loader Interface ("formatted as a NUL-terminated UTF-16 string"). `loader_vars.c` now terminates them; `test_loader_vars.c` requires the NUL and still reads a writer's unterminated value.
  - Control: the unterminated encoder fails eight `loader-vars` tests, the live readback test and the valid-store smoke leg.
  - Review: firmware reporting success with an inflated size would have let the readback read past its buffer. `uefi_var_get()` (`src/kernel/uefi_vars.c`) now returns BUFFER_TOO_SMALL for it (`uefi_var_get_checked_status()`), as the syscall path already did; tested, control fails.
  - A one-shot set by a running OS and honoured on the next boot stays covered by `boot_loader_one_shot_consume()` tests: no guest writer exists yet.
- [x] Bootstrap idempotency scenario: covered by §16 `emit-seed: idempotent` + 3-entry default (`test_bootcfg.py`).
- [x] BlackBox-vs-NVRAM audit scenario: covered by §12 (`test_boot_audit.c` + bootcfg mutation-log).
- [x] Audit dual-write-failure dedup harness: `boot_audit_sequence_advance()` behind an I/O seam (`include/kernel/boot_audit.h`), driven by `test_boot_audit.c` over all 4,096 fault patterns
  - The harness found the property was NOT correct by construction: both writes failing after truncation emptied both files and restarted the counter at 1, re-publishing a used `boot_seq`. Fixed: the low file is written first, the high-water file only after it succeeds.
  - At `UINT32_MAX` the counter used to repeat its value; it is now exhausted and publishes nothing.
  - A damaged or unreadable file used to read as 0, so damage to the only current copy after a partial write re-published used numbers. Files are now VALID, STALE (this writer's own interrupted-write residue, read as 0) or UNKNOWN (read error, short read, garbage, above range), and UNKNOWN fails closed with nothing written.
  - Controls: the old order (shadow first, both always written) fails 6 assertions, 301 repeated or backward publications across the fault patterns; dropping the UNKNOWN check fails 5.
  - FAT32 (owner fix, same commit): a write reported every byte even when the directory entry's size update or first-cluster link failed. It now reports only the bytes the entry records, and fails a write whose new cluster cannot be linked, leaving that cluster allocated for fsck since the entry may have landed (`test_fat32.c`, control: 7 assertions fail on the old driver).
  - Durability: `audit_vfs_write()` returns success only after a full write and `vfs_flush()`, so the high-water copy is never truncated before its replacement is on the medium.
  - Residual: `vfs_open()` reports every failure as NULL, so a failed open reads as ABSENT -> XREF: 05-storage-filesystems/TODO-05 §3 (item: "`vfs_open()` status: report not-found apart from lookup, sharing, handle-table and device errors").
- [x] Audit JSONL rotation on FAT32 LFN: `test_fat32_audit_rotation_replaces_generation_across_clusters` (`src/kernel/test/test_fat32.c`, `TEST_CAT_FS`).
  - The blocker cleared: cross-cluster LFN removal shipped in 05-storage-filesystems/TODO-04 §18 and the replace-existing refusal is gone.
  - The test performs the rename `audit_append_file()` makes, `history.jsonl` over `history.1.jsonl`, four times, with the old generation's two-slot long name starting in the directory's first cluster and ending in its second.
  - Each round: the rotated name holds the current data, the replaced generation's chain is free, one bound name each; then a read-only `fat32_fsck` reports no errors. Control: skipping `fat32_free_chain()` in `fat32_rename_replace()` fails both.
  - Its review found read-only fsck rewrote the FAT mirror and reported clean, and that an unreadable FAT entry let fsck free a live tail; both fixed in FAT32 -> XREF: 05-storage-filesystems/TODO-04 §18 (item: "fsck never frees or repairs on a FAT it could not read").
  - The VFS wrapper itself (threshold check, then append at offset 0) is covered by code reading: no kernel test mounts a fixture volume behind a drive path.
- [/] `bootcfg.exe` round-trip scenario: offline subset shipped via §11+§16 (`test_bootcfg.py`); live-boot `bootcfg.exe` deferred per [`§11`](#11-boot-entry-editor-tooling).
- [x] Commit: `"test: boot entry test suite aggregation + status"` (commit `27c6d2c8`)
- [x] Boot `boot_policy.c`, `boot_history.c`, `boot_sticky.c` and `boot_entry_kind.c` through the real loader: every `SMOKE_VALID_STORE` leg asserts their serial lines (`scripts/test-smoke.sh`)
  - Why: the kernel suite compiles these files into a different image, so a loader-only failure stays invisible. The parser passed 6,000+ boot tests while the shipped loader read `parsed 0 entries (1 skipped)` (no `loader_self_relocate()`).
  - Every valid-store leg: entry-kind decoded `kernel="\boot\kernel.exe"`, policy picked `STORE_DEFAULT smoke-entry`, no `[WARN] boot_sticky:`/`boot_history:`, the loader's `ebs-success` record read back by the kernel, and the kernel's Phase-3 mark numbered next.
  - Fresh variable store: history seq=1, BootSticky absent, try counter bootstrapped to `smoke-entry+2-1`. The `repaired` leg (third boot on one variable store) must show seq >= 2 and the previous boot's `kernel-Phase3` record.
  - Control: `boot_history_read_seq()` returning 0 fails the `repaired` leg with "history seq=1 on a reused variable store". BootSticky's read-ok path needs a record carrying a trigger, which nothing creates yet (item below).
- [/] Booted-image BootSticky leg: a boot that sets a sticky trigger, then a second boot on the same variable store whose loader reads it (`boot_sticky_read_into_boot_info()`) and acts on it. Blocked: nothing creates the record yet.
  - Only the absent path runs in the booted image today. The kernel's one write (`audit_ack_consumed_triggers()`, `boot_audit.c`) rewrites a record that already carries a trigger, and §12's boundary names TODO-23 as the producer.
  - Blocker owner: -> XREF: 01-boot-platform/TODO-23 §4 (item: "Set NVRAM flag: `watchdog_triggered = 1`" at line 134), itself parked on that file's §1 watchdog ISR. Control once unblocked: skipping the read fails the second boot.

**Test checkpoint:** Per-section unit suites already register under `TEST_CAT_BOOT` and run with `make test-boot` (or `bash scripts/test.sh SUITE=boot`); the §17 commit is pure status reconciliation -- no new test code lands here. Live-boot scenarios depend on harness infrastructure that is not in this TODO's scope; XREF'd to their owner sections above. Bare-metal pass requires the user's laptop -- not gateable from WSL. Test on: host (`python3 tools/bootcfg/test_bootcfg.py` + `python3 tools/boot-entry-validate/test_validate.py`) + QEMU TCG (`make test-boot`).

> **Test runner:** N/A (aggregation section -- no new test file) | validation: per-section runners; aggregate via `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)

> **Notes:**
> - What shipped: status reconciliation only -- every checklist item maps to per-section coverage, a blocker XREF, or a live-boot harness gap.
> - Structure / consumers: §17 is the umbrella; §1-§16 carry the actual cases (parser / policy / decision / menu / entry-kind / audit / health / smbios / bootcfg). Per-section Test runner lines have the current counts. `make test-boot` aggregates them.
> - Downstream effects: closes the umbrella. Remaining gaps are explicit per-item with concrete blocker XREFs; the live-boot integration harness has no current owner -- see the in-section Note below.
> - Canonical doc: this section + per-section Test runner lines.
> - Scope boundary: §17 owns aggregation + status. New tests for covered behavior belong in their owner sections; dual-write testability + FAT32 LFN belong elsewhere.

> **Verified:** 2026-11-09 | commits `888f520e6`, `6cc12836b`, `6b7b3b609` | 13/18 items + 5 parked on named owners (TODO-21 §1, TODO-22 §1, TODO-27 §1 / TODO-25 §7 / TODO-26 §3, TODO-12 §3 via §11, TODO-23 §4) | build OK | full suite 36807 kernel + 17 user PASS | smoke matrix 18/18 | Codex adversarial + re-adversarial approve
> **Notes:** Reopened 2026-11-07, was Deferred: [L] Audit dual-write-failure dedup harness. The stamp named this section's own item, and that item recorded no external blocker.
> **Quality reviewed:** 2026-05-19 | Codex 8x (adversarial 6x + consistency + perf) | 0C+1H+9M fixed, 0 open | scope: N/A (docs-only aggregation section)

---

## 18. Boot Loader Interface (systemd BLI) Parity Fixes

The shipped §15 (loader UEFI vars) + §6 (menu render) publish systemd Boot Loader Interface variables but diverge from the BLI/BLS contract in three ways a `bootctl` / `systemd-analyze` / `systemctl reboot --boot-loader-menu=` consumer would hit (Codex gap-audit 2026-06-13). This section corrects them without re-opening the shipped sections.

- [x] BLS display ordering by `sort_key` then `machine_id` (`id` tiebreak), shared by the menu + `LoaderEntries`. Helpers `boot_entry_bls_less`/`boot_entries_bls_sort` in `boot_entries_parser.c`; unit-tested in `test_boot_entry_parser.c`.
- [x] `LoaderConfigTimeoutOneShot`: 0 = menu no timeout, 1..60 honored, 61..3600 clamped to 60 + `[WARN]` (watchdog-window divergence, not a silent drop); a present one-shot forces the menu. Kiosk stays on per-entry `timeout_override`.
- [x] Distinct `LoaderTimeInitUSec`/`ExecUSec`, both counted from reset as the BLI defines them: Init is the TSC at loader entry, Exec the TSC just before the final memory map. `LoaderTimeMenuUSec` not added (not in upstream BLI).
  - Shipped 2026-11-08 with the §21 review: `loader_vars_report(..., LOADER_VARS_TIMING)` in `bootx64.c`; unit test `test_loader_vars_timing_call`; the smoke check requires both set on a calibrated TSC.
- [x] Full BLS order + `LoaderFeatures` bit 8: `boot_entry_bls_less()` in `boot_entries_parser.c` puts bad entries last, keyed entries first, then `sort_key`, `machine_id`, `version` descending, then the id.
  - Bad = a TRIES_EXHAUSTED record, marked by `boot_policy_bad_mask()` (`boot_policy.c`) and passed to the menu sort and to `LoaderEntries` (`entry_bad` in `loader_vars.h`).
  - Optional `version` field, compared by UAPI.10 (`boot_version_compare()`); `_` and `+` allowed, other characters and escapes refused by the parser and by `tools/boot-entry-validate/validate.py` (raw scan of envelope values).
  - Corrected to the spec: an entry without a `sort_key` now sorts after keyed ones, and the id tiebreak is decreasing version order, as in systemd-boot. `LOADER_FEATURE_SORT_KEY` (bit 8) is advertised; `LoaderFeatures` is now `0x11E`.
  - Tests: UAPI.10 rule table, version order, bad-last (with a no-mask control), parser version cases, `boot_policy_bad_mask`, `LoaderEntries` bad-last, validator escape cases. Controls: dropping the bad rule or the character check fails them.
- [x] LoaderTime publish moved after TSC calibration: `boot_loader_vars_publish_timing()` in `loader_vars.c` runs post-`load_kernel`, pre-EBS, setting or clearing only the two timing variables.
  - The main publication still clears both (the TSC is uncalibrated there), so a boot whose late call fails reports them absent, never stale.
- [x] Commit: `"boot: TODO-07 §18 -- systemd BLI parity (BLS display order, one-shot timeout, distinct loader timestamps)"`

**Test checkpoint:** against a multi-entry store, `LoaderEntries` matches the on-screen menu order (bad-counted last, then `sort_key`); `LoaderConfigTimeoutOneShot=0` shows the menu with no countdown (or warns+clamps per the chosen policy); `systemd-analyze` reports non-zero loader time. Extend `test_boot_entries` with out-of-array `sort_key` + bad-counter ordering assertions. Test on: QEMU TCG (deterministic); bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 7 BLS-order suites (sort_key/id, machine_id, UAPI.10 compare, version order, bad last, version field, bad mask) + the LoaderEntries bad-last case, 0 failures; host: `python3 tools/boot-entry-validate/test_validate.py`

> **Notes:**
> - **What shipped:** systemd-BLI parity: the full BLS display order (bad last, `sort_key`, `machine_id`, `version` by UAPI.10, id) for the menu + `LoaderEntries`, with `LoaderFeatures` bit 8; `LoaderConfigTimeoutOneShot` (0 = menu no-timeout, 1..60 honored, >60 clamped to 60 + WARN, forces the menu even on quiet paths); distinct calibrated `LoaderTime` values.
> - **How it integrates:** the sort applies to `cand_idx` after `boot_policy_menu_collect` + inside the `LoaderEntries` publisher (`lv_entries` in `loader_vars.c` since §21); a present one-shot forces `show_menu` + bypasses `hide_when_alone`, and value 0 sets `g_menu_no_autoboot`.
> - **Downstream:** `bootctl` consumers now see consistent menu/LoaderEntries order; `systemd-analyze` loader time ships through the late timing call (2026-11-08). Design adoptions in the commit message.
> - **Canonical doc:** `docs/boot/boot-entry-schema.md`; systemd Boot Loader Interface spec for var semantics.
> - **Scope boundary:** none open. The full BLS order and bit 8 shipped 2026-11-09; the LoaderTime relocation 2026-11-08. The default entry is still chosen by the policy ladder, not by display order.

> **Verified:** 2026-11-09 | commit `4cd7fd19e` | 6/6 items | build OK | full suite 36834 kernel + 17 user PASS | smoke matrix 18/18 | validator 156/156, bootcfg 50/50
> **Notes:** Reopened 2026-11-07, was Deferred: [H] distinct `LoaderTimeInitUSec`/`ExecUSec` blocked -- `timing.tsc_freq` is 0 at the pre-`load_kernel` publish point. The stamp named this section's own item, and that item recorded no external blocker.
> **Notes:** Reopened 2026-11-07, was Deferred: [M] full BLS order (bad-counted-last + `version` sub-key) + `LoaderFeatures` bit 8 not advertised. The stamp named this section's own item, and that item recorded no external blocker.
> **Quality reviewed:** 2026-11-09 | Codex 8x (adversarial 4x, re-adversarial, post-ship adversarial, consistency, perf) | 1H+2M+1L fixed, 0 open | scope: boot-code-quality

---

## 19. Repeated Top-Level Keys Defeat the Entries-Array Bound

> **Spawned-by:** root
> **User impact:** a boot-entry store on the ESP can overflow the parser's fixed 64-entry output array by ~36 KB. The store is disk-sourced config the bootloader parses before ExitBootServices, so anything that can write the ESP can corrupt bootloader memory during boot.

Found while reviewing `02-kernel-core/TODO-33` §16, which moved that parser's TEST fixtures onto a scratch allocator. The parser itself was never in that section's scope, so the defect is filed here with its owner rather than fixed there. -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §16 (item: "Decided: it DID block the charter, and is routed through the HHDM")

**The mechanism, confirmed at file:line.** `boot_entries_parse()` walks the root object with `while (1)` over key/value pairs and an `else if` chain, and tracks NO set of already-seen keys (`src/boot/uefi/boot_entries_parser.c:1101`), so a second top-level `"entries"` key is structurally accepted. Inside that branch `idx` and `all_ids_count` are BLOCK-SCOPED (`boot_entries_parser.c:1164`, `1172`) and therefore reset to 0 on the second occurrence, while `out->entry_count` is never reset. The only cap check is against the per-occurrence `idx` (`boot_entries_parser.c:1195`), but the write is `out->entries[out->entry_count]` (`boot_entries_parser.c:1227`). So `{"schema_version":1,"crc32":"0x0","entries":[64 valid],"entries":[64 valid]}` passes every check and writes indices 64-127 into `boot_entry_envelope_t entries[BOOT_ENTRIES_MAX_ENTRIES]` (`include/boot/boot_entries_parser.h:117`, cap 64 at `include/boot/boot_entries.h:75`).

- [x] Reject a repeated key outright at BOTH object levels the parser walks (root and entry object) -- `key_seen_before()` rescans the current object's already-walked prefix; new code `BOOT_ENTRIES_REJECT_DUPLICATE_KEY`.
  - `key_seen_before()` in `src/boot/uefi/boot_entries_parser.c` ships a prefix RESCAN, not a seen-key table. A fixed table must hard-fail on overflow, which would turn otherwise-skipped forward-compat extension keys into a whole-store rejection at boot AND force the host validator to mirror the same ceiling. The rescan needs no per-object scratch on the firmware stack and imposes no key-count limit.
  - `all_ids_count` resetting meant cross-occurrence duplicate IDs went undetected too. The repeated key is now caught first, which closes both holes; `schema_version` and `crc32` were equally re-assignable and fall under the same rule.
  - The rescan FAILS CLOSED. All three review legs independently found that the first version was bypassable: the value skipper charges its depth budget from where it starts, and the root rescan skips the whole `entries` value (paying for the array and entry object that the main parser descends structurally), so a store the parser ACCEPTED could exhaust the budget -- and "could not finish" was read as "no duplicate", disabling the check on attacker-chosen input. Fixed by giving the rescan exactly the two extra levels of difference and making scan failure a distinct answer the callers reject on.
- [x] Key names must be spelled LITERALLY -- `key_is_literal()` rejects any key token carrying a backslash with `BOOT_ENTRIES_REJECT_ESCAPED_KEY`. Raised by the design review.
  - `key_is_literal()` in `src/boot/uefi/boot_entries_parser.c` closes this. `bytes_eq` compares raw bytes, so `"id"` was a distinct (unknown) key to the firmware while the host's `json.loads` decoded it into the real one. Codification rather than a new restriction: the CRC-header locate has always compared the key's RAW bytes (`find_crc32_key()` when §19 shipped, `find_crc_field()`'s structural walk since §20), so an escaped spelling was never readable there.
- [x] Bound the WRITE as defence in depth: `out->entries[out->entry_count]` is gated on the 64-entry cap, so no future refactor of the key guard can reopen the overflow.
- [x] Unit tests in `src/kernel/test/test_boot_entry_parser.c` (`TEST_CAT_BOOT`): 20 added, 31 -> 51 in the file, including four ACCEPT controls.
  - Two `entries` arrays; duplicate id split across two `entries` arrays; repeated `schema_version`, `crc32` and unknown root key; repeated entry-object key; escaped root and entry key. Controls: 30 distinct unknown keys still parse, and prefix-sharing keys (`sort` vs `sort_key`) are not confused.
  - Every fixture asserts the REJECT and uses ONE-entry arrays, so no case performs the overflowing parse that would corrupt the runner executing it.
  - The two guards ordinary input CANNOT reach (the fail-closed rescan result and the bounded append `entries_retain()`, both in `src/boot/uefi/boot_entries_parser.c`) are tested by calling the helpers directly, because a backstop no test can fail is one that can be deleted unnoticed. Mutation-checked: making the rescan fail OPEN, and an off-by-one in the skip budget, each fail exactly their own test and nothing else.
  - Depth regressions at extension depths 6, 7 and 8 cover the rescan bypass, with a control proving a deep store carrying ONE `entries` array is still accepted. Verified as probes: reverting the budget makes 7 and 8 fail (`DEPTH_LIMIT` instead of `DUPLICATE_KEY`) while 6 passes either way, which is what the depth arithmetic predicts.
- [x] Host-side validator parity via `strict_loads()` in `tools/boot-entry-validate/validate.py`; 18 host cases added, validator 86 -> 99 and bootcfg 33 -> 38.
  - Routed through it: the validator CLI, `bootcfg.py` store reads and `cmd_add`, which the design review caught calling bare `json.loads` and so bypassing the validator's rules entirely.
  - Those bootcfg paths now have their own rejection tests: reverting either call to `json.loads` fails three of them. The atomic-write failure matrix uses DISTINCT old and new bytes, so it can tell preservation from replacement, and covers the pre-replace, post-replace and unsupported-flush cases separately.
  - `bootcfg` also serialized a literal non-ASCII key back out ESCAPED (`ensure_ascii=True`) and validated the dict rather than the bytes, so it could write a store the firmware rejects and it could not itself reload. Now writes UTF-8 literally and re-parses the exact bytes before persisting.
  - Literal-spelling is checked by a left-to-right `_key_spans()` scanner, not a regex: a regex alternation can begin matching at an escaped quote INSIDE a value, so a value like `"\"a\\b\":x"` was one edit away from reading as a key. It passed by luck, not structure.
- [x] Close the host-vs-firmware DEPTH divergence the parity claim exposed -- `validate_value_depths()` mirrors the firmware's context-relative 8-container budget for every value the firmware skips wholesale.
  - Pre-existing, and found by round 2 review: a nine-deep array under an unknown extension key validated on the host and then booted to invalid-store fallback. Leaving it would have made this section's own host/firmware agreement claim false.
  - The budget RESTARTS at each skipped value (root unknown values, entry unknown values, `payload`), so it is 8 levels from that value, not 8 from the root. Pinned by a depth-8-accept / depth-9-reject pair on BOTH sides, so the two implementations cannot drift apart silently.
  - Both host writers now serialize keys literally and re-parse their own output before persisting: `bootcfg`'s `_canonical_dumps` and the validator's `--emit-crc`, which had the identical escaping defect.
  - Directory-flush errors are no longer swallowed wholesale: a platform limitation (Windows cannot fsync a directory fd) is skipped, a genuine EIO propagates. A blanket suppression reported a durable write whose directory entry may never have reached disk, which is the exact guarantee the protocol exists to make. Covered by errno-injection at both flush points.
  - The durability protocol now has ONE implementation (`atomic_write_bytes`), which both writers call. The copies had already diverged in the part that matters: the validator's used a predictable `<path>.tmp` that another process or a symlink could own, so a concurrent run or a planted link defeated the very preservation guarantee it was added for.
  - `--emit-crc` also did its whole pipeline against the DESTINATION file, so a rejected store was left holding an invalid placeholder and a non-UTF-8-encodable one (a lone surrogate, which `json.loads` accepts) was truncated to zero bytes by `write_text` before the encoder raised. It now serializes, encodes, re-parses, validates and stamps the CRC entirely in memory, then replaces the file atomically. Regression asserts a FAILED run leaves the original byte-identical; against the old ordering it measures 1321 -> 0 bytes.
- [x] Make the rejection stick in every CONSUMER, not just the parser -- a rejected store now yields no menu candidates and publishes no loader entries.
  - Found by the review: `boot_policy_decide()` short-circuits correctly, but `boot_policy_menu_collect()` ignored `reject_code` and iterated `entry_count`, so a store rejected for a repeated `entries` key still listed the entries parsed before the repeat -- and picking one through the F11 menu made it the boot selection, bypassing the active/machine_id filtering the ladder would have applied.
  - Because the ladder short-circuits WITHOUT filtering, `decision->rejected[]` is empty on that path, so the collector's hard-hide filters had nothing to match on and every retained entry showed. Guarded at `boot_policy.c` plus the `LoaderEntries` and `LoaderEntryDefault` publishers in `bootx64.c` (since §21 the former is `lv_entries` in `loader_vars.c`, and `LoaderEntryDefault` is no longer written).
  - The loader-variable guards CLEAR rather than skip, across all three of `LoaderEntries`, `LoaderEntryDefault` and `LoaderEntrySelected`. They are `EFI_VARIABLE_NON_VOLATILE`, so not writing them leaves the PREVIOUS boot's list published while this boot rejects the store and offers nothing; stale is worse than absent, since a BLI consumer would list entries the loader refuses to boot, or read the previous boot's id as this boot's selection. Both caught by re-adversarial review of this very fix -- two consecutive rounds, each on the round before it.
  - Regression + accept-control in `test_boot_policy.c`; mutation-checked (deleting the guard exposes 2 candidates where 0 are expected). The loader-variable clearing had no kernel test surface then (the clear called `SetVariable` directly) and was validated by serial log; §21 moved it into `loader_vars.c`, where `test_loader_vars.c` covers it.
- [x] Commit: `"boot: reject repeated top-level keys in the boot entry store"`

**Test checkpoint:** full `scripts/test.sh` green with `SUITE=boot` green in its own right and the new reject cases failing before the fix and passing after. `scripts/test-smoke-matrix.sh` 4/4 legs, since this is pre-EBS boot-path code. Platforms: QEMU KVM + TCG.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 51 boot-entries suites (20 added), 0 failures; host side `python3 tools/boot-entry-validate/test_validate.py` 99 cases and `python3 tools/bootcfg/test_bootcfg.py` 38 cases, 0 failures

> **Notes:**
> - Shipped: repeated key names rejected at both parsed object levels plus literal-spelling enforcement, closing a ~36 KB out-of-bounds write reachable from any writer of the ESP.
> - `key_seen_before()` rescans the current object's walked prefix instead of accumulating a seen-key table, so the fix adds no firmware-stack scratch and no ceiling on distinct keys; a rescan that cannot complete is a REJECT, never a silent pass.
> - The write at `out->entries[out->entry_count]` now carries its own 64-entry bound; it is unreachable through the parser by construction and exists so a later refactor of the key guard cannot reopen the overflow.
> - Host and firmware were brought into agreement on key rules AND on the value-depth budget, across every reader and both writers; review round 2 found the writers escaping keys and the host missing the depth cap.
> - Canonical doc: [docs/boot/boot-entry-schema.md](../../docs/boot/boot-entry-schema.md) section 3 records both key rules and the deliberate payload-level asymmetry.
> - Scope boundary: per-entry payload contents stay the host validator's and the policy filter's business; the firmware parser still skips payload objects wholesale.
> **Verified:** 2026-09-05 | commit `a4f5f4982` + review fixes | 9/9 items | build OK | 34026 kernel + 17 user tests PASS | 99 validator + 38 bootcfg host cases PASS | lint 0 errors | smoke matrix 4/4 legs (KVM/TCG x 1/2 CPU)
> **Accepted:** [M] The CRC header was located by scanning for the literal bytes `"crc32"` anywhere, so a store carrying that text in a string VALUE before the real header was rejected by firmware while the host accepted it (reason: distinct capability -- locator plus producer patch offset on both sides, with its own test matrix). CLOSED by §20 -> XREF: 01-boot-platform/TODO-07-boot-entry-store-menu-policy.md §20 (item: "`find_crc_field()` (`src/boot/uefi/boot_entries_parser.c`) is a bounded STRUCTURAL walk of the ROOT object")
> **Accepted:** [M] `LoaderDevicePartUUID` is left stale when the partition identity is unknown; that path is not reached by store rejection, so §19's fix does not cover it (reason: different axis, owned by the loader-variable publication surface) -> XREF: 01-boot-platform/TODO-07-boot-entry-store-menu-policy.md §21 (item: "Clear `LoaderDevicePartUUID` when a valid GPT identity is unavailable OR when formatting it fails, and require GPT partition style before publishing it at all")
> **Quality reviewed:** 2026-09-05 | Codex 6x (adversarial, consistency, perf, re-adversarial x3) + boot-quality-auditor + kernel-quality-auditor | 1H+3M+2L fixed, 0 open | scope: boot-code-quality + kernel-code-quality

---

## 20. `crc32` Header Located by String Match, Not Structurally

> **Spawned-by:** §19 (review)
> **User impact:** a boot-entry store whose text contains the characters `crc32` inside a STRING VALUE before the real header is rejected by the firmware while the host validator accepts it. The user's configured boot selection is silently discarded and the machine boots the in-firmware fallback entry instead, with nothing on screen explaining why.

Found by the §19 consistency review, which reproduced it with a 290-byte CRC-correct fixture. Pre-existing, and NOT introduced by §19 -- but §19 cites this function as the reason key names must be spelled literally, so the same locator being value-blind is the other half of that story. -> XREF: `01-boot-platform/TODO-07-boot-entry-store-menu-policy.md` §19 (item: "Key names must be spelled LITERALLY").

**The mechanism, confirmed at file:line (past tense since §20 shipped).** `find_crc32_key()` (`src/boot/uefi/boot_entries_parser.c:135` at the time) was a linear scan for the seven bytes `"crc32"` anywhere in the file, with no notion of object depth or key position. A store beginning `{"note":"crc32","schema_version":1,...}` matched inside the NOTE'S VALUE first; `find_crc_field()` then expected a colon after it, found a comma, and returned 0, so `boot_entries_parse()` rejected with `BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD` before any key was parsed. The host validator locates the field with the `_CRC_FIELD_RE` regex (`tools/boot-entry-validate/validate.py`), which has the same value-blindness but different tie-breaking, so the two sides disagree about which bytes are the CRC. A nested `crc32` member inside `payload` is the same shape one level down.

- [x] `find_crc_field()` (`src/boot/uefi/boot_entries_parser.c`) is a bounded STRUCTURAL walk of the ROOT object, using the file's own lexer plus `skip_value_depth()`; only a key at depth 1 can match, and `find_crc32_key()` is deleted.
  - Returns a tri-state `crc_loc_t` so reject codes stay honest: `CRC_LOC_MALFORMED` (root grammar broken before the header) maps to `BOOT_ENTRIES_REJECT_JSON_PARSE`, and absent-or-misshaped maps to `BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD` as before.
  - Budget is `BOOT_ENTRIES_MAX_SCAN_DEPTH`, not the parse budget: the walk skips the root `entries` value from two container levels further out than the authoritative walk ever skips from, so anything the parser accepts stays locatable. The root object is walked with `lex_next` and costs no budget.
  - The "8 hex digits at a known offset" contract is intact and tightened to EXACTLY `0x` + 8 hex digits with a lowercase `x`, which is what the host's whole-store check already required. The firmware previously accepted `0X` and an over-long value, so the two sides disagreed on those.
- [x] Host mirror in `tools/boot-entry-validate/validate.py`: `_root_crc32_span()` plus `_next_token()`, `_scan_string()` and `_skip_value()` replace the `_CRC_FIELD_RE` search.
  - The token rules are the FIRMWARE's, not Python's: strict RFC 8259 strings, exact literals, `lex_number`-shaped numbers, and a grammar-validating container stack rather than a brace count, so the host cannot locate a header in a store the firmware calls malformed.
- [x] Paired regressions on both sides, each checked against an INDEPENDENT offset oracle rather than against a successful parse.
  - 11 kernel cases under `TEST_CAT_BOOT`, and 38 in-process host locator and control checks plus 4 end-to-end validator cases.
  - Both suites carry an IN-SUITE control that runs the old value-blind locator (`old_string_match_locate()` kernel-side, the old regex host-side) and states, per fixture, whether it broke or agreed.
  - The controls earned their place three times over: the first draft of the decoy fixtures did NOT reproduce the defect (the firmware fixture spelled the decoy `"crc32 lives below"`, whose bytes never contain the 7-byte needle, and both host decoys sat AFTER the header), and the escaped-quote decoy turned out to break neither locator because a backslash sits where the old needle wanted a quote. A fixture that does not reproduce the defect looks identical to one that does.
  - Each fixture is therefore classified by MEASUREMENT, not assumption, and the two sides differ: the old FIRMWARE scan failed the string-value decoy outright while the old HOST regex handled it (the regex required a colon and `"0x` after the key), so that shape is a break case kernel-side and an agreement case host-side. Only the nested, extension-object and array-contained shapes break both.
- [x] `docs/boot/boot-entry-schema.md` states the structural rule as the algorithm and carries the reference locator; the value-blind regex is documented as a defect rather than a recipe.
- [x] Commit: `"boot: locate the crc32 header structurally rather than by string match"`

**Test checkpoint:** `SUITE=boot` green with the two new fixtures failing before the fix and passing after; host `python3 tools/boot-entry-validate/test_validate.py` green. `scripts/test-smoke-matrix.sh` 4/4 legs, since this is pre-EBS boot-path code.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 6317 tests, 0 failures; host `python3 tools/boot-entry-validate/test_validate.py` 141 cases, `bash tools/boot-entries-parser-tests/run.sh` 6 cases

> **Notes:**
> - Shipped: `find_crc_field()` rewritten as a tri-state structural walk of the root object on the firmware side, and `_root_crc32_span()` plus a firmware-faithful tokenizer replacing `_CRC_FIELD_RE` on the host side.
> - Integrates at `boot_entries_parse()`, which now separates a malformed root (`JSON_PARSE`) from an absent or misshaped header (`BAD_CRC32_FIELD`); the CRC still gates the structure, the locate merely finds the span first.
> - Downstream: the firmware value-shape rule tightened to exactly `0x` + 8 hex digits with a lowercase `x`, closing two cases (`0X`, over-long) where firmware accepted what the host rejected.
> - Canonical doc: [docs/boot/boot-entry-schema.md](../../docs/boot/boot-entry-schema.md) section 5, which now states the structural rule as the algorithm and names the text search as the defect.
> - Scope boundary: the 16 KiB store size cap has host over-cap coverage (`test_validate.py` `_oversize_file`, a 17 KiB store) but no coverage at the boundary on either side and none at all firmware-side; filed as §22, and this section does not touch it.

> **Verified:** 2026-09-05 | commit `598bc0749` | 5/5 items | build OK | 34089 kernel + 17 user tests, 6317 boot-suite, 141 host validator cases, 6 standalone parser cases, smoke matrix 4/4, lint 0 errors
> **Accepted:** [M] Every store-rejection path logs only to serial, so a rejected store boots the fallback with nothing on screen; the error-screen facility exists but is never invoked (reason: owned by the no-silent-failure TODO, whose Goal sentence covers it) -> XREF: 01-boot-platform/TODO-03-bootloader-error-recovery.md §22 (item: "Render the existing error screen (or a non-fatal variant of it) when the store is rejected, carrying the `boot_entries_reject_code_t` and the `selection_reason`, so the user sees WHICH failure happened rather than a fallback boot" at line 793)
> **Deferred:** [M] A corrupt store has no shadow copy, no A/B pair and no regeneration route, only one hardcoded fallback entry (reason: needs a model decision this section cannot make) -> XREF: 01-boot-platform/TODO-07-boot-entry-store-menu-policy.md §23 (item: "Decide the model and record it: a shadow copy of the last-known-good store, an A/B pair for the store itself, or regeneration from a source of truth")
> **Deferred:** [L] The 16 KiB size cap has no boundary coverage on either side and no firmware coverage at all (reason: needs a scratch-backed fixture the 2048-byte static cannot provide) -> XREF: 01-boot-platform/TODO-07-boot-entry-store-menu-policy.md §22 (item: "Kernel cases at 16383, 16384 and 16385 bytes, with the `crc32` header near EOF so the locate is exercised at the boundary too")
> **Quality reviewed:** 2026-09-05 | Codex 8x (design, test-coverage, adversarial x3, consistency x2, perf x2, re-adversarial) | 0H+7M+2L fixed, 0 open | scope: boot-code-quality

---

## 21. Published Loader Variables Need an Explicit Set-or-Clear Outcome

> **Spawned-by:** §19 (review)
> **User impact:** a BLI consumer (`bootctl`, or anything reading the systemd loader interface) is told the PREVIOUS boot's partition identity belongs to THIS boot. After a boot that publishes a partition UUID, a later boot whose partition identity is unknown leaves `LoaderDevicePartUUID` in place, so tooling misidentifies which disk the running system booted from.

Found by the §19 re-adversarial rounds while auditing exactly this class. §19 fixed the three ENTRY variables (`LoaderEntries`, `LoaderEntryDefault`, `LoaderEntrySelected`), which went stale specifically on a REJECTED store. This section owns the general rule for the rest. -> XREF: `01-boot-platform/TODO-07-boot-entry-store-menu-policy.md` §15 (OS-visible loader UEFI variables -- shipped and stamped; this is the follow-up its publication paths need).

**The mechanism, confirmed at file:line.** The loader's variable writer (since §21 replaced by `boot_loader_vars_publish()`) writes with `EFI_VARIABLE_NON_VOLATILE` (`src/boot/uefi/bootx64.c:6242`), so every published value survives the reboot. Several publishers return early on the "nothing to publish" path instead of clearing, which is not the same thing: absent means unknown, but stale means wrong. `LoaderDevicePartUUID` (`bootx64.c:6291`) returns without clearing when the partition identity is unavailable, and its formatter-failure path has the same omission. Store rejection does NOT reach this path, which is why §19's fix did not cover it.

- [x] Every variable the loader owns is set or cleared on every boot, and is volatile, so no path can leave a previous boot's value standing.
  - Root cause, found by the design review: the old variable writer in `bootx64.c` wrote `EFI_VARIABLE_NON_VOLATILE`, so a value outlived its boot. systemd-boot writes these names with no `NON_VOLATILE` flag (`src/boot/export-vars.c`), and a persistent copy also leaked into a later boot by another loader, which only fills them when absent.
  - New pure publisher `boot_loader_vars_publish()` in `src/boot/uefi/loader_vars.c` (`include/boot/loader_vars.h`), shared by the loader and the kernel tests; `bootx64.c` keeps only the `SetVariable` adapter `loader_vars_fw_set()` and the inputs.
  - Each write deletes first (attributes 0, size 0), which also migrates a persistent copy an older loader left: UEFI refuses to rewrite a variable with different attributes.
  - The 11 owned variables and their clear conditions are tabled in `docs/boot/loader-vars.md`: firmware info clears with no system table or an empty or over-256-character vendor (never truncated); both timings clear when unmeasured or 0 (systemd rejects a zero ExecUSec; the main publication clears both and the late timing call sets them once the TSC is calibrated, see section 18); `LoaderEntries` clears when nothing is visible; `ImpossibleOSLoaderFeaturesExt` follows the same rules.
  - `LoaderConfigTimeout` and `LoaderEntryDefault` are no longer written: they are persistent settings owned by `bootctl` or another loader, which this loader does not honour (bits 0 and 2 not advertised). Writing them each boot overwrote, and on a rejected store deleted, someone else's setting. Decision and reversal cost in `todo/answers.md`; §24's `d` key becomes their only writer.
- [x] Every path that leaves the loader reports or clears, through `loader_vars_report()` in `bootx64.c`.
  - The normal path, and each of the three policy allocation failures (no store facts, so the store-derived variables clear).
  - Before a chainload, `boot_loader_vars_clear_all()` clears every owned variable, because the chained loader reports its own boot.
- [x] `LoaderDevicePartUUID` is published only for a GPT partition with a GUID that is neither all-zero nor all-FF, via `boot_loader_part_uuid_text()`; otherwise it is cleared.
  - GPT means `boot_partition_style == 2`. An MBR disk signature is never published as a partition UUID.
- [x] Validation: `src/kernel/test/test_loader_vars.c` runs the shipped publisher against an in-memory UEFI variable store (12 cases).
  - The store is keyed by name and 16-byte GUID value, drops volatile variables at a simulated reset, refuses attribute-changing rewrites, counts every call, and fails writes or deletes per name.
  - Cases: a fully known boot sets all 11 volatile, with both feature bitmaps checked byte-exact under their own GUIDs; an unknown boot after a known one inherits nothing, even from seeded persistent copies; MBR, unknown style, zero and all-FF GUIDs, unmeasured or zero timing and a missing system table each delete an old value; persistent copies migrate under both GUIDs; the user's timeout and default survive publish and chainload; a seeded old `LoaderEntries` is deleted for a rejected, empty, absent and all-hidden store; a full 64-entry list publishes whole; write and delete failures are reported, and bad arguments make no firmware call; a chainload clears everything and a refused delete there is reported without stopping the loop; zero timing, oversize vendor, MBR.
  - Controls (each run, each failed, restored with `cmp`): writing `NON_VOLATILE` (14 failures), no delete before the write (6), MBR accepted (1), unmeasured timing published (7); after review: extension under the systemd GUID (5), hidden store not cleared (2), `clear_all` clearing nothing (2), scratch-size guard removed (1), `clear_all` ignoring failures (1), entry ids corrupted past the second character (1).
  - Live: the loader prints `[BOOT] loader-vars: published N, cleared M, failed K`. `scripts/test-smoke.sh` fails a leg that reaches the prompt without that line, with a failure, or with a total other than 11; the valid-store leg requires exactly `published 9, cleared 2, failed 0`. A plain boot logs `published 7, cleared 4, failed 0` (no store, no timing).
  - The kernel test image sits at its `USER_BASE` ceiling (02-kernel-core/TODO-33); the publisher takes caller-owned scratch, the test fixture is a `TEST_SCRATCH_KBUF`, and the test file (with its copy of the publisher) is built `minsize`, so the suite adds no BSS and stays one page under the ceiling.
  - Post-ship review (2026-11-08) added: `LoaderImageIdentifier` from LoadedImage FilePath via `loader_image_path()` (shared with self-measurement); firmware revisions as `major.minor` with a two-digit minor; ids bounded by their field, empty ids skipped.
  - Also from the review: the late timing call (section 18 items); the one-shot consumer moved into `boot_loader_one_shot_consume()`, honouring a value only when well formed, readable and deleted, never a printable prefix.
  - Review controls (each failed, restored with `cmp`): no minor padding, empty id kept, timing call touching `LoaderEntrySelected`, honouring despite a refused delete, non-printable accepted, no timeout cap, read error taken as absence.
- [x] Commit: `"boot: publish loader variables volatile, set or cleared every boot"`

**Test checkpoint:** `SUITE=boot` green with the twelve `loader-vars:` cases (the two-boot check runs against the simulated store, since volatile variables cannot outlive a real reset); `scripts/test-smoke-matrix.sh` all legs, with the valid-store leg's `published 9, cleared 2, failed 0` line and a `timing, published 2` line.

> **Test runner:** `scripts/debug/kernel/run-boot-tests.bat` (expect the twelve "loader-vars:" cases PASS in SUITE=boot).

> **Verified:** 2026-11-08 | commit `71b85447e` + review fixes | 5/5 items | build OK | SUITE=boot 7665 kernel + 17 user tests PASS | smoke matrix 13/13 on the final tree, plain smoke `published 7, cleared 4` and `timing, published 2` | 10 review mutation controls all failed as expected
> **Quality reviewed:** 2026-11-08 | Codex 7x (adversarial x3, consistency, perf, test-coverage x2) + boot-quality-auditor + parity-research-analyst | 0H+4M+5L fixed, 0 open | scope: boot-code-quality (boot-quality-auditor)

---

## 22. Store Size Cap Is Untested on Both Sides of the 16 KiB Boundary

> **Spawned-by:** §20 (review)
> **User impact:** a boot entry store that grows to the 16 KiB ceiling is rejected by one side and accepted by the other, so `bootcfg` writes a store the firmware then refuses. The user's configured selection is silently discarded and the machine boots the in-firmware fallback, with nothing on screen explaining why -- the same user-visible failure §20 fixed for a different cause.

Found by the §20 test-coverage review, which asked for boundary cases at the cap and found the boundary has no coverage at all on either side. Not introduced by §20. -> XREF: `01-boot-platform/TODO-07-boot-entry-store-menu-policy.md` §20 (item: "Paired regressions on both sides").

**The mechanism, confirmed at file:line.** The firmware rejects `raw_len > BOOT_ENTRIES_MAX_TOTAL_BYTES` with `BOOT_ENTRIES_REJECT_FILE_TOO_LARGE` (`src/boot/uefi/boot_entries_parser.c` in `boot_entries_parse`), and the host rejects `len(raw) > MAX_TOTAL_BYTES` at `tools/boot-entry-validate/validate.py:1032` and again on the on-disk size at `:1081`. The host has ONE over-cap case, `_oversize_file` (a 17 KiB store, rejected pre-read); the firmware has none, and neither side has a case at the boundary. So the two caps are asserted equal by reading rather than by measurement, and the off-by-one direction (`>` against `>=`) is unverified on both sides. The §20 review's first draft of this section claimed the cap was untested outright, which the consistency leg corrected against `test_validate.py`.

- [x] Kernel cases at 16383, 16384 and 16385 bytes, `crc32` as the root's last key after whitespace padding: `test_store_size_cap_boundary` in `test_boot_entry_parser.c`.
  - Control: the parser's cap flipped to `>=` fails the at-cap case (2 assertions).
  - The loader's pre-read check (`bootx64.c`) and the parser now share one predicate, `boot_entries_size_within_cap()`, so these verdicts are the loader's too; it is pinned directly, including a 64-bit size that wraps 32 bits back under the cap. Control: the predicate made exclusive fails 3 assertions.
  - `s_fixture_buf` is 2048 bytes (`src/kernel/test/test_boot_entry_parser.c`), so this needs a scratch-backed buffer rather than the existing static; `TEST_SCRATCH_KBUF` is the pattern already used for the parse result. Do NOT grow the static: it is `.bss` in a kernel with a measured ceiling.
- [x] Host cases at the same three sizes, same verdict as the firmware at each: `_store_of_size` in `tools/boot-entry-validate/test_validate.py`.
  - Control: the host pre-read check flipped to `>=` fails the at-cap case.
  - `validate_store()` is also called directly at the three sizes, because the CLI's pre-read check means its own `len(raw)` guard is otherwise never reached over the cap. Control: removing that guard fails the over-cap case.
  - A disagreement here is a store the host stamps and the firmware then refuses. The existing `_oversize_file` case stays; it proves the over-cap path, not the boundary.
- [x] The caps are the same number by construction: `_header_mirror_drift()` in `test_validate.py` checks every mirrored limit against the C sources.
  - Sources read: `include/boot/boot_entries.h` and `boot_entries_parser.h` and `boot_entries_parser.c`, 13 limits including the health-subset and parse/scan depth limits; a derived define (the scan depth) is resolved from the defines it names.
  - Control: `MAX_TOTAL_BYTES` raised by one fails the check. The inclusive cap is stated in `docs/boot/boot-entry-schema.md`.
- [x] Commit: `"boot: cover the boot entry store size cap on both sides of the boundary"`
- [x] Review fixes (post-ship review of `4432bd926`):
  - Consistency [M]: the drift check also covers `MAX_PARSE_DEPTH` and `MAX_SCAN_DEPTH`. Control: each depth moved on one side fails.
  - Boot auditor [L]: the oversize serial line prints the full 64-bit size and the cap from its define; a `_Static_assert` pins the detail text's "16 KiB".
  - Boot auditor [L]: the predicate's declaration no longer splits another function's comment.

**Test checkpoint:** `SUITE=boot` green with the three new kernel cases; host `python3 tools/boot-entry-validate/test_validate.py` green with the three mirrored cases. No smoke run needed unless the fixture change touches parser code.

> **Test runner:** `scripts/debug/kernel/run-boot-tests.bat` (expect the "boot-entries: store size cap boundary" case PASS in SUITE=boot) and `python3 tools/boot-entry-validate/test_validate.py` (148/148).
> **Verified:** 2026-11-08 | commit `4432bd926` + review fixes | 5/5 items | build OK | SUITE=boot 7551 kernel + 17 user tests PASS | 148/148 host validator cases | smoke matrix 13/13 at `4432bd926`, plain smoke to `C:\>` after review fixes | lint 0 errors
> **Quality reviewed:** 2026-11-08 | Codex 8x (adversarial, test-coverage, re-adversarial x3, adversarial, consistency x2, perf) | 0H+3M+2L fixed, 0 open | scope: boot-code-quality (boot-quality-auditor)
> **Notes:**
> - The loader pre-read check and the parser share `boot_entries_size_within_cap()`, so the kernel boundary tests are the loader's tests too.
> - Every mutation control was run: parser predicate exclusive (3 fail), host pre-read and post-read guards, each mirrored limit moved on one side.

---

## 23. A Rejected Store Has No Repair Path, Only a One-Entry Fallback

> **Spawned-by:** §20 (review)
> **User impact:** once `bootentries.json` is corrupt, the user cannot get their boot configuration back. The machine boots one hardcoded fallback entry forever, and there is no second copy to fall back to, no way to regenerate the store from anything, and no offline editor shipped yet. Every entry they configured is gone and the only route back is to author the file by hand, on another machine, with a CRC they have to compute themselves.

Found by the parity pass on §20. Distinct from the already-tracked authentication gap (Ed25519, Branch B) and from the visibility gap, which is owned elsewhere. -> XREF: `01-boot-platform/TODO-03-bootloader-error-recovery.md` §22 (item: "Render the existing error screen (or a non-fatal variant of it) when the store is rejected, carrying the `boot_entries_reject_code_t` and the `selection_reason`, so the user sees WHICH failure happened rather than a fallback boot").

**The mechanism, confirmed at file:line.** The Fallback Contract (`docs/boot/boot-entry-schema.md` section 8) synthesizes exactly one hardcoded entry (`id="fallback"`) and boots it; `boot_entries_synthesize_fallback()` in `src/boot/uefi/boot_entries_parser.c` is the whole of it. There is no backup copy of the store, no A/B pair for the store itself (the doc's "No A/B reference today" line concerns kernel slots, a different mechanism), and the only stated repair route is `bootcfg.exe`, marked "(later)" in the same doc. Both incumbents pair their integrity story with redundancy that this does not have: the Windows BCD is a registry hive whose format carries a base-block checksum plus transaction-log self-healing, with `bootrec /rebuildbcd` rescanning disks to reconstruct the store from scratch; GRUB has no checksum at all, but `grub.cfg` is a GENERATED artifact, so the standard repair is regenerating it with `grub-mkconfig` rather than patching the corrupt file.

- [x] Decide the model and record it: a last-known-good shadow of the store, kept by the loader (`todo/answers.md` Q33).
  - Regeneration was rejected: the only source of truth is the seed in host-side `bootcfg.py`, and it would rebuild a default store, not the user's. An A/B pair needs every writer to alternate slots for the same protection.
  - An absent primary does not consult the shadow (deleting the store resets to the fallback), and restoring the primary stays explicit (design review: automatic restore overwrites the evidence and may persist a stale policy).
- [x] Implement the model in the store read path, including the case where both copies fail: `store_shadow_maintain()` and `store_shadow_recover()` in `src/boot/uefi/bootx64.c`.
  - Refresh: an accepted store that differs from `bootentries.lkg.json` is written to `bootentries.lkg.tmp`, then copied to the published name, and the staging copy is deleted once the published copy matches. No rename (adversarial review: a FAT SetInfo rename is not power-fail atomic). No write when the copy is current.
  - The plan (`boot_store_shadow_plan()`) and its executor (`boot_store_shadow_sync()`, `boot_store_shadow_pick()`) are pure code in `boot_entries_parser.c` behind `boot_store_shadow_ops`; `bootx64.c` supplies only the UEFI file calls.
  - `test_boot_entry_parser.c` drives the executor against an in-memory volume, injecting an unreadable copy and a failure at every write and delete (file unchanged, gone or torn), twice, from all 16 start states: no valid copy is ever lost, and the next clean boot settles.
  - A copy whose read failed may be the only valid one, so it is rewritten only beside a valid copy; otherwise the boot holds (adversarial round 2, `BOOT_STORE_SHADOW_HOLD`). Empty and oversize copies are confirmed broken and stay replaceable. The fake volume models the empty case (read returns 2), and the `empty-tmp` smoke leg proves the loader adapter reports it the same way.
  - Recovery: a present-but-rejected primary boots the published copy, else the staging copy, after the parser and CRC accept it; both broken is the fallback. The notice says "Starting from the last good copy instead". Recovery never writes.
  - `bootcfg restore` copies the shadow back, keeping the broken store as the next free `.bad`/`.bad.N` and doing nothing when the store already matches; `restore` mutation-log kind; twelve cases in `tools/bootcfg/test_bootcfg.py` including injected write failures, an unreadable store, exhausted `.bad` names and a `.bad` published concurrently (claimed with `O_EXCL`, never replaced), and a host that cannot open directories.
- [x] Write the recovery route in the schema doc's Fallback Contract (`docs/boot/boot-entry-schema.md` section 8), and point the kernel's `Boot entry store unusable` warning at `bootcfg restore`.
- [x] Commit: `"boot: give a corrupt boot entry store a way back"`
- [x] Flush restore's evidence directory through the validator's `_fsync_dir`, so a Windows host (which cannot open a directory) still restores (post-ship adversarial + consistency [high]; `t_restore_windows_dir`).
- [x] Check `Delete` after a short shadow write; add POST16 `0xB0BB`/`0xB0BC` around shadow refresh and recovery (boot-quality-auditor).
  - Both codes are classified optional and decoded in `vpd.c`, with the `0xB0B9`/`0xB0BA` names the table lacked.
- [x] Fault every op index below `BOOT_STORE_SYNC_MAX_STEPS` (a bound of 5 missed real ops) and make the fake volume refuse oversize writes instead of clipping them (kernel-quality-auditor).

**Test checkpoint:** `SUITE=boot` green with cases covering a corrupt primary and a good shadow, both corrupt, and a shadow that is stale rather than corrupt; `scripts/test-smoke-matrix.sh` green, including the new `recovered`, `recovered-tmp` and `empty-tmp` legs (corrupt store plus a valid published copy; a torn published copy plus a valid staging copy; a valid store beside an empty staging copy, which must be replaced, not held).

> **Test runner:** `bash scripts/test.sh SUITE=boot` (store shadow source choice, refresh plan, sync under faults and recovery pick cases), `python3 tools/bootcfg/test_bootcfg.py` (50/50), and `SMOKE_CORRUPT_STORE=1 SMOKE_SHADOW_STORE=1` (or `=tmp`) `bash scripts/test-smoke.sh`.
> **Verified:** 2026-11-09 | commit `04404fee5` + review fixes | 8/8 items | build OK | full suite 36680 kernel + 17 user tests PASS | 50/50 bootcfg | smoke matrix 16/16 on the final tree, plain smoke to `C:\>` | mutation controls (naive plan, stale keep, ignored failure, buffer leak, no HOLD, empty as unreadable, loader adapter, no `O_EXCL`, direct dir fsync) all failed as expected
> **Quality reviewed:** 2026-11-09 | Codex 12x (design, test-coverage x3, adversarial x4, re-adversarial x3, consistency, perf) + boot-quality-auditor + kernel-quality-auditor | 3H+3M+4L fixed, 0 open | scope: boot-code-quality, kernel-code-quality
> **Notes:**
> - Firmware status translations (Open, Read, Flush and Close failures, allocation failure) and an oversize shadow are not reachable from the fake volume or the smoke fixtures; each is handled as a failure that degrades, unproven on real firmware.
> - Restoring is explicit: the loader boots the copy but never rewrites the primary (Q33 in `todo/answers.md`).

---

## 24. Boot Menu Help, Power and Saved-Default Keys

> **Spawned-by:** §6 (review)
> **User impact:** at the boot menu a user cannot find out that F8, F10 or Esc exist, cannot restart or power off without the firmware's own keys, and cannot make the entry they keep choosing the default without editing the store offline.

Found by the parity pass on §6 against systemd-boot (https://man.archlinux.org/man/systemd-boot.7, https://www.freedesktop.org/software/systemd/man/latest/loader.conf.html). PageUp/PageDown and 1-9 shipped in §6; these three need new UI or new persistent state, so they are a section of their own. Nothing else owns them: §7 owns F8/F10/F11 only, and §15 publishes loader variables but has no key that writes one.

- [x] `docs/design/shell.md#boot-menu` specifies the menu: colours, geometry, footer text with "F1 help.", the notice line, the help screen and its paging, and the key legend.
  - Written before the help screen was drawn; the boot menu had no design spec until then.
- [x] `h`, `?` and F1 open help (`boot_menu_paint_help` in `bootx64.c`); any key pages or closes it, the menu repaints in full, and the footer names F1. Opening it stops the countdown.
  - `boot_menu_help_pages()` splits the 9 help lines over the rows the mode fits, so a small GOP mode pages instead of clipping; ConOut prints them all at once.
- [x] Shift+B reboots and Shift+O powers off through `ResetSystem` (`boot_menu_power`), logging the choice first; a refused or absent `ResetSystem` keeps the menu up with a footer notice.
- [x] `d` saves the highlighted entry as `LoaderEntryDefault` (NV|BS|RT), and the §3 ladder boots it next time; `d` on the saved entry clears it. `LoaderFeatures` bit 2 is advertised.
  - `boot_loader_entry_default_read/_write` in `loader_vars.c`; the writer refuses anything the reader would reject. An unusable value is logged and left in place.
  - A discovered OS entry (`chainload-N`, `BOOT_ENTRY_FLAG_SYNTHESIZED`) is a slot, not an OS: `d` refuses it with a notice and the ladder ignores it (post-ship review); stable ids are TODO-27 §9.
  - Rank (Q34 in `todo/answers.md`): below a one-shot and a tagged firmware option, above the lowest `sort_key`. Reported as `STORE_DEFAULT` with `saved_default_used`, so an older kernel still accepts the handoff.
  - `boot_menu_key_action()` maps every key; `boot_menu_repeat_check()` makes a held toggle key one action, so a held `d` cannot save and clear in a loop. The notice says, warning first, when a tagged firmware option will still win.
- [x] Commit: `"boot: menu help, power and saved-default keys"`

**Test checkpoint:** `SUITE=boot` green with pure-helper cases for key mapping and default precedence; `SMOKE_MENU=1 bash scripts/test-smoke.sh` still auto-boots; a smoke leg that sends `d` and confirms the second boot selects the saved entry.

> **Test runner:** `bash scripts/test.sh SUITE=boot` (key map, repeat guard, help pages, toggle, notice repaint, saved-default ladder, `LoaderEntryDefault` read/write, held `d`), then `SMOKE_MENU=default` and `SMOKE_MENU=saved` `bash scripts/test-smoke.sh` in that order (both in the smoke matrix).
> **Verified:** 2026-11-09 | commit `00508f960` + review fixes | 5/5 items | build OK | full suite 36732 kernel + 17 user tests PASS | smoke matrix 18/18 on the final tree, plain smoke to `C:\>` | mutation controls (no ladder rung, no repeat guard, no notice repaint, no saved-default read in the loader, no reader cap, no synthesized guard) all failed as expected
> **Quality reviewed:** 2026-11-09 | Codex 9x (design, test-coverage, adversarial x3, re-adversarial x2, consistency, perf) + boot-quality-auditor + parity-research-analyst | 2H+6M+2L fixed, 0 open | scope: boot-code-quality
> **Notes:**
> - The text-console menu (no usable GOP mode) shows the same notices and legend as the graphical footer, but serial cannot see ConOut, so that path is proven only by reading the code, not by a smoke leg.
> - A real `ResetSystem` that returns is not reachable under OVMF; the restart and power-off failure notices are unproven on real firmware.
> - Entry ids are at most 47 characters (`BOOT_ENTRIES_MAX_ID_LEN`), well inside the 62 the writer accepts, so `d` cannot be refused for length.
> - A discovered OS entry (`chainload-N`) cannot be saved: its id is a slot. Stable ids are filed as [TODO-27 §9](TODO-27-uefi-advanced.md#9-stable-ids-for-discovered-os-entries); timeout keys and `e` as §25.

## 25. Menu Timeout Keys and One-Off Edit

> **Spawned-by:** §24 (review)
> **User impact:** a user cannot change how long the menu waits without editing the store offline, `bootctl set-timeout` has no effect, and a user whose boot fails cannot add a kernel option such as safe graphics for one boot from the menu.

Found by the §24 parity pass against systemd-boot (https://www.freedesktop.org/software/systemd/man/latest/systemd-boot.html), which has `t`/`T` (and `+`/`-`) to change the timeout and `e` to edit an entry's options once. The store already carries an `allow_editor` flag that nothing reads. Q32 in `todo/answers.md` stopped this loader writing `LoaderConfigTimeout` as a report; honouring it is the separate decision this section makes.

**Design:** [boot menu](../../docs/design/shell.md#boot-menu)

- [x] `LoaderConfigTimeout` (systemd GUID) is honoured; `LoaderFeatures` advertises bit 0 and bit 13, menu-disable words (`0x211F`). Rank and reasons in `todo/answers.md` Q35.
  - `boot_menu_timeout_resolve()` in `boot_policy.c` ranks the one-shot, then `LoaderConfigTimeout`, then the default entry's `timeout_override`, then 5 s.
  - `boot_loader_timeout_parse()` in `loader_vars.c` reads decimal seconds and `menu-force` / `menu-hidden` / `menu-disabled` for both variables; `boot_loader_config_timeout_read()` never deletes, and a malformed value is left for its owner.
  - `menu-disabled` skips the menu and the F11 probe; `menu-hidden` skips it and F11 still opens it. A persistent or entry `0` hides, a one-shot `0` waits for a key. A countdown above 60 s is clamped to the watchdog window and serial says so (`[BOOT] menu: timeout Ns from <source>`).
  - A one-shot or saved timeout that asks for a visible menu shows it, even with one entry.
- [x] `t` / `+` and `T` / `-` step one scale (hidden, 1-60 s, `menu-force`) from this boot's timeout and save it at once; no key ever saves `menu-disabled`.
  - `boot_menu_timeout_step()` is the pure step and `boot_loader_config_timeout_write()` writes NV|BS|RT in `bootctl`'s format. The §24 repeat guard makes a held key one step, and the footer names the result.
  - Help and `docs/design/shell.md#boot-menu` list the keys and the scale.
- [x] `e` opens a one-line editor on a split or safe entry whose flags include `allow_editor`; the edit applies to this boot only and is never written back.
  - `boot_menu_edit_allowed()` refuses a discovered entry, and a UKI because its options are inside its signed image.
  - `boot_menu_edit_init()` / `boot_menu_edit_key()` are the pure buffer (insert, Backspace, Delete, Left, Right, Home, End, 255 characters); `boot_menu_edit_view_start()` in `boot_menu_layout.h` (GOP pixels) and `boot_menu_edit_cols_start()` (console columns) scroll the field to keep the caret visible. GOP draws it on the highlight bar, ConOut on one fixed row.
  - The text starts as the entry's own `cmdline`, else boot.conf's. `boot_policy_apply_cmdline()` in `bootx64.c` runs after the kind validator accepts the booted entry and writes the edit, else the entry's own non-empty `cmdline` (which before this section was validated and dropped), into `boot_config.cmdline`.
  - `boot_config.cmdline_source` (offset 28, the old reserved bytes, mirrored and in the ABI manifest; 0 boot.conf, 1 entry, 2 edited) carries the source; the kernel logs it on the `boot_decision:` line (`cmdline=edited`) and as `CONF: cmdline source=...`, at warning level for an edit.
  - Secure Boot needs no separate rule: `allow_editor` is the entry's own permission, and the signed-image case (UKI) is always refused.
- [x] Unit tests for the timeout rank, the step scale and the edit buffer bounds; a smoke leg that sends `t` and confirms the next boot's timeout, and one that boots an edit.
  - `test_loader_vars_config_timeout`, `test_menu_timeout_resolve`, `test_menu_timeout_step`, `test_menu_edit_allowed`, `test_menu_edit_core`, `test_boot_menu_edit_view`, the key map and `boot_cmdline_source_name()`.
  - Smoke: the default leg presses `t` (4 s to a saved 5 s) and `e` with `test=0` typed, and the kernel must log `cmdline=edited`; the saved leg must count down 5 s from `LoaderConfigTimeout` and boot with `cmdline=boot.conf`.
  - The suite needed kernel image room: `test_boot_audit.c`'s four 28 KiB `boot_info` fixtures and their line buffers moved from `.bss` to the test scratch allocator (the pattern of the [higher-half kernel relocation TODO](../02-kernel-core/TODO-33-higher-half-kernel-relocation.md) section 16), taking `__kernel_end` from `0x800000` back to `0x7d8000`.
- [x] Commit: `"boot: menu timeout keys and one-off edit"`

**Test checkpoint:** `SUITE=boot` green; `SMOKE_MENU=1` still auto-boots; the timeout leg passes.

> **Test runner:** `bash scripts/test.sh SUITE=boot` (timeout parse/read/write, rank, step scale, key map, editor buffer and field, cmdline source names), then `SMOKE_MENU=default` and `SMOKE_MENU=saved` `bash scripts/test-smoke.sh` in that order (both in the smoke matrix).
> **Notes:**
> - Mutation controls: swapping the one-shot and `LoaderConfigTimeout` ranks fails 3 tests, an editor insert that does not move the terminator fails 5, and writing `menu-hide` fails 2.
> - The ConOut editor line and the GOP field are proven by reading the code; serial sees the accepted text, not the drawing. The ConOut field is one row from `QueryMode`'s width, scrolled by `boot_menu_edit_cols_start()`, with the console cursor on the caret; Enter is refused, and the edit cancelled, when the last paint did not reach the console or the cursor cannot be shown, so a failed write can never boot text the user did not see. The `bootx64.c` paint functions call firmware and have no host harness, so that refusal has no unit test.
> - The kernel halts on an unknown boot key, so a mistyped edit stops that boot with `[CONF] unknown boot key`; the next boot has the entry's own options again.
> - Secure Boot needs no editor rule of its own: `allow_editor` is off unless an entry opts in, a UKI is refused, and an edited `nointegritychecks=1` or `testsigning=1` cannot relax code integrity while Secure Boot is on (`ci_policy.c:49` relaxes only when Secure Boot is known off).
> - Review fixes: `LoaderFeatures` bit 13 (`MENU_DISABLE`) is advertised, without which `bootctl` writes `menu-force` as `4294967295`, and that number is now read as `menu-force` too; a keyboard that returns only device errors for 1 s restarts a 5 s countdown (`boot_menu_input_fault_step()`), so a dead keyboard after a key press cannot hold the boot forever; the kernel logs a bounded copy of the loader's command line and warns on an out-of-range `cmdline_source`.
> **Verified:** 2026-11-09 | commit `75e4d3e57` + review fixes | 5/5 items | build OK | SUITE=boot 7902 kernel + 17 user tests PASS | `SMOKE_MENU=default` and `saved` PASS | mutation controls (rank, editor terminator, timeout word, fault reset) each fail
> **Quality reviewed:** 2026-11-09 | Codex 6x (adversarial, consistency x2, perf, re-adversarial x2) + boot-quality-auditor + kernel-quality-auditor + parity-research-analyst | 2M+5L fixed, 0 open | scope: boot-code-quality, kernel-code-quality

## OS Comparison

| ⭐  | Feature                                               | 🪟 Win11                         | 🐧 Linux                            | 🚀 Impossible OS                                                                 |
| --- | ----------------------------------------------------- | -------------------------------- | ----------------------------------- | -------------------------------------------------------------------------------- |
| 💎  | Structured boot entries                               | ✅ BCD store + bcdedit           | ✅ systemd-boot/GRUB BLS            | ⬜ Planned -- §1, §2                                                             |
| 💎  | BootNext one-shot read (firmware-layer)               | ✅ Firmware BootNext + BCD       | ✅ efibootmgr -n                    | ✅ Firmware fields read + OS-side ladder + live ESP store wiring                 |
| 💎  | Recovery boot entry                                   | ✅ WinRE                         | ✅ GRUB recovery                    | ⬜ Planned -- §9                                                                 |
| 💎  | Safe mode entry                                       | ✅ msconfig safeboot flag        | ✅ rescue.target / single           | 🟦 §8 KIND_SAFE + boot_mode=1 + F8 hotkey; subset payload deferred to §13        |
| 💎  | Previous-kernel rollback entry                        | ⚠️ BCD bootsequence (limited UX) | ✅ GRUB previous kernel             | ⬜ Planned -- §10                                                                |
| 💎  | Boot menu UI (text + graphical)                       | ⚠️ Minimal "Choose an OS"        | ✅ GRUB / systemd-boot menus        | ✅ §6 GOP/ConOut/serial; §7 hotkeys; §24 help, default; §25 timeout, `e` edit    |
| 💎  | Offline entry editor tooling                          | ✅ bcdedit / bcdboot             | ✅ grub-mkconfig / bootctl          | 🟦 §11 host-side `bootcfg.py` + validator parity; live-boot binary deferred      |
| 💎  | Loop prevention via per-entry try counter             | ⚠️ Recovery loop after 2 fails   | ✅ systemd-boot tries=/done=        | 🟦 BLS counter grammar + ladder gate in §3; crash-tolerant rename in §5          |
| 💎  | Per-entry health-gated mark-good                      | ❌ Implicit (timer past)         | ✅ systemd-bless + boot-complete    | 🟦 §14 entry gate + registry + MarkGood handoff; net/crash checks pending owners |
| 💎  | Entry kinds (UKI, chainload, network, resume)         | ⚠️ BCD osloader / resume (split) | ✅ BLS Type 1 / Type 2 (UKI)        | 🟦 §13 SPLIT/UKI/SAFE validators + loader; CHAINLOAD/NETWORK/RESUME deferred     |
| 💎  | OS-visible loader UEFI variables                      | ❌ None standardized             | ✅ systemd LoaderXxx interface      | ✅ §15 12 LoaderXxx vars; honors OneShots, EntryDefault, ConfigTimeout           |
| 💎  | BLS display order (bad last, sort-key, version)       | ⚠️ BCD displayorder, manual      | ✅ systemd-boot BLS order           | ✅ §18 one order for menu + LoaderEntries; LoaderFeatures bit 8                  |
| 💎  | First-install bootstrap (idempotent offline seed)     | ✅ bcdboot from install media    | ✅ bootctl install / grub-install   | 🟦 §16 `emit-seed` writes a byte-identical 3-entry default; self-seed TODO-22 §1 |
| ⭐  | Documented deterministic policy precedence (2 layers) | ⚠️ BCD rules underdocumented     | ⚠️ Per-bootloader behavior          | ✅ §3 ladder + 6-example doc + live producer in §4 🚀                            |
| ⭐  | Demote-not-drop bad entries with last_failure_reason  | ❌ Hidden recovery loop          | ⚠️ Bad entries silently sorted last | 🟦 §4 records reject reasons in boot_info v19; menu UI label in §9 🚀            |
| ⭐  | Per-decision audit trail (BlackBox primary)           | ❌ Event log only                | ❌ journalctl scattered             | ✅ §12 JSONL `X:\Boot\history.jsonl` + 256-byte sticky NVRAM ring 🚀             |
| ⭐  | Schema-versioned + CRC-checksummed entry store        | ❌ Binary BCD, no checksum       | ❌ INI / cfg, no checksum           | ✅ §1 schema_version=1 + CRC-32 IEEE 802.3 🚀                                    |
| ⭐  | Per-entry mutation audit (add/remove/reorder logged)  | ❌ Not logged                    | ❌ Not logged                       | ✅ §12 `bootcfg.py --mutation-log` JSONL with requester + prior/new CRC 🚀       |
| ⭐  | Unique + literally-spelled key names enforced         | ❌ Binary BCD, N/A               | ⚠️ Last-wins on repeated BLS key    | ✅ §19 hard reject at both object levels, host + firmware in agreement 🚀        |
| ⭐  | Integrity header located structurally, not by text search             | ❌ Binary BCD, N/A               | ❌ No checksum to locate             | ✅ §20 root-member locate, firmware and host agree byte-for-byte

> **After §1-§16:** Impossible OS matches Windows 11 and Linux on structured entries, BootNext provenance, recovery, safe mode, previous-kernel rollback, menu UX (renderer §6 + indicators §7), offline tooling, loop prevention (ladder §3 + crash-tolerant decrement §5), health-gated mark-good, entry kinds, OS-visible loader vars, and first-install bootstrap.
> **After §3 + §9 + §12 + §1 ⭐ rows:** Impossible OS surpasses both with documented two-layer precedence, demote-not-drop UX, per-decision audit, schema+CRC store, and per-entry mutation audit.

---

## Unit Tests

Per-section test files were shipped under each implemented section instead of a single monolithic `test_boot_entry.c`. §17 above maps each scenario to its owner test file. No single registration function is owed -- each per-section file already registers under `TEST_CAT_BOOT`.

Live-boot scenarios (menu rendering, A/B rollback, recovery auto-select, loader-var readback) require a live-boot integration harness that this TODO does not own; they remain `[ ]` in §17 with the blocker XREFs above.

---

## Verification

- [x] `bash scripts/build.sh` -> `tail -1 build/build.log` -> `=== BUILD OK ===` (re-verified at every section ship).
- [/] QEMU + bare-metal scenarios: unit-level coverage shipped for every implemented section (see Implementation Order [x]/[/] rows + per-section Test runner lines). Live-boot scenarios deferred to the live-boot harness owner.
- [x] `make test-boot` aggregates all `TEST_CAT_BOOT` registrations from per-section files.
- [ ] Commit: `"boot: TODO-07 entry store + kinds + policy + health + loader vars + bootstrap complete"` (closes when §9, §10, §16 first-boot self-seed, §17 deferred items all land).

**Test runner:** see per-section Test runner lines for current counts; aggregate via `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot).
