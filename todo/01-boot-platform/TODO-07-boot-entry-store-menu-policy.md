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
| 💎  |  18   | systemd BLI parity (BLS display order, one-shot, loader timestamps) | §6, §15                                 |  [/]   |

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
- [/] Dirty-rectangle repaint for `boot_menu_render()`: draw title once, repaint only changed rows on selection moves + footer once per second.
  - PARKED with no external blocker: a §6-owned bootloader optimization in `src/boot/uefi`, which sits outside the kernel image ceiling, so it is unscheduled rather than gated. The trigger is recorded in this section's Deferred stamp (full-band redraw on a 4K GOP causes key-repeat jank).
- [x] Commit: `"boot: menu renderer + countdown + input infrastructure"`

**Test checkpoint:** Menu renders 4+ entries on QEMU WHPX (GOP); countdown decrements 5..0 then auto-selects; arrow keys move + cancel countdown; Enter boots the highlighted entry. Serial mirror shows the same lines on TCG (no GOP). `bootloader_secureboot_active()` returns 1 with SecureBoot=1 + SetupMode=0; 0 otherwise. Smoke test verified menu-skip path: serial shows `[BOOT] menu: skipped (no viable candidates)` on FALLBACK_STORE_INVALID with no bootentries.json, smoke PASS in 2.55s on KVM. Test on: QEMU WHPX (GOP), TCG (serial fallback), VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 11 menu-helper suites, 0 failures

> **Notes:**
> - What shipped: `boot_menu_render` + `boot_menu_run` + pure helpers `boot_policy_menu_should_show` + `boot_policy_menu_collect`; `EFI_SCAN_*` constants; POST16 0xB0B6; 11 unit tests.
> - How it integrates: `boot_policy_invoke()` calls collect -> should_show -> menu_run after ladder, before counter decrement; `timeout_override==0` skips UI; user-pick flips reason to HOTKEY.
> - Downstream effects: live menu-render path; serial mirror gives kiosk/headless operators visibility on selection.
> - Canonical doc: [`docs/boot/boot-policy.md`](../../docs/boot/boot-policy.md) (menu UX) + [`docs/boot/boot-menu.md`](../../docs/boot/boot-menu.md) (§7 indicator legend lands there).
> - Scope boundary: §6 owns renderer + countdown + input + filter; indicators + hotkeys + `hide_when_alone` are §7; demote-not-drop visual is §9.

> **Verified:** 2026-05-09 | commit `b4a076c4` (review fixup over `34ae373c`) | 5/6 items + 1 deferred | build OK | smoke PASS (KVM 2.49s)
> **Deferred:** [M] Dirty-rectangle repaint for `boot_menu_render` (full-band redraw on 4K GOP causes key-repeat jank) -> XREF: 01-boot-platform/TODO-07 §6 (item: "Dirty-rectangle repaint for `boot_menu_render()`" at line 236)
> **Quality reviewed:** 2026-05-09 | Codex 8x (design + adversarial + test-coverage + 3 re-adversarial + consistency + perf) | 4H+5M fixed, 1M deferred | scope: boot-code-quality

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
> **Deferred:** [M] Live-boot bootcfg native binary (set-bootnext-hint / set-oneshot / dump-history) -> XREF: 01-boot-platform/TODO-07 §11 (item: "Live-boot subcommands deferred to a future user-mode binary owner" at line 348 -- needs UEFI var write + BlackBox JSONL access)
> **Quality reviewed:** 2026-05-09 | Codex 5x (design + adversarial + 2 re-adversarial + consistency + perf) | 2H+5M fixed | scope: N/A (host-side Python tool)

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
> **Deferred:** [L] Audit dual-write-failure dedup harness -> XREF: 01-boot-platform/TODO-07 §17 (item: "Audit dual-write-failure dedup harness" at line 572)
> **Deferred:** [L] Audit JSONL rotation on FAT32 LFN -> XREF: 01-boot-platform/TODO-07 §17 (item: "Audit JSONL rotation on FAT32 LFN" at line 573)
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

