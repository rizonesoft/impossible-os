---
schema_version: 1
id: addr2line
domain: 14-host-tools
status: active
title: "TODO-03 -- ixfs-addr2line (Enhanced Address Resolver)"
---

# TODO-03 -- ixfs-addr2line (Enhanced Address Resolver)

> **Goal:** Replace `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` with a purpose-built tool that resolves crash addresses to function + file + line + source context, decodes error codes, and maps CR2 to known memory regions. One command for full crash analysis.

## Inputs

- `build/kernel.map` -- `llvm-nm-19 -n` text output (address, type, name), written on every kernel link (`Makefile`)
- `build/kernel.sym` -- packed binary `KSYM` table made from `kernel.map` by `tools/convert_symmap.py` (32-byte names, truncated at 31 chars); the kernel's own `symtab.c` reads it, a host tool should read `kernel.map`
- [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h) -- POST code definitions
- Kernel source tree (`src/`) -- for source context display

## Outcome

```
$ ixfs-addr2line 0x001234
  Function:  isr_handler
  File:      src/kernel/idt.c:214
  Source:
    212 |     if (handlers[vec]) {
    213 |         result = handlers[vec](frame);
  > 214 |         goto irql_restore;
    215 |     }
```

```
$ ixfs-addr2line --crash RIP=0x800000 CR2=0x800000 ERR=0x15
  RIP:  0x800000 → _start (user/hello.c:1) [user ELF entry]
  CR2:  0x800000 → page fault target (user ELF range 0x800000-0x815020)
  ERR:  0x15 = Page Fault: user-mode read, page not present, no reserved bit
  POST: Last POST 0x3031 = DESKTOP_READY (Phase 3)
```

## Implementation Order

| ⭐  | Order | Deliverable                                     | Depends On | Status |
| --- | :---: | ----------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Symbol map parser (kernel.sym + kernel.map)     | --         |  [ ]   |
| ⭐  |   2   | Source context display (show surrounding lines) | §1         |  [ ]   |
| ⭐  |   3   | Error code decoder (PF, GP, DF, MCE)            | --         |  [ ]   |
| ⭐  |   4   | Memory region mapper (CR2 → what was accessed)  | §1         |  [ ]   |
| ⭐  |   5   | Full crash decode mode (--crash flag)           | §1-§4      |  [ ]   |

---

## 1. Symbol Map Parser
Parse `build/kernel.map` (nm format: `address T symbol_name`); section boundaries come from the ELF section headers of `build/kernel.exe` (`kernel.map` is nm output, not a linker map).

**Files:** `sdk/src/addr2line/addr2line.c`

- [ ] Parse kernel.map: build sorted array of (address, name) pairs
- [ ] Binary search: find closest symbol ≤ input address
- [ ] Show: `function_name + offset` (e.g., `isr_handler+0x42`)
- [ ] Read .text, .data, .bss section ranges from the ELF section headers of `build/kernel.exe`
- [ ] Identify which section the address falls in
- [ ] Commit: `"sdk: addr2line symbol resolver from kernel.sym"`

## 2. Source Context Display
Find the source file and line number, show surrounding code with the crash line highlighted.

**Files:** `sdk/src/addr2line/addr2line.c`

- [ ] Parse DWARF debug info from `build/kernel.exe` (ELF .debug_line section) OR use the simpler approach: run `llvm-addr2line` as a subprocess and parse its output
- [ ] Display 5 lines of context around the crash line with `>` marker
- [ ] Color output: crash line in red, context in gray
- [ ] If DWARF unavailable: fall back to symbol-only output
- [ ] Commit: `"sdk: addr2line source context display"`

## 3. Error Code Decoder
Decode x86-64 exception error codes into human-readable descriptions.

**Files:** `sdk/src/addr2line/decode.c`

- [ ] Page Fault (err code): Present, Write, User, Reserved, Instruction Fetch bits
- [ ] General Protection: segment selector, IDT/GDT/LDT source
- [ ] Double Fault: always 0
- [ ] POST code lookup: parse boot_init.h `#define POST16_*` constants
- [ ] Commit: `"sdk: addr2line error code decoder -- PF, GP, DF, POST"`

## 4. Memory Region Mapper
Map a CR2 or address to known memory regions (kernel text, heap, user ELF, framebuffer, MMIO).

**Files:** `sdk/src/addr2line/regions.c`

- [ ] Known regions from `include/kernel/mm/memmap.h` (the address-space source of truth), not hard-coded: kernel image, heap, user range, framebuffer, LAPIC, IOAPIC
- [ ] Use the ELF section headers of `build/kernel.exe` for exact image boundaries
- [ ] Output: `CR2 0x800000 → user ELF entry (0x800000-0x815020)`
- [ ] Commit: `"sdk: addr2line memory region mapper"`

## 5. Full Crash Decode Mode
Single command that decodes an entire BSOD register dump.

**Files:** `sdk/src/addr2line/addr2line.c`

- [ ] `--crash RIP=0x... CR2=0x... ERR=0x... CR3=0x... CS=0x...`
- [ ] Auto-decode: RIP → function, CR2 → region, ERR → description, CS → ring level
- [ ] Detect: user vs kernel mode crash (CS & 3)
- [ ] Suggest: likely cause based on error pattern
- [ ] Commit: `"sdk: addr2line full crash decode mode"`

## Verification

- [ ] `ixfs-addr2line 0x001234` -- shows function + file + source context
- [ ] `ixfs-addr2line --crash RIP=0x800000 CR2=0x800000 ERR=0x15` -- full decode
- [ ] Works on both Linux and Windows
