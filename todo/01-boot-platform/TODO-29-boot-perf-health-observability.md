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
> - TSC_AUX MSR per-CPU (TODO-02-kernel-core/TODO-09 §x86-64)
> - PAT WC retry on Hyper-V (TODO-03-memory-concurrency/TODO-01 §VMM)
> - AT_PHDR derivation in ELF loader (TODO-10-platform-services/TODO-10 §linux-compat)
> - ExitBootServices retry hardening (TODO-03 §bootloader-error-recovery)
> - Capsule update write path (TODO-27 §uefi-advanced)
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
- `X:\Perf\boot-trend.json` rolls the last 16 boots; CI gate fails when median grows >15% over a 3-run window.
- SMBIOS init drops from 1.3s to <100ms.
- Mouse PS/2 init drops from 1.1s to <100ms.
- Font/icon load drops from ~1.3s synchronous-blocking to <50ms post-desktop-ready (deferred async loader).
- MAT W^X violations name the violating descriptor index + classification reason (not just a count).
- A boot heartbeat marker fires every 250 ms during long phases so a hang is distinguishable from "still working" on serial.

## Implementation Order

| ⭐ | Order | Deliverable                                         | Depends On                                | Status |
| -- | :---: | --------------------------------------------------- | ----------------------------------------- | :----: |
| 💎 |   1   | Per-phase boot perf budgets + threshold alarms      | --                                        |  [x]   |
| 💎 |   2   | Boot health audit JSON (`X:\Diag\boot-health.json`) | §1, T04 §11 (VFS fix)                     |  [ ]   |
| ⭐ |   3   | Boot perf trend file + regression detection         | §1, §2                                    |  [ ]   |
| 💎 |   4   | SMBIOS init profiling + optimization                | §1                                        |  [ ]   |
| 💎 |   5   | MAT W^X violation root-cause attribution            | T04 §5                                    |  [ ]   |
| 💎 |   6   | Mouse PS/2 init profiling + optimization            | §1                                        |  [ ]   |
| ⭐ |   7   | Async font/icon loader (post-desktop-ready)         | §1                                        |  [ ]   |
| ⭐ |   8   | Boot heartbeat telemetry during long phases         | §1, T14 §6 (alive-blink), T23 (watchdog)  |  [ ]   |

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

> **Verified:** 2026-05-03 | this commit | 8/8 items | build OK | tests 2729/2729 PASS
> **Quality reviewed:** 2026-05-03 | Codex 4x (design + adversarial + consistency + perf) | 2H+2M+1L fixed, 0 open | scope: kernel-code-quality

---

## 2. Boot Health Audit JSON (`X:\Diag\boot-health.json`)

A single operator-facing dashboard collapsing every "what's wrong on this boot" signal currently scattered across serial. Consumers: `sysinfo.exe boothealth` (TODO-04 §10 host decoder), CI regression gate (TODO-28), bare-metal triage when serial is unavailable.