- [x] Publish 12 read-only `Loader*` vars via `loader_publish_readonly_vars()` in `bootx64.c`, after policy decision + counter decrement, before kernel handoff.
- [x] Consume + delete `LoaderEntryOneShot` + `LoaderConfigTimeoutOneShot` in `loader_consume_one_shot_vars()`; EntryOneShot overlays `bootcurrent_entry_id`; TimeoutOneShot overrides menu countdown with pre-multiply overflow guard (3600s cap).
- [x] `LoaderFeatures` uses canonical systemd-boot bit positions (bit 1, 3, 4 advertised). Impossible OS extensions in separate `ImpossibleOSLoaderFeaturesExt` var under our vendor GUID.
- [x] Every `gRT->SetVariable` is wrapped in `loader_set_var()`; failure flips `boot_info.loader_vars_degraded = 1` and continues boot.
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
- [/] QEMU boot menu timeout/default-selection scenario. Pre-EBS UEFI menu_run is not kernel-testable; needs a live-boot harness.
  - PARKED, operator-gated: `menu_run` is pre-EBS UEFI, so exercising a timeout or default-selection needs keystroke injection into firmware from a live-boot harness that does not exist and has no TODO owner. Only an operator standing up that harness clears it.
- [/] Firmware-vs-OS layer separation scenario: §3 ladder + §4 reject-record path tested; §6 menu_should_show covers UNKNOWN_BOOTCURRENT rendering. Full end-to-end fall-through to in-store default needs an integration / live-boot harness.
- [/] A/B rollback entry-selection scenario (1/2/3-fail). Blocked: [`TODO-21 §1`](TODO-21-ab-boot-rollback.md), [`TODO-21 §3`](TODO-21-ab-boot-rollback.md), [`TODO-21 §4`](TODO-21-ab-boot-rollback.md).
  - Blocker owner: -> XREF: 01-boot-platform/TODO-21 §1 (item: "Choose storage: UEFI NVRAM ... vs GPT metadata partition" at line 5). The scenario needs real 1/2/3-fail counter state from the A/B producers.
- [/] Recovery auto-select + ALL_PATHS_BAD scenario. Blocked: [`TODO-22 §1`](TODO-22-recovery-partition.md), [`TODO-22 §2`](TODO-22-recovery-partition.md).
  - Blocker owner: -> XREF: 01-boot-platform/TODO-22 §1 (item: "GPT layout: EFI (64 MiB) + Slot A ..." at line 5). The scenario needs the recovery load path and the ALL_PATHS_BAD sentinel.
- [/] Per-entry health-gate scenario: unit-level coverage shipped via §14 (`test_boot_health_check.c` + health_check_subset parser fixtures); full live-boot scenario needs the live-boot harness.
- [/] Entry-kind dispatch: SPLIT/UKI/SAFE covered via §13; chainload -> [`T27 §1`](TODO-27-uefi-advanced.md), network -> [`T25 §7`](TODO-25-network-pxe-http-boot.md), resume -> [`T26 §3`](TODO-26-hibernation-resume-fast-startup-handoff.md).
- [/] Loader UEFI variable scenario: helper-level coverage shipped via §15 (`test_smbios_parse.c`); live post-boot readback needs the live-boot harness.
- [x] Bootstrap idempotency scenario: covered by §16 `emit-seed: idempotent` + 3-entry default (`test_bootcfg.py`).
- [x] BlackBox-vs-NVRAM audit scenario: covered by §12 (`test_boot_audit.c` + bootcfg mutation-log).
- [/] Audit dual-write-failure dedup harness. No current owner -- scope-gap-protocol Branch C: needs a static-helper testability seam in `boot_audit.c` ([L]). Property already correct by construction.
  - PARKED with no external blocker: what is missing is a static-helper testability seam in `boot_audit.c` (scope-gap-protocol Branch C, [L]), which is §17-owned work nobody has scheduled. The property itself is already correct by construction, so this is test reach and not a defect.
- [/] Audit JSONL rotation on FAT32 LFN. Blocked on cross-cluster LFN removal in [`../05-storage-filesystems/TODO-04 §16`](../05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md) (currently refuses non-first-cluster destinations).
  - Blocker owner: -> XREF: 05-storage-filesystems/TODO-04 §16 (item: "Refuses non-first-cluster destinations" at line 408). That item shipped the REFUSAL of non-first-cluster destinations, not the cross-cluster LFN removal this needs, so the block still holds as of 2026-09-03.
