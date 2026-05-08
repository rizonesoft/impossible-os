---
schema_version: 1
id: boot-entry-store-menu-policy
domain: 01-boot-platform
status: active
title: "TODO-07 -- Boot Entry Store, Menu & Policy"
---

# TODO-07 -- Boot Entry Store, Menu & Policy

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

| ⭐  | Order | Deliverable                                            | Depends On                              | Status |
| --- | :---: | ------------------------------------------------------ | --------------------------------------- | :----: |
| 💎  |   1   | Boot entry file format                                 | --                                      |  [x]   |
| 💎  |   2   | Boot entry parser and validator                        | §1                                      |  [x]   |
| 💎  |   3   | Boot policy merge order (firmware vs OS layer split)   | §2, T01 §1, T05 §6                      |  [x]   |
| 💎  |   4   | ESP store read + Boot#### OptionalData + policy wiring | §3                                      |  [x]   |
| 💎  |   5   | Crash-tolerant counter protocol + v19 ABI validator    | §3, §4                                  |  [x]   |
| 💎  |   6   | Boot menu renderer + countdown + input infrastructure  | §4, T15 §3, T15 §4                      |  [x]   |
| 💎  |   7   | Boot menu indicators + hotkeys + hide_when_alone       | §6                                      |  [x]   |
| 💎  |   8   | Safe mode, test mode, and diagnostics entries          | §3, T10 §7                              |  [/]   |
| 💎  |   9   | A/B and recovery entry integration (DEFERRED)          | §3, §4, T21 §1+§3+§4, T22 §1+§2         |  [ ]   |
| 💎  |  10   | Previous-kernel and known-good entries                 | §2, §14, T06 §1                         |  [ ]   |
| 💎  |  11   | Boot entry editor tooling                              | §1-§10                                  |  [ ]   |
| ⭐  |  12   | Policy audit (BlackBox primary, NVRAM exceptional)     | §3, §4, §9                              |  [ ]   |
| 💎  |  13   | Entry kinds: split, UKI, chainload, network, resume    | §1, §2, T02 §11, T26 §3, T25 §7, T27 §1 |  [ ]   |
| 💎  |  14   | Per-entry health-gated mark-good                       | §3, §5, §9, T21 §5, D02 T02 §10         |  [ ]   |
| 💎  |  15   | OS-visible loader UEFI variables                       | §3, §4, §9, §13                         |  [ ]   |
| 💎  |  16   | Bootstrap and first-install entry seeding              | §1, §9, §13, T22 §1, T06 §1             |  [ ]   |
| 💎  |  17   | Boot entry tests                                       | §1-§16                                  |  [ ]   |

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
- [ ] Dirty-rectangle repaint for `boot_menu_render()`: draw title once, repaint only changed rows on selection moves + footer once per second.
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
> **Deferred:** [M] Dirty-rectangle repaint for `boot_menu_render` (full-band redraw on 4K GOP causes key-repeat jank) -> XREF: 01-boot-platform/TODO-07 §6 (item: "Dirty-rectangle repaint for `boot_menu_render()`" at line 232)
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
- [ ] `kind: test` admission + `test_suite` payload parsing deferred to §13 (per-kind handler reads payload; without it every test entry would run all categories).
- [ ] `kind: diagnostics` admission deferred to §13 + an early-policy-read redesign (`boot_config.verbose` is consumed pre-policy).
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

- [ ] Generate entries for slot A, slot B, recovery, fallback. Blocked: TODO-21 §1+§3, TODO-22 §1.
- [ ] Merge A/B success/failure counters into entry labels. Blocked: TODO-21 §1+§4.
- [ ] Auto-select recovery on double-fail with `BOOT_SELECTION_FALLBACK_ALL_PATHS_BAD`. Blocked: TODO-21 §4 + TODO-22 §2; sentinel enum value lands when TODO-22 §2 ships.
- [ ] Demote-not-drop visual (greyed-out + `last_failure_reason` label). Partially unblocked: §6+§8 keep TRIES_EXHAUSTED rows visible; §7 adds `[FAIL]` indicator. The greyed style + label string remains §9 scope.
- [ ] Display rollback reason in menu line + feed to VPD. Blocked: TODO-21 §6 (rollback-reason source) + VPD label hooks.
- [ ] Widen `supported_kinds_mask` to include `BOOT_ENTRY_KIND_RECOVERY`. Blocked: TODO-22 §2 (recovery load path).
- [ ] Commit: `"boot: integrate A/B and recovery entries"`

