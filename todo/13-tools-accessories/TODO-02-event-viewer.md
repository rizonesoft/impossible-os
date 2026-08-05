---
schema_version: 1
id: event-viewer
domain: 13-tools-accessories
status: active
title: "TODO-02 -- Event Viewer (Log Viewer)"
---

# TODO-02 -- Event Viewer (Log Viewer)

> **Goal:** A user-mode GUI tool (`eventview.exe`) that reads `X:\Logs\events.jsonl` and displays kernel events in a colour-coded, filterable table -- like Windows Event Viewer. Shows timestamp, level, subsystem, and message. Filter by level (INFO/WARN/ERROR), subsystem tag, and text search. Essential for diagnosing boot issues, driver failures, and runtime errors without parsing serial logs.

> [!IMPORTANT]
> **Current state:** `events.jsonl` is written by klog during boot (one JSON object per line). No user-mode tool to read it. Logs are only visible via serial output or raw file inspection. The klog ring buffer holds 1000 entries in memory; `events.jsonl` on disk is the persistent record.

---

## Inputs

- `src/kernel/klog_disk.c` -- writes `events.jsonl` (JSONL format: `{"ts":N,"level":"INFO","tag":"net","msg":"..."}`)
- `include/kernel/klog.h` -- `log_level_t` enum (DEBUG=0, INFO=1, WARN=2, ERROR=3, FATAL=4)
- `X:\Logs\events.jsonl` -- on-disk event log
- → XREF: `02-kernel-core/TODO-04-system-logging.md` -- JSON Lines event format (was TODO-02 §6)
- → XREF: `00-infrastructure/TODO-03-kernel-test-harness.md` -- unit test wiring

---

## Outcome

- `eventview.exe` deployed to `C:\Impossible\System32\`
- Launchable from shell: `C:\> eventview` or from Start Menu / Control Panel
- Table view: timestamp, level (colour-coded), subsystem, message
- Filter bar: level dropdown, subsystem dropdown, text search
- Auto-refresh: tail new entries as they arrive
- Useful for post-mortem diagnosis and live monitoring

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Console-mode log viewer (text table to stdout) | --         |  [ ]   |
| ⭐  |   2   | GUI Event Viewer with filterable table         | §1         |  [ ]   |
| ⭐  |   3   | Live tail mode and auto-refresh                | §2         |  [ ]   |

> 💎 = parity -- Windows ships Event Viewer; Linux has journalctl.
> ⭐ = exclusive -- colour-coded JSONL viewer integrated into the OS.

---

## 1. Console-Mode Log Viewer

Text-based event log reader -- works before GUI is needed.

- [ ] Create `user/apps/eventview.c`
- [ ] Open and parse `X:\Logs\events.jsonl` line by line
- [ ] Parse each JSON line for `ts`, `level`, `tag`, `msg` fields (minimal JSON parser or string scan)
- [ ] Print formatted table: `[HH:MM:SS] [LEVEL] [tag] message`
- [ ] Colour output: green for INFO, yellow for WARN, red for ERROR/FATAL
- [ ] Optional filter args: `eventview --level WARN` shows WARN+ only, `eventview --tag net` filters by subsystem
- [ ] Commit: `"tools: console-mode eventview.exe -- JSONL log viewer"`

---

## 2. GUI Event Viewer

Graphical version with filterable table.

- [ ] 800×500 window: toolbar (filters) + scrollable table
- [ ] Table columns: Time, Level, Subsystem, Message
- [ ] Level column colour-coded: green/yellow/red/bold-red
- [ ] Filter bar: level dropdown (All/INFO/WARN/ERROR), subsystem dropdown, text search field
- [ ] Sort by any column (click header)
- [ ] Status bar: total entries, filtered count
- [ ] Commit: `"tools: GUI eventview.exe -- filterable colour-coded event table"`

---

## 3. Live Tail and Auto-Refresh

Watch for new events and update the display.

- [ ] Poll `events.jsonl` file size every 1s; if grown, read new lines
- [ ] Scroll-to-bottom on new entries (if already at bottom)
- [ ] Toolbar toggle: "Live" / "Paused"
- [ ] Commit: `"tools: eventview.exe live tail mode"`

---

## OS Comparison

| ⭐  | Feature             | 🪟 Win11                | 🐧 Linux         | 🚀 Impossible OS |
| --- | ------------------- | ----------------------- | ---------------- | ---------------- |
| 💎  | Event log viewer    | ✅ Event Viewer         | ✅ journalctl    | ⬜ §1–§2         |
| 💎  | Level filtering     | ✅ Filter by type       | ✅ journalctl -p | ⬜ §1–§2         |
| ⭐  | Built-in colour GUI | ⚠️ Separate MMC snap-in | ❌ CLI only      | ⬜ §2            |
| ⭐  | Live tail in GUI    | ❌ Manual refresh       | ✅ journalctl -f | ⬜ §3            |

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_eventview()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).

- [ ] Create `src/kernel/test/test_eventview.c` with:
  - `events.jsonl` exists after boot with debug=1
  - File contains at least 10 entries
  - Each line is valid JSON with `ts`, `level`, `tag`, `msg` fields
- [ ] Register in `test_runner_init()`: `test_register_eventview()`
- [ ] Commit: `"test: add event viewer JSONL validation test suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` → `=== BUILD OK ===`
- [ ] QEMU WHPX: `C:\> eventview` prints colour-coded log table
- [ ] QEMU WHPX: `eventview --level WARN` filters correctly
- [ ] GUI version shows table with working filters
- [ ] Bare metal: eventview reads FAT32-backed events.jsonl correctly
- [ ] Commit: `"tools: eventview.exe verified -- JSONL log viewer complete"`