- [/] `bootcfg.exe` round-trip scenario: offline subset shipped via §11+§16 (`test_bootcfg.py`); live-boot `bootcfg.exe` deferred per [`§11`](#11-boot-entry-editor-tooling).
- [x] Commit: `"test: boot entry test suite aggregation + status"` (commit `27c6d2c8`)

**Test checkpoint:** Per-section unit suites already register under `TEST_CAT_BOOT` and run with `make test-boot` (or `bash scripts/test.sh SUITE=boot`); the §17 commit is pure status reconciliation -- no new test code lands here. Live-boot scenarios depend on harness infrastructure that is not in this TODO's scope; XREF'd to their owner sections above. Bare-metal pass requires the user's laptop -- not gateable from WSL. Test on: host (`python3 tools/bootcfg/test_bootcfg.py` + `python3 tools/boot-entry-validate/test_validate.py`) + QEMU TCG (`make test-boot`).

> **Test runner:** N/A (aggregation section -- no new test file) | validation: per-section runners; aggregate via `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)

> **Notes:**
> - What shipped: status reconciliation only -- every checklist item maps to per-section coverage, a blocker XREF, or a live-boot harness gap.
> - Structure / consumers: §17 is the umbrella; §1-§16 carry the actual cases (parser / policy / decision / menu / entry-kind / audit / health / smbios / bootcfg). Per-section Test runner lines have the current counts. `make test-boot` aggregates them.
> - Downstream effects: closes the umbrella. Remaining gaps are explicit per-item with concrete blocker XREFs; the live-boot integration harness has no current owner -- see the in-section Note below.
> - Canonical doc: this section + per-section Test runner lines.
> - Scope boundary: §17 owns aggregation + status. New tests for covered behavior belong in their owner sections; dual-write testability + FAT32 LFN belong elsewhere.

> **Verified:** 2026-05-19 | commit `27c6d2c8` (impl) + `a6e2124e` (commit-row flip) | 4/14 items + 5 partial + 5 blocked | build OK (no source changes; verified during §16 commit `17ee74f9`) | lint clean
> **Deferred:** [L] Audit dual-write-failure dedup harness -> XREF: 01-boot-platform/TODO-07 §17 (item: "Audit dual-write-failure dedup harness" at line 572)
> **Deferred:** [L] Audit JSONL rotation on FAT32 LFN -> XREF: 05-storage-filesystems/TODO-04 §16 (item: "Refuses non-first-cluster destinations until cross-cluster LFN removal lands" at line 393)
> **Quality reviewed:** 2026-05-19 | Codex 8x (adversarial 6x + consistency + perf) | 0C+1H+9M fixed, 0 open | scope: N/A (docs-only aggregation section)

---

## 18. Boot Loader Interface (systemd BLI) Parity Fixes

The shipped §15 (loader UEFI vars) + §6 (menu render) publish systemd Boot Loader Interface variables but diverge from the BLI/BLS contract in three ways a `bootctl` / `systemd-analyze` / `systemctl reboot --boot-loader-menu=` consumer would hit (Codex gap-audit 2026-06-13). This section corrects them without re-opening the shipped sections.

- [x] BLS display ordering by `sort_key` then `machine_id` (`id` tiebreak), shared by the menu + `LoaderEntries`. Helpers `boot_entry_bls_less`/`boot_entries_bls_sort` in `boot_entries_parser.c`; unit-tested in `test_boot_entry_parser.c`.
- [x] `LoaderConfigTimeoutOneShot`: 0 = menu no timeout, 1..60 honored, 61..3600 clamped to 60 + `[WARN]` (watchdog-window divergence, not a silent drop); a present one-shot forces the menu. Kiosk stays on per-entry `timeout_override`.
- [/] Distinct `LoaderTimeInitUSec`/`ExecUSec`: deferred -- `timing.tsc_freq` is 0 at the pre-`load_kernel` publish point so both publish 0; folded into the publish-relocation follow-up below. `LoaderTimeMenuUSec` not added (not in upstream BLI).
  - PARKED as a §18-owned follow-up, no external blocker: `timing.tsc_freq` is 0 at the pre-`load_kernel` publish point, so both values publish 0. It folds into the publish-relocation item below and clears with it.
- [/] BLS full order + `LoaderFeatures` bit 8: add bad-counted-last (join the §5 counter state) + a `version` sub-key (absent from `boot_entry_envelope_t`); advertise bit 8 only once both land. (§18 follow-up)
  - PARKED as a §18-owned follow-up, no external blocker: needs bad-counted-last joined to the §5 counter state plus a `version` sub-key that `boot_entry_envelope_t` does not carry. Advertise `LoaderFeatures` bit 8 only once both land.
- [/] Relocate the LoaderTime publish to after TSC calibration (post-`load_kernel`, pre-EBS) so distinct non-zero `LoaderTimeInitUSec`/`ExecUSec` publish (today `tsc_freq` is 0 at the publish point, so both read 0). (§18 follow-up)
  - PARKED as a §18-owned follow-up, no external blocker: move the LoaderTime publish to after TSC calibration (post-`load_kernel`, pre-EBS). This is the item the two above wait on.
- [x] Commit: `"boot: TODO-07 §18 -- systemd BLI parity (BLS display order, one-shot timeout, distinct loader timestamps)"`

**Test checkpoint:** against a multi-entry store, `LoaderEntries` matches the on-screen menu order (bad-counted last, then `sort_key`); `LoaderConfigTimeoutOneShot=0` shows the menu with no countdown (or warns+clamps per the chosen policy); `systemd-analyze` reports non-zero loader time. Extend `test_boot_entries` with out-of-array `sort_key` + bad-counter ordering assertions. Test on: QEMU TCG (deterministic); bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2 BLS-order suites added (sort_key/id + machine_id tiebreak), 0 failures

> **Notes:**
> - **What shipped:** 2 systemd-BLI parity fixes in `bootx64.c` -- shared BLS display order (`sort_key`/`machine_id`) for the menu + `LoaderEntries`; `LoaderConfigTimeoutOneShot` (0 = menu no-timeout, 1..60 honored, >60 clamped to 60 + WARN, forces the menu even on quiet paths).
> - **How it integrates:** the sort applies to `cand_idx` after `boot_policy_menu_collect` + inside `loader_set_entries`; a present one-shot forces `show_menu` + bypasses `hide_when_alone`, and value 0 sets `g_menu_no_autoboot`.
> - **Downstream:** `bootctl` consumers now see consistent menu/LoaderEntries order; `systemd-analyze` loader-time parity stays deferred (the timestamp publish-relocation follow-up). Design adoptions in the commit message.
> - **Canonical doc:** `docs/boot/boot-entry-schema.md`; systemd Boot Loader Interface spec for var semantics.
> - **Scope boundary:** two `[ ]` follow-ups -- full BLS order (bad-counted-last + `version` field + `LoaderFeatures` bit 8) and relocating the LoaderTime publish after `load_kernel`.

> **Verified:** 2026-06-13 | commit `6d382664` | 2/5 items | build OK | kernel tests 2546 PASS (incl 6 new BLS-order assertions)
> **Deferred:** [H] distinct `LoaderTimeInitUSec`/`ExecUSec` blocked -- `timing.tsc_freq` is 0 at the pre-`load_kernel` publish point -> XREF: 01-boot-platform/TODO-07 §18 (item: "Relocate the LoaderTime publish to after TSC calibration")
> **Deferred:** [M] full BLS order (bad-counted-last + `version` sub-key) + `LoaderFeatures` bit 8 not advertised -> XREF: 01-boot-platform/TODO-07 §18 (item: "BLS full order + `LoaderFeatures` bit 8")
> **Quality reviewed:** 2026-06-13 | Codex 5x (design, adversarial, consistency, perf, re-adversarial) | 2H+2M fixed, 1H+1M deferred | scope: boot-code-quality

---

## OS Comparison

| ⭐  | Feature                                               | 🪟 Win11                         | 🐧 Linux                            | 🚀 Impossible OS                                                                 |
| --- | ----------------------------------------------------- | -------------------------------- | ----------------------------------- | -------------------------------------------------------------------------------- |
| 💎  | Structured boot entries                               | ✅ BCD store + bcdedit           | ✅ systemd-boot/GRUB BLS            | ⬜ Planned -- §1, §2                                                             |
| 💎  | BootNext one-shot read (firmware-layer)               | ✅ Firmware BootNext + BCD       | ✅ efibootmgr -n                    | ✅ Firmware fields read + OS-side ladder + live ESP store wiring                 |
| 💎  | Recovery boot entry                                   | ✅ WinRE                         | ✅ GRUB recovery                    | ⬜ Planned -- §9                                                                 |
| 💎  | Safe mode entry                                       | ✅ msconfig safeboot flag        | ✅ rescue.target / single           | 🟦 §8 KIND_SAFE + boot_mode=1 + F8 hotkey; subset payload deferred to §13        |
| 💎  | Previous-kernel rollback entry                        | ⚠️ BCD bootsequence (limited UX) | ✅ GRUB previous kernel             | ⬜ Planned -- §10                                                                |
| 💎  | Boot menu UI (text + graphical)                       | ⚠️ Minimal "Choose an OS"        | ✅ GRUB / systemd-boot menus        | ✅ §6 GOP+ConOut+serial; §7 indicators + hotkeys (F8/F10/F11) + hide_when_alone  |
| 💎  | Offline entry editor tooling                          | ✅ bcdedit / bcdboot             | ✅ grub-mkconfig / bootctl          | 🟦 §11 host-side `bootcfg.py` + validator parity; live-boot binary deferred      |
| 💎  | Loop prevention via per-entry try counter             | ⚠️ Recovery loop after 2 fails   | ✅ systemd-boot tries=/done=        | 🟦 BLS counter grammar + ladder gate in §3; crash-tolerant rename in §5          |
| 💎  | Per-entry health-gated mark-good                      | ❌ Implicit (timer past)         | ✅ systemd-bless + boot-complete    | 🟦 §14 entry gate + registry + MarkGood handoff; net/crash checks pending owners |
| 💎  | Entry kinds (UKI, chainload, network, resume)         | ⚠️ BCD osloader / resume (split) | ✅ BLS Type 1 / Type 2 (UKI)        | 🟦 §13 SPLIT/UKI/SAFE validators + loader; CHAINLOAD/NETWORK/RESUME deferred     |
| 💎  | OS-visible loader UEFI variables                      | ❌ None standardized             | ✅ systemd LoaderXxx interface      | ✅ §15 12 LoaderXxx vars under the systemd-boot GUID; honors both OneShot vars   |
| 💎  | First-install bootstrap (idempotent offline seed)     | ✅ bcdboot from install media    | ✅ bootctl install / grub-install   | 🟦 §16 `emit-seed` writes a byte-identical 3-entry default; self-seed TODO-22 §1 |
| ⭐  | Documented deterministic policy precedence (2 layers) | ⚠️ BCD rules underdocumented     | ⚠️ Per-bootloader behavior          | ✅ §3 ladder + 6-example doc + live producer in §4 🚀                            |
| ⭐  | Demote-not-drop bad entries with last_failure_reason  | ❌ Hidden recovery loop          | ⚠️ Bad entries silently sorted last | 🟦 §4 records reject reasons in boot_info v19; menu UI label in §9 🚀            |
| ⭐  | Per-decision audit trail (BlackBox primary)           | ❌ Event log only                | ❌ journalctl scattered             | ✅ §12 JSONL `X:\Boot\history.jsonl` + 256-byte sticky NVRAM ring 🚀             |
| ⭐  | Schema-versioned + CRC-checksummed entry store        | ❌ Binary BCD, no checksum       | ❌ INI / cfg, no checksum           | ✅ §1 schema_version=1 + CRC-32 IEEE 802.3 🚀                                    |
| ⭐  | Per-entry mutation audit (add/remove/reorder logged)  | ❌ Not logged                    | ❌ Not logged                       | ✅ §12 `bootcfg.py --mutation-log` JSONL with requester + prior/new CRC 🚀       |

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
