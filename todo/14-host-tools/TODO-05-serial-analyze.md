---
schema_version: 1
id: serial-analyze
domain: 14-host-tools
status: active
title: "TODO-05 -- serial-analyze (Boot Log Analyzer)"
---

# TODO-05 -- serial-analyze (Boot Log Analyzer)

> **Goal:** Parse serial boot logs to extract timing, highlight warnings/errors, detect anomalies, compare boot runs, and generate visual reports. Replaces manually scanning hundreds of log lines.

## Outcome

```
$ serial-analyze serial.log
  === BOOT SUMMARY ===
  Total:    9.7s (Phase 0: 164ms, Phase 1: 320ms, Phase 2: 6.5s, Phase 3: 2.7s)
  Status:   SUCCESS (all 20 subsystems OK)
  Warnings: 1 (MAT W^X violation)
  Platform: Hyper-V (QEMU WHPX), 1 CPU, 2046 MiB RAM

  === SLOWEST SUBSYSTEMS ===
  1. klog flush        14.2s  (X:\ FAT32 write)
  2. PCI scan           1.1s  (xHCI timeout)
  3. IXFS mount         0.9s  (inode scan)

  === WARNINGS ===
  Line 87: [WARN] UEFI: MAT: W^X VIOLATION -- 1 regions writable+executable

$ serial-analyze --compare boot1.log boot2.log
  === REGRESSION DETECTED ===
  Phase 2:  3.5s → 14.2s (+10.7s)  ← klog flush 10x slower
  PCI scan: 1.1s → 1.1s  (stable)
  Timer:    Tier 1 → Tier 1  (stable)
```

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| 💎  |   1   | Log parser (timestamps, phases, subsystems) | --         |  [ ]   |
| ⭐  |   2   | Timing analysis (per-phase, per-subsystem) | §1         |  [ ]   |
| ⭐  |   3   | Warning/error highlighter                | §1         |  [ ]   |
| ⭐  |   4   | Boot comparison (two logs, find regressions) | §2         |  [ ]   |
| 💎  |   5   | HTML report output (timing waterfall chart) | §2         |  [ ]   |

---

## 1. Log Parser
Parse serial log format: `[timestamp] [level] subsys: message` and `[PHASEn] STEP (0xNNNN)`.

- [ ] Extract: timestamp (float seconds), log level (OK/WARN/INFO/ERROR), subsystem, message
- [ ] Extract: phase transitions (`[PHASE0]`, `[PHASE1]`, etc.)
- [ ] Extract: POST codes and subsystem names
- [ ] Extract: boot timing steps from the `BOOT: [PHASE0] +Nms` block
- [ ] Handle ANSI color codes in VBox serial output (strip `[90m`, `[32m`, etc.)

## 2. Timing Analysis
Break down boot time by phase and subsystem.

- [ ] Per-phase total: Phase 0 = Nms, Phase 1 = Nms, etc.
- [ ] Per-subsystem: sorted by duration, show top 5 slowest
- [ ] Flag anomalies: any subsystem > 2s, any phase > expected threshold
- [ ] Show timer calibration tier and result

## 3. Warning/Error Highlighter
Extract and categorize all warnings and errors.

- [ ] Filter log lines by level (WARN, ERROR, FATAL)
- [ ] Categorize: hardware, firmware, driver, subsystem
- [ ] Color output: red for errors, yellow for warnings
- [ ] Summary count: N warnings, M errors

## 4. Boot Comparison
Compare two serial logs and detect regressions.

- [ ] `serial-analyze --compare old.log new.log`
- [ ] Per-phase delta: show time increase/decrease
- [ ] Per-subsystem delta: flag significant slowdowns (>20%)
- [ ] New warnings: show warnings in new log that weren't in old
- [ ] Missing subsystems: flag any subsystem present in old but missing in new

## 5. HTML Report
Generate a visual boot timing report.

- [ ] ASCII waterfall chart for terminal
- [ ] HTML output with CSS-styled timeline bars
- [ ] Save to file: `serial-analyze --html report.html serial.log`

## Verification

- [ ] Parse QEMU WHPX, TCG, VBox, and bare metal serial logs correctly
- [ ] Timing matches the `BOOT: [PHASE0] +Nms` block
- [ ] Comparison detects known regression (e.g., klog flush slowdown)
