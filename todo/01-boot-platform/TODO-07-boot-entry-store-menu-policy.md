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
- -> XREF: [`TODO-21-ab-boot-rollback.md §3, §5`](TODO-21-ab-boot-rollback.md) -- A/B slot selection (§3) feeds §6; slot-level `mark_boot_successful` (§5) is the floor §11 layers on top of
- -> XREF: [`TODO-22-recovery-partition.md §1, §7`](TODO-22-recovery-partition.md) -- recovery partition (§1) feeds §6 + §13; recovery UI (§7) consumes §9 audit
- -> XREF: [`TODO-15-visual-post-display.md §3, §4`](TODO-15-visual-post-display.md) -- micro-font + pre-splash renderer used by §4 menu
- -> XREF: [`TODO-10-bare-metal-hardening.md §7`](TODO-10-bare-metal-hardening.md) -- graceful-degradation flags feed §5
- -> XREF: [`TODO-06-boot-media-image-installer-handoff.md §1`](TODO-06-boot-media-image-installer-handoff.md) -- artifact manifest feeds §7 + §13 installer entry
- -> XREF: [`TODO-02-uefi-hardening-secureboot.md §11, §16`](TODO-02-uefi-hardening-secureboot.md) -- UKI artifact + signed payload PE sections (shipped) define §10 `kind: uki`
- -> XREF: [`TODO-25-network-pxe-http-boot.md §7`](TODO-25-network-pxe-http-boot.md) -- network fallback consumes §10 `kind: network`
- -> XREF: [`TODO-26-hibernation-resume-fast-startup-handoff.md §3, §5`](TODO-26-hibernation-resume-fast-startup-handoff.md) -- resume eligibility + handoff consume §10 `kind: resume`
- -> XREF: [`TODO-13-tpm-measured-boot-attestation.md §9`](TODO-13-tpm-measured-boot-attestation.md) -- attestation export carries §3 selected entry id
- -> XREF: [`TODO-11-interrupt-timer-arch.md §1`](TODO-11-interrupt-timer-arch.md) -- pre-APIC clock source for §4 menu countdown
- -> XREF: [`TODO-18-usb-hid-keyboard-mouse.md`](TODO-18-usb-hid-keyboard-mouse.md) -- USB HID handoff for §4 menu input
- -> XREF: [`TODO-27-uefi-advanced.md §1`](TODO-27-uefi-advanced.md) -- UEFI multi-OS chainload contributor consumes §10 `kind: chainload`
- -> XREF: [`../02-kernel-core/TODO-02-kernel-configuration-policy.md §1, §4, §5, §10`](../02-kernel-core/TODO-02-kernel-configuration-policy.md) -- kernel policy merge, control-set, safe-mode, acceptance ledger feed §11

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
| 💎  |   2   | Boot entry parser and validator                        | §1                                      |  [ ]   |
| 💎  |   3   | Boot policy merge order (firmware vs OS layer split)   | §2, T01 §1, T05 §6                      |  [ ]   |
| 💎  |   4   | Text and graphical boot menu                           | §2, T15 §3, T15 §4, T11 §1              |  [ ]   |
| 💎  |   5   | Safe mode, test mode, and diagnostics entries          | §3, T10 §7                              |  [ ]   |
| 💎  |   6   | A/B and recovery entry integration                     | §3, T21 §3, T22 §1                      |  [ ]   |
| 💎  |   7   | Previous-kernel and known-good entries                 | §2, §11, T06 §1                         |  [ ]   |
| 💎  |   8   | Boot entry editor tooling                              | §1-§7                                   |  [ ]   |
| ⭐  |   9   | Policy audit (BlackBox primary, NVRAM exceptional)     | §3, §6                                  |  [ ]   |
| 💎  |  10   | Entry kinds: split, UKI, chainload, network, resume    | §1, §2, T02 §11, T26 §3, T25 §7, T27 §1 |  [ ]   |
| 💎  |  11   | Per-entry health-gated mark-good                       | §3, §6, T21 §5, D02 T02 §10             |  [ ]   |
| 💎  |  12   | OS-visible loader UEFI variables                       | §3, §6, §10                             |  [ ]   |
| 💎  |  13   | Bootstrap and first-install entry seeding              | §1, §6, §10, T22 §1, T06 §1             |  [ ]   |
| 💎  |  14   | Boot entry tests                                       | §1-§13                                  |  [ ]   |

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

