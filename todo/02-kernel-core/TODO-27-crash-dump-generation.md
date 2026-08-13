---
schema_version: 1
id: crash-dump-generation
domain: 02-kernel-core
status: active
title: "TODO-27 -- Crash Dump Generation"
---

# TODO-27 -- Crash Dump Generation

> **Goal:** Replace the current text-only `crashdump.log` with a complete binary crash-dump system: Windows-style STOP codes (`KeBugCheckEx`), FPU/XMM/XSAVE state capture, a kernel module registry, WinDbg-compatible MDMP binary format (Minidump / Kernel / Full variants), a raw-partition dump sink that bypasses the VFS (so a filesystem panic can still produce a dump), and a post-boot `dmpanalyze.exe` crash analyzer. When complete, any crash on real hardware produces a `.dmp` file that a developer can open directly in WinDbg or analyze with the built-in tool.

> [!IMPORTANT]
> **Current state (code-truth; `panic.c` is ~975 lines today):** **Implemented:** `KeBugCheckEx()` at `src/kernel/panic.c:120`, STOP taxonomy in `include/kernel/bugcheck.h`, `panic_capture_fpu_state()` at `panic.c:275` and `panic_build_context()` at `panic.c:328` (Win64 `CONTEXT`), module registry wiring with `exec_iterate_modules()` / `exec_iterate_modules_lockless()` in `src/kernel/exec.c`, MDMP structs and stream IDs in `include/kernel/crashdump.h`, unit coverage in `src/kernel/test/test_crashdump.c` (`TEST_CAT_BOOT`). **Still on VFS text path:** `panic_screen()` writes `C:\Impossible\System\crashdump.log` when no raw sink exists. **Still open:** `crashdump_write_minidump()` (§5), kernel/full writers + LZ4 (§6), `dump_sink_*` / `DUMP_PARTITION_HEADER` (§7), `dump_recovery_check()` + UI (§8), `dmpanalyze.exe` (§9), and §10 policy or interop documentation. **Doc note:** the older "greenfield" list is stale; STOP codes, FPU capture, module enumeration helpers, and MDMP type layout are in tree; writers, sink, recovery, analyzer, and §10 doc are not.

> [!CAUTION]
> **Memory rule:** The dump writer runs after a kernel panic: `kmalloc` may be unavailable (heap could be corrupt). All dump buffers must use `pmm_alloc_contiguous()` or statically allocated scratch pages reserved at boot. Never call `kmalloc` from the dump path.

---

## Inputs