**Test checkpoint:** Corrupt slot A kernel; reboot 3 times; menu shows `Slot A (try 3/3, kernel CRC fail)` greyed out and auto-selects slot B. Both slots fail -> recovery selected without user input. Recovery partition corrupt + both slots fail -> fallback default with ALL_PATHS_BAD reason. Test on: QEMU TCG (deterministic); bare metal. **Cannot run today** -- requires TODO-21 + TODO-22 producers.

---

## 10. Previous-Kernel and Known-Good Entries

After an update, the previous kernel stays available until the new kernel is confirmed good. Health gate (§14) is the gating signal, not a wall-clock timer.

- [ ] Persist last-known-good kernel path, manifest digest, and slot id in the entry store.
- [ ] Insert "Previous kernel (known-good)" entry post-update; remove it ONLY after [`§14`](#14-per-entry-health-gated-mark-good) marks the new kernel "good" (not slot-level `mark_boot_successful`).
- [ ] Interlock with code integrity (TODO-13 measured boot) and [`TODO-06 §1`](TODO-06-boot-media-image-installer-handoff.md) manifest format -- entry digest must match the manifest digest.
- [ ] Failed health gate or panic on the new kernel auto-promotes the previous-kernel entry; the new entry's `last_failure_reason` shows why.
- [ ] Manifest-missing fallback: unreadable manifest -> `MANIFEST_UNREADABLE` reason, refuse to retire previous-kernel entry.
- [ ] Commit: `"boot: previous-kernel known-good entries"`

**Test checkpoint:** Apply update -> reboot -> menu shows new + previous. Boot new + health gate passes -> reboot -> previous-kernel entry retired. Boot new and panic 3x -> previous-kernel entry auto-selected. Manifest deleted post-update -> previous-kernel stays around. Test on: QEMU TCG; bare metal.

---

## 11. Boot Entry Editor Tooling

Online and offline editors so operators are not stuck hex-editing the JSON.

- [ ] Add `bootcfg.exe` user-mode tool for list / add / remove / set-default / set-bootnext-hint / set-oneshot / dump-history.
- [ ] Add offline image-edit mode that operates on a mounted ESP for CI and release tooling (no kernel boot required).
- [ ] All writes go through the §2 validator before persisting; reject invalid stores rather than corrupting the file. Atomic CoW file replacement (write `bootentries.json.new` then rename).
- [ ] Update install/release scripts to use `bootcfg.exe --offline --seed` for ESP seeding (consumed by §16 bootstrap).
- [ ] Document command surface in `docs/boot/bootcfg.md`.
- [ ] Commit: `"tools: boot entry editor"`

**Test checkpoint:** `bootcfg list` shows current entries; `bootcfg set-default <id>` round-trips through reboot. Offline mode rejects an entry that points outside ESP. Atomic write survives a simulated power-cut between rename steps. Test on: live boot + Linux host offline edit.

---

## 12. Policy Audit Trail and Rollback Reason Codes

Per-decision audit. Disk-first (BlackBox JSONL) -- NVRAM is exceptional-only because per-boot writes wear flash and BLS rationale (`https://uapi-group.org/specifications/specs/boot_loader_specification/#why-not-use-efi-variables-for-storing-the-boot-counter`) explicitly rejects EFI vars for high-frequency state. ⭐ vs Win11 (event log only) and Linux (journalctl scattered).

- [ ] Define reason codes (UNKNOWN_BOOTCURRENT, WATCHDOG_ROLLBACK, AB_FAIL, RECOVERY, STORE_INVALID, FALLBACK_DEFAULT, MANIFEST_UNREADABLE, ALL_PATHS_BAD, etc.) in `include/boot/boot_audit_codes.h`.
- [ ] Per-boot record -> BlackBox JSONL (`X:\BlackBox\boot\history.jsonl`, append-only, 4 MiB rotated). Schema in `docs/boot/boot-history-schema.md`.
- [ ] NVRAM ring (`ImpossibleOS-BootSticky`, single 256-byte var) is exceptional-only: LAST_OUTCOME bit, recovery-trigger sticky, watchdog-rollback request that survived a reset. NOT per-boot history.
- [ ] Per-entry mutation audit: `bootcfg.exe` writes log every add/remove/reorder/edit to `X:\BlackBox\boot\mutations.jsonl` with requester + prior-state. ⭐ neither Win11 nor Linux logs entry-store mutations.
- [ ] On NVRAM-write failure (quota / wear / locked): set `boot_info.audit_degraded`, degrade to BlackBox-only with serial WARN; never block boot.
- [ ] Feed the boot diagnostics page (TODO-14) and the recovery UI ([`TODO-22 §7`](TODO-22-recovery-partition.md)).
- [ ] Commit: `"boot: audit boot entry policy decisions"`

**Test checkpoint:** Force watchdog rollback -> NVRAM sticky bit set + BlackBox JSONL gains a record with `reason_code=WATCHDOG_ROLLBACK`; subsequent boots see the sticky flag and auto-recover. Per-boot records always go to BlackBox even when NVRAM is full (synthetic-quota harness). `bootcfg history` returns matching entries. Test on: QEMU WHPX + TCG; bare metal.

---

## 13. Entry Kinds: Split, UKI, Chainload, Network, Resume

The §1 envelope only carries the discriminator; this section owns the per-kind validators, payload-loading paths, and the integration with neighboring TODOs that produce/consume each kind. Without this section, the schema is a paper exercise -- §1 supports a flag-based "kernel + initrd" model that cannot represent the UKI artifact already shipped in [`TODO-02 §11`](TODO-02-uefi-hardening-secureboot.md), or the resume / network / chainload cases owned by neighboring TODOs.

- [ ] `kind: split` -- legacy split-payload entry. Validator: `kernel`/`initrd[]` paths exist, `cmdline` ASCII, `root` is slot id or partition GUID. Loader: existing split-path code in `bootx64.c`.
- [ ] `kind: uki` -- single PE under `\EFI\Linux\` or `\EFI\ImpossibleOS\`; consumes shipped UKI fast path ([`TODO-02 §11, §16`](TODO-02-uefi-hardening-secureboot.md)). Sets `BOOT_FLAG_INVOKED_VIA_UKI`. No disk-side cmdline.
- [ ] `kind: chainload` -- non-IPOS UEFI app ([`TODO-27 §1`](TODO-27-uefi-advanced.md)); requires `trusted_chainload` + Secure Boot. Loader: UEFI `LoadImage`/`StartImage`.
- [ ] `kind: network` -- HTTP/TFTP target ([`TODO-25 §7`](TODO-25-network-pxe-http-boot.md)); requires `url`+`asset_digest` (sha256). Verify digest before exec.
- [ ] `kind: resume` -- hibernation snapshot ([`TODO-26 §3, §5`](TODO-26-hibernation-resume-fast-startup-handoff.md)); digest must match §1 metadata. Loader sets `boot_info.resume_payload_*`.
- [ ] `kind: recovery`/`installer`/`safe`/`diagnostics`/`test` -- defer to §8 + §9 + §16 for production rules; this section just enforces validators exist and unknown kinds are rejected.
- [ ] Per-kind hook table `g_entry_kind_handlers[ENTRY_KIND_*]` in `bootx64.c` (`validate`/`prepare`/`boot` callbacks); new kinds appended.
- [ ] Wire `load_kernel()` to consume `decision->selected.payload_*` so the §4 ladder's chosen entry drives the kernel/cmdline/initrd actually loaded, not just `selected_entry_id` recording.
- [ ] Document each kind's required + optional fields in `docs/boot/boot-entry-schema.md`.
- [ ] Widen `supported_kinds_mask` in `boot_policy_invoke()` to include CHAINLOAD / NETWORK / RESUME once their per-kind handlers land.
- [ ] Commit: `"boot: per-entry-kind validators and load paths"`

**Test checkpoint:** Each kind has a matching parser-fixture test (valid + 2 rejection cases). Selecting a `kind: uki` entry sets `BOOT_FLAG_INVOKED_VIA_UKI` and reaches the kernel without disk-side cmdline override. Selecting a `kind: chainload` entry under Secure Boot without `trusted_chainload` is rejected. `kind: resume` digest mismatch falls through to the next priority entry per §3. Test on: QEMU WHPX + TCG (UKI), TCG (network sim, chainload), bare metal once §4 menu ships.

---

## 14. Per-Entry Health-Gated Mark-Good

Bootloader marks try-down (decrement `tries_left`); userspace marks good ONLY after a configurable health check passes. Layered ABOVE [`TODO-21 §5`](TODO-21-ab-boot-rollback.md) (slot-level `mark_boot_successful`) and [`02-kernel-core/TODO-02 §10`](../02-kernel-core/TODO-02-kernel-configuration-policy.md) (boot acceptance ledger) -- those ship the foundations; this section adds entry-level gating + configurable health checks. Linux parity (systemd-bless-boot.service after `boot-complete.target`, plus `systemd-boot-check-no-failures.service` and Fedora greenboot's required-vs-wanted model). Without this gate, a boot that reaches the kernel but fails required services / safe-mode policy / resume validation can retire the previous-kernel entry and poison rollback.

- [ ] Add syscall + helper API: `mark_entry_successful(entry_id)` -- calls TODO-21 §5 slot-level mark + removes per-entry tries counter from store.
- [ ] Define health-check registry: required (must all pass) and wanted (logged, non-blocking) checks; each check returns `OK` / `SOFT_FAIL` / `HARD_FAIL`.
- [ ] Default required checks: kernel reached desktop-ready, no `klog(LOG_ERR, ...)` from `boot_*` subsystems, no triggered panic.
- [ ] Default wanted checks: network reachable, X:\ mountable, no service crash in first 60s.
- [ ] `boot_health_check_run()` from desktop-ready in `boot_desktop.c`: OK -> `mark_entry_successful()`; HARD_FAIL -> entry stays indeterminate.
- [ ] Configurable per-entry: `health_check_subset` field can override required/wanted lists (e.g. recovery skips network-reachable).
- [ ] User-mode tool: `bootcfg health` prints last health check report from BlackBox.
- [ ] Document model + check registration API in `docs/boot/boot-health.md`.
- [ ] Commit: `"boot: per-entry health-gated mark-good"`

**Test checkpoint:** Boot reaches desktop, all required checks pass -> entry marked good (tries counter removed from store). Boot reaches desktop but kernel logged `klog(LOG_ERR, ...)` from a boot subsystem -> entry stays indeterminate, tries_left already decremented, next reboot will retry. Force `HARD_FAIL` on the network check (a wanted check) -> entry still marked good. Test on: QEMU WHPX + TCG; bare metal.

---

## 15. OS-Visible Loader UEFI Variables

Publish the live entry list, capability bitmap, selected entry, and one-shot overrides as systemd-boot-compatible `LoaderXxx` UEFI variables under vendor UUID `4a67b082-0a4c-41cf-b6c7-440b29bb8c4f` so userspace tooling (and ported third-party tools like `bootctl`-equivalents) does not need to re-parse the on-disk store. Without this, dynamic entries (A/B-generated, recovery-generated, network-discovered, chainload) are invisible to userspace until the on-disk store is regenerated. Linux parity (`https://systemd.io/BOOT_LOADER_INTERFACE/`).

- [ ] Publish read-only LoaderXxx vars (bootloader -> OS) per `docs/boot/loader-vars.md`: entries, selected, device, timeout, init/exec time, image+firmware identity, features bitmap.
- [ ] Honor OS-written one-shot vars: `LoaderEntryOneShot`, `LoaderConfigTimeoutOneShot`. Bootloader reads + deletes after consumption.
- [ ] Define `LoaderFeatures` capability bits 0-5 (one-shot honors, entries publish, tries publish, kinds, audit). Additive; new feature -> new bit. Table in `docs/boot/loader-vars.md`.
- [ ] Variables written via `gRT->SetVariable` after boot-info publish, before kernel handoff; failure sets `boot_info.loader_vars_degraded` and continues boot.
- [ ] Document compatibility in `docs/boot/loader-vars.md`: a Linux user-mode tool reading the systemd-boot interface format Just Works against an Impossible OS-booted system.
- [ ] Extract SMBIOS table 1 (System Information) UUID pre-EBS, expose via `boot_info`, and wire `boot_policy_inputs.local_machine_id` to it so machine-pinned `machine_id` entries become selectable.
- [ ] Commit: `"boot: OS-visible loader UEFI variables"`

**Test checkpoint:** Post-boot, `efivar -l | grep Loader` (or equivalent) shows all read-only vars; values match `boot_info`. Setting `LoaderEntryOneShot` to a known entry id then rebooting causes that entry to boot once, then revert to the store default. `LoaderFeatures` advertises the bits implemented. NVRAM-quota-full harness sets `loader_vars_degraded` without aborting boot. Test on: QEMU WHPX + TCG; bare metal.

---

## 16. Bootstrap and First-Install Entry Seeding

Who creates the FIRST default entry on a freshly-installed system? Who creates the recovery entry on the first boot after the recovery partition first appears? This is missing today: §11 covers list/add/remove and §9 generates from runtime state, but the bootstrap path is implicit. Idempotent offline seeding closes the gap.

- [ ] `bootcfg.exe --offline --seed` is idempotent: deterministic id generation from `(machine-id, kind, slot)` tuples; second run produces byte-identical store.
- [ ] Installer integration: 15-installer-release release script invokes `bootcfg --offline --seed` after artifacts + recovery partition land; produces 3-entry default (slot-A, slot-B, recovery).
- [ ] First-boot self-seed: missing store + recovery partition present + known-good slot -> synthesize 3-entry default atomically. Handles wiped-ESP recovery.
- [ ] Image build path: `scripts/release/` invokes `bootcfg --offline --seed --image=<vhdx>` so .vhdx/.iso/.raw artifacts ship with seeded store; CI verifies via §2 parser.
- [ ] Widen `supported_kinds_mask` in `boot_policy_invoke()` to include INSTALLER once installer entries drive a distinct installer-image load path AND offline + first-install seeding is complete.
- [ ] Document bootstrap order + ownership boundary (installer vs first-boot vs CI) in `docs/boot/bootstrap.md`.
- [ ] Commit: `"boot: bootstrap and first-install entry seeding"`

**Test checkpoint:** Run `bootcfg --offline --seed` twice on the same mounted ESP; resulting `bootentries.json` is byte-identical. Delete `bootentries.json` from a known-good install; reboot; first-boot self-seed reconstructs it without user intervention. CI image builds verify seeded store passes §2 validation. Test on: host (offline) + QEMU WHPX (first-boot self-seed) + bare metal.

---

## 17. Boot Entry Tests

Unit + scenario tests so regressions surface in CI, not on a user's laptop.

- [ ] Parser fixture tests for valid + every invalid case enumerated in §2 + per-kind cases from §13.
- [ ] QEMU boot menu timeout/default-selection scenario.
- [ ] Firmware-vs-OS layer separation scenario (§3): firmware sets `BootNext` to an unknown `Boot####`; bootloader logs `UNKNOWN_BOOTCURRENT` and falls through to in-store default.
- [ ] A/B rollback entry-selection scenario across 1-fail, 2-fail, 3-fail boundaries (demote-not-drop UX preserved).
- [ ] Recovery auto-select scenario (both slots failed); ALL_PATHS_BAD scenario (recovery also corrupt).
- [ ] Per-entry health-gate scenario: kernel reaches desktop with klog ERR -> entry stays indeterminate; clean boot -> entry marked good.
- [ ] Entry-kind dispatch scenario: `kind: uki` boots via UKI fast path, `kind: chainload` rejected without Secure Boot when `trusted_chainload` unset.
- [ ] Loader UEFI variable scenario: post-boot `LoaderEntries`, `LoaderEntrySelected`, `LoaderFeatures` readable; `LoaderEntryOneShot` honored once.
- [ ] Bootstrap idempotency scenario: `bootcfg --offline --seed` twice yields byte-identical store.
- [ ] BlackBox-vs-NVRAM audit scenario: per-boot history goes to BlackBox JSONL; NVRAM-full harness still allows boot with `audit_degraded` set.
- [ ] `bootcfg.exe` round-trip scenario (offline + online).
- [ ] Commit: `"test: boot entry store, kinds, policy, health, and variables"`

**Test checkpoint:** All fixtures pass under `make test-boot`; all QEMU scenarios complete without manual intervention. Bare-metal pass on at least one test laptop. Test on: host fixtures + QEMU WHPX + TCG; bare metal.

---

## OS Comparison

| ⭐  | Feature                                                | 🪟 Win11                          | 🐧 Linux                              | 🚀 Impossible OS         |
| --- | ------------------------------------------------------ | --------------------------------- | ------------------------------------- | ------------------------ |
| 💎  | Structured boot entries                                | ✅ BCD store + bcdedit            | ✅ systemd-boot/GRUB BLS              | ⬜ Planned -- §1, §2     |
| 💎  | BootNext one-shot read (firmware-layer)                | ✅ Firmware BootNext + BCD        | ✅ efibootmgr -n                      | ✅ Firmware fields read + OS-side ladder + live ESP store wiring |
| 💎  | Recovery boot entry                                    | ✅ WinRE                          | ✅ GRUB recovery                      | ⬜ Planned -- §9         |
| 💎  | Safe mode entry                                        | ✅ msconfig safeboot flag         | ✅ rescue.target / single             | 🟦 §8 KIND_SAFE admitted + boot_mode=1 materialized; F8 hotkey wired; subset payload deferred to §13 |
| 💎  | Previous-kernel rollback entry                         | ⚠️ BCD bootsequence (limited UX)  | ✅ GRUB previous kernel               | ⬜ Planned -- §10        |
| 💎  | Boot menu UI (text + graphical)                        | ⚠️ Minimal "Choose an OS"         | ✅ GRUB / systemd-boot menus          | ✅ §6 GOP+ConOut+serial; §7 indicators ([SB]/[REC]/[NET]/[FAIL]) + hotkeys (F8/F10/F11) + hide_when_alone |
| 💎  | Offline entry editor tooling                           | ✅ bcdedit / bcdboot              | ✅ grub-mkconfig / bootctl            | ⬜ Planned -- §11        |
| 💎  | Loop prevention via per-entry try counter              | ⚠️ Recovery loop after 2 fails    | ✅ systemd-boot tries=/done=          | 🟦 BLS counter grammar + ladder gate ship in §3; crash-tolerant bootloader rename in §5 |
| 💎  | Per-entry health-gated mark-good                       | ❌ Implicit (timer past)          | ✅ systemd-bless + boot-complete      | ⬜ Planned -- §14        |
| 💎  | Entry kinds (UKI, chainload, network, resume)          | ⚠️ BCD osloader / resume (split)  | ✅ BLS Type 1 / Type 2 (UKI)          | ⬜ Planned -- §13        |
| 💎  | OS-visible loader UEFI variables                       | ❌ None standardized              | ✅ systemd LoaderXxx interface        | ⬜ Planned -- §15        |
| 💎  | First-install bootstrap (idempotent offline seed)      | ✅ bcdboot from install media     | ✅ bootctl install / grub-install     | ⬜ Planned -- §16        |
| ⭐  | Documented deterministic policy precedence (2 layers)  | ⚠️ BCD rules underdocumented      | ⚠️ Per-bootloader behavior            | ✅ §3 ladder + 6-example doc + live producer in §4 🚀 |
| ⭐  | Demote-not-drop bad entries with last_failure_reason   | ❌ Hidden recovery loop           | ⚠️ Bad entries silently sorted last   | 🟦 §4 records reject reasons in boot_info v19; menu UI label in §9 🚀 |
| ⭐  | Per-decision audit trail (BlackBox primary)            | ❌ Event log only                 | ❌ journalctl scattered               | ⬜ Planned -- §12 🚀     |
| ⭐  | Schema-versioned + CRC-checksummed entry store         | ❌ Binary BCD, no checksum        | ❌ INI / cfg, no checksum             | ✅ §1 schema_version=1 + CRC-32 IEEE 802.3 🚀 |
| ⭐  | Per-entry mutation audit (add/remove/reorder logged)   | ❌ Not logged                     | ❌ Not logged                         | ⬜ Planned -- §12 🚀     |

> **After §1-§16:** Impossible OS matches Windows 11 and Linux on structured entries, BootNext provenance, recovery, safe mode, previous-kernel rollback, menu UX (renderer §6 + indicators §7), offline tooling, loop prevention (ladder §3 + crash-tolerant decrement §5), health-gated mark-good, entry kinds, OS-visible loader vars, and first-install bootstrap.
> **After §3 + §9 + §12 + §1 ⭐ rows:** Impossible OS surpasses both with documented two-layer precedence, demote-not-drop UX, per-decision audit, schema+CRC store, and per-entry mutation audit.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_boot_entry()` -- register in `src/kernel/test/test_runner.c` under `TEST_CAT_BOOT`.
> Tests run with `debug=1` or `test=1` in `boot.conf`.

- [ ] `test_boot_entry_parse_valid` -- minimal valid store yields the expected entry count and id list.
- [ ] `test_boot_entry_rejects_duplicate_id` -- store with two entries sharing an id returns parse error.
- [ ] `test_boot_entry_rejects_bad_crc` -- corrupted checksum byte rejected.
- [ ] `test_boot_entry_rejects_path_escape` -- entry pointing outside `\EFI\ImpossibleOS\` rejected unless `trusted_chainload` + Secure Boot on.
- [ ] `test_boot_entry_rejects_unknown_kind` -- entry with `kind: future_kind` rejected with logged reason.
- [ ] `test_boot_policy_layer_separation` -- firmware sets `BootNext` to unknown `Boot####`; OS layer sees `UNKNOWN_BOOTCURRENT`, falls through to in-store default.
- [ ] `test_boot_policy_watchdog_overrides_os_layer` -- watchdog rollback flag (BlackBox sticky) beats OS-layer A/B selection (firmware-layer `BootNext` is NOT under OS control).
- [ ] `test_boot_policy_ab_rollback_selection` -- 3-fail slot demoted (visible with `last_failure_reason`), alternate slot selected.
- [ ] `test_boot_policy_loop_prevention` -- entry with `tries_left=0` demoted, policy re-runs.
- [ ] `test_entry_kind_uki_first_class` -- `kind: uki` entry sets `BOOT_FLAG_INVOKED_VIA_UKI` and reaches kernel without disk-side cmdline override.
- [ ] `test_entry_kind_chainload_secure_boot_gate` -- `kind: chainload` without `trusted_chainload` + Secure Boot rejected.
- [ ] `test_entry_kind_resume_digest_mismatch` -- `kind: resume` with bad digest falls through to next priority.
- [ ] `test_health_gate_blocks_premature_mark_good` -- kernel reaches desktop but health-check `HARD_FAIL` -> entry NOT marked good.
- [ ] `test_health_gate_userspace_signal` -- after passing health, `mark_entry_successful(id)` removes the tries counter from the store.
- [ ] `test_loader_variables_published` -- `LoaderEntries` / `LoaderEntrySelected` / `LoaderFeatures` readable post-boot; values match `boot_info`.
- [ ] `test_loader_one_shot_consumed` -- `LoaderEntryOneShot` honored once, then deleted.
- [ ] `test_bootstrap_idempotent_offline_seed` -- `bootcfg --offline --seed` twice yields byte-identical `bootentries.json`.
- [ ] `test_bootstrap_first_boot_self_seed` -- missing `bootentries.json` + present recovery partition + known-good slot -> first-boot synthesis.
- [ ] `test_audit_blackbox_primary_nvram_exceptional` -- per-boot record lands in BlackBox JSONL; NVRAM-quota-full harness still boots with `audit_degraded` set.
- [ ] `test_audit_mutation_log` -- `bootcfg add/remove/reorder` writes a mutation record with requester + prior-state.
- [ ] Create `src/kernel/test/test_boot_entry.c`; register via `test_register_boot_entry()`.
- [ ] Author / extend `scripts/debug/kernel/run-boot-tests.bat` (already exists) so the new tests run in the boot suite.
- [ ] Commit: `"test: add boot entry test suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] QEMU boot menu keyboard selection (GOP).
- [ ] QEMU firmware-vs-OS layer separation: firmware `BootNext` to unknown `Boot####`, bootloader falls through to OS-layer default with `UNKNOWN_BOOTCURRENT` reason.
- [ ] A/B rollback auto-selection after 3-fail threshold; demoted slot stays visible with `last_failure_reason`.
- [ ] UKI / chainload / network / resume kinds each dispatch through their per-kind handler.
- [ ] Health gate blocks mark-good when kernel logs `LOG_ERR`; passes mark-good on clean boot.
- [ ] LoaderXxx UEFI variables present post-boot; one-shot honored once.
- [ ] Bootstrap idempotent offline seed yields byte-identical store on repeat runs.
- [ ] Bare-metal menu over GOP and serial fallback.
- [ ] `make test-boot` passes (boot suite includes the new TODO-07 tests).
- [ ] Commit: `"boot: TODO-07 entry store + kinds + policy + health + loader vars + bootstrap complete"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 20 suites pending implementation