---

## 2. Boot Entry Parser and Validator

Bootloader-side parser. Must be heap-free (UEFI pre-EBS), bounded, and reject hostile input without halting boot.

- [ ] Parse entries with a fixed-size arena (no `AllocatePool` per entry); cap at 64 entries.
- [ ] Validate envelope: paths under `\EFI\ImpossibleOS\` or `\EFI\BOOT\` (or trusted-roots list), unique IDs, schema version recognized, total-size cap (16 KiB), CRC matches.
- [ ] Reject entries that point outside allowed ESP/recovery paths unless `flags.trusted_chainload` is set AND firmware Secure Boot is on.
- [ ] Per-kind validation hook (defined in §10) is called after envelope passes; kind-specific rejection codes propagate to §3.
- [ ] On any parse failure: emit serial reason, fall back to the in-firmware default entry, set `boot_info.selection_reason = ENTRY_STORE_INVALID`.
- [ ] Commit: `"boot: parse boot entry store"`

**Test checkpoint:** Parser fixture suite (valid / duplicate-id / bad-CRC / oversize / path-escape / unknown-kind) all classified correctly; serial log shows `boot-entries: parsed N entries` or `boot-entries: rejected (<reason>)`. Test on: host fixture run + QEMU WHPX + TCG.

---

## 3. Boot Policy Merge Order

Two distinct layers, deterministically composed. **Firmware layer**: UEFI `BootCurrent` / `BootNext` / `BootOrder` are read-only inputs -- the firmware has already deleted `BootNext` before our bootloader runs (UEFI 2.10 §3.1.5), so we observe the *result* via `BootCurrent`, never override it. **OS layer**: once an Impossible OS `Boot####` has been selected by firmware, this section picks the internal entry id from the store. Win11's BCD merge and Linux's per-loader rules conflate these layers, which makes precedence opaque -- documenting the split is where Impossible OS earns ⭐.

- [ ] Capture firmware-layer provenance into `boot_info.firmware_boot_provenance`: `BootCurrent`, observed-or-absent `BootNext`, `BootOrder[0..]`. Read-only, diagnostics only.
- [ ] Map `BootCurrent` -> internal entry id via `Boot####` `OptionalData` tagged blob (`IPOS\1` + entry-id); unknown `Boot####` falls through to store default with `UNKNOWN_BOOTCURRENT` reason.
- [ ] Define OS-layer precedence: hotkey > watchdog rollback (BlackBox sticky, §9) > A/B try-state > recovery request > store default (BootNext is a soft hint here) > fallback. Worked examples in `docs/boot/boot-policy.md`.
- [ ] Persist `boot_info.selected_entry_id`, `boot_info.selection_reason`, and the rejected/skipped entries list with reason codes for diagnostics.
- [ ] Loop prevention: per-entry `tries_left`/`tries_done` in the store (BLS-style atomic CoW rename, NOT NVRAM). Decrement pre-handoff; `tries_left=0` -> demoted but visible (§6).
- [ ] Document precedence + worked examples (BootCurrent unknown, watchdog overrides A/B, A/B fail + recovery, store-invalid fallback) in `docs/boot/boot-policy.md`.
- [ ] Commit: `"boot: define boot entry policy precedence"`

**Test checkpoint:** Six precedence scenarios from `docs/boot/boot-policy.md` produce the documented selection on QEMU; serial log shows reason code per case. `boot_info.selection_reason` matches expected enum value. `firmware_boot_provenance` records `was_bootnext=1` when QEMU sets `BootNext`; OS layer does not claim to "override" it. Test on: QEMU WHPX + TCG; bare metal once §4 menu lands.

---

## 4. Text and Graphical Boot Menu

User-visible selector. GOP for graphical, serial+text for headless, both first-class -- a kiosk hosts must be selectable without keyboard.

