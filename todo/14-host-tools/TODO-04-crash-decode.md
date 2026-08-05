---
schema_version: 1
id: crash-decode
domain: 14-host-tools
status: active
title: "TODO-04 -- crash-decode (Post-Mortem Crash Analyzer)"
---

# TODO-04 -- crash-decode (Post-Mortem Crash Analyzer)

> **Goal:** Paste a BSOD screenshot or serial dump, get a complete crash analysis: stack trace with source lines, root cause hypothesis, affected subsystem, and suggested fix. The tool every OS developer wishes existed.

## Inputs

- Serial log output (copy-paste or file)
- BSOD register dump (from screenshot or serial)
- `build/kernel.sym`, `build/kernel.map`, `build/kernel.exe`

## Outcome

```
$ crash-decode serial.log
  === CRASH ANALYSIS ===
  Stop code:   PAGE_FAULT
  Location:    isr_handler (src/kernel/idt.c:214)
  Faulting addr: 0x800000 (user ELF entry, not present in current PML4)
  Mode:        User (CS=0x1B, Ring 3)
  CR3:         0x1000 (per-process PML4)
  Root cause:  User page at 0x800000 missing User bit in PML4 level
  Last POST:   0x3031 (DESKTOP_READY)
  Boot phase:  Phase 3 (desktop was up, crash during cmd.exe execution)

  === STACK TRACE ===
  #0 0x800000  _start          user/hello.c:1
  (no further frames -- user-mode entry point)

  === TIMELINE ===
  Boot succeeded through Phase 3 in 9.7s
  Crash occurred 0.2s after cmd.exe task creation
```

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| 💎   |   1   | Serial log parser (extract BSOD block)   | --         |  [ ]   |
| ⭐   |   2   | Register dump decoder (integrate addr2line) | TODO-03    |  [ ]   |
| ⭐   |   3   | Timeline extraction (boot phases + crash point) | §1         |  [ ]   |
| ⭐   |   4   | Root cause hypothesis engine             | §2, §3     |  [ ]   |

---

## 1. Serial Log Parser
Extract the BSOD register dump block from a serial log or stdin.

- [ ] Detect BSOD block: find `Stop code:` and `Register Dump` markers
- [ ] Extract: RIP, CR2, CR3, CS, SS, RFLAGS, error code, all GP registers
- [ ] Extract: stack trace addresses (if available)
- [ ] Also accept manual input: `crash-decode --rip 0x800000 --cr2 0x800000 --err 0x15`

## 2. Register Dump Decoder
Decode all registers using addr2line (TODO-03) integration.

- [ ] RIP → function + file + line + source context
- [ ] CR2 → memory region (user ELF, kernel heap, framebuffer, MMIO)
- [ ] CR3 → identify PML4 (kernel boot PML4 vs per-process)
- [ ] CS → ring level (0 = kernel, 3 = user)
- [ ] RFLAGS → IF, AC, direction, carry -- highlight anomalies
- [ ] Error code → full bit decode per exception type

## 3. Timeline Extraction
Parse boot timing from the serial log to show where in boot the crash occurred.

- [ ] Parse `[PHASE0]`, `[PHASE1]`, etc. markers with timestamps
- [ ] Parse `Boot complete in N.Ns` line
- [ ] Identify: crash happened N seconds after boot, during Phase N
- [ ] Show last successful POST code before crash

## 4. Root Cause Hypothesis Engine
Based on error patterns, suggest likely causes.

- [ ] Page fault + User + not-present + CR2 in ELF range → "User page missing User bit"
- [ ] Page fault + kernel + write + NX range → "Write to read-only kernel page"
- [ ] Double fault → "Stack overflow or corrupted IDT"
- [ ] Triple fault (no BSOD) → "IST stack also failed -- check IST allocation"
- [ ] GP fault + CS=kernel → "Invalid segment selector or privileged instruction"
- [ ] Pattern database: extensible, add new patterns as bugs are found

## Verification

- [ ] Feed serial.log from a known crash → correct analysis output
- [ ] Manual input mode works with copy-pasted register values
- [ ] Works on Linux and Windows
