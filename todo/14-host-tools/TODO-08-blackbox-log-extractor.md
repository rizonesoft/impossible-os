---
schema_version: 1
id: blackbox-log-extractor
domain: 14-host-tools
status: active
title: "TODO-08 -- BlackBox Log Extractor"
---

# TODO-08 -- BlackBox Log Extractor

> **Goal:** Host-side tools (Linux + Windows) that extract, view, and analyze logs from the BlackBox partition (`X:\`) inside raw disk images or mounted drives. Enables Claude Code, CI pipelines, and developers to read kernel logs, crash dumps, boot timelines, and events.jsonl without booting the OS. Solves the "not verifiable from serial log, requires filesystem inspection" gap in TODO-02 verification.

> [!IMPORTANT]
> **Current state:** No host-side tool can read logs from the disk image. Verification items in TODO-02 (network.log content, boot.log content, events.jsonl parsing, rotation, crash_recovery.log) are all blocked on filesystem inspection. Serial output is limited to ~5000 chars when pasted into Claude Code. This tool closes that gap.

## Inputs

- [`tools/make-system-disk.c`](../../tools/make-system-disk.c) -- GPT layout, partition offsets
- [`build/system-disk.img`](../../build/system-disk.img) -- raw disk image with BlackBox partition
- -> XREF: `01-boot-platform/TODO-24-blackbox-service-partition.md` -- BlackBox partition layout (128 MiB FAT32, GPT name "BlackBox")
- -> XREF: `02-kernel-core/TODO-04-system-logging.md` -- log file formats, events.jsonl schema, verification items
- -> XREF: `14-host-tools/TODO-06-disk-inspect.md` -- disk image browser (complementary -- disk-inspect shows raw structures, this shows log content)

## Outcome

- `blackbox` CLI tool runs on Linux (primary) and Windows (cross-compiled or native)
- Extract all logs from BlackBox partition to a local directory
- View specific log files (kernel.log, events.jsonl, boot session logs)
- Parse events.jsonl with filtering (by level, subsystem, time range, CPU, PID)
- Verify HMAC integrity chain (when §10 is implemented)
- Claude Code can invoke `blackbox extract` to read logs after a QEMU run
- CI pipelines can validate log content as part of automated testing

## Implementation Order

| S   | Order | Deliverable                              | Depends On  | Status |
| --- | :---: | ---------------------------------------- | ----------- | :----: |
| 💎   |   1   | GPT parser -- find BlackBox partition by name | --          |  [ ]   |
| 💎   |   2   | FAT32 reader -- traverse dirs, read files | S1          |  [ ]   |
| 💎   |   3   | `blackbox extract` -- dump all logs to local dir | S1, S2      |  [ ]   |
| 💎   |   4   | `blackbox cat` -- view a specific log file | S1, S2      |  [ ]   |
| 💎   |   5   | `blackbox events` -- parse/filter events.jsonl | S1, S2      |  [ ]   |
| 💎   |   6   | `blackbox boot` -- show boot timeline from JSON | S1, S2      |  [ ]   |
| S   |   7   | `blackbox verify` -- HMAC chain verification | S5, T02 S10 |  [ ]   |
| 💎   |   8   | Windows build -- MSVC or MinGW cross-compile | S1-S6       |  [ ]   |
| S   |   9   | Shell wrapper scripts for Claude Code integration | S3          |  [ ]   |

> 💎 = parity -- standard tooling expectation for OS development.
> S = scope -- internal project or unique feature.

---

## 1. GPT Parser -- Find BlackBox Partition by Name

Read GPT header and partition entries from a raw disk image to locate the BlackBox partition.

- [ ] Parse protective MBR (LBA 0) to confirm GPT disk
- [ ] Read GPT header (LBA 1): verify "EFI PART" signature, read partition entry LBA and count
- [ ] Iterate partition entries: match GPT name "BlackBox" (UTF-16LE comparison)
- [ ] Extract start LBA and end LBA for the matched partition
- [ ] Calculate byte offset: `start_lba * 512`
- [ ] Fallback: if no "BlackBox" name match, try second non-EFI FAT32 partition
- [ ] Error handling: print clear message if disk has no BlackBox partition
- [ ] Commit: `"tools: blackbox GPT parser -- find BlackBox partition by name"`

**Test checkpoint:** `blackbox info build/system-disk.img` prints: `BlackBox partition at LBA NNNN (offset 0xNNNNNN), 128 MiB, FAT32`.

## 2. FAT32 Reader -- Traverse Dirs, Read Files

Minimal FAT32 read-only implementation for the host tool. Does not need write support.

- [ ] Parse FAT32 BPB: bytes per sector, sectors per cluster, reserved sectors, FAT size, root cluster
- [ ] Read FAT table: follow cluster chains
- [ ] Read directory entries: 8.3 SFN + LFN reassembly
- [ ] Read file content: follow cluster chain, output to buffer or file
- [ ] Directory listing: recursive traversal with indented tree output
- [ ] Handle subdirectories: Logs/, Boot/, Crash/, Perf/, Diag/, Tools/
- [ ] Commit: `"tools: blackbox FAT32 reader -- directory traversal and file read"`

**Test checkpoint:** `blackbox ls build/system-disk.img` shows the BlackBox directory tree. `blackbox cat build/system-disk.img X:\Logs\kernel.log` outputs file content.

## 3. `blackbox extract` -- Dump All Logs to Local Directory

Extract the entire BlackBox partition content to a local directory.

- [ ] `blackbox extract <disk.img> [output_dir]` -- default output: `./blackbox-extract/`
- [ ] Recreate directory structure: `output/Logs/`, `output/Boot/`, `output/Crash/`, etc.
- [ ] Extract all files preserving names and paths
- [ ] Print summary: N files extracted, total size
- [ ] Skip empty directories (create them but note "empty")
- [ ] Overwrite existing output dir with `--force` flag
- [ ] Commit: `"tools: blackbox extract -- dump BlackBox partition to local directory"`

**Test checkpoint:** `blackbox extract build/system-disk.img /tmp/bb` creates `/tmp/bb/Logs/kernel.log`, `/tmp/bb/Boot/*.LOG`, etc. Files match what the OS wrote.

## 4. `blackbox cat` -- View a Specific Log File

Quick single-file viewer without full extraction.

- [ ] `blackbox cat <disk.img> <path>` -- e.g., `blackbox cat disk.img Logs/kernel.log`
- [ ] Path is relative to BlackBox root (no `X:\` prefix needed, but accept it if given)
- [ ] Output to stdout (pipeable to `grep`, `less`, `head`, etc.)
- [ ] `--tail N` flag: show last N lines only
- [ ] Decompress `.N.lz4` rotated logs: parse `klog_lz4_hdr_t` by explicit little-endian offsets (big-endian-host-safe), CRC32-validate, LZ4-decompress -- mirrors `klog_decompress_rotated` (XREF: TODO-04 §13 byte-order owner)
- [ ] Commit: `"tools: blackbox cat -- view log file from disk image"`

**Test checkpoint:** `blackbox cat build/system-disk.img Logs/kernel.log | head -20` shows the first 20 lines of the kernel log. `blackbox cat build/system-disk.img Logs/events.jsonl | jq .` parses correctly.

## 5. `blackbox events` -- Parse and Filter events.jsonl

Structured event log viewer with filtering.

- [ ] `blackbox events <disk.img>` -- parse and display events.jsonl
- [ ] `--level WARN` -- filter by minimum level (DEBUG/INFO/WARN/ERROR/FATAL)
- [ ] `--subsystem net` -- filter by subsystem tag
- [ ] `--cpu 1` -- filter by CPU ID
- [ ] `--pid 2` -- filter by process ID
- [ ] `--after 1000` -- filter events after timestamp (ms)
- [ ] `--before 5000` -- filter events before timestamp (ms)
- [ ] `--json` -- output raw JSON (default: human-readable formatted table)
- [ ] `--count` -- show event count per subsystem/level (summary mode)
- [ ] Human-readable output: colorized level, aligned columns, timestamp in seconds
- [ ] Commit: `"tools: blackbox events -- parse and filter events.jsonl"`

**Test checkpoint:** `blackbox events build/system-disk.img --level WARN` shows only WARN/ERROR/FATAL entries. `blackbox events build/system-disk.img --subsystem net --json | jq length` returns the count of network events.

## 6. `blackbox boot` -- Show Boot Timeline from JSON

Parse boot timeline JSON and display formatted boot timing analysis.

- [ ] `blackbox boot <disk.img>` -- find latest boot session in Boot/ directory
- [ ] Parse YYMMDDN.json boot timeline entries
- [ ] Display: phase, step name, start time, duration, sorted by duration (longest first)
- [ ] `--session YYMMDDN` -- view a specific boot session (not just latest)
- [ ] `--compare` -- compare two boot sessions side by side (regression detection)
- [ ] Total boot time summary at the bottom
- [ ] Commit: `"tools: blackbox boot -- boot timeline viewer and comparison"`

**Test checkpoint:** `blackbox boot build/system-disk.img` shows boot phases with timing. Output matches what serial log shows for boot step durations.

## 7. `blackbox verify` -- HMAC Chain Verification

Verify the integrity of events.jsonl using the HMAC chain (requires TODO-02 §10).

- [ ] `blackbox verify <disk.img>` -- verify HMAC chain in events.jsonl
- [ ] Read session key from Registry hive (if accessible) or accept `--key <hex>` parameter
- [ ] Walk each JSON line, recompute HMAC, compare to stored value
- [ ] Report: "N entries verified, chain intact" or "TAMPERED at line M"
- [ ] Exit code 0 = valid, 1 = tampered, 2 = no HMAC fields found
- [ ] Commit: `"tools: blackbox verify -- HMAC chain integrity checker"`

**Test checkpoint:** `blackbox verify build/system-disk.img` returns exit code 0 on an unmodified log. After hex-editing one events.jsonl entry, returns exit code 1 with the tampered line number.

## 8. Windows Build

Cross-compile or native build for Windows.

- [ ] MinGW cross-compile from WSL: `x86_64-w64-mingw32-gcc` target
- [ ] Or native MSVC build via `cl.exe` with a `build-blackbox.bat`
- [ ] Output: `sdk/tools/blackbox.exe`
- [ ] Test on Windows: `blackbox.exe extract \\wsl.localhost\...\build\system-disk.img`
- [ ] Commit: `"tools: blackbox Windows build (MinGW cross-compile)"`

**Test checkpoint:** `blackbox.exe info build\system-disk.img` runs on Windows and finds the BlackBox partition.

## 9. Shell Wrapper Scripts for Claude Code Integration

Helper scripts so Claude Code can invoke the tool after QEMU runs.

- [ ] `scripts/tools/blackbox-extract.sh` -- extract logs from build/system-disk.img to build/blackbox/
- [ ] `scripts/tools/blackbox-events.sh` -- show filtered events (accepts --level, --subsystem args)
- [ ] `scripts/tools/blackbox-cat.sh <path>` -- quick file view
- [ ] Add to CLAUDE.md: document `bash scripts/tools/blackbox-extract.sh` as the way to inspect logs
- [ ] Commit: `"tools: blackbox shell wrappers for Claude Code integration"`

**Test checkpoint:** `bash scripts/tools/blackbox-extract.sh && cat build/blackbox/Logs/kernel.log | head -5` shows log content.

---

## OS Comparison

| S   | Feature                    | 🪟 Win11                | 🐧 Linux               | 🚀 Impossible OS                |
| --- | -------------------------- | ---------------------- | --------------------- | ------------------------------ |
| 💎   | Host log viewer            | ✅ Event Viewer (GUI)   | ✅ journalctl          | ⬜ S4-S5 -- blackbox cat/events |
| 💎   | Log extraction from image  | ⚠️ Mount + copy        | ✅ mount -o loop       | ⬜ S3 -- blackbox extract       |
| 💎   | Structured event filtering | ✅ Event Viewer filters | ✅ journalctl -p/-u    | ⬜ S5 -- blackbox events        |
| 💎   | Boot timing analysis       | ⚠️ xbootmgr (separate) | ✅ systemd-analyze     | ⬜ S6 -- blackbox boot          |
| S   | HMAC log verification      | ❌ Not available        | ❌ Not available       | ⬜ S7 -- blackbox verify        |
| 💎   | Cross-platform tool        | ❌ Windows only         | ❌ Linux only          | ⬜ S8 -- Linux + Windows        |
| S   | AI-agent integration       | ❌ Not designed for AI  | ❌ Not designed for AI | ⬜ S9 -- Claude Code wrappers   |

> After S1-S6, developers have complete host-side log inspection matching journalctl/Event Viewer capability.
> S7 (HMAC verify) and S9 (AI integration) are unique competitive advantages.

## Unit Tests

> Host tool -- tests run on Linux via `make test` in the tool's build directory.

- [ ] GPT parser: correctly identifies BlackBox partition in a test disk image
- [ ] GPT parser: returns error on disk without BlackBox partition
- [ ] FAT32 reader: reads known file content from a pre-built test image
- [ ] FAT32 reader: handles LFN entries correctly (lowercase filenames)
- [ ] events.jsonl parser: filters by level, subsystem, CPU, PID correctly
- [ ] Boot timeline parser: computes durations correctly from sample JSON
- [ ] Commit: `"test: blackbox host tool unit tests"`

## Verification

- [ ] `blackbox info build/system-disk.img` -- finds BlackBox partition, prints offset and size
- [ ] `blackbox extract build/system-disk.img /tmp/bb` -- extracts all files, directory structure intact
- [ ] `blackbox cat build/system-disk.img Logs/kernel.log` -- outputs kernel log content
- [ ] `blackbox events build/system-disk.img --level WARN` -- filters correctly
- [ ] `blackbox boot build/system-disk.img` -- shows boot timeline with durations
- [ ] `blackbox.exe` runs on Windows (MinGW build)
- [ ] Claude Code: `bash scripts/tools/blackbox-extract.sh && cat build/blackbox/Logs/kernel.log` works
- [ ] Commit: `"tools: blackbox log extractor verified -- extract, cat, events, boot, Windows"`