- [ ] Define `schema_version=1` wire format in `docs/boot/boot-health-schema.md` mirroring `firmware-tables-schema.md` style: `schema_version`, `generated_at_utc`, `boot_seq`, `degraded_caps[]` (from `boot_caps`), `degraded_subsystems[]` (from `kernel_subsystem_ready` oracle), `missing_capabilities[]` (TPM, USB, NVMe, network -- queried from each subsystem's accessor), `perf_breaches[]` (from §1), `mat_wx_violations[]` (from §5), `firmware_quirks_active[]` (from `firmware_quirks_iter_next`), `recent_boot_times_s[]` (last 3 from `boot_history`), `secureboot_state` (UNKNOWN/ENABLED/DISABLED/SETUP).
- [ ] Implement `boot_health_publish_json()` in NEW `src/kernel/main/boot_health.c` (~250 LOC). Single-shot call from `boot_phase3()` AFTER `boot_progress_dump_summary()` and the §1 budget check, BEFORE the firmware-tables JSON publish (so a single Phase-3 file-write batch covers all artifacts).
- [ ] Use the same single-open `vfs_open(VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC)` shape that `klog_disk` uses (TODO-04 §11 fix lands first). 8 KiB pmm buffer, RFC 8259 escapes, civil-from-days ISO-8601.
- [ ] Use stable accessor functions from each owning subsystem so the writer reads from canonical sources (no copy-paste of subsystem state).
- [ ] Commit: `"boot: publish consolidated boot-health.json"`

**Test checkpoint:** Post-boot, `X:\Diag\boot-health.json` exists, parses through `cJSON_Parse`, contains every required top-level key, and reflects the live boot's actual degraded state (e.g. on this current OVMF run: `degraded_caps` lists 6 entries, `missing_capabilities` lists `TPM` + `USB` + `NVMe`, `mat_wx_violations` count is 1).

---

## 3. Boot Perf Trend File + Regression Detection

The §1 budget check catches absolute breaches, but a slow drift inside the budget (e.g. SMBIOS creeping from 80ms to 95ms over 50 commits) stays invisible. A rolling per-phase median makes the drift visible.

- [ ] `X:\Perf\boot-trend.json` rolls the last 16 boots: `{ "schema_version": 1, "boots": [ { "boot_seq": N, "unix_time": ..., "phase_durations_ms": { "PMM": 8, "SMBIOS": 80, ... } } ] }`. Newest at index 0, oldest at 15; ring-buffer overwrite on the 17th boot.
- [ ] Read-modify-write cycle: load existing file (or empty array if missing), prepend current boot's per-phase durations, truncate to 16, write. Failure path leaves the file untouched (no partial write).
- [ ] Compute 3-run median per phase. Emit `[WARN] BOOT-TREND: <phase> 3-run median grew <pct>% (<old>ms -> <new>ms)` when median grows >15% vs the previous 3-run window.
- [ ] CI hook (under TODO-28 §boot-validation, not here) can read the same JSON and fail a release gate when the trend shows regression. This section ships only the data file and the warn line; the CI gate is owned by TODO-28.
- [ ] Commit: `"boot: rolling boot-trend.json + regression alarms"`

**Test checkpoint:** Synthetic 4-boot sequence with `SMBIOS=80,80,80,150` produces a 56% regression alarm. `SMBIOS=80,82,80,84` stays silent (within noise band). Unit test seeds the file with 16 fixture entries and asserts the next write trims correctly.

---

## 4. SMBIOS Init Profiling + Optimization

Observed: `SMBIOS 852ms 1342ms P1` -- 1.3s for SMBIOS parsing on a QEMU OVMF boot with a typical 22-structure table. Real hardware with 50-100 structures will be worse. Investigate root cause + fix.

- [ ] Instrument `smbios_init()` and the per-structure parser with TSC samples at every loop iteration. Dump a one-shot summary: number of structures parsed, bytes per structure histogram, total time, top 3 slowest structures.
- [ ] Identify the bottleneck. Suspects: (a) `kmalloc()` per-structure (replace with single arena allocation pre-walk); (b) string copy via `smbios_copy_string` doing N walks of `smbios_get_string` (hoist string-table walk to one pass); (c) UEFI-time slow firmware reads (move parsing to post-Phase-1 if possible).
- [ ] Apply the fix that the profile identifies. Target: <100ms on the 22-structure OVMF table.
- [ ] Add a unit test that builds a synthetic 100-structure SMBIOS blob and asserts `smbios_init()` returns within 250ms (10x typical bare-metal worst case).
- [ ] Commit: `"boot: SMBIOS init optimization (1.3s -> <100ms)"`

**Test checkpoint:** Boot log `SMBIOS Xms` value drops from ~1342ms to <100ms on QEMU OVMF. 100-structure synthetic fixture parses in <250ms. No content regressions in `HKLM\HARDWARE\BIOS\*` or `HKLM\HARDWARE\System\*`.

---

## 5. MAT W^X Violation Root-Cause Attribution

Today `[WARN] UEFI: MAT: W^X VIOLATION -- 1 regions are writable+executable` reports a count but no actionable detail. To debug a violation on bare metal you have to grep `MAT[N]` lines and match the offset by hand.

- [ ] Extend `mat_init()` (`src/kernel/uefi_config.c`) to record per-violation metadata: descriptor index, phys address, page count, attribute bits, classification path (which combination of EFI_MEMORY_RO/XP/RP set/clear caused the WX label).
- [ ] Emit `[WARN] UEFI: MAT[N] WX VIOLATION at phys=0xNN pages=P attr=0xXX (RO=%d XP=%d RP=%d)` per violation, replacing the current single-count line. Cap at 8 lines to bound serial floods on broken firmware.
- [ ] Add `mat_violations_iter_next(prev_index)` and `mat_violation_describe(idx, struct mat_violation_record *out)` accessors. Consumed by §2 boot-health.json and the existing `firmware-tables.json` MAT block.
- [ ] Cross-reference firmware-quirks: if a known SMBIOS vendor/product matches a recorded `bogus_mat` quirk (TODO-04 §9), the WARN downgrades to INFO with a "known-bad firmware" tag.
- [ ] Commit: `"boot: MAT W^X violation root-cause attribution"`

**Test checkpoint:** Synthetic MAT fixture with one descriptor having `attr=0` (writable+executable) emits a single `[WARN] UEFI: MAT[N] WX VIOLATION ...` line with phys/pages/attr fields populated. `mat_violations_iter_next` walks all violations exactly once. boot-health.json `mat_wx_violations[]` contains the parsed metadata.

---

## 6. Mouse PS/2 Init Profiling + Optimization

Observed: `MOUSE 2213ms 1149ms P1` -- 1.1s for PS/2 mouse init even though FADT reports `8042=0` (no PS/2 controller). The init is doing something expensive on a port that should fall through to "not present" fast.

- [ ] Instrument `mouse_init()` (`src/kernel/drivers/mouse.c`) with TSC samples at every i8042 command + ack wait. Dump a per-command-ms breakdown.
- [ ] Identify the bottleneck. Suspects: (a) i8042 ack timeout too long (200ms × N commands = the whole 1.1s); (b) FADT `8042=0` not consulted before probe (probe runs full init even when ACPI says no PS/2 exists); (c) interrupt setup blocking on a wedged controller.
- [ ] If FADT reports `8042=0` and port probe fails, short-circuit init in <10ms total. The current "FADT says no i8042 but port probe OK" message indicates the probe IS succeeding on QEMU even when ACPI says it shouldn't -- decide whether to trust ACPI (skip) or trust the probe (continue but bound the timeout).
- [ ] Cross-reference TODO-04-11 (input-system) for the canonical input-device init policy.
- [ ] Commit: `"boot: PS/2 mouse init optimization (1.1s -> <100ms)"`

**Test checkpoint:** Boot log `MOUSE Xms` drops from ~1149ms to <100ms on QEMU. Mouse still works post-init (cursor tracks, clicks register). `MOUSE 0ms` short-circuit when FADT says no PS/2 AND port probe fails.

---

## 7. Async Font and Icon Loader (Post-Desktop-Ready)

Observed: ~1.3s of font + icon loading happens INSIDE the desktop boot phase before `DESKTOP_READY` fires. The desktop is unusable for that 1.3s window. Async load: render a placeholder font + monochrome icons, then swap in the full set as each file completes.

- [ ] Define a "minimal boot face": single 18px atlas of ASCII (already 64KB, already loaded by Phase 1). Desktop opens with this face.
- [ ] Move `font_init_full()` (selawk/CascadiaCode/selawksb/CascadiaCode-Bold/selawkb) to a deferred work-queue task spawned at `DESKTOP_READY`. The compositor uses the minimal face until each TTF slot completes; on completion, the face swaps in atomically.
- [ ] Same pattern for icon fonts (Filled/Regular/Light/Resizable) and `icons.ires`. Show monochrome ASCII placeholders until each loads.
- [ ] Commit: `"desktop: async font + icon load (1.3s -> <50ms blocking)"`

**Test checkpoint:** `DESKTOP_READY` fires within 50ms of `boot_phase3()` start (excluding wallpaper + first compositor frame). All 4 TTFs + 4 icon fonts load within 2s post-desktop-ready, no visual glitch. Visual regression test (post-load screenshot) byte-identical to pre-async baseline.

---

## 8. Boot Heartbeat Telemetry During Long Phases

A 1.3s SMBIOS init looks identical on serial to a hung boot until either (a) the next `boot_progress` line emits or (b) the watchdog fires (TODO-23). A 250ms heartbeat fills the gap with "still working" telemetry.

- [ ] `boot_heartbeat_arm(phase_ms_target)` and `boot_heartbeat_pet()` in [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h). When armed, every 250ms a LAPIC timer ISR emits `[BOOT-HB] phase=<step> elapsed=<ms> target=<target>` on serial and increments `boot_info.heartbeat_seq`.
- [ ] Wire arm/pet into `boot_progress`: every step entry arms (with the §1 budget as target), every step exit pets (cancels the next heartbeat). Long-running steps emit 4 heartbeats/sec; short steps emit zero.
- [ ] Coordinate with TODO-14 §6 alive-blink (visual) and TODO-23 (watchdog NMI on hard cap). The heartbeat is the soft signal; alive-blink is the visual; watchdog is the hard reset.
- [ ] Boot-health.json (§2) records the maximum heartbeat gap observed during the boot, surfacing pauses that stayed under the WARN threshold but were unusually slow.
- [ ] Commit: `"boot: heartbeat telemetry during long phases"`

**Test checkpoint:** A 1342ms SMBIOS init with target 100ms emits 4-5 `[BOOT-HB] phase=SMBIOS elapsed=Xms target=100` lines. A <250ms phase emits zero heartbeats. Heartbeat ISR is harmless when LAPIC timer is the active scheduler tick (no double-fire, no priority inversion).

---

## OS Comparison

| ⭐ | Feature                            | 🪟 Win11                          | 🐧 Linux                          | 🚀 Impossible OS                   |
| -- | ---------------------------------- | --------------------------------- | --------------------------------- | ---------------------------------- |
| 💎 | Per-phase boot perf budgets        | ⚠️ ETW boot trace (post-hoc)     | ⚠️ systemd-analyze (post-hoc)    | ⬜ §1 boot-time alarms             |
| ⭐ | Consolidated boot health JSON      | ⚠️ msinfo32 + Event Viewer       | ⚠️ journalctl + scattered tools   | ⬜ §2 single boot-health.json      |
| ⭐ | Boot perf trend regression alarm   | ❌ no built-in                    | ❌ no built-in                    | ⬜ §3 boot-trend.json + 15% gate   |
| 💎 | SMBIOS init speed                  | ⚠️ NT HAL parses lazily          | ⚠️ dmidecode-driven, scattered    | ⬜ §4 <100ms target                |
| ⭐ | MAT W^X root-cause attribution     | ❌ unsupported                    | ⚠️ /sys/firmware/efi/* raw       | ⬜ §5 per-violation phys+attr      |
| 💎 | PS/2 mouse init speed              | ⚠️ HAL probes serially            | ⚠️ atkbd serial probe             | ⬜ §6 <100ms target                |
| ⭐ | Async font / icon load             | ✅ Win11 SystemAssets fade-in     | ⚠️ DE-dependent (KDE/GNOME async)| ⬜ §7 minimal face + swap          |
| 💎 | Boot heartbeat telemetry           | ✅ ETW Microsoft-Windows-Boot     | ⚠️ printk timestamps only         | ⬜ §8 250ms HB + LAPIC ISR         |

---

## Unit Tests

Tests live under `src/kernel/test/test_boot_health.c` (NEW). Registered via `test_register_boot_health()` in `TEST_CAT_BOOT`. Pure helpers only -- no calls to live `boot_progress`, `boot_health_publish_json`, or any subsystem `_init`. Per Test Code Policy.

- [ ] §1: `boot_perf_budget_check_pure(steps[], budgets[])` walks a fixed-array fixture and returns the breach list. Tests cover: zero breaches, one soft breach, one hard breach, missing-budget step (passes), out-of-order steps.
- [ ] §2: `boot_health_serialize_pure(state, buf, cap)` writes JSON into a caller-provided buffer; tests assert byte-for-byte schema conformance for each field, RFC 8259 escapes, ISO-8601 timestamps, truncation behavior.
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