- [ ] Render menu via GOP when handle is available; fall back to UEFI text protocol + serial mirror otherwise.
- [ ] Reuse [`TODO-15 §3`](TODO-15-visual-post-display.md) micro-font and [`TODO-15 §4`](TODO-15-visual-post-display.md) pre-splash renderer; do not duplicate font assets.
- [ ] Countdown timer source: PIT or HPET via [`TODO-11 §1`](TODO-11-interrupt-timer-arch.md) UTS HAL; LAPIC is not yet calibrated when this menu renders.
- [ ] Keyboard navigation via firmware `SIMPLE_TEXT_INPUT` first; later USB HID handoff ([`TODO-18`](TODO-18-usb-hid-keyboard-mouse.md)) takes over once shipped.
- [ ] Show indicators: Secure Boot state, measured boot status, active slot, recovery available, network entries, last-failure reason for any demoted entry (§6 demote-not-drop UX).
- [ ] Hotkeys: F8 safe-mode, F10 firmware setup, F11 boot menu force, Esc exit; document the table in `docs/boot/boot-menu.md`.
- [ ] Honor `flags.hide_when_alone` -- single-entry stores skip the menu and boot directly (Win11/Linux parity).
- [ ] Commit: `"boot: add boot entry menu"`

**Test checkpoint:** Menu renders with 4+ entries, countdown ticks, arrow-key navigation works, Enter selects. Serial mirror shows menu when GOP unavailable. Single-entry store boots directly. Test on: QEMU WHPX (GOP), TCG (serial fallback), VirtualBox, bare metal -- VM behavior differs and bare metal must pass before this section ships.

---

## 5. Safe Mode, Test Mode, and Diagnostics Entries

Convert today's ad-hoc `boot.conf` booleans into structured entries with explicit flags.

- [ ] Represent safe mode as `kind: safe` entry with explicit `safe_mode_subset` field (minimal / network / cmd) rather than a `boot.conf` switch.
- [ ] Add a `kind: test` entry that names the `TEST_CAT_*` suite (`mm` / `fs` / `boot` / etc.) instead of running all tests.
- [ ] Add `kind: diagnostics` entry that enables verbose POST + extended boot logging.
- [ ] On selection, `boot_config` is materialized from the entry's flag set; existing kernel consumers see the same `boot_config` they read today (no consumer churn).
- [ ] Cross-domain wiring: [`TODO-10 §7`](TODO-10-bare-metal-hardening.md) and [`02-kernel-core/TODO-02 §5`](../02-kernel-core/TODO-02-kernel-configuration-policy.md) consume safe-mode entry flags and populate `KUSER_SHARED_DATA.SafeBootMode`.
- [ ] Commit: `"boot: structured safe/test/diagnostic entries"`

**Test checkpoint:** Selecting safe-mode entry yields `boot_config.safe_mode == 1` and serial shows `safe-mode active`; KUSD `SafeBootMode` non-zero post-boot. Test-mode entry runs the matching `TEST_CAT_*` suite. Diagnostics entry enables verbose POST. Test on: QEMU WHPX + TCG; bare metal.

---

## 6. A/B and Recovery Entry Integration

Generate entries from runtime state; the user sees a single coherent menu instead of two parallel mechanisms. Demote-not-drop on failure.

- [ ] Generate entries for slot A, slot B, recovery, and fallback kernel from [`TODO-21 §3`](TODO-21-ab-boot-rollback.md) slot metadata and [`TODO-22 §1`](TODO-22-recovery-partition.md) recovery layout.
- [ ] Merge A/B success/failure counters into entry display labels (`Slot A (current, 0 failures)`, `Slot B (try 2/3)`).
- [ ] Auto-select recovery on double-fail; if recovery partition is also corrupt, fall through to fallback default with `selection_reason = ALL_PATHS_BAD`.
- [ ] Demote-not-drop: bad entries stay visible greyed out with `last_failure_reason` label so users see WHY a slot is bad. ⭐ vs Linux BLS silent-sort-last.
- [ ] Display rollback reason in the menu line and feed it to VPD.
- [ ] Commit: `"boot: integrate A/B and recovery entries"`

**Test checkpoint:** Corrupt slot A kernel; reboot 3 times; menu shows `Slot A (try 3/3, kernel CRC fail)` greyed out and auto-selects slot B. Both slots fail -> recovery selected without user input. Recovery partition corrupt + both slots fail -> fallback default with ALL_PATHS_BAD reason. Test on: QEMU TCG (deterministic); bare metal.

---

## 7. Previous-Kernel and Known-Good Entries