- `src/kernel/panic.c` -- current text-dump writer; extend, do not replace
- `include/kernel/panic.h` -- `panic_screen` declaration
- `include/kernel/bugcheck.h` -- `BUGCHECK_CODE`, `bugcheck_name()`, `KeBugCheckEx` declarations
- `include/kernel/crashdump.h` -- `MINIDUMP_*` structs, `ImpossibleOSInfoStream`, stream type constants
- `include/kernel/idt.h` -- `struct interrupt_frame` (15 GPRs + int_no + err_code + CPU-pushed RIP/CS/RFLAGS/RSP/SS)
- `include/kernel/symtab.h` -- `symtab_resolve(addr, offset)`
- `src/kernel/mm/pmm.c` -- `pmm_used_page_count()`, `pmm_for_each_used_page()` needed for Full dump page walk
- `include/kernel/drivers/blkdev.h` -- `blkdev_write()`, `blkdev_sync()` for raw sink sector I/O (names in §7 must match this API)
- → XREF: `TODO-04-system-logging.md §4` -- structured crash event written to per-subsystem crash log at panic time; `dmpanalyze.exe` (§9) reads these log entries too
- → XREF: `TODO-04-system-logging.md §8` -- structured JSON crash event (§8) should include bugcheck code, params, and RIP in `events.jsonl` at panic time; note: the raw-partition dump workspace (§1) must not use physical page `0x80000` -- choose a different address or probe PMM for a contiguous free region (this constraint is a local §1 design note, not defined in TODO-27 §7)
- → XREF: `TODO-04-system-logging.md §7` -- pstore/ramoops-style persistent RAM crash log (text); complements this TODO's binary CPU/memory state to disk; both are needed (no overlap)
- → XREF: `TODO-17-binary-system.md §7` -- `exec_register_module()` populates the global `LOADED_MODULE` crash registry; §3 of this TODO consumes it for the ModuleList stream
- → XREF: `TODO-23-exception-dispatch-seh.md §1` -- LANDED: `EXCEPTION_RECORD` / `CONTEXT` / `XMM_SAVE_AREA32` now live in `except.h` (same 1232-byte layout); `panic.h` includes it, so §4 MDMP streams already consume the single-source T23 types
- → XREF: `TODO-23-exception-dispatch-seh.md §1` -- source of the §2 cross-task FPU dump-leak follow-up (T23 §1's frame converter deliberately does no FXSAVE for this reason)
- → XREF: `TODO-26-power-management.md §4` -- S4 hibernate partition GPT GUID reused as the raw dump partition backing store (§7); both share the same raw-write path
- → XREF: `TODO-28-bsod-ux-enhancements.md` §13 -- BSOD progress UI (`panic_set_progress`) while the dump writer runs; T28 owns on-screen UX, this TODO owns dump bytes and completion callbacks
- → XREF: `TODO-10-kernel-security-hardening.md` -- `CrashDumpEncryption` design and `X:\Crash\` default SDDL: security review gates with T10 before enforcement code in §9 or §6

> [!NOTE]
> **Planned doc (not in tree yet):** §10 creates `docs/kernel/crashdump-policy-interop.md` on the first §10 documentation commit. Until then, do not list that path under Inputs as a required on-disk anchor.

> [!NOTE]
> **Deferred: Live kernel dump (non-crashing).** Windows Task Manager live kernel dump (BugCheck `0x161` / `LIVE_SYSTEM_DUMP`) and legacy LiveKD-style tools capture memory without a full reboot. This stays deferred until after §9 (`dmpanalyze.exe`) and the crash dump pipeline are complete; §10 doc cites Microsoft Learn for stakeholders.

---

## Outcome

- Every kernel panic fires `KeBugCheckEx(code, p1, p2, p3, p4)` with a Windows-compatible STOP code; the BSOD screen shows the hex code.
- The crashing thread's FPU/XMM/YMM state is captured via `XSAVE`.
- A `LOADED_MODULE` registry tracks every driver and user ELF/PE with its base address, size, and name for inclusion in the ModuleList stream.
- Three dump types are written to the raw dump partition (VFS-bypass):
  - **Minidump** (always written): crashing thread context + 256 KiB stack
    + directly referenced memory pages; typically < 4 MiB.
  - **Kernel dump** (default): all kernel-mapped pages; typically 32--256 MiB.
  - **Full dump** (opt-in): all used physical pages, LZ4-compressed.
- At next boot, the dump is moved from the raw partition to `X:\Crash\*.dmp` before the desktop starts.
- `dmpanalyze.exe` opens any `.dmp` and produces a `!analyze -v` style report; WinDbg can open the same `.dmp` file directly.
- `docs/kernel/crashdump-policy-interop.md` maps Windows dump-type knobs, WinDbg KDUMP-ZLIB limits, and Linux `makedumpfile` filter bits to registry fields before §5 through §10 ship (file is planned; see Inputs NOTE).

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On     | Status |
| --- | :---: | ---------------------------------------------- | -------------- | :----: |
| 💎  |   1   | Bugcheck codes & `KeBugCheckEx`                | --             |  [x]   |
| 💎  |   2   | FPU/XMM/XSAVE state capture                    | §1             |  [/]   |
| 💎  |   3   | Module registry (wire `exec_register_module`)  | T17 §6         |  [x]   |
| 💎  |   4   | MDMP binary format: header, directory, streams | §1, §2, §3     |  [x]   |
| 💎  |   5   | Minidump writer (crashing thread + memory)     | §4             |  [ ]   |
| 💎  |   6   | Kernel dump & full dump variants               | §5             |  [ ]   |
| 💎  |   7   | Raw-partition dump sink (VFS bypass)           | §5, T26 §4     |  [ ]   |
| 💎  |   8   | Post-boot crash recovery + shutdown dialog     | §7, T01 §4     |  [ ]   |
| ⭐  |   9   | `dmpanalyze.exe` crash analyzer                | §4, §8         |  [ ]   |
| ⭐  |  10   | Dump policy, WinDbg interop, confidentiality   | §4, §6, T04 §6 |  [ ]   |

> 💎 = parity work: matches what Windows 11 and Linux already do.
> ⭐ = exclusive work: Impossible OS is superior or first.

---

## 1. Bugcheck Codes & `KeBugCheckEx`

Define Windows-compatible STOP code taxonomy and the `KeBugCheckEx` entry point that every kernel panic flows through.

- [x] Define `BUGCHECK_CODE` typedef and STOP code constants in `include/kernel/bugcheck.h` -- 11 Windows-compatible codes (0x0A, 0x1E, 0x50, 0x3B, 0x77, 0x7A, 0x139, 0xEF, 0xC5, 0xD1, 0xE2) plus 4 Impossible OS exclusive codes (0xE0000001--0xE0000004)
- [x] `const char *bugcheck_name(BUGCHECK_CODE code)` -- lookup table in `panic.c`, 15 entries, returns `"UNKNOWN"` for unrecognized codes
- [x] `KeBugCheckEx(code, p1, p2, p3, p4)` in `src/kernel/panic.c` -- stores params in `g_last_bugcheck`, builds STOP description string, routes to `panic_screen()`, `POST16(0xDE40)` on entry
- [x] Update `KPANIC(msg)` macro to call `KeBugCheckEx(BUGCHECK_MANUALLY_INITIATED_CRASH, ...)` internally; `KPANIC_FRAME` unchanged (frame-aware)
- [x] Registry persistence: writes `HKLM\SYSTEM\LastBugCheck\{Code, Param1-4}` via `RegCreateKeyEx`/`RegSetValueEx`, gated on `SUBSYS_REGISTRY` ready
- [x] NMI-triggered crash: `nmi_crash_handler` registered on vector 2 via `idt_register_handler` in `bugcheck_init()`, called from Phase 1 after IDT setup
- [x] Keyboard-triggered crash: `bugcheck_keyboard_check()` called from keyboard IRQ handler on every scancode; Ctrl+ScrollLock x2 within 2s triggers `KeBugCheckEx` if `HKLM\SYSTEM\CrashControl\CrashOnCtrlScroll` == 1
- [x] 6 test assertions in `test_crashdump.c`: name resolution (known, unknown, exclusive), info struct, constants, POST codes
- [x] Commit: `"kernel/panic: KeBugCheckEx, STOP code table, NMI/keyboard crash triggers"`
- [ ] **Unified panic owner token**: one first-caller claim for KeBugCheckEx + direct panic_screen entries, claimed AFTER async-worker isolation (an async AP must never park holding ownership and silence later panics)
- [ ] **Nested/NMI owner re-entry policy**: owner-side NMI or nested fault must not republish `g_last_bugcheck`/desc; route to a minimal serial-only emergency halt instead of re-entering the collector
- [ ] **POST16 after owner arbitration**: losers must not touch shared POST/framebuffer state pre-claim (raw port breadcrumb only)
- [ ] **Fault-context gating in `panic_screen_impl`** (TODO-23 §4 `KeBugCheckExFrame`): shared terminal still runs FPU-capture framebuffer POST16 + auto-restart registry I/O; gate both off for fault context (corrupt-fb/held-lock nested-faults).
- [ ] **Two-slot crash-dump publication**: generation + checksum protocol replacing in-place TRUNC so a failed rewrite never destroys the last good dump
- [ ] **IXFS flush honesty for the dump path**: `ixfs` flush must propagate bitmap/cache/journal errors and reach a device durability boundary before `write_crash_dump` may report success

**Test checkpoint:** `KeBugCheckEx(0xE2, 1, 2, 3, 4)` stores correct values in `g_last_bugcheck`. `bugcheck_name(0x50)` returns `"PAGE_FAULT_IN_NONPAGED_AREA"`. BSOD screen shows hex STOP code. `POST16(0xDE40)` on entry. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 2. FPU/XMM/XSAVE State Capture

Capture the crashing thread's FPU/XMM/YMM state and build a Windows-compatible `CONTEXT` record for inclusion in the MDMP.

- [x] Allocate static 4 KiB XSAVE scratch buffer in `panic.c`: `uint8_t g_panic_xsave_buf[4096] __attribute__((aligned(64)));` -- exported for test access
- [x] `panic_capture_fpu_state()` -- called from `panic_screen()` after async isolation, gated behind atomic `panic_try_claim_owner()`; clears CR0.TS/EM, checks CR4.OSXSAVE at runtime, uses `xsave64` with `xcr0_active` mask or `fxsave64` fallback; `POST16(0xDE42)` on entry
- [x] `CONTEXT` struct at Windows x64 layout (1232 bytes; GPRs, segment/debug regs, `Rip`, 512-byte `FltSave`, `M128A VectorRegister[26]`, LBR); size static-asserted. Canonical home is `include/kernel/except.h` (T23 §1); `panic.h` includes it
- [x] `panic_build_context(frame, ctx)` -- populates `CONTEXT` from `interrupt_frame` + `g_panic_xsave_buf`; reads live DS/ES/FS/GS and debug registers; copies FXSAVE region into `FltSave`, MXCSR into top-level `MxCsr`, AVX YMM high halves into `VectorRegister[0..15]`; sets `ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT`
- [ ] Cross-task FPU dump leak: sample CR0.TS in `panic_capture_fpu_state()` before `clts`; when set (faulting task deferred FPU), `panic_build_context()` inits `FltSave` + clears `CONTEXT_FLOATING_POINT` -> XREF: 02-kernel-core/TODO-23 §1
- [x] Commit: `"kernel/panic: XSAVE FPU capture, CONTEXT record population"`

**Test checkpoint:** After panic, `g_panic_xsave_buf` is non-zero (FPU was in use). `CONTEXT.MxCsr` is non-default. `CONTEXT.Rip` matches `interrupt_frame.rip`. `POST16(0xDE42)` on entry. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

**Regression risk:** `xsave64` causes #UD on CPUs without XSAVE (including TCG default). MUST check CPUID first. If broken, revert to `fxsave64` fallback.

---

## 3. Module Registry for Crash Dumps

> [!NOTE]
> **Already implemented:** `loaded_module_t` and `exec_register_module()` exist in `include/kernel/exec.h` / `src/kernel/exec.c` (TODO-17 §7). This section wires the existing module list into the crash dump writer -- it does NOT create a parallel module registry.

Wire the existing `exec_register_module()` / `exec_find_module_by_pc()` infrastructure (TODO-17 §7) into the crash dump pipeline so the ModuleList stream can enumerate all loaded binaries.

- [x] In `boot_phase1()` after bugcheck_init: call `exec_register_module()` for the kernel itself -- `base_address = __kernel_start` (1 MiB), `size_of_image = __kernel_end - __kernel_start`, `entry_point = kernel_main`, `name = "kernel.exe"`, `full_path = "C:\Impossible\System32\kernel.exe"`, `format = EXEC_FMT_PE`; `__kernel_start` added to linker script
- [x] Verified ELF/PE/EIF loaders: PE loader calls `exec_register_module()` in `pe.c:767`; ELF path calls in `task.c:1183`; EIF tracked in TODO-20 §4 (not yet wired)
- [x] Added `exec_iterate_modules(out, max)` (irqsave locked snapshot copy) and `exec_iterate_modules_lockless(out, max)` (NMI-safe, no lock) in `exec.c`/`exec.h` for crash dump ModuleList stream writer (§4)
- [x] Commit: `"kernel/crashdump: wire exec module registry into crash dump pipeline"`

**Test checkpoint:** 6 test assertions in `test_crashdump.c`: kernel module registered (count >= 1), find by PC (kernel_main resolves to "kernel.exe"), base/size match linker symbols, iterate snapshot, lockless iterate, POST code uniqueness. `POST16(0xDE44)` on entry. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 4. MDMP Binary Format: Header, Directory & Stream Types

Define the WinDbg-compatible MDMP binary format structures for writing crash dump files.

- [x] Define in `include/kernel/crashdump.h`: `MINIDUMP_HEADER` (32 bytes), `MDMP_SIGNATURE` (0x504D444D), `MDMP_VERSION` (0xA793), `MINIDUMP_TYPE` flags (`MiniDumpNormal`, `MiniDumpWithFullMemory`, `MiniDumpFilterMemory`, `MiniDumpWithCodeSegs` = 0x2000, `MiniDumpWithoutOptionalData`); all pack(4) matching Windows SDK `pshpack4.h`
- [x] `MINIDUMP_DIRECTORY` (12 bytes): `StreamType`, `DataSize`, `Rva`; `MINIDUMP_LOCATION_DESCRIPTOR` (8 bytes): `DataSize`, `Rva`
- [x] Stream type constants: `ThreadListStream` (3) through `MiscInfoStream` (15), plus `ImpossibleOSInfoStream` (0x8001, vendor range 0x8000+)
- [x] `MINIDUMP_EXCEPTION` (152 bytes) and `MINIDUMP_EXCEPTION_STREAM` (168 bytes): separate `MINIDUMP_EXCEPTION` type matching Windows layout, `ExceptionInformation[15]` for bugcheck params
- [x] `MINIDUMP_MODULE` (108 bytes, pack(4)): `BaseOfImage`, `SizeOfImage`, `CheckSum`, `TimeDateStamp`, `ModuleNameRva` (RVA -> `MINIDUMP_STRING`), `VS_FIXEDFILEINFO` (52 bytes), `CvRecord`, `MiscRecord`, `Reserved0/1`
- [x] `MINIDUMP_THREAD` (48 bytes): `ThreadId`, `SuspendCount`, `PriorityClass`, `Priority`, `Teb`, `Stack` (`MINIDUMP_MEMORY_DESCRIPTOR`), `ThreadContext`
- [x] `MINIDUMP_MEMORY_DESCRIPTOR` (16 bytes): `StartOfMemoryRange`, `Memory` (`MINIDUMP_LOCATION_DESCRIPTOR`)
- [x] `MINIDUMP_SYSTEM_INFO` (56 bytes): `ProcessorArchitecture` (AMD64=9), `ProcessorLevel/Revision`, `NumberOfProcessors/ProductType`, `MajorVersion=10`, `MinorVersion=0`, `BuildNumber`, `PlatformId` (2), `CSDVersionRva`, `SuiteMask`, `CPU_INFORMATION` union (24 bytes, X86CpuInfo + OtherCpuInfo)
- [x] `IMPOSSIBLE_OS_INFO` (436 bytes, custom stream): `Magic` (0x10DEAD00), `KernelBuild`, `BugCheckCode`, `BugCheckParams[4]`, `BugCheckName[64]`, `PanicTimestamp`, `PhysicalMemoryKiB`, `CpuCount`, `CpuBrandString[48]`, `KernelPath[256]`
- [x] 15 unit tests in `test_crashdump.c` S4: struct sizes, field offsets (MINIDUMP_HEADER, EXCEPTION_STREAM, MODULE, THREAD), all stream type constants, MINIDUMP_TYPE flag values, signature, version, POST16 code
- [x] Commit: `"kernel/crashdump: MDMP header, directory, all stream type structs, ImpossibleOSInfo"`

**Test checkpoint:** `sizeof(MINIDUMP_HEADER)` == 32 bytes. `MINIDUMP_DIRECTORY` == 12 bytes. `MINIDUMP_MODULE` == 108 bytes (pack(4)). `MDMP_SIGNATURE == 0x504D444D`. `ImpossibleOSInfoStream` type == 0x8001. Static asserts on all 12 struct sizes + 5 MINIDUMP_TYPE flags + signature/stream ID. `POST16(0xDE46)` on entry. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> [!NOTE]
> **T23 alignment (LANDED):** `CONTEXT` now lives in `except.h` (`TODO-23-exception-dispatch-seh.md §1`) and `panic.h` includes it -- same 1232-byte layout, single source of truth. §5's writer must still assert MDMP `ThreadContext` RVAs stay binary-compatible with WinDbg against those types (item below).

---

## 5. Minidump Writer

- [ ] Assert MDMP `ThreadContext` / `CONTEXT` layout matches WinDbg against the T23 §1 `except.h` types (CONTEXT now migrated there): static assert or `test_crashdump` table
- [ ] Minidump memory layout (fixed region order, offsets computed at write time):
  ```
  [0x0000] MINIDUMP_HEADER         (28 bytes)
  [0x001C] MINIDUMP_DIRECTORY[N]   (12 bytes x N streams)
  [...]    CONTEXT record           (1232 bytes)
  [...]    ExceptionStream          (sizeof)
  [...]    ThreadListStream         (one entry per task)
  [...]    ModuleListStream         (one entry per module)
  [...]    SystemInfoStream
  [...]    MiscInfoStream
  [...]    ImpossibleOSInfoStream
  [...]    MemoryListStream descriptors
  [...]    Raw memory page data     (stack + referenced pages)
  [...]    Module name strings (UTF-16)
  [...]    OS CSD string
  ```

- [ ] Crashing thread's kernel stack: 8 pages (32 KiB) around `RSP`
- [ ] User-space stack (if crash was in user context): 16 pages around user RSP from `interrupt_frame.rsp`
- [ ] Code page at `RIP`: 1 page
- [ ] Any memory addresses appearing as arguments in the top 8 stack frames (heuristic: values in `[0x1000, KERNEL_BASE)` for user, or `[KERNEL_BASE, KERNEL_END)` for kernel) -- include the page they point to

- [ ] `crashdump_write_minidump(frame, ctx)`:
  1. Allocate two contiguous scratch pages (`pmm_alloc_contiguous(2)`) as the write buffer; the dump writer fills these pages and flushes via the raw sink (§7)
  2. Build `MINIDUMP_HEADER` + `MINIDUMP_DIRECTORY` array in scratch buffer
  3. Serialise each stream in order; compute RVA offsets as bytes from file start; update the corresponding `MINIDUMP_DIRECTORY` entry
  4. Append all raw memory page data (§5) at the end; update `MemoryListStream` descriptors with their final RVAs
  5. Compute CRC32C of the entire dump; write into `MINIDUMP_HEADER.Checksum`
  6. Call `dump_sink_write(buf, total_size)` (§7) to flush to disk

- [ ] Commit: `"kernel/crashdump: minidump writer, stream serialisation, CRC32C, memory page selection"`

**Test checkpoint:** `crashdump_write_minidump()` produces non-empty buffer with MDMP signature at offset 0. ExceptionStream contains bugcheck code. ModuleList contains kernel.exe. MemoryList includes stack pages. CRC32C validates. `POST16(0xDE48)` on entry. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 6. Kernel Dump & Full Dump Variants

- [ ] `dump_classify_page(pa)` -> `PAGE_CLASS`:
  - `PAGE_CLASS_KERNEL`: physical page mapped in the kernel's address range (upper half, `va >= 0xFFFF800000000000`)
  - `PAGE_CLASS_USER`: mapped in user space (`va < 0x0000800000000000`)
  - `PAGE_CLASS_HARDWARE`: MMIO / framebuffer / LAPIC ranges -- skip in kernel dump, include description in `ImpossibleOSInfoStream`
  - `PAGE_CLASS_FREE`: not in PMM used list -- skip always

- [ ] `crashdump_write_kernel_dump(frame, ctx)`:
  - Same MDMP structure as minidump but `Flags |= MiniDumpWithCodeSegs`
  - `MemoryListStream` uses `Memory64ListStream` format (64-bit base/size pairs + a single bulk RVA for all page data) instead of per-range RVAs
  - Walk PMM used-page list; include all `PAGE_CLASS_KERNEL` pages
  - On a typical system this is 32--256 MiB; apply per-page LZ4 block compression: each 4 KiB page is compressed to a `{compressed_size (u16), data[compressed_size]}` record; uncompressed pages (compression ratio < 1) are stored raw with `compressed_size = 0x8000 | 4096`
- [ ] Active Memory Dump variant: like kernel dump but excludes free page lists, standby page cache, and file-backed pages -- produces a smaller dump that still contains all diagnostic-relevant pages. `MINIDUMP_HEADER.Flags |= MiniDumpFilterMemory`
- [ ] `crashdump_type` Registry value: `0`=minidump (default), `1`=kernel dump, `2`=full dump, `3`=active dump; read in `panic_screen()` to select the writer

- [ ] `crashdump_write_full_dump(frame, ctx)`:
  - Walk all physical pages reported in `boot_info.memory_map` as `MEMORY_TYPE_USABLE` and `MEMORY_TYPE_KERNEL`
  - LZ4-compress each 64-page (256 KiB) chunk; write compressed chunk prefixed with `{orig_size_u32, comp_size_u32}`
  - On a 4 GiB machine: uncompressed ~4 GiB; after LZ4 typically 1--2 GiB
  - `MINIDUMP_HEADER.Flags |= MiniDumpWithFullMemory`
  - Raw-partition dump sink (§7) must be large enough; warn at boot time if dump partition < total RAM; fall back to kernel dump if too small

- [ ] Commit: `"kernel/crashdump: kernel dump variant, full dump with LZ4 compression, dump-type selector"`

**Test checkpoint:** Kernel dump includes all `PAGE_CLASS_KERNEL` pages. Full dump includes all used physical pages. LZ4 compression reduces dump size (compressed < uncompressed). Registry `crashdump_type` value selects correct writer. `POST16(0xDE4A)` on entry. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 7. Raw-Partition Dump Sink (VFS Bypass)

- [ ] **Prerequisite:** no `blkdev_open_by_gpt_type` exists in tree today; implement `dump_sink_probe()` using `gpt_*` plus existing `blkdev_*` (new helper name or inline GPT scan) before calling a nonexistent API
- [ ] Dedicated GPT partition identified by type GUID `{IMPOSSIBLE-DUMP-GUID}`: the same raw-block mechanism used by S4 hibernate (→ XREF `TODO-26-power-management.md §4`); if that GUID is present the partition doubles as the dump sink; if absent, fall back to the VFS text path
- [ ] At kernel init (Phase 1 -- before VFS): call `dump_sink_probe()` to scan GPT for the dump type GUID and open the raw block device (use the §7 prerequisite helper, not a nonexistent `blkdev_open_by_gpt_type`); cache the handle in static `g_dump_blkdev`; must work before any filesystem driver is mounted

- [ ] `dump_sink_write(buf, size)` -- streaming write engine:
  - Maintains a 512-byte sector-aligned write position `g_dump_offset`
  - Pads the buffer to the next 512-byte boundary with zeros
  - Issues synchronous sector writes via `blkdev_write()` from `include/kernel/drivers/blkdev.h` (there is no `blkdev_write_raw` in tree; map `g_dump_offset` to LBA and sector count); must not use IRQs or wait queues in panic context; busy-poll or driver-specific synchronous completion as required
  - If `g_dump_blkdev` is NULL (no dump partition): fall back to the existing VFS text-dump path from `panic.c`

- [ ] `DUMP_PARTITION_HEADER` at physical sector 0 of the dump partition:
  ```c
  typedef struct {
      uint32_t Magic;        /* 0xDEAD0001 */
      uint32_t DumpPresent;  /* 1 = valid dump waiting to be moved to FS */
      uint64_t DumpSize;     /* bytes of dump data (sectors 1 onwards) */
      uint32_t DumpType;     /* 0=minidump, 1=kernel, 2=full */
      uint32_t Checksum;     /* CRC32C of this header */
  } DUMP_PARTITION_HEADER;
  ```
- [ ] After writing the full dump body, `dump_sink_finalize()` writes the header to sector 0 with `DumpPresent = 1`
- [ ] `dump_sink_clear()` -- sets `DumpPresent = 0` after the dump has been safely moved to the filesystem (called by §8)

- [ ] `dump_emit_raw(str)` -- panic-safe emitter replacing `klog` in panic-path dumpers: `klog_emit` takes the blocking `s_klog_lock`, so a panic that interrupted logging stalls the owner. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §10`
  - Third consumer (2026-08-13): `01-boot-platform/TODO-10 §16` shipped the abort-safe SERIAL layer (`serial_write_emergency`, try-lock + bounded UART wait, latch armed post-owner-claim), which bounds `g_serial_lock` but NOT `s_klog_lock`. So `transition_ring_dump_to_serial`, `kernel_subsystem_dump` and `quota_dump_crash` still stall if the panic interrupted a `klog` holding `s_klog_lock` (`src/kernel/klog.c:1178`). The panic reason + register dump are emitted BEFORE that point and survive regardless; this item is what makes the later dumps survive too.
- [ ] Commit: `"kernel/crashdump: raw partition sink, VFS-bypass DMA write, dump partition header"`

**Test checkpoint:** `dump_sink_probe()` finds dump partition by GPT GUID. `dump_sink_write()` writes to raw partition without VFS. `DUMP_PARTITION_HEADER.DumpPresent` == 1 after write. `dump_sink_clear()` sets `DumpPresent` == 0. `POST16(0xDE4C)` on entry. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 8. Post-Boot Crash Recovery & CrashDumps Archive

- [ ] In kernel init Phase 2 (→ XREF `TODO-01-kernel-init-sequencing.md §4`), after VFS mounts but before the desktop starts: call `dump_recovery_check()`:
  1. `dump_sink_probe()` -- open dump partition
  2. Read sector 0; check `DUMP_PARTITION_HEADER.DumpPresent == 1`
  3. If present: read `DumpSize` bytes; write to `X:\Crash\{timestamp}.dmp` (create directory if absent; timestamp from `DumpPartitionHeader` or system RTC if unavailable)
  4. Also write a companion `.txt` file with the text dump (same content as the current `crashdump.log` -- keep the existing text path as a supplementary artifact)
  5. Call `dump_sink_clear()` -- zero the `DumpPresent` flag
  6. Set Registry `HKLM\SYSTEM\LastCrashDump` = the `.dmp` path and `HKLM\SYSTEM\CrashPending = 1`

- [ ] On desktop startup: check `HKLM\SYSTEM\CrashPending == 1`; if true:
  - Display a dialog: `"Impossible OS shut down unexpectedly.\n\nCrash dump saved to {path}.\n\n[View Report]  [Submit Report]  [Close]"`
  - `[View Report]`: launch `dmpanalyze.exe {path}` (§9)
  - `[Submit Report]` (opt-in): HTTP POST the minidump to the crash server URL from `HKLM\SYSTEM\CrashReporting\ServerUrl` (default empty = disabled)
  - `[Close]`: dismiss; set `HKLM\SYSTEM\CrashPending = 0`
- [ ] Dialog does not block shell startup; shown as a non-intrusive banner in the system tray notification area

- [ ] Keep at most 5 `.dmp` files in `X:\Crash\`; oldest is deleted when the 6th would be created
- [ ] `HKLM\SYSTEM\CrashDumps\MaxFiles` (REG_DWORD, default 5) controls the limit
- [ ] `HKLM\SYSTEM\CrashDumps\LastDump` (REG_SZ) = path to most recent `.dmp`; updated on each recovery pass

- [ ] Commit: `"kernel/crashdump: post-boot dump recovery, CrashDumps archive, unexpected-shutdown dialog"`

**Test checkpoint:** After simulated crash + reboot, `X:\Crash\*.dmp` exists. Registry `CrashPending` == 1. Dialog shows crash path. Archive rotation deletes 6th file when 5 exist. `POST16(0xDE4E)` on entry. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 9. `dmpanalyze.exe` Crash Analyzer

- [ ] `src/apps/dmpanalyze/dmpanalyze.c` -- standalone command-line app:
  - Open `.dmp` file; validate `MINIDUMP_HEADER.Signature == MDMP_SIGNATURE`
  - Parse `MINIDUMP_DIRECTORY` to locate all streams by `StreamType`
  - Extract `ExceptionStream`, `ModuleListStream`, `ThreadListStream`, `SystemInfoStream`, and `ImpossibleOSInfoStream`

- [ ] `dmpanalyze.exe <path.dmp>` produces a text report to stdout:
  ```
  === IMPOSSIBLE OS CRASH ANALYSIS ===
  STOP: 0x00000050 PAGE_FAULT_IN_NONPAGED_AREA
  Params: 0xDEADBEEF 0x0000000000000000 0x0000000000000001 0x0000000000000000

  FAULTING_IP: kernel!pmm_alloc_frame+0x42
    fffff800`00012345 48894808   mov qword ptr [rax+8],rcx

  CONTEXT:
    RAX=... RBX=... ... RIP=fffff80000012345 RSP=fffff80001234000

  STACK TRACE:
    #0  kernel!pmm_alloc_frame+0x42
    #1  kernel!vmm_alloc_pages+0x1F4
    #2  kernel!kmalloc+0x88
    ...

  LOADED MODULES:
    fffff80000000000  kernel.exe  (2.3 MiB)  \boot\kernel.exe
    ...

  SYSTEM INFO:
    OS: Impossible OS build 1024
    CPU: Intel(R) Core(TM) i7-1165G7 x 8
    RAM: 16384 MiB
  ```
- [ ] Stack trace: read thread stack pages from the dump's `MemoryListStream`; simulate the RBP chain walk the same way `panic.c` does; look up each frame in `ModuleListStream` + the embedded `symtab` (load `kernel.sym` if it exists on disk, or read symbol names from `ImpossibleOSInfoStream`)

- [ ] The `.dmp` file produced by the dump writer (§5) uses the standard MDMP format (`MDMP` magic, standard stream types, standard CONTEXT layout). A Windows developer can open it directly in WinDbg 10+ and run `!analyze -v`; module names and addresses from `ModuleListStream` appear in the module list; the `CONTEXT` record allows stack unwinding via PDB symbols if available
- [ ] Note: WinDbg needs matching PDB symbols for deep analysis; for kernel-only debugging, the MDMP alone is sufficient for `!analyze -v`

- [ ] `dmpanalyze /compare <dump1.dmp> <dump2.dmp>` -- diff two crash dumps:
  - Compare bugcheck code, faulting module, top-5 stack frames
  - Output: `SAME_STOP_CODE`, `SAME_FAULTING_MODULE`, `LIKELY_SAME_BUG` / `DIFFERENT_CRASH` classification
  - Useful for identifying duplicate crashes in a fleet of machines

- [ ] Commit: `"apps/dmpanalyze: MDMP parser, !analyze output, WinDbg-compatible dump, /compare"`

**Test checkpoint:** `dmpanalyze.exe test.dmp` produces output with STOP code, faulting function, and >= 3 stack frames. `/compare` on two identical dumps reports `LIKELY_SAME_BUG`. WinDbg opens the .dmp without format errors. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 10. Dump Policy, WinDbg Interoperability & Confidentiality

Windows 11 exposes multiple crash-dump settings (small, kernel, complete, automatic, active). Linux uses `kdump` plus `makedumpfile` filter bits and optional compression. WinDbg 1.2402+ can open Linux KDUMPs but only certain compressions. This section locks parity documentation and registry stubs so §5 through §9 do not diverge silently from industry behavior.

- [ ] Author `docs/kernel/crashdump-policy-interop.md`: map Microsoft Learn kernel dump varieties (complete, kernel, small, automatic, active) to planned `crashdump_type` in §6 and paging-file or raw-partition sizing notes
- [ ] Same doc: WinDbg Linux KDUMP rules (WinDbg 1.2402+; ZLIB KDUMP supported; LZO or Snappy unsupported per Microsoft Learn); record primary workflow as "ship native `.dmp` for WinDbg" and optional deflate second path if Linux KDUMP interop is required
- [ ] Same doc: map Linux `makedumpfile` dump_level filter bits to Impossible OS `PAGE_CLASS_*` in §6 for active-dump style subsets
- [ ] Registry design stub in the doc: `HKLM\SYSTEM\CrashControl\CrashDumpEncryption` (DWORD `0` = off, future `1` = encrypt-at-rest on raw sink); gate any implementation on `→ XREF: TODO-10-kernel-security-hardening.md` review
- [ ] When object ACL or SDDL APIs exist: `dump_set_default_sd()` contract for `X:\Crash\` (SYSTEM + Administrators read); until then §8 recovery path emits one `klog(LOG_WARN, "CrashDumps: ACL not enforced")` on first archive write
- [ ] Mirror JSON field names into `TODO-04-system-logging.md §8` when TODO-27 §7 first sets `DUMP_PARTITION_HEADER` (`dump_encryption_state`, `dump_present_on_raw`; see T04 for the unchecked follow-up)
- [ ] Doc cites deferred live kernel dump (`0x161`) with Microsoft Learn task-manager live dump article; no kernel code until post-§9
- [ ] Commit: `"docs/kernel: crashdump policy, WinDbg-Linux interop, confidentiality stubs"`

**Test checkpoint:** After the doc commit, `test -f docs/kernel/crashdump-policy-interop.md` passes in CI or manual check; first `klog(LOG_WARN, "CrashDumps: ACL not enforced")` appears once §8 recovery runs on a build without ACL APIs. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## OS Comparison

| ⭐  | Feature                 | 🪟 Win11        | 🐧 Linux         | 🚀 Impossible OS |
| --- | ----------------------- | --------------- | ---------------- | ---------------- |
| 💎  | Text panic log          | ✅ Event log    | ✅ dmesg journal | ✅ crashdump.log |
| 💎  | STOP bugcheck codes     | ✅ Full codes   | ❌ OOPS differs  | ✅ Done §1       |
| 💎  | FPU XMM in dump         | ✅ CONTEXT      | ✅ ptrace core   | ✅ Done §2       |
| 💎  | Loaded module list      | ✅ WinDbg lm    | ✅ kcore modules | ✅ Done §3       |
| 💎  | MDMP structs WinDbg     | ✅ WER native   | ❌ ELF not MDMP  | ✅ Done §4       |
| 💎  | Minidump writer         | ✅ Full         | ⚠️ ELF mini      | ⬜ §5            |
| 💎  | Kernel full dump        | ✅ MEMORY.DMP   | ✅ makedumpfile  | ⬜ §6            |
| 💎  | Raw partition sink      | ✅ Pagefile     | ✅ kdump part    | ⬜ §7            |
| 💎  | Post boot WER dialog    | ✅ WER UI       | ✅ apport        | ⬜ §8            |
| 💎  | Dump archive rotate     | ✅ Minidump dir | ⚠️ distro        | ⬜ §8            |
| 💎  | WinDbg open same file   | ✅ Yes          | ❌ ELF no        | ⬜ §5            |
| 💎  | LZ4 compression         | ❌ Xpress       | ✅ makedumpfile  | ⬜ §6            |
| ⭐  | ImpossibleOSInfo stream | ❌ None         | ❌ None          | ✅ Done §4       |
| ⭐  | dmpanalyze compare      | ❌ Portal       | ❌ None          | ⬜ §9            |
| 💎  | Active memory dump      | ✅ Win10+       | ❌ None          | ⬜ §6            |
| 💎  | Automatic memory dump   | ✅ Pagefile     | ❌ N/A           | ⬜ §10 doc       |
| 💎  | WinDbg Linux KDUMP ZLIB | ✅ ZLIB only    | ✅ kdump         | ⭐ §10 doc       |
| 💎  | Crash dump dir ACL      | ✅ SDDL         | ⚠️ perms         | ⬜ §10           |
| 💎  | NMI crash dump          | ✅ NMI          | ✅ sysrq         | ✅ Done §1       |
| ⭐  | Ctrl ScrollLock crash   | ✅ Registry     | ❌ None          | ✅ Done §1       |
| ⭐  | On device dmpanalyze    | ❌ WinDbg       | ❌ crash tool    | ⬜ §9            |

After §1 through §4, Impossible OS matches Windows-style STOP codes, `CONTEXT`, module plumbing, and MDMP struct layout. §5 through §9 add writers, raw sink, recovery UI, and `dmpanalyze.exe`. §10 documents Windows dump-type parity, WinDbg versus Linux KDUMP compression limits, ACL or encryption stubs, and `makedumpfile` filter mapping before those knobs ship in code. Linux stays on ELF or KDUMP; Impossible OS keeps native `.dmp` plus `ImpossibleOSInfoStream` and on-device `/compare`.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_crashdump()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`; same pattern as `TODO-11-peb-teb-user-abi.md` Unit Tests).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [x] `src/kernel/test/test_crashdump.c` exists; `test_register_crashdump()` wired in `test_runner_init()`; suites use `TEST_CAT_BOOT`
- [x] §1 bugcheck: `bugcheck_name()` known/unknown/exclusive, `g_last_bugcheck` / POST codes (`test_crashdump.c`)
- [x] §2 FPU/CONTEXT: `CONTEXT` size/offsets, `panic_build_context`, XSAVE buffer alignment, FPU POST (`test_crashdump.c`)
- [x] §3 module registry: kernel module registered, `exec_find_module_by_pc`, iterate snapshot/lockless (`test_crashdump.c`)
- [x] §4 MDMP structs: header/directory/exception/module/thread sizes, signature, stream IDs, `ImpossibleOSInfoStream`, type flags (`test_crashdump.c`)
- [ ] §5 minidump writer: `crashdump_write_minidump()` MDMP signature at offset 0; `MemoryListStream` includes stack pages; CRC32C; `POST16(0xDE48)` when writer lands
- [ ] §6 and §7 kernel or full dump plus raw sink: `dump_sink_probe()` / `dump_sink_write()` smoke with mock or test partition when implemented
- [ ] §8 recovery: `dump_recovery_check()` moves `.dmp` to `X:\Crash\` and clears `DumpPresent` (integration or harness test)
- [ ] §9 `dmpanalyze.exe`: parse sample `.dmp`; stdout contains STOP line and >= 3 frames; `/compare` classification
- [ ] §10: `docs/kernel/crashdump-policy-interop.md` exists after first §10 doc commit; file names Windows automatic dump row and states WinDbg KDUMP ZLIB-only rule for Linux vmcores
- [ ] Commit: `"test: extend crash dump suite for §5 through §10 (writers, sink, recovery, dmpanalyze, doc)"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` shows all `test_crashdump_*` PASS; new §5 through §10 cases PASS once implemented; `tail -1 build/build.log` is `=== BUILD OK ===`. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## Verification

- [ ] **STOP code**: trigger `KeBugCheckEx(BUGCHECK_MANUALLY_INITIATED_CRASH, 1, 2, 3, 4)` from a shell command; BSOD screen shows `STOP: 0x000000E2 (0x1, 0x2, 0x3, 0x4)`; Registry `HKLM\SYSTEM\LastBugCheck\Code` == `0xE2`.
- [ ] **XSAVE**: verify `g_panic_xsave_buf` is non-zero after a panic (FPU was in use); confirm `CONTEXT.MxCsr` is non-default.
- [ ] **Module list**: `exec_find_module_by_pc(kernel_main, &mod)` returns 0 and `mod.name` equals `"kernel.exe"`; `exec_find_module_by_pc(0, &mod)` returns -1 (not found).
- [ ] **Minidump validity**: trigger a test panic; recover `.dmp` from `X:\Crash\`; open with `dmpanalyze.exe`; output must include STOP code, faulting function name, and at least 3 stack frames.
- [ ] **WinDbg round-trip**: copy the `.dmp` to a Windows machine; open in WinDbg 10+; `!analyze -v` must complete without errors (symbol names optional; raw addresses are sufficient for format validation).
- [ ] **VFS-bypass**: deliberately trigger a panic after VFS unmount; verify the dump partition header shows `DumpPresent = 1` at sector 0.
- [ ] **Full dump CRC**: write a full dump; `dmpanalyze.exe` reads and validates `MINIDUMP_HEADER.Checksum` CRC32C; value must match.
- [ ] **§10 doc:** repository contains `docs/kernel/crashdump-policy-interop.md` with Windows dump-type table, WinDbg KDUMP compression limits, and `makedumpfile` dump_level mapping notes (see §10 checklist).
- [ ] **Remaining limits**: three, none of them blocking the sink itself
      - `dmpanalyze /compare` is a convenience feature, not part of the dump contract.
      - Full `!analyze -v` symbol resolution requires `kernel.sym` on the analysis machine.
      - The `[Submit Report]` WER upload path waits on the HTTP client from `07-networking` before it can be wired up.
- [ ] Commit: `"kernel/crashdump: KeBugCheckEx, MDMP minidump/kernel/full dump, raw-partition sink, dmpanalyze"`

**Test checkpoint:** Every Verification bullet above passes where hardware allows; `bash scripts/test.sh SUITE=boot` green for `test_crashdump_*`; `tail -1 build/build.log` is `=== BUILD OK ===`. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot); crash dump suites use `TEST_CAT_BOOT`.

---

## History

| Date       | Action   | Summary |
| ---------- | -------- | ------- |
| 2026-04-13 | gap-analysis | Web research (Win11 dump varieties, WinDbg Linux KDUMP, live dump 0x161, tamper-resistance queries); Learn fetch dump varieties + Linux crash dumps; refreshed IMPORTANT code-truth; Inputs `bugcheck.h`/`crashdump.h`/doc; new §10 + Impl order 10; OS table + T04/T17/TODO-10 mirrors; §4 left at 11 items (split deferred). |
| 2026-04-13 | validate     | Full-file validate: repaired T10 XREF + planned-doc NOTE; Impl `--`; §10 Depends On trimmed; §4 vs T23 §1 drift (Depends On + Inputs + NOTE); §7 blkdev API names; OS table; `---` before Unit Tests; blkdev.h Input. |
| 2026-04-17 | validate | Full-file validate: CAUTION and legend prose `--` fixes; `→ XREF` Inputs; Impl row 1 `(none)`; OS blurb § ranges; merged §9 checklist continuation; `---` before Unit Tests; Unit Tests + Verification **Test checkpoint**; History created; `run-boot-tests.bat` present; continuation-line rg hits are fenced blocks only. |