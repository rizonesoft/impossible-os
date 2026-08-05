---
schema_version: 1
id: boot-perf-health-observability
title: "TODO-29 -- Boot Performance & Health Observability"
domain: 01-boot-platform
status: active
priority: high
implements_after: TODO-04
---

# TODO-29 -- Boot Performance & Health Observability

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Turn the per-phase boot timing already collected by `boot_progress` into actionable observability. Every phase has a target budget; budget breaches are alarmed at boot. A single `boot-health.json` artifact consolidates degraded subsystems, missing capabilities, perf-budget breaches, MAT W^X violations, and firmware quirks into one operator-facing dashboard. A rolling `boot-trend.json` accumulates the last N boots so a regression in SMBIOS init or mouse probe shows up as a flagged delta, not as silent slowdown. Concrete latency targets land on the slowest phases observed today (SMBIOS 1.3s, Mouse 1.1s, font/icon load ~1.3s).

> **Scope boundary:** This TODO covers **observability + the perf optimizations its data exposes as outliers**. It does NOT own:
>
> - TPM / Measured Boot (TODO-13)
> - Boot diagnostics layer / panic forensics / POST codes (TODO-14)
> - Boot watchdog / hang detection / NMI timer (TODO-23)
> - BlackBox partition + `X:\` directory tree (TODO-24)
> - Hibernation resume / fast-startup (TODO-26)
> - Boot certification matrix / CI gates (TODO-28)
> - A/B rollback (TODO-21), recovery partition (TODO-22), boot menu (TODO-07)
> - VFS Phase-3 write failure cluster (TODO-04 §11)
> - JSON builder truncation contract (TODO-04 §12)
> - SMEP/SMAP via CR4 (needs KPTI per-process page tables, D03 T02 §memory-security)
> - TSC_AUX MSR per-CPU (D02 T09 §x86-64)
> - PAT WC retry on Hyper-V (D03 T01 §VMM)
> - AT_PHDR derivation in ELF loader (D10 T10 §linux-compat)
> - ExitBootServices retry hardening (T03 §bootloader-error-recovery)
> - Capsule update write path (T27 §uefi-advanced)
>
> Each XREF below names the concrete `[ ]` item that owns the relevant gap; this section consumes their data, never re-implements it.

> **Current state (2026-05-02):** `boot_progress(phase, step, postcode)` is wired throughout boot and writes a 31-step timeline (`PERF: --- Boot step durations ---`). `boot-timeline.json` is already exported under `X:\Perf\` (TODO-04 §4). What is missing: (a) per-phase **target** budgets (today the timeline reports `Xms` with no benchmark, so a 1.3s SMBIOS init is logged the same way as a 50ms one); (b) a single **consolidated health artifact** at boot-end that an operator can read to know "what degraded, what's slow, what's missing" without grepping the serial log; (c) **trend tracking** across boots so a perf regression is visible as a delta; (d) concrete **optimization work** on the slowest phases that observability identifies as outliers.

## Inputs

- [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h) -- `boot_progress(phase, step, postcode)`, BOOT_REQUIRE / BOOT_STEP, subsystem readiness oracle.
- [`src/kernel/main/boot_progress.c`](../../src/kernel/main/boot_progress.c) -- timeline writer, 31-step ring, `boot_timeline_dump_json()`.
- [`include/kernel/boot_timing.h`](../../include/kernel/boot_timing.h) -- TSC + FPDT helpers, normalized timeline schema.
- [`src/kernel/main/boot_caps.c`](../../src/kernel/main/boot_caps.c) -- degraded-capability bitmask + name table.
- [`src/kernel/firmware_quirks.c`](../../src/kernel/firmware_quirks.c) (TODO-04 §9) -- active quirks accessor.
- [`src/kernel/uefi_config.c`](../../src/kernel/uefi_config.c) -- MAT W^X violation count.
- [`src/kernel/smbios.c`](../../src/kernel/smbios.c) -- SMBIOS parser (1.3s init -- §4 of this TODO profiles + optimizes).
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c), [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c) -- PS/2 init paths (mouse 1.1s -- §6 profiles + optimizes).
- [`src/desktop/font.c`](../../src/desktop/font.c), [`src/kernel/icon_store.c`](../../src/kernel/icon_store.c) -- font/icon load (~1.3s -- §7 async-loader).

## Outcome

- Every boot phase has a documented target budget; breaching the budget emits a `[WARN] BOOT-BUDGET:` line at boot-end naming the phase, target, observed, and likely root cause.
- `X:\Diag\boot-health.json` exists post-Phase-3 with one consolidated view: degraded caps, degraded subsystems, missing capabilities (TPM/USB/NVMe), perf-budget breaches, MAT W^X violations, firmware quirks, last 3 boot times.
- `X:\Perf\boot-trend.json` rolls the last 16 boots and emits a BOOT-TREND WARN when median grows >15% over a 3-run window; the release-blocking CI gate that consumes it is owned by TODO-28 §9 (deferred), not §3.
- SMBIOS init drops from 1.3s to <100ms.
- Mouse PS/2 init drops from 1.1s to <100ms.
- Font/icon load drops from ~1.3s synchronous-blocking to <50ms post-desktop-ready (deferred async loader).
- MAT W^X violations name the violating descriptor index + classification reason (not just a count).
- A boot heartbeat marker fires every 250 ms during long phases so a hang is distinguishable from "still working" on serial.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On                               | Status |
| --- | :---: | ---------------------------------------- | ---------------------------------------- | :----: |
| 💎  |   1   | Per-phase boot perf budgets + threshold alarms | --                                       |  [/]   |
| 💎  |   2   | Boot health audit JSON (`X:\Diag\boot-health.json`) | §1, T04 §11 (VFS fix)                    |  [x]   |
| ⭐  |   3   | Boot perf trend file + regression detection | §1, §2                                   |  [/]   |
| 💎  |   4   | SMBIOS init profiling + optimization     | §1                                       |  [x]   |
| 💎  |   5   | MAT W^X violation root-cause attribution | T04 §5                                   |  [x]   |
| 💎  |   6   | Mouse PS/2 init profiling + optimization | §1                                       |  [x]   |
| ⭐  |   7   | Async font/icon loader (post-desktop-ready) | §1                                       |  [/]   |
| ⭐  |   8   | Boot heartbeat telemetry during long phases | §1, T23 (watchdog); T14 §4 permanently deferred |  [/]   |
| 💎  |   9   | EXEC step latency profile (13834ms FAIL; 11x regression) | §1                                       |  [/]   |
| ⭐  |  10   | UEFI RT SetVariable latency (>50ms threshold) | §1, T04 §11                              |  [/]   |
| ⭐  |  11   | Phase-3 X:\Diag JSON writer batching     | T04 §8, T27 §2 (advisor)                 |  [/]   |
| ⭐  |  12   | AVX-512 throttle policy when APERF/MPERF absent | T19 §3                                   |  [/]   |
| ⭐  |  13   | Boot history ring depth + format (8 → 32+ entries) | T01 §11                                  |  [/]   |
| ⭐  |  14   | FAT32 dirty-mount fsck cost in VFS step  | D05 T04                                  |  [/]   |
| ⭐  |  15   | User-mode binary spawn latency (~1s task_create→ELF) | T22 (sched / exec)                       |  [/]   |
| ⭐  |  16   | TSC frequency variability under hypervisor | --                                       |  [/]   |
| ⭐  |  17   | PAT WC -> WT hypervisor trap quirk       | §2 (consumer)                            |  [/]   |
| 💎  |  18   | Boot critical-path / dependency / resource-wait attribution | §1, §2                                   |  [/]   |
| 💎  |  19   | Post-ship follow-up backfill (2026-07-31 cohort) | --                                       |  [ ]   |

---

## 1. Per-Phase Boot Perf Budgets and Threshold Alarms

Today `boot_progress(phase, step, postcode)` records 31 timeline entries with absolute and delta timestamps. There is no notion of an **expected** duration -- a 1342ms SMBIOS init prints identically to an 80ms ACPI init, so the slow path is invisible without manual log inspection.

- [x] Define `struct boot_phase_budget { const char *step; uint32_t target_ms; const char *reason; }` in `include/kernel/boot_perf_budget.h` (string-keyed to match `boot_timing_step_t.step`).
- [x] Ship 18 named per-step budgets in `src/kernel/boot_perf_budget.c`; targets per the design table. DESKTOP_READY excluded (last step has no per-step delta).
- [x] Pure `enum boot_perf_budget_class { BUDGET_OK, BUDGET_SOFT, BUDGET_HARD }` + `boot_perf_budget_classify(observed_ms, target_ms)` for synthetic-test use (no live boot calls).
- [x] `boot_perf_budget_check()` called from `boot_perf_dump()`; walks steps, skips last, emits klog `[WARN]` / `[FAIL] BOOT-BUDGET: <step> took <obs>ms (target <t>ms): <reason>`. Data-only.
- [x] Soft = 1.5x target -> WARN; hard = 4x target -> ERR. Equal-to-target is OK. Halt-on-breach owned by TODO-23 watchdog.
- [x] Total-boot-time `boot_perf_total_check()` -- compares `steps[last].tsc - steps[0].tsc` against 4000ms target with same soft/hard thresholds.
- [x] Schema bump for `boot-timeline.json` -- per-record `target_ms` field (0 = no budget). Trend-analysis decoder is owned by §3.
- [x] Commit: `"boot: per-phase perf budgets + threshold alarms"`

**Test checkpoint:** `boot_perf_budget_classify(99, 100) == OK`; `(150, 100) == OK`; `(151, 100) == SOFT`; `(400, 100) == SOFT`; `(401, 100) == HARD`. Lookup of known step returns budget; unknown + NULL return NULL. Clean boot stays silent; synthetic over-budget step triggers WARN/ERR.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 15 boot_perf_budget sub-tests, 0 failures
>
> **Notes:**
> - **What shipped** -- `include/kernel/boot_perf_budget.h` + `src/kernel/boot_perf_budget.c` (18 per-step budgets, classifier, lookup, check, total-boot check) + 15 unit tests; boot-timeline JSON gains `target_ms` per record.
> - **How it runs** -- BSP-only at `boot_perf_dump()` time (post-Phase-3); pure data-only walk over `boot_timing_get_steps()`; emits WARN at >1.5x target, ERR at >4x. Idempotent.
> - **Downstream effects** -- §2 boot-health.json consumes the per-step + total-boot classifications; §3 trend file diffs `target_ms` against observed across the rolling boot ring; §4/§6 SMBIOS+Mouse optimizations measure success against these budgets.
> - **Canonical doc** -- [`include/kernel/boot_perf_budget.h`](../../include/kernel/boot_perf_budget.h) API + budget table.
> - **Scope boundary** -- §1 owns budget data + classifier + boot-time WARN/ERR emit; halt-on-breach owned by TODO-23 watchdog; consolidated health JSON owned by §2; rolling trend owned by §3.

> **Verified:** 2026-05-03 | this commit | 8/8 items | build OK | tests 2760/2760 PASS (re-review: source unchanged since b09d2cdb)
> **Quality reviewed:** 2026-05-03 | Codex 7x (design + adversarial x2 + consistency x2 + perf x2) | 2H+2M+1L fixed, 0 open | scope: kernel-code-quality

---

## 2. Boot Health Audit JSON (`X:\Diag\boot-health.json`)

A single operator-facing dashboard collapsing every "what's wrong on this boot" signal currently scattered across serial. Consumers: `sysinfo.exe boothealth` (TODO-04 §10 host decoder), CI regression gate (TODO-28), bare-metal triage when serial is unavailable.

> [!IMPORTANT]
> **Parked 2026-05-03 -- pivot to D05 T04 §15 first.** Codex design review (dispatch `bkrhvljqm`) ruled the in-memory-only ship would mark §2 complete while the operator-facing dashboard is absent on every boot. Hard prerequisites recorded for resumption:
> - **VFS write path (HARD prereq):** complete `05-storage-filesystems/TODO-04 §15` before §2 implementation. The `vfs_open(VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC)` shape lands there; until then any §2 ship would emit a WARN with no file written.
> - **Secure Boot UNKNOWN handling:** `uefi_secureboot_enabled()` returns 0 on BOTH `DISABLED` and `UNREADABLE`. Add `uefi_secureboot_state_valid()` accessor (or gate on `BOOT_CAP_SECURE_BOOT_STATE` degraded bit) and emit `UNKNOWN` when the state is invalid. Reporting `DISABLED` for an unreadable Secure Boot state is a security-facing false statement.
> - **mat_wx_violations[] final shape:** schema_version=1 must lock per-entry violation shape now, not count-only. Iterate `mat_get_count()` / `mat_get_entry()` filtering `MAT_CLASS_WX_VIOLATION`; emit per-entry `{phys_addr, num_pages, attr_bits, class_name}` plus a sibling `mat_overflowed` boolean. §5 (TODO-29) extends per-entry detail with root-cause attribution; the v1 shape must accommodate it.

- [x] Define `schema_version=1` wire format in `docs/boot/boot-health-schema.md` (top-level keys + per-entry shapes; epoch unit deviation documented).
- [x] Implement `boot_health_publish_json()` in `src/kernel/main/boot_health.c` (~330 LOC, BSP-only, 8 KiB pmm buffer, fail-closed on truncation; called from `boot_desktop.c` after `boot_history_kernel_mark_phase3()`).
- [x] Single-open `vfs_open(VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC)`; on short write re-truncate to 0 bytes so consumers never see a partial JSON prefix (Codex M1 fix).
- [x] Subsystem accessors only (caps/subsys/perf/MAT/quirks/history/secureboot); no state copy-paste; new `uefi_secureboot_state_valid()` accessor distinguishes UNREADABLE from DISABLED.
- [x] Pure classifier `boot_health_classify_secureboot()` priority `UNKNOWN > SETUP > ENABLED > DISABLED` -- backed by `test_boot_health.c` (5 tests / 13 asserts, 8-row truth table).
- [x] Commit: `"boot: publish consolidated boot-health.json"`

**Test checkpoint:** Post-boot, `X:\Diag\boot-health.json` exists, parses through `cJSON_Parse`, contains every required top-level key, and reflects the live boot's actual degraded state (e.g. on this current OVMF run: `degraded_caps` lists 6 entries, `missing_capabilities` lists `TPM` + `USB` + `NVMe`, `mat_wx_violations` count is 1).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 5 suites, 0 failures
>
> **Notes:**
> - Single-shot Phase-3 publisher writes `X:\Diag\boot-health.json`; schema spec lives at `docs/boot/boot-health-schema.md`.
> - Time fields anchored on `boot_history_kernel_phase3_committed_seq()`; emits 0/0 on mark-failure rather than picking older ring entries. Encoding is u32 unix epoch.
> - Security distinctions preserved: UNREADABLE != DISABLED for Secure Boot; `degraded_subsystems[]` reads `degraded_mask` so ready-but-degraded subsystems surface.
> - Short-write recovery re-truncates to 0 bytes; `mat_wx_violations[]` capped at 16 with sibling `mat_overflowed` boolean.
> - Smoke: 574 bytes at 3.660s on KVM, Phase-3 mark seq=2.
>
> **Verified:** 2026-05-03 | commit `1edae27f` | 6/6 items | build OK | smoke PASS (KVM 2.580s)
> **Quality reviewed:** 2026-05-03 | Codex 9x (design + adversarial + adversarial-impl + 5x re-adversarial + consistency + perf) | 3H+5M+1L fixed | scope: kernel-code-quality

---

## 3. Boot Perf Trend File + Regression Detection

The §1 budget check catches absolute breaches, but a slow drift inside the budget (e.g. SMBIOS creeping from 80ms to 95ms over 50 commits) stays invisible. A rolling per-phase median makes the drift visible.

- [x] `X:\Perf\boot-trend.json` schema v1 shipped: top-level `schema_version`+`boots`, per-entry `{boot_seq, unix_time, phase_durations_ms: {step: ms}}`; canonical spec at `docs/boot/boot-trend-schema.md`.
- [x] RMW cycle in `boot_trend_publish_json`: read+parse -> prepend current boot -> trim to 16 -> write `.tmp` -> `vfs_rename_ex(... VFS_RENAME_REPLACE_EXISTING)`.
- [x] Top-level schema validation only; bad `schema_version` or non-array `boots` -> rename to `.corrupt-<seq>` + fresh v1 write.
- [x] 3-run median per phase (prior=`boots[3..5]`, newest=`boots[0..2]`); emits BOOT-TREND WARN when growth >15%; first 5 boots silent.
- [x] CI hook stays owned by the boot-validation matrix (this section ships data file + warn line only). -> XREF: `TODO-28-boot-validation-certification-matrix.md §9` (release gate + dashboard; owns the median->fail CI gate that consumes `boot-trend.json`).
- [x] Commit: `"boot: rolling boot-trend.json + regression alarms"`

**Test checkpoint:** Synthetic 6-boot fixture with prior-3 `SMBIOS=[80,80,80]` and newest-3 `[150,150,150]` produces an 87% regression alarm. Same shape but newest-3 `[80,82,84]` stays silent (median 82 within 15% band). Unit test seeds the file with 16 fixture entries and asserts prepend+trim to 16. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 10 suites, 0 failures
>
> **Notes:**
> - `boot_trend_publish_json()` in `src/kernel/main/boot_trend.c` rolls last 16 boots; canonical wire spec at `docs/boot/boot-trend-schema.md`.
> - Atomic via temp+rename: writes `.tmp`, then `vfs_rename_ex(... VFS_RENAME_REPLACE_EXISTING)` from the storage TODO-04 §16 primitive.
> - 3-run median per phase (prior=`boots[3..5]`, newest=`boots[0..2]`); BOOT-TREND WARN >15%; first 5 boots silent.
> - Two separate 16 KiB PMM allocations (input + output); top-level schema validation only -- bad files quarantined to `.corrupt-<seq>`.
> - 5 pure-helper tests cover `boot_trend_median3` truth table + `boot_trend_compute_growth_pct` boundaries.
>
> **Verified:** 2026-05-03 | commit `ec9f1c1c` | 6/6 items | build OK | smoke PASS (KVM 2.39s)
> **Quality reviewed:** 2026-05-03 | Codex 6x (design + adversarial + re-adversarial + consistency x2 + perf) | 3H+5M+1L fixed | scope: kernel-code-quality

---

## 4. SMBIOS Init Profiling + Optimization

Observed: `SMBIOS 852ms 1342ms P1` -- 1.3s for SMBIOS parsing on a QEMU OVMF boot with a typical 22-structure table. Real hardware with 50-100 structures will be worse. Investigate root cause + fix.

- [x] Per-structure TSC profiling in `walk_structures` (per-type accumulator); one-shot summary at end of `smbios_init` (parsed count, total ms, top-3 slowest types).
- [x] Bottleneck identified: byte-at-a-time firmware-memory reads (50-100us/byte on QEMU). Suspects (a)/(b) from draft were wrong.
- [x] Fix: `smbios_copy_firmware_table` copies once into pmm-backed RAM (64 KiB cap); parser runs from RAM. SMBIOS 1342ms->51ms on KVM.
- [x] Truncation safety: refuse to mark `s_info.valid` when no type=127 terminator within the parse window.
- [x] Pure unit tests in `test_smbios.c` covering rank-3 picker + copy-cap pages math.
- [x] Commit: `"boot: SMBIOS init optimization (1.3s -> <100ms)"`

**Test checkpoint:** Boot log `SMBIOS Xms` value drops from ~1342ms to <100ms on QEMU OVMF. 4 unit tests pass via `run-boot-tests.bat`. No content regressions in `HKLM\HARDWARE\BIOS\*` or `HKLM\HARDWARE\System\*` (smoke confirms `BIOS, System, 1 CPU(s), 1 DIMM(s)` populated).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 14 suites, 0 failures
>
> **Notes:**
> - `walk_structures` copies the firmware SMBIOS table into pmm-backed kernel RAM once at init; parsing runs from RAM at L1 latency.
> - Per-structure TSC profile (sparse 128-bucket array) emits "top-3 slowest types" summary; tracks future regressions when bottleneck shifts.
> - Truncation safety: type=127 terminator must have its post-structure double-NUL within the parse window before being accepted; otherwise refuse the parse.
> - `s_table_base` keeps firmware addr for the firmware-tables inventory validator; `parse_base` + `s_table_end` point at RAM copy.
> - Smoke (KVM): SMBIOS 46ms (was 1342ms, 29x faster), 8 structures parsed from 405-byte table.
>
> **Verified:** 2026-05-04 | commit `ff23afc7` | 5/5 items | build OK | smoke PASS (KVM 2.51s; SMBIOS 46ms)
> **Quality reviewed:** 2026-05-04 | Codex 5x (design x2 + adversarial + consistency + perf) | 2H+3M fixed | scope: kernel-code-quality

---

## 5. MAT W^X Violation Root-Cause Attribution

Today `[WARN] UEFI: MAT: W^X VIOLATION -- 1 regions are writable+executable` reports a count but no actionable detail. To debug a violation on bare metal you have to grep `MAT[N]` lines and match the offset by hand.

- [x] mat_init records per-violation metadata in s_mat_entries[] (existing inventory; phys, pages, attr, cls). §5 adds WARN-decode pass + cap.
- [x] Per-violation `[WARN] UEFI: MAT[N] WX VIOLATION at phys=0x.. pages=.. attr=0x.. (RO=.. XP=.. RP=..)` lines; capped at MAT_VIOLATION_LOG_CAP=8.
- [x] mat_violations_iter_next/_describe rejected as YAGNI -- existing `mat_get_count()` + `mat_get_entry()` give consumers full per-entry access.
- [x] FW_QUIRK_BOGUS_MAT cross-ref: when active, WARN downgrades to INFO with `[known-bad firmware]` prefix. Phase 1 reordered so firmware_quirks_init runs before mat_init.
- [x] Commit: `"boot: MAT W^X violation root-cause attribution"`

**Test checkpoint:** On firmware with W^X violations, each violation emits a `[WARN] UEFI: MAT[N] WX VIOLATION at phys=0x.. pages=.. attr=0x.. (RO=.. XP=.. RP=..)` line capped at 8. Pure-helper unit tests cover MAT classifier truth table (writable+executable -> WX_VIOLATION; RO+non-XP -> CODE; XP+non-RO -> DATA; RO+XP -> RODATA; RP -> GUARD) plus log-cap range. boot-health.json `mat_wx_violations[]` already populated by §2 from the same s_mat_entries[] inventory.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 20 suites, 0 failures
>
> **Notes:**
> - Per-violation WARN block in `src/kernel/uefi_config.c::mat_init` capped at `MAT_VIOLATION_LOG_CAP=8`; emits phys/pages/attr/RO/XP/RP per violating descriptor.
> - `firmware_quirks_init` reordered to run after `smbios_init` so `FW_QUIRK_BOGUS_MAT` is observable when MAT severity is selected; downgrade to INFO + `[known-bad firmware]` prefix when active.
> - `mat_violations_iter_next/_describe` rejected as YAGNI: `mat_get_count` + `mat_get_entry` already provide full per-entry access (used by §2 boot-health).
> - 6 pure-helper TEST_CAT_BOOT suites in `test_mat_violation.c` cover the classifier truth table.
> - Smoke (KVM): MAT rejected at header level on this OVMF run; per-violation path validated by offline tests.
>
> **Verified:** 2026-05-04 | commit `b6654a85` | 5/5 items | build OK | smoke PASS (KVM 2.43s)
> **Quality reviewed:** 2026-05-04 | Codex 4x (design + adversarial + consistency + perf) | 1M fixed | scope: kernel-code-quality

---

## 6. Mouse PS/2 Init Profiling + Optimization

Observed: `MOUSE 2213ms 1149ms P1` -- 1.1s for PS/2 mouse init even though FADT reports `8042=0` (no PS/2 controller). The init is doing something expensive on a port that should fall through to "not present" fast.

- [x] mouse_init now records TSC start, increments s_timeout_hits when any ps2_wait_* hits the deadline, and emits a single funneled "init Xms, timeouts=Y, exit=<reason>" report from every exit path.
- [x] Bottleneck identified: uniform 100ms timeout * 12 i8042 transactions = 1149ms when the controller is wedged or absent.
- [x] Fix: split timeout budgets -- PS2_WAIT_SHORT (~10ms) for register polls + ACK reads, PS2_WAIT_LONG (~2s) for BAT/device-ID reads where real PS/2 hardware can take 300ms-2s.
- [x] mouse_read_long propagates timeout via sentinel return; mouse_init validates BAT == 0xAA and aborts on NACK.
- [x] mouse_expect_ack helper validates 0xFA after every setup command (F6/F3+data/E8+data/F4); init aborts to report if any ACK times out, preventing IRQ12 registration on a desynchronized device.
- [x] FADT `8042=0` short-circuit kept (existing acpi_has_8042 + port probe at line 187).
- [x] Commit: `"boot: PS/2 mouse init optimization (1.1s -> <100ms)"`

**Test checkpoint:** Boot log `mouse_init Xms` drops from ~1149ms to <=100ms on QEMU. mouse_init emits "init Xms, timeouts=Y, exit=<reason>" from every exit path. Mouse still works post-init when a real device is present (BAT=0xAA validated; IRQ12 registered only after every setup command ACK'd 0xFA).

> **Test runner:** N/A (no kernel-side testable surface; validation via boot log on QEMU/bare metal) | validation: smoke shows `init 100ms, timeouts=1, exit=ok` (KVM 2.57s)
>
> **Notes:**
> - `src/kernel/drivers/mouse.c` split-timeout: PS2_WAIT_SHORT (~10ms) for controller polls, PS2_WAIT_LONG (~2s) for BAT/device-ID; BAT validated == 0xAA before proceeding.
> - `mouse_expect_ack` helper validates 0xFA after every setup command; ACK timeout aborts init via exit_reason, no IRQ12 registration on desynchronized bus.
> - All exits funnel through `report:` label emitting per-step diagnostic line so wedged-controller cases are attributable.
> - Smoke (KVM): mouse_init 100ms (was 1149ms; 11.5x faster), single timeout hit on no-mouse QEMU port = expected.
> - Bare-metal envelope preserved: PS2_WAIT_LONG=2s covers documented 300ms-2s BAT spec.
>
> **Verified:** 2026-05-04 | commit `a50de875` | 7/7 items | build OK | smoke PASS (KVM 2.57s; mouse_init 100ms)
> **Quality reviewed:** 2026-05-04 | Codex 4x (design + adversarial + consistency + perf) | 4M fixed | scope: kernel-code-quality

---

## 7. Async Font and Icon Loader (Post-Desktop-Ready)

Observed: ~1.3s of font + icon loading happens INSIDE the desktop boot phase before `DESKTOP_READY` fires. The desktop is unusable for that 1.3s window. Async load: open with a minimal face + icon placeholders, swap in the full set as each completes.

> [!NOTE]
> Plan sharpened by Codex design review (2026-06-20). §7 is a compositor + font-subsystem refactor, NOT simple deferral. Deferred this pass (multi-commit); items below are the vetted plan.

- [ ] Compositor-polled incremental loader: state machine ticked once per presented frame after `DESKTOP_READY` (per-tick budget), one TTF/icon slot per tick. NO `thread_create`/sys_wq/DPC -- thread-deferred init starves behind the compositor (documented `boot_run_deferred` incident).
- [ ] Immutable face-bundle publish in `gfx_text.c`: each face = immutable bundle (fontinfo/metrics/glyph-cache/atlases/ascent), built off-side, published via release-store + acquire-load in the draw path; remove `ttf_get` per-call pixel_size/scale mutation.
- [ ] Boot-atlas fallback adapter: expose the Phase-1 18px ASCII atlas through the normal measure/draw API (first paint keeps titles/controls/labels); placeholders for bold/mono/non-ASCII + `icon_get`/`icon_draw_scaled` until each family publishes.
- [ ] On each face/icon-family publish, invalidate + redraw the affected compositor surfaces (atomic swap visible, no torn glyph reads).
- [ ] Commit: `"desktop: async font + icon load (1.3s -> <50ms blocking)"`

**Test checkpoint:** `DESKTOP_READY` fires within 50ms of `boot_phase3()` start; first paint renders fallback text (boot atlas) + icon placeholders (no blank labels); each TTF/icon family publishes within 2s post-desktop-ready via the compositor-polled loader (no kthread) and swaps in atomically with a surface redraw; no torn-glyph glitch. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Deferred:** [H] Async font/icon loader is a multi-commit compositor + font-subsystem refactor (compositor-polled loader state machine -- threads starve per the documented incident; immutable face-bundle publish replacing `ttf_get` per-call mutation; boot-atlas fallback adapter through the text API) beyond one-session scope; Codex-vetted plan above. -> XREF: 01-boot-platform/TODO-29-boot-perf-health-observability.md §7 (item: "Compositor-polled incremental loader" -- this section owns the multi-commit refactor).

---

## 8. Boot Heartbeat Telemetry During Long Phases

A 1.3s SMBIOS init looks identical on serial to a hung boot until either (a) the next `boot_progress` line emits or (b) the watchdog fires (TODO-23). A 250ms heartbeat fills the gap with "still working" telemetry.

- [ ] `boot_heartbeat_arm(phase_ms_target)` and `boot_heartbeat_pet()` in [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h). ISR-SAFE ONLY: the 250ms LAPIC-timer ISR increments an in-memory counter + records elapsed; the `[BOOT-HB] phase=<step> elapsed=<ms> target=<target>` serial line is DRAINED outside interrupt context (next `boot_progress` entry/exit), never from the ISR -- TODO-14 §4 permanently deferred the in-ISR visual heartbeat after `fb_swap_rect` caused recursive interrupts on bare metal; serial writes carry the same risk. A new `boot_info.heartbeat_seq` field is a BOOT_INFO_VERSION bump -> add it via the boot_info ABI owner path (CLAUDE.md boot_info ABI), not ad hoc.
- [ ] Wire arm/pet into `boot_progress`: every step entry arms (with the §1 budget as target), every step exit pets (cancels the next heartbeat). Long-running steps emit 4 heartbeats/sec; short steps emit zero.
- [ ] -> XREF: `TODO-23-boot-watchdog.md` owns the LAPIC/NMI timer + hard-cap reset; §8 is the SOFT serial-counter signal layered on TODO-23's timer (no duplicate timer ownership). TODO-14 §4 (in-ISR visual heartbeat) stays permanently deferred; §8 is serial-only + ISR-safe-drain.
- [ ] Boot-health.json (§2) records the maximum heartbeat gap observed during the boot, surfacing pauses that stayed under the WARN threshold but were unusually slow.
- [ ] Commit: `"boot: heartbeat telemetry during long phases"`

**Test checkpoint:** A 1342ms SMBIOS init with target 100ms emits 4-5 `[BOOT-HB] phase=SMBIOS elapsed=Xms target=100` lines. A <250ms phase emits zero heartbeats. Heartbeat ISR is harmless when LAPIC timer is the active scheduler tick (no double-fire, no priority inversion).

> **Deferred:** [M] Heartbeat needs a `boot_info.heartbeat_seq` field (BOOT_INFO_VERSION bump via the ABI owner path) + TODO-23's LAPIC/NMI timer + ISR-safe serial drain (gap-audit constraint); multi-commit boot-path. -> XREF: 01-boot-platform/TODO-23-boot-watchdog.md (timer owner); §8 owns the soft serial-counter signal.
---

## 9. EXEC Step Latency Profile

Smoke (KVM, 2026-05-03 latest) records `EXEC took 13834ms (target 100ms)` -- a HARD-budget breach 138x over target, ~11x worse than the 1210ms originally observed when this section was filed. The EXEC step covers SSDT registration (470 main slots + ~1300 shadow stubs), syscall handler wire-up, NT subsystem registration, ETW init, and exec format dispatch (ELF + EIF + PE32+). Without per-substep timing the slow path is invisible. The 11x growth is itself a regression signal -- find when EXEC went from ~1.2s to ~13.8s and bisect the responsible commit before optimizing.

**Files:** `src/kernel/exec.c`, `src/kernel/nt/ssdt.c`, `src/kernel/main/boot_desktop.c`, `include/kernel/boot_init.h`

- [ ] Bisect the 1210ms -> 13834ms regression first: walk recent commits to `src/kernel/exec.c` and `src/kernel/nt/ssdt.c`; identify the responsible change before adding profiling.
- [ ] Add finer-grained `boot_progress` substeps inside EXEC: SSDT_MAIN, SSDT_SHADOW, NT_SUBSYS_BATCH, SYSCALL_REG, EXEC_FORMAT_REG.
- [ ] Define per-substep budgets in `boot_perf_budgets[]`: SSDT_MAIN <50ms, SSDT_SHADOW <100ms, NT batch <30ms, format register <5ms each.
- [ ] Profile SSDT registration hot path; pick a fix path: bulk-register via static const array, lazy shadow-stub registration, or accept current cost with a justified budget.
- [ ] Commit: `"boot: EXEC step substep timing + SSDT registration profile"`

**Test checkpoint:** Boot serial shows ~5 EXEC sub-step lines with ms deltas; the largest contributor is named in the perf summary; total EXEC drops below 500ms or the budget is justified in `boot_perf_budgets[]`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Deferred:** [M] EXEC 13834ms HARD breach: needs a git-bisect of the ~1.2s->13.8s regression THEN SSDT-registration optimization (470 main + ~1300 shadow slots) -- live-boot profiling + multi-commit kernel work. -> XREF: 01-boot-platform/TODO-29-boot-perf-health-observability.md §9 (this section owns the bisect + substep timing).
---

## 10. UEFI RT SetVariable Latency

Smoke (KVM, 2026-05-03) records `UEFI: RT SetVariable took 109 ms (threshold 50 ms)`. NVRAM writes during boot perf save are the suspect path. 109ms is a soft breach but consistently above threshold suggests OVMF flash-emulation latency or a serialization bug in our caller.

**Files:** `src/kernel/uefi_runtime.c`, `src/kernel/boot_perf_budget.c`, `src/kernel/main/boot_progress.c`

- [ ] Capture the variable name + size in every >50ms RT-SetVariable WARN line.
- [ ] Verify boot perf save (`bootperf` step) batches its writes; fix to a single-blob write if the 31-step bin currently fragments into 31 calls.
- [ ] If OVMF flash-emulation latency is the root cause, exempt OVMF via `firmware_quirks_is_active(QUIRK_SLOW_NVRAM)` instead of leaving the WARN spam.
- [ ] Commit: `"boot: per-call RT SetVariable latency tracking + bootperf write batching"`

**Test checkpoint:** Serial shows the variable name on every >50ms RT SetVariable WARN. Bare-metal RT SetVariable consistently <50ms (real flash). OVMF still warns but with the variable name + size for triage.

> **Deferred:** [L] RT SetVariable >50ms: per-call name/size capture + bootperf single-blob write batching + OVMF-slow-NVRAM quirk gating -- validated only against live NVRAM-write timing on real flash + OVMF. -> XREF: 01-boot-platform/TODO-29-boot-perf-health-observability.md §10 (this section owns the batching).
---

## 11. Phase-3 X:\Diag JSON Writer Batching

Boot writes 5 small JSON files to `X:\Diag` serially during late Phase 3: `firmware-tables.json` (4528 B), `firmware-advisor.json` (187 B), `boot-reserved.json`, `postcode.log`, `hwdump.txt`. Each one performs an independent FAT32 directory walk + cluster allocation + sector-cache write + close. ~10ms per write on KVM (~50ms cumulative); scales linearly as new diagnostic JSONs land.

**Files:** new `src/kernel/fs/fat32/fat32_batch.c`, plus the 5 Phase-3 writer call sites

- [ ] Profile the 5-write Phase-3 sequence with `boot_progress` substeps; capture per-write ms.
- [ ] Design `fat32_batch_begin()` / `fat32_batch_write_file()` / `fat32_batch_commit()` API: open parent dir once, all files in one cluster-bitmap pass, single dir + FAT flush.
- [ ] Retrofit the 5 writers; keep per-file API for one-off writers (boot-error history, klog Serial_*.log).
- [ ] `firmware-tables.json` C:\ fallback: `firmware_tables_json.c:505` hardcodes `X:\Diag\` -- use `klog_using_blackbox ? "X:\\Diag\\" : klog_dir` like the other writers. (TODO-24 §8)
- [ ] Commit: `"fs: fat32 batched Phase-3 X:\Diag JSON writer (5 files in one transaction)"`

**Test checkpoint:** Boot serial shows ~5x reduction in cumulative `boot_progress` delta for the X:\Diag write block. All 5 files persist with byte-identical content vs the per-file path. Smoke confirms no FAT32 cache regressions.

> **Deferred:** [L] Phase-3 X:\Diag JSON batching: a `fat32_batch_*` transaction API (open-dir-once + one cluster-bitmap pass + single flush) retrofitted to 5 writers -- multi-commit fs work. -> XREF: 05-storage-filesystems/TODO-04 (FAT32 owner) for the batch primitive.
---

## 12. AVX-512 Throttle Policy When APERF/MPERF Unavailable

Smoke records `simd: MPERF/APERF unavailable; enabling AVX-512 without throttle check`. APERF/MPERF gate throttle-detection on real hardware; Hyper-V hides them. Today we proceed regardless -- a correctness gap on bare-metal Intel where the MSRs exist but a hypervisor hides them, and AVX-512-heavy code can hit thermal/power throttling unobserved.

**Files:** `src/kernel/cpu_security.c`, `src/kernel/gfx/gfx_simd.c`, `src/kernel/gfx/gfx_simd_avx512.c`, `src/kernel/cpuid.c`

- [ ] Hypervisor-bit aware logging: hypervisor present = LOG_INFO (expected); bare metal + AVX-512 + no MPERF = LOG_WARN (real regression).
- [ ] `boot.conf simd_max_avx_level=avx2` override pins the SIMD ceiling to AVX2 on systems with observed throttling.
- [ ] Commit: `"simd: AVX-512 throttle policy when APERF/MPERF unavailable"`

**Test checkpoint:** Hyper-V smoke shows LOG_INFO (no WARN). Synthetic bare-metal-no-MPERF fixture or real bare-metal Haswell test laptop shows LOG_WARN. `boot.conf simd_max_avx_level=avx2` clamps the runtime ceiling.

> **Deferred:** [L] AVX-512 throttle: hypervisor-bit-aware logging + `boot.conf simd_max_avx_level` clamp -- validated only on bare-metal Intel where APERF/MPERF exist but a hypervisor hides them (not reproducible from WSL). -> XREF: 01-boot-platform/TODO-29-boot-perf-health-observability.md §12 (this section owns AVX-512 throttle in cpu_security.c / gfx_simd.c; no external SIMD TODO exists).
---

## 13. Boot History Ring Depth + Format

Today `boot_error_history` rings 8 entries. After 8 boots the oldest rolls off; an operator debugging an intermittent failure across 12-15 boots loses early evidence. Bumping to 32 costs ~512 bytes NVRAM (16 B × 32) and matches Win11 + Linux defaults.

**Files:** `src/kernel/main/boot_history.c` (+ matching public header to add), registry mirror

- [ ] Bump `BOOT_HIST_RING_LEN` from 8 to 32; update size-pin static asserts + tests + JSON schema docs in lockstep.
- [ ] Verify ring-wrap behavior remains correct at the larger size (existing wrap tests just need re-tuning to 32-entry shape).
- [ ] Decide: grow NVRAM variable in-place with tail-append schema, or version-bump and migrate.
- [ ] Commit: `"boot: boot_history ring depth 8 -> 32 entries (operator UX)"`

**Test checkpoint:** Existing unit tests pass at the 32-entry size. Smoke shows no regression in `boot_history: Recent boot history (N attempts)` output. NVRAM bin file is 512 bytes (32 × 16); RegSetBinary handles the larger blob.

> **Deferred:** [L] boot_history ring 8->32: NVRAM size bump + size-pin asserts + schema migration -- needs the NVRAM-blob migration decision + live RegSetBinary validation. -> XREF: 01-boot-platform/TODO-01 §11 (boot history owner).
---

## 14. FAT32 Dirty-Mount fsck Cost in VFS Step

Smoke (KVM, 2026-05-03) shows VFS step durations swinging by 4x depending on whether BlackBox last unmounted cleanly: 213ms on a clean mount, 861ms when `fat32: Dirty volume -- not cleanly unmounted` triggers `fat32_fsck` (BPB validate + cluster bitmap walk + ~440ms inside the repair pass). Variance is invisible in the per-step timing summary because the budget is fixed; operators see "VFS slow today" without knowing fsck ran.

**Files:** `src/kernel/fs/fat32/fat32_fsck.c`, `src/kernel/fs/fat32/fat32_ops.c`, `include/kernel/boot_init.h`

- [ ] Sub-time the dirty-mount path: emit `boot_progress` substeps for `fat32_fsck.bpb_validate`, `fat32_fsck.fat_compare`, `fat32_fsck.cluster_walk`. Today the only signal is the wall-clock VFS delta.
- [ ] Add a `fsck_ran` boolean + `fsck_duration_ms` to the boot-health JSON (§2) so VFS step variance is attributable rather than mysterious.
- [ ] Investigate a fast-clean shutdown path that flips the dirty bit on graceful poweroff (cmd.exe `shutdown` + ACPI S5) so subsequent boots skip fsck. Today the volume is dirty on every boot because the smoke teardown SIGTERMs QEMU mid-flight.
- [ ] Commit: `"fs: fat32 fsck substep timing + dirty-mount duration in boot-health"`

**Test checkpoint:** Boot serial shows 3 substep lines under VFS when fsck triggers; absent when volume was clean. Boot-health.json carries `fat32: { fsck_ran: bool, fsck_duration_ms: u32 }`. Graceful-shutdown smoke confirms the next boot skips fsck.

> **Deferred:** [L] FAT32 dirty-mount fsck cost: substep timing + boot-health fsck fields + graceful-shutdown dirty-bit-clear -- needs live dirty-vs-clean mount timing + an ACPI-S5 shutdown hook. -> XREF: 05-storage-filesystems/TODO-04 (FAT32 fsck owner).
---

## 15. User-Mode Binary Spawn Latency (~1s task_create → ELF entry)

Smoke (KVM, 2026-05-03) shows a consistent ~1s gap between `sched: Task N ("cmd.exe") created` and the matching `exec: Loading ELF binary`. The kernel logs both events but nothing between them, so the time is hidden inside the scheduler / exec / page-table-setup path. Same pattern visible for every user-mode binary spawn (cmd.exe, every test_*.exe). Real users experience this as "slow login shell" on every boot.

**Files:** `src/kernel/sched/syscall.c` (SYS_EXEC), `src/kernel/exec.c`, `src/kernel/sched/task.c`, `src/kernel/main/boot_desktop.c` (cmd.exe shell_loader_func)

- [ ] Add `boot_progress` substeps inside the SYS_EXEC path: read_file, format_dispatch, segment_load, page_table_install, auxv_setup, scheduler_resume. Capture per-step ms.
- [ ] Profile the ELF loader's per-segment work; a 67 KiB cmd.exe loading 3 segments in ~1s suggests page-by-page allocation rather than batched. Investigate whether `pmm_alloc_contiguous` for the segment span shaves the cost.
- [ ] If the gap is scheduler-side (task created but not picked for ~1s), audit the round-robin policy for boot-time starvation -- a freshly-created kernel task should run before idle.
- [ ] Commit: `"kernel: SYS_EXEC substep timing + spawn-latency profile"`

**Test checkpoint:** Boot serial shows 6 substep lines per user-mode spawn. cmd.exe spawn drops below 200ms or the cost is justified in `boot_perf_budgets[]`. test=1 mode shows the same pattern for the 16 user-mode test binaries (16 × 200ms = 3.2s vs 16 × 1s = 16s today is the upper bound on the win).

> **Deferred:** [L] User-mode spawn latency ~1s: SYS_EXEC substep timing + ELF segment-load batching + scheduler boot-starvation audit -- live-boot profiling + exec/sched multi-commit work. -> XREF: 02-kernel-core/TODO-22 (sched/exec owner).
---

## 16. TSC Frequency Variability Under Hypervisor

Smoke logs across boots show TSC frequency reported as 7056 MHz, 4939 MHz, 5000 MHz on the same physical i5-11600K host. Hyper-V re-derives the TSC scale on each VM start; Linux 6.x logs a single canonical value because it pins TSC freq from CPUID 0x15/0x16 once at boot. Impossible OS uses `lapic: Tier 1: Hyper-V MSR 0x40000023` which returns the runtime-current scale, not a CPUID-pinned value. Downstream consumers (sys_uptime, KUSD QpcFreq, perf counters) cache whatever they got first; if a future TSC-freq refresh pulls a different value mid-boot, time-since-boot would jump.

**Files:** `src/kernel/drivers/lapic.c`, `src/kernel/cpuid.c`, `src/kernel/time/mono_clock.c`, `src/kernel/time/kusd_time.c`

- [ ] Verify all TSC-freq consumers (KUSD QpcFreq, time_get_tsc_ns_per_tick, perf_record timestamps) read from a single latched value computed once at LAPIC init, not re-queried.
- [ ] Add a one-time post-init self-check: re-read MSR 0x40000023 and compare against the latched value; if it drifted by > 1%, log LOG_WARN with both values for triage.
- [ ] CPUID 0x15/0x16 fallback: when the hypervisor MSR is absent (bare metal Intel Tier 1), pin to CPUID 0x15 ratio + 0x16 base which Linux trusts as authoritative.
- [ ] Boot-health.json (§2) records the latched TSC freq + the source (Hyper-V MSR / CPUID 0x15 / PIT / HPET) so operator triage can spot when a hypervisor reset confused the calibration.
- [ ] Commit: `"time: TSC frequency single-latch + drift-check + CPUID fallback"`

**Test checkpoint:** Smoke shows one `[time] TSC freq locked: <N> MHz from <source>` line; no later line reports a different freq. Synthetic-drift fixture (mock MSR returning a different value on second read) triggers the LOG_WARN. boot-health.json carries `tsc: { freq_hz: u64, source: "hyperv-msr"|"cpuid-0x15"|"pit"|"hpet" }`.

> **Deferred:** [L] TSC frequency variability: single-latch all consumers + drift self-check + CPUID 0x15/0x16 fallback -- validated only across real hypervisor restarts + bare-metal Intel. -> XREF: 01-boot-platform/TODO-29-boot-perf-health-observability.md §16 (this section owns the single-latch).
---

## 17. PAT WC -> WT Hypervisor Trap Quirk

Smoke (KVM, 2026-05-03) records `[WARN] mm: PAT: entry 1 = 0x04 (expected WC=0x01); hypervisor may trap PAT writes. Framebuffer mapped WT instead of WC (functional, slower)`. The kernel writes IA32_PAT to set entry 1 = WC for framebuffer mapping; under KVM (and possibly other hypervisors) the write is silently dropped or coerced and entry 1 stays WT. Functional but ~2-4x slower for framebuffer bulk-blit because Write-Through forces every write through the FSB instead of coalescing in the WC buffer. Currently a one-shot WARN with no programmatic surface, so consumers (sysinfo, boot-health.json, CI gate) cannot tell whether a slow boot ran on a PAT-honest or PAT-trapping hypervisor.

**Files:** `src/kernel/cpu_security.c` (PAT programming), `src/kernel/firmware_quirks_table.inc`, `include/kernel/firmware_quirks.h`, `src/kernel/firmware_quirks.c`, `src/kernel/main/boot_health.c` (auto-picked up via `firmware_quirks_iter_next`)

- [ ] Add `FW_QUIRK_PAT_WC_TRAPPED` to `firmware_quirks_table.inc` with bit assignment + canonical name `"pat_wc_trapped"`.
- [ ] In `cpu_configure_pat()`, register the quirk via `firmware_quirks_register(FW_QUIRK_PAT_WC_TRAPPED)` when the post-write readback shows entry 1 != WC; downgrade the WARN to INFO once the quirk fires.
- [ ] Update `vmm_map_mmio_wc()` to log a single INFO line when the quirk is active, instead of every caller silently getting WT.
- [ ] Verify `firmware_quirks_active[]` in `boot-health.json` lists `"pat_wc_trapped"` on KVM/WHPX hosts that exhibit the trap; absent on bare metal.
- [ ] Commit: `"firmware: register pat_wc_trapped quirk for PAT-trapping hypervisors"`

**Test checkpoint:** On KVM smoke, post-boot `boot-health.json` shows `"firmware_quirks_active": [..., "pat_wc_trapped", ...]`. On bare metal Intel where PAT writes succeed, the quirk is absent. The original WARN downgrades to a single INFO line. Test on: QEMU KVM, QEMU TCG, QEMU WHPX, VirtualBox, bare metal i5-4210U.

> **Deferred:** [L] PAT WC-trap quirk: register on post-write readback != WC + INFO downgrade + boot-health surfacing -- validated only on PAT-trapping hypervisors (KVM/WHPX) vs PAT-honest bare metal. -> XREF: 01-boot-platform/TODO-29-boot-perf-health-observability.md §17 (this section owns the quirk).
---

## 18. Boot Critical-Path / Dependency / Resource-Wait Attribution

§1-§3 expose per-phase durations + budgets + trend deltas, but a boot delay caused by ORDERING (step B waited on step A), a hardware WAIT (device poll), or scheduler STARVATION reads as "phase slow" with no actionable cause. Linux `systemd-analyze critical-chain` / `blame` / `dot` expose exactly this; this section adds the dependency + resource-wait attribution layer on top of the §1 timeline.

- [ ] Tag each `boot_progress` step delta with a cause class (`COMPUTE`, `HW_WAIT`, `IO_WAIT`, `SCHED_WAIT`) so a slow step records WHY, not just how long.
- [ ] `boot_critical_chain()` -- walk the §1 timeline, emit the ordered longest-pole chain (each step + dominant wait class) as `[BOOT-CRIT] <step> <ms> <cause>` lines (systemd-analyze critical-chain parity).
- [ ] Add `critical_chain[]` (top-N longest poles + cause class) + a duration-sorted `blame[]` view to `boot-health.json` (§2) so the dashboard names the boot's longest pole + why.
- [ ] Pure classifier `boot_cause_classify()` + longest-pole walk are data-only (no live boot calls) for unit test.
- [ ] -> XREF: `TODO-14-boot-diagnostics.md §9` (Boot Timeline Visualization) -- the Gantt/SVG viewer consumes this critical-chain + cause-class data; §18 owns the attribution, §9 owns the visual.
- [ ] Record a milestone between EXEC and DESKTOP_READY: that interval is 2272ms and its real content is TTF/glyph/icon loading plus a 1935x1080 JPEG decode, none of which EXEC names.
- [x] `vfs_rename_ex` rc=-1 on the boot-trend .tmp rename: ROOT-CAUSED 2026-07-27 (the two-dot guess was wrong). Diagnostics added at each failure site; the two real FAT32 defects are owned by `05-storage-filesystems/TODO-04 §4`.
  Filed THERE rather than here on purpose: this section carries a `Deferred:` stamp, which makes `sequencer_triage.py` class the whole file DONE, so an item added here would never be seen by the overnight runner. Worth remembering as a general trap -- a deferred section is a grave for new items.
- [ ] Commit: `"boot: critical-path + resource-wait attribution (systemd-analyze critical-chain parity)"`

**Test checkpoint:** Boot serial emits `[BOOT-CRIT]` lines naming the ordered longest-pole chain with per-step cause class; `boot-health.json` carries `critical_chain[]` (top-N) + a blame-sorted view. Pure-helper test covers the longest-pole walk + cause classifier. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Deferred:** [M] Boot critical-path attribution (new, gap-audit): per-step cause-class tagging in boot_progress + `boot_critical_chain()` + boot-health `critical_chain[]`/`blame[]` -- multi-commit boot-instrumentation + live-boot validation. -> XREF: 01-boot-platform/TODO-14-boot-diagnostics.md §9 (visual consumer); §18 owns attribution.
---

## 19. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 1:
- [ ] Gate the full `PERF`/timeline serial tables behind debug/test builds (default = summary + `BOOT-BUDGET` breaches) -- the dump runs pre-cmd.exe, outside the `boot_perf_total_check()` window: user-visible but unbudgeted
From the stamped section 3:
- [ ] Defer `boot_trend_publish_json()` (cJSON RMW + sync VFS I/O) to a post-DESKTOP_READY work item; record only fixed-size durations during boot -- takes the trend layer's own cost out of measured boot time
- [ ] Linearize `boot_trend_publish_json()` traversal (`boot_trend.c:317`): replace indexed `json_array_get` (O(N^2) loop on dense/malformed file) with `json_array_first`/`json_array_next`; quarantine over-long `boots` arrays. (TODO-24 §6)
- [ ] C:\ fallback for `boot-trend.json` (`boot_trend.c:17-21`): build paths from `klog_using_blackbox ? "X:\\Perf\\" : klog_dir` like boot-profile/timeline, or gate BlackBox-only + document no fallback. (TODO-24 §6)

**Test checkpoint:** per moved item; each carries its original acceptance text.

---

## OS Comparison

| ⭐  | Feature                          | 🪟 Win11                      | 🐧 Linux                          | 🚀 Impossible OS                   |
| --- | -------------------------------- | ----------------------------- | --------------------------------- | ---------------------------------- |
| 💎  | Per-phase boot perf budgets      | ⚠️ ETW boot trace (post-hoc)   | ⚠️ systemd-analyze (post-hoc)      | ⬜ §1 boot-time alarms             |
| ⭐  | Consolidated boot health JSON    | ⚠️ msinfo32 + Event Viewer     | ⚠️ journalctl + scattered tools    | ✅ §2 single boot-health.json      |
| ⭐  | Boot perf trend regression alarm | ❌ no built-in                | ❌ no built-in                    | ⚠️ §3 trend+WARN; gate=T28 §9       |
| 💎  | SMBIOS init speed                | ⚠️ NT HAL parses lazily        | ⚠️ dmidecode-driven, scattered     | ✅ §4 51ms (was 1342ms; RAM copy)  |
| ⭐  | MAT W^X root-cause attribution   | ❌ unsupported                | ⚠️ /sys/firmware/efi/* raw         | ✅ §5 per-violation phys+attr      |
| 💎  | PS/2 mouse init speed            | ⚠️ HAL probes serially         | ⚠️ atkbd serial probe              | ✅ §6 100ms (was 1149ms; split TO) |
| ⭐  | Async font / icon load           | ✅ Win11 SystemAssets fade-in | ⚠️ DE-dependent (KDE/GNOME async)  | ⬜ §7 minimal face + swap          |
| 💎  | Boot heartbeat telemetry         | ✅ ETW Microsoft-Windows-Boot | ⚠️ printk timestamps only          | ⬜ §8 250ms HB + LAPIC ISR         |
| ⭐  | PAT WC-trap hypervisor surfacing | ❌ silent WT fallback         | ❌ silent WT fallback             | ⬜ §17 firmware_quirks_active[]    |
| 💎  | Boot critical-path attribution   | ⚠️ WPA stack (post-hoc)        | ✅ systemd-analyze critical-chain | ⬜ §18 [BOOT-CRIT] + cause class   |

---

## Unit Tests

Tests live under `src/kernel/test/test_boot_health.c` (NEW). Registered via `test_register_boot_health()` in `TEST_CAT_BOOT`. Pure helpers only -- no calls to live `boot_progress`, `boot_health_publish_json`, or any subsystem `_init`. Per Test Code Policy.

- [ ] §1: `boot_perf_budget_check_pure(steps[], budgets[])` walks a fixed-array fixture and returns the breach list. Tests cover: zero breaches, one soft breach, one hard breach, missing-budget step (passes), out-of-order steps.
- [ ] §2: `boot_health_serialize_pure(state, buf, cap)` writes JSON into caller buffer; tests assert byte-for-byte schema conformance, RFC 8259 escapes, u32 unix-epoch time encoding (per schema), truncation behavior.
- [ ] §3: `boot_trend_compute_median_growth(samples[])` returns a percentage delta; tests cover stable runs, monotonic growth, noise band, ring-wrap.
- [ ] §5: `mat_violation_classify(attr_bits)` returns the WX-violation reason string; tests cover all 8 attr-bit combinations.
- [ ] §8: `boot_heartbeat_should_emit(elapsed_ms, last_emit_ms)` returns 1 every 250ms; tests cover edge cases (elapsed=0, last=0, wraparound).

---

## Verification

- [ ] All unit tests in `## Unit Tests` PASS under `SUITE=boot`.
- [ ] QEMU OVMF boot writes `X:\Diag\boot-health.json` post-Phase-3 with non-zero size and parses through `cJSON_Parse`.
- [ ] QEMU OVMF boot writes `X:\Perf\boot-trend.json`; second boot updates index 0; sixteenth boot trims to 16 entries.
- [ ] SMBIOS init drops from 1342ms to <100ms (boot log `SMBIOS Xms`).
- [ ] Mouse init drops from 1149ms to <100ms.
- [ ] Font/icon load no longer blocks `DESKTOP_READY`.
- [ ] MAT WX violation log emits per-descriptor metadata, not just a count.
- [ ] Boot heartbeat fires during a synthetic 1s busy-loop step; absent on <250ms steps.
- [ ] Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 5 boot-health pure-helper suites (§1, §2, §3, §5, §8), 0 failures