After an update, the previous kernel stays available until the new kernel is confirmed good. Health gate (§11) is the gating signal, not a wall-clock timer.

- [ ] Persist last-known-good kernel path, manifest digest, and slot id in the entry store.
- [ ] Insert "Previous kernel (known-good)" entry post-update; remove it ONLY after [`§11`](#11-per-entry-health-gated-mark-good) marks the new kernel "good" (not slot-level `mark_boot_successful`).
- [ ] Interlock with code integrity (TODO-13 measured boot) and [`TODO-06 §1`](TODO-06-boot-media-image-installer-handoff.md) manifest format -- entry digest must match the manifest digest.
- [ ] Failed health gate or panic on the new kernel auto-promotes the previous-kernel entry; the new entry's `last_failure_reason` shows why.
- [ ] Manifest-missing fallback: unreadable manifest -> `MANIFEST_UNREADABLE` reason, refuse to retire previous-kernel entry.
- [ ] Commit: `"boot: previous-kernel known-good entries"`

**Test checkpoint:** Apply update -> reboot -> menu shows new + previous. Boot new + health gate passes -> reboot -> previous-kernel entry retired. Boot new and panic 3x -> previous-kernel entry auto-selected. Manifest deleted post-update -> previous-kernel stays around. Test on: QEMU TCG; bare metal.

---

## 8. Boot Entry Editor Tooling

Online and offline editors so operators are not stuck hex-editing the JSON.

- [ ] Add `bootcfg.exe` user-mode tool for list / add / remove / set-default / set-bootnext-hint / set-oneshot / dump-history.
- [ ] Add offline image-edit mode that operates on a mounted ESP for CI and release tooling (no kernel boot required).
- [ ] All writes go through the §2 validator before persisting; reject invalid stores rather than corrupting the file. Atomic CoW file replacement (write `bootentries.json.new` then rename).
- [ ] Update install/release scripts to use `bootcfg.exe --offline --seed` for ESP seeding (consumed by §13 bootstrap).
- [ ] Document command surface in `docs/boot/bootcfg.md`.
- [ ] Commit: `"tools: boot entry editor"`

**Test checkpoint:** `bootcfg list` shows current entries; `bootcfg set-default <id>` round-trips through reboot. Offline mode rejects an entry that points outside ESP. Atomic write survives a simulated power-cut between rename steps. Test on: live boot + Linux host offline edit.

---

## 9. Policy Audit Trail and Rollback Reason Codes

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

## 10. Entry Kinds: Split, UKI, Chainload, Network, Resume

The §1 envelope only carries the discriminator; this section owns the per-kind validators, payload-loading paths, and the integration with neighboring TODOs that produce/consume each kind. Without this section, the schema is a paper exercise -- §1 supports a flag-based "kernel + initrd" model that cannot represent the UKI artifact already shipped in [`TODO-02 §11`](TODO-02-uefi-hardening-secureboot.md), or the resume / network / chainload cases owned by neighboring TODOs.

- [ ] `kind: split` -- legacy split-payload entry. Validator: `kernel`/`initrd[]` paths exist, `cmdline` ASCII, `root` is slot id or partition GUID. Loader: existing split-path code in `bootx64.c`.
- [ ] `kind: uki` -- single PE under `\EFI\Linux\` or `\EFI\ImpossibleOS\`; consumes shipped UKI fast path ([`TODO-02 §11, §16`](TODO-02-uefi-hardening-secureboot.md)). Sets `BOOT_FLAG_INVOKED_VIA_UKI`. No disk-side cmdline.
- [ ] `kind: chainload` -- non-IPOS UEFI app ([`TODO-27 §1`](TODO-27-uefi-advanced.md)); requires `trusted_chainload` + Secure Boot. Loader: UEFI `LoadImage`/`StartImage`.
- [ ] `kind: network` -- HTTP/TFTP target ([`TODO-25 §7`](TODO-25-network-pxe-http-boot.md)); requires `url`+`asset_digest` (sha256). Verify digest before exec.
- [ ] `kind: resume` -- hibernation snapshot ([`TODO-26 §3, §5`](TODO-26-hibernation-resume-fast-startup-handoff.md)); digest must match §1 metadata. Loader sets `boot_info.resume_payload_*`.
- [ ] `kind: recovery`/`installer`/`safe`/`diagnostics`/`test` -- defer to §5 + §6 + §13 for production rules; this section just enforces validators exist and unknown kinds are rejected.
- [ ] Per-kind hook table `g_entry_kind_handlers[ENTRY_KIND_*]` in `bootx64.c` (`validate`/`prepare`/`boot` callbacks); new kinds appended.
- [ ] Document each kind's required + optional fields in `docs/boot/boot-entry-schema.md`.
- [ ] Commit: `"boot: per-entry-kind validators and load paths"`

**Test checkpoint:** Each kind has a matching parser-fixture test (valid + 2 rejection cases). Selecting a `kind: uki` entry sets `BOOT_FLAG_INVOKED_VIA_UKI` and reaches the kernel without disk-side cmdline override. Selecting a `kind: chainload` entry under Secure Boot without `trusted_chainload` is rejected. `kind: resume` digest mismatch falls through to the next priority entry per §3. Test on: QEMU WHPX + TCG (UKI), TCG (network sim, chainload), bare metal once §4 menu ships.

---

## 11. Per-Entry Health-Gated Mark-Good

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

## 12. OS-Visible Loader UEFI Variables

Publish the live entry list, capability bitmap, selected entry, and one-shot overrides as systemd-boot-compatible `LoaderXxx` UEFI variables under vendor UUID `4a67b082-0a4c-41cf-b6c7-440b29bb8c4f` so userspace tooling (and ported third-party tools like `bootctl`-equivalents) does not need to re-parse the on-disk store. Without this, dynamic entries (A/B-generated, recovery-generated, network-discovered, chainload) are invisible to userspace until the on-disk store is regenerated. Linux parity (`https://systemd.io/BOOT_LOADER_INTERFACE/`).

- [ ] Publish read-only LoaderXxx vars (bootloader -> OS) per `docs/boot/loader-vars.md`: entries, selected, device, timeout, init/exec time, image+firmware identity, features bitmap.
- [ ] Honor OS-written one-shot vars: `LoaderEntryOneShot`, `LoaderConfigTimeoutOneShot`. Bootloader reads + deletes after consumption.
- [ ] Define `LoaderFeatures` capability bits 0-5 (one-shot honors, entries publish, tries publish, kinds, audit). Additive; new feature -> new bit. Table in `docs/boot/loader-vars.md`.
- [ ] Variables written via `gRT->SetVariable` after boot-info publish, before kernel handoff; failure sets `boot_info.loader_vars_degraded` and continues boot.
- [ ] Document compatibility in `docs/boot/loader-vars.md`: a Linux user-mode tool reading the systemd-boot interface format Just Works against an Impossible OS-booted system.
- [ ] Commit: `"boot: OS-visible loader UEFI variables"`

**Test checkpoint:** Post-boot, `efivar -l | grep Loader` (or equivalent) shows all read-only vars; values match `boot_info`. Setting `LoaderEntryOneShot` to a known entry id then rebooting causes that entry to boot once, then revert to the store default. `LoaderFeatures` advertises the bits implemented. NVRAM-quota-full harness sets `loader_vars_degraded` without aborting boot. Test on: QEMU WHPX + TCG; bare metal.

---

## 13. Bootstrap and First-Install Entry Seeding

Who creates the FIRST default entry on a freshly-installed system? Who creates the recovery entry on the first boot after the recovery partition first appears? This is missing today: §8 covers list/add/remove and §6 generates from runtime state, but the bootstrap path is implicit. Idempotent offline seeding closes the gap.

- [ ] `bootcfg.exe --offline --seed` is idempotent: deterministic id generation from `(machine-id, kind, slot)` tuples; second run produces byte-identical store.
- [ ] Installer integration: 15-installer-release release script invokes `bootcfg --offline --seed` after artifacts + recovery partition land; produces 3-entry default (slot-A, slot-B, recovery).
- [ ] First-boot self-seed: missing store + recovery partition present + known-good slot -> synthesize 3-entry default atomically. Handles wiped-ESP recovery.
- [ ] Image build path: `scripts/release/` invokes `bootcfg --offline --seed --image=<vhdx>` so .vhdx/.iso/.raw artifacts ship with seeded store; CI verifies via §2 parser.
- [ ] Document bootstrap order + ownership boundary (installer vs first-boot vs CI) in `docs/boot/bootstrap.md`.
- [ ] Commit: `"boot: bootstrap and first-install entry seeding"`

**Test checkpoint:** Run `bootcfg --offline --seed` twice on the same mounted ESP; resulting `bootentries.json` is byte-identical. Delete `bootentries.json` from a known-good install; reboot; first-boot self-seed reconstructs it without user intervention. CI image builds verify seeded store passes §2 validation. Test on: host (offline) + QEMU WHPX (first-boot self-seed) + bare metal.

---

## 14. Boot Entry Tests

Unit + scenario tests so regressions surface in CI, not on a user's laptop.

- [ ] Parser fixture tests for valid + every invalid case enumerated in §2 + per-kind cases from §10.
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
| 💎  | BootNext one-shot read (firmware-layer)                | ✅ Firmware BootNext + BCD        | ✅ efibootmgr -n                      | ⬜ Planned -- §3         |
| 💎  | Recovery boot entry                                    | ✅ WinRE                          | ✅ GRUB recovery                      | ⬜ Planned -- §6         |
| 💎  | Safe mode entry                                        | ✅ msconfig safeboot flag         | ✅ rescue.target / single             | ⬜ Planned -- §5         |
| 💎  | Previous-kernel rollback entry                         | ⚠️ BCD bootsequence (limited UX)  | ✅ GRUB previous kernel               | ⬜ Planned -- §7         |
| 💎  | Boot menu UI (text + graphical)                        | ⚠️ Minimal "Choose an OS"         | ✅ GRUB / systemd-boot menus          | ⬜ Planned -- §4         |
| 💎  | Offline entry editor tooling                           | ✅ bcdedit / bcdboot              | ✅ grub-mkconfig / bootctl            | ⬜ Planned -- §8         |
| 💎  | Loop prevention via per-entry try counter              | ⚠️ Recovery loop after 2 fails    | ✅ systemd-boot tries=/done=          | ⬜ Planned -- §3, §6     |
| 💎  | Per-entry health-gated mark-good                       | ❌ Implicit (timer past)          | ✅ systemd-bless + boot-complete      | ⬜ Planned -- §11        |
| 💎  | Entry kinds (UKI, chainload, network, resume)          | ⚠️ BCD osloader / resume (split)  | ✅ BLS Type 1 / Type 2 (UKI)          | ⬜ Planned -- §10        |
| 💎  | OS-visible loader UEFI variables                       | ❌ None standardized              | ✅ systemd LoaderXxx interface        | ⬜ Planned -- §12        |
| 💎  | First-install bootstrap (idempotent offline seed)      | ✅ bcdboot from install media     | ✅ bootctl install / grub-install     | ⬜ Planned -- §13        |
| ⭐  | Documented deterministic policy precedence (2 layers)  | ⚠️ BCD rules underdocumented      | ⚠️ Per-bootloader behavior            | ⬜ Planned -- §3 🚀      |
| ⭐  | Demote-not-drop bad entries with last_failure_reason   | ❌ Hidden recovery loop           | ⚠️ Bad entries silently sorted last   | ⬜ Planned -- §3, §6 🚀  |
| ⭐  | Per-decision audit trail (BlackBox primary)            | ❌ Event log only                 | ❌ journalctl scattered               | ⬜ Planned -- §9 🚀      |
| ⭐  | Schema-versioned + CRC-checksummed entry store         | ❌ Binary BCD, no checksum        | ❌ INI / cfg, no checksum             | ⬜ Planned -- §1 🚀      |
| ⭐  | Per-entry mutation audit (add/remove/reorder logged)   | ❌ Not logged                     | ❌ Not logged                         | ⬜ Planned -- §9 🚀      |

> **After §1-§13:** Impossible OS matches Windows 11 and Linux on structured entries, BootNext provenance, recovery, safe mode, previous-kernel rollback, menu UX, offline tooling, loop prevention, health-gated mark-good, entry kinds, OS-visible loader vars, and first-install bootstrap.
> **After §3 + §6 + §9 + §1 ⭐ rows:** Impossible OS surpasses both with documented two-layer precedence, demote-not-drop UX, per-decision audit, schema+CRC store, and per-entry mutation audit.

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
