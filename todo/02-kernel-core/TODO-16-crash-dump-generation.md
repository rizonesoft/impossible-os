# TODO-16 -- Crash Dump Generation

> **Goal:** Replace the current text-only `crashdump.log` with a complete
> binary crash-dump system: Windows-style STOP codes (`KeBugCheckEx`), FPU/XMM/XSAVE state capture, a kernel module registry, WinDbg-compatible MDMP binary format (Minidump / Kernel / Full variants), a raw-partition dump sink that bypasses the VFS (so a filesystem panic can still produce a dump), and a post-boot `dmpanalyze.exe` crash analyzer. When complete, any crash on real hardware produces a `.dmp` file that a developer can open directly in WinDbg or analyze with the built-in tool.

> [!IMPORTANT]
> **Current state:** `src/kernel/panic.c` (558 lines) writes a text dump to
> `C:\Impossible\System\crashdump.log` via VFS. It captures: all 16 GPRs from `struct interrupt_frame`, an RBP-chain stack trace resolved through `symtab_resolve()`, CR2/CR3, and a BSOD screen. `include/kernel/symtab.h` provides `symtab_resolve(addr, offset)` loaded from `kernel.sym`.
> **Greenfield gaps:** no STOP code taxonomy, no FPU/XMM state, no module
> list, no binary MDMP format, no raw-partition dump sink (VFS path fails if the FS is the crash cause), no dump-type variants, no post-boot recovery dialog, and no analysis tool.

> [!CAUTION]
> **Memory rule:** The dump writer runs after a kernel panic -- `kmalloc` may
> be unavailable (heap could be corrupt). All dump buffers must use `pmm_alloc_contiguous()` or statically allocated scratch pages reserved at boot. Never call `kmalloc` from the dump path.

---

## Inputs

- `src/kernel/panic.c` -- current text-dump writer; extend, do not replace
- `include/kernel/panic.h` -- `panic_screen` declaration
- `include/kernel/idt.h` -- `struct interrupt_frame` (15 GPRs + int_no + err_code + CPU-pushed RIP/CS/RFLAGS/RSP/SS)
- `include/kernel/symtab.h` -- `symtab_resolve(addr, offset)`
- `src/kernel/mm/pmm.c` -- `pmm_used_page_count()`, `pmm_for_each_used_page()` needed for Full dump page walk
- → XREF: `TODO-02-system-logging.md §2` -- structured crash event written to per-subsystem crash log at panic time; `dmpanalyze.exe` (§9) reads these log entries too
- → XREF: `TODO-02-system-logging.md §6` -- structured JSON crash event (§6) should include bugcheck code, params, and RIP in `events.jsonl` at panic time; note: the raw-partition dump workspace (§7) must not use physical page `0x80000` -- choose a different address or probe PMM for a contiguous free region (this constraint is a local §7 design note, not defined in TODO-02 §6)
- → XREF: `TODO-02-system-logging.md §8` -- crash-persistent ring buffer capture to reserved physical memory; complementary to binary dump -- §8 captures text log, this TODO captures CPU/memory state
- → XREF: `TODO-08-binary-system.md §6` -- `exec_register_module()` populates the global `LOADED_MODULE` crash registry; §3 of this TODO consumes it for the ModuleList stream
- → XREF: `TODO-10-exception-dispatch-seh.md §1` -- `EXCEPTION_RECORD` and `CONTEXT` types defined there; §4 of this TODO embeds them verbatim in the MDMP Exception stream
- → XREF: `TODO-15-power-management.md §4` -- S4 hibernate partition GPT GUID reused as the raw dump partition backing store (§7); both share the same raw-write path

---

## Outcome

- Every kernel panic fires `KeBugCheckEx(code, p1, p2, p3, p4)` with a Windows-compatible STOP code; the BSOD screen shows the hex code.
- The crashing thread's FPU/XMM/YMM state is captured via `XSAVE`.
- A `LOADED_MODULE` registry tracks every driver and user ELF/PE with its base address, size, and name for inclusion in the ModuleList stream.
- Three dump types are written to the raw dump partition (VFS-bypass):
  - **Minidump** (always written): crashing thread context + 256 KiB stack
    + directly referenced memory pages; typically < 4 MiB.
  - **Kernel dump** (default): all kernel-mapped pages; typically 32–256 MiB.
  - **Full dump** (opt-in): all used physical pages, LZ4-compressed.
- At next boot, the dump is moved from the raw partition to `X:\Crash\*.dmp` before the desktop starts.
- `dmpanalyze.exe` opens any `.dmp` and produces a `!analyze -v` style report; WinDbg can open the same `.dmp` file directly.

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On          | Status |
| --- | :---: | ---------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | Bugcheck codes & `KeBugCheckEx`                | --                  |  [ ]   |
| 💎  |   2   | FPU/XMM/XSAVE state capture                   | §1                  |  [ ]   |
| 💎  |   3   | Module registry (wire `exec_register_module`)  | T08 §6              |  [ ]   |
| 💎  |   4   | MDMP binary format: header, directory, streams | §1, §2, §3, T10 §1 |  [ ]   |
| 💎  |   5   | Minidump writer (crashing thread + memory)     | §4                  |  [ ]   |
| 💎  |   6   | Kernel dump & full dump variants               | §5                  |  [ ]   |
| 💎  |   7   | Raw-partition dump sink (VFS bypass)            | §5, T15 §4         |  [ ]   |
| 💎  |   8   | Post-boot crash recovery + shutdown dialog     | §7, T01 §4         |  [ ]   |
| ⭐  |   9   | `dmpanalyze.exe` crash analyzer                | §4, §8              |  [ ]   |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.

---

## 1. Bugcheck Codes & `KeBugCheckEx`

Define Windows-compatible STOP code taxonomy and the `KeBugCheckEx` entry point that every kernel panic flows through.

- [ ] Define `BUGCHECK_CODE` typedef and STOP code constants in `include/kernel/bugcheck.h` -- Windows-compatible codes (0x0A, 0x1E, 0x50, 0x3B, 0x77, 0x7A, 0x139, 0xEF, 0xC5, 0xD1, 0xE2) plus Impossible OS exclusive codes (0xE0000001--0xE0000004)
- [ ] `const char *bugcheck_name(BUGCHECK_CODE code)` -- returns human-readable string for BSOD and dump analysis
- [ ] `KeBugCheckEx(code, p1, p2, p3, p4)` in `src/kernel/panic.c` -- stores params in static `g_last_bugcheck`, updates BSOD stop code line, triggers dump pipeline (§5)
- [ ] Update `PANIC(msg)` and `PANIC_IF(cond, msg)` macros to call `KeBugCheckEx(BUGCHECK_MANUALLY_INITIATED_CRASH, ...)` internally
- [ ] Registry persistence: write `HKLM\SYSTEM\LastBugCheck\{Code, Param1-4, Timestamp}` early in crash path (-> XREF TODO-13 §4)
- [ ] Commit: `"kernel/panic: KeBugCheckEx, STOP code table, bugcheck Registry persistence"`

**Test checkpoint:** `KeBugCheckEx(0xE2, 1, 2, 3, 4)` stores correct values in `g_last_bugcheck`. `bugcheck_name(0x50)` returns `"PAGE_FAULT_IN_NONPAGED_AREA"`. BSOD screen shows hex STOP code. `POST16(0xDE40)` on entry. Test on: QEMU WHPX + TCG; bare metal.

---

## 2. FPU/XMM/XSAVE State Capture
Capture the crashing thread's FPU/XMM/YMM state and build a Windows-compatible `CONTEXT` record for inclusion in the MDMP.

- [ ] Allocate static 4 KiB XSAVE scratch buffer in `panic.c`: `static uint8_t g_panic_xsave_buf[4096] __attribute__((aligned(64)));`
- [ ] `panic_capture_fpu_state()` -- called at top of `panic_screen()`: use `xsave64` if XSAVE CPUID bit set, fall back to `fxsave64` otherwise
- [ ] Define `CONTEXT` struct in `include/kernel/panic.h` at Windows x64 layout: `ContextFlags`, `MxCsr`, segment registers, debug registers, all GPRs, `Rip`, 512-byte `FltSave`, YMM high halves, LBR fields
- [ ] `panic_build_context(frame, ctx)` -- populate `CONTEXT` from `interrupt_frame` + `g_panic_xsave_buf`; set `ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT`
- [ ] Commit: `"kernel/panic: XSAVE FPU capture, CONTEXT record population"`

**Test checkpoint:** After panic, `g_panic_xsave_buf` is non-zero (FPU was in use). `CONTEXT.MxCsr` is non-default. `CONTEXT.Rip` matches `interrupt_frame.rip`. `POST16(0xDE42)` on entry. Test on: QEMU WHPX + TCG; bare metal.

**Regression risk:** `xsave64` causes #UD on CPUs without XSAVE (including TCG default). MUST check CPUID first. If broken, revert to `fxsave64` fallback.

---

## 3. Module Registry for Crash Dumps

> [!NOTE]
> **Already implemented:** `loaded_module_t` and `exec_register_module()` exist in `include/kernel/exec.h` / `src/kernel/exec.c` (TODO-08 §6). This section wires the existing module list into the crash dump writer -- it does NOT create a parallel module registry.

Wire the existing `exec_register_module()` / `exec_find_module_by_pc()` infrastructure (TODO-08 §6) into the crash dump pipeline so the ModuleList stream can enumerate all loaded binaries.

- [ ] In `kernel_main()` Phase 1: call `exec_register_module()` for the kernel itself -- `base_address = KERNEL_IMAGE_BASE`, `size_of_image` from `__kernel_end - __kernel_start` linker symbols, `name = "kernel.exe"`
- [ ] Verify ELF/PE/EIF loaders already call `exec_register_module()` (done in TODO-08 §6/§8) -- no new code needed, just verify the chain
- [ ] In the crash dump ModuleList stream writer (§4): iterate `exec_module_count()` entries via `exec_find_module_by_pc()` or a new `exec_iterate_modules()` helper to enumerate all registered modules
- [ ] Commit: `"kernel/crashdump: wire exec module registry into crash dump pipeline"`

**Test checkpoint:** After boot, `exec_find_module_by_pc(kernel_entry)` returns `"kernel.exe"` with correct base. After loading cmd.exe, module count >= 2. `POST16(0xDE44)` on entry. Test on: QEMU WHPX + TCG; bare metal.

---

## 4. MDMP Binary Format: Header, Directory & Stream Types

Define the WinDbg-compatible MDMP binary format structures for writing crash dump files.

- [ ] Define in `include/kernel/crashdump.h`:
  ```c
  #define MDMP_SIGNATURE  0x504D444D  /* "MDMP" */
  #define MDMP_VERSION    0x0000A793  /* matching Windows minidump version */

  typedef struct {
      uint32_t Signature;        /* MDMP_SIGNATURE */
      uint32_t Version;          /* MDMP_VERSION | (impl_version << 16) */
      uint32_t NumberOfStreams;
      uint32_t StreamDirectoryRva; /* byte offset to directory array */
      uint32_t Checksum;         /* CRC32 of entire dump file, 0=optional */
      uint32_t TimeDateStamp;    /* Unix timestamp */
      uint64_t Flags;            /* MINIDUMP_TYPE flags */
  } MINIDUMP_HEADER;

  /* Flags */
  #define MiniDumpNormal            0x00000000
  #define MiniDumpWithFullMemory    0x00000002
  #define MiniDumpFilterMemory      0x00000008
  #define MiniDumpWithCodeSegs      0x00000020
  #define MiniDumpWithoutOptionalData 0x00000400
  ```
- [ ] `MINIDUMP_DIRECTORY` entry (8 bytes each):
  ```c
  typedef struct {
      uint32_t StreamType; /* stream type code */
      uint32_t DataSize;   /* byte length of stream data */
      uint32_t Rva;        /* byte offset from file start */
  } MINIDUMP_DIRECTORY;
  ```

- [ ] Stream type constants:
  ```c
  #define ThreadListStream          3
  #define ModuleListStream          4
  #define MemoryListStream          5
  #define ExceptionStream           6
  #define SystemInfoStream          7
  #define ThreadExListStream        8
  #define Memory64ListStream        9
  #define UnloadedModuleListStream 14
  #define MiscInfoStream           15
  /* Impossible OS extension stream */
  #define ImpossibleOSInfoStream   0x8001
  ```

- [ ] `MINIDUMP_EXCEPTION_STREAM`:
  ```c
  typedef struct {
      uint32_t ThreadId;
      uint32_t __Alignment;
      struct {
          uint32_t ExceptionCode;     /* = bugcheck code */
          uint32_t ExceptionFlags;
          uint64_t ExceptionRecord;   /* VA of chained record, 0 if none */
          uint64_t ExceptionAddress;  /* = crashing RIP */
          uint32_t NumberParameters;  /* = 4 */
          uint32_t __Unused;
          uint64_t ExceptionInformation[15]; /* bugcheck params p1–p4 */
      } ExceptionRecord;
      struct { uint32_t DataSize; uint32_t Rva; } ThreadContext; /* → CONTEXT */
  } MINIDUMP_EXCEPTION_STREAM;
  ```
- [ ] `MINIDUMP_MODULE` per loaded module (72 bytes):
  - `BaseOfImage`, `SizeOfImage`, `CheckSum`, `TimeDateStamp`, `ModuleNameRva` (RVA → null-terminated UTF-16 name string), `VersionInfo`, `CvRecord`, `MiscRecord`
- [ ] `MINIDUMP_THREAD`:
  - `ThreadId`, `SuspendCount`, `PriorityClass`, `Priority`, `Teb` (TEB VA), `Stack` (`{StartOfMemoryRange, DataSize, Rva}`), `ThreadContext` (`{DataSize, Rva}`)
- [ ] `MINIDUMP_MEMORY_DESCRIPTOR`:
  - `StartOfMemoryRange` (physical/virtual base), `Memory` (size + RVA)
- [ ] `MINIDUMP_SYSTEM_INFO`:
  - `ProcessorArchitecture=PROCESSOR_ARCHITECTURE_AMD64 (9)`, `ProcessorLevel`, `ProcessorRevision`, `NumberOfProcessors`, `MajorVersion=10` (Win10 compat), `MinorVersion=0`, `BuildNumber=IMPOSSIBLE_OS_BUILD`, `PlatformId=VER_PLATFORM_WIN32_NT (2)`, `CSDVersionRva` (OS name string RVA)

- [ ] Custom stream appended after standard streams:
  ```c
  typedef struct {
      uint32_t Magic;              /* 0xI0DEAD00 */
      uint32_t KernelBuild;        /* build number */
      uint64_t BugCheckCode;
      uint64_t BugCheckParams[4];
      char     BugCheckName[64];   /* e.g. "PAGE_FAULT_IN_NONPAGED_AREA" */
      uint64_t PanicTimestamp;     /* TSC + wall clock */
      uint64_t PhysicalMemoryKiB;
      uint32_t CpuCount;
      char     CpuBrandString[48]; /* from CPUID 0x80000002–0x80000004 */
      char     KernelPath[256];
  } IMPOSSIBLE_OS_INFO;
  ```

- [ ] Commit: `"kernel/crashdump: MDMP header, directory, all stream type structs, ImpossibleOSInfo"`

**Test checkpoint:** `sizeof(MINIDUMP_HEADER)` == 28 bytes. `MINIDUMP_DIRECTORY` == 12 bytes. `MDMP_SIGNATURE == 0x504D444D`. `ImpossibleOSInfoStream` type == 0x8001. Static asserts on all struct sizes. `POST16(0xDE46)` on entry. Test on: QEMU WHPX + TCG; bare metal.

---

## 5. Minidump Writer

- [ ] Minidump memory layout (fixed region order, offsets computed at write time):
  ```
  [0x0000] MINIDUMP_HEADER         (28 bytes)
  [0x001C] MINIDUMP_DIRECTORY[N]   (12 bytes × N streams)
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
  4. Append all raw memory page data (§5.2) at the end; update `MemoryListStream` descriptors with their final RVAs
  5. Compute CRC32C of the entire dump; write into `MINIDUMP_HEADER.Checksum`
  6. Call `dump_sink_write(buf, total_size)` (§7) to flush to disk

- [ ] Commit: `"kernel/crashdump: minidump writer, stream serialisation, CRC32C, memory page selection"`

**Test checkpoint:** `crashdump_write_minidump()` produces non-empty buffer with MDMP signature at offset 0. ExceptionStream contains bugcheck code. ModuleList contains kernel.exe. MemoryList includes stack pages. CRC32C validates. `POST16(0xDE48)` on entry. Test on: QEMU WHPX + TCG; bare metal.

---

## 6. Kernel Dump & Full Dump Variants

- [ ] `dump_classify_page(pa)` → `PAGE_CLASS`:
  - `PAGE_CLASS_KERNEL`: physical page mapped in the kernel's address range (upper half, `va ≥ 0xFFFF800000000000`)
  - `PAGE_CLASS_USER`: mapped in user space (`va < 0x0000800000000000`)
  - `PAGE_CLASS_HARDWARE`: MMIO / framebuffer / LAPIC ranges -- skip in kernel dump, include description in `ImpossibleOSInfoStream`
  - `PAGE_CLASS_FREE`: not in PMM used list -- skip always

- [ ] `crashdump_write_kernel_dump(frame, ctx)`:
  - Same MDMP structure as minidump but `Flags |= MiniDumpWithCodeSegs`
  - `MemoryListStream` uses `Memory64ListStream` format (64-bit base/size pairs + a single bulk RVA for all page data) instead of per-range RVAs
  - Walk PMM used-page list; include all `PAGE_CLASS_KERNEL` pages
  - On a typical system this is 32–256 MiB; apply per-page LZ4 block compression: each 4 KiB page is compressed to a `{compressed_size (u16), data[compressed_size]}` record; uncompressed pages (compression ratio < 1) are stored raw with `compressed_size = 0x8000 | 4096`
- [ ] `crashdump_type` Registry value: `0`=minidump (default), `1`=kernel dump, `2`=full dump; read in `panic_screen()` to select the writer

- [ ] `crashdump_write_full_dump(frame, ctx)`:
  - Walk all physical pages reported in `boot_info.memory_map` as `MEMORY_TYPE_USABLE` and `MEMORY_TYPE_KERNEL`
  - LZ4-compress each 64-page (256 KiB) chunk; write compressed chunk prefixed with `{orig_size_u32, comp_size_u32}`
  - On a 4 GiB machine: uncompressed ~4 GiB; after LZ4 typically 1–2 GiB
  - `MINIDUMP_HEADER.Flags |= MiniDumpWithFullMemory`
  - Raw-partition dump sink (§7) must be large enough; warn at boot time if dump partition < total RAM; fall back to kernel dump if too small

- [ ] Commit: `"kernel/crashdump: kernel dump variant, full dump with LZ4 compression, dump-type selector"`

**Test checkpoint:** Kernel dump includes all `PAGE_CLASS_KERNEL` pages. Full dump includes all used physical pages. LZ4 compression reduces dump size (compressed < uncompressed). Registry `crashdump_type` value selects correct writer. `POST16(0xDE4A)` on entry. Test on: QEMU WHPX + TCG; bare metal.

---

## 7. Raw-Partition Dump Sink (VFS Bypass)

- [ ] Dedicated GPT partition identified by type GUID `{IMPOSSIBLE-DUMP-GUID}` -- the same raw-block mechanism used by S4 hibernate (→ XREF `TODO-15-power-management.md §4`); if that GUID is present the partition doubles as the dump sink; if absent, fall back to the VFS text path
- [ ] At kernel init (Phase 1 -- before VFS): call `dump_sink_probe()` -- scan GPT via `blkdev_open_by_gpt_type(DUMP_GUID)`; cache the raw block device handle in a static `g_dump_blkdev`; this works before any filesystem driver is mounted

- [ ] `dump_sink_write(buf, size)` -- streaming write engine:
  - Maintains a 512-byte sector-aligned write position `g_dump_offset`
  - Pads the buffer to the next 512-byte boundary with zeros
  - Issues synchronous DMA writes via `blkdev_write_raw(g_dump_blkdev, g_dump_offset, buf, aligned_size)` -- must not use IRQs or wait queues (panic context); busy-poll DMA completion status register directly
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

- [ ] Commit: `"kernel/crashdump: raw partition sink, VFS-bypass DMA write, dump partition header"`

**Test checkpoint:** `dump_sink_probe()` finds dump partition by GPT GUID. `dump_sink_write()` writes to raw partition without VFS. `DUMP_PARTITION_HEADER.DumpPresent` == 1 after write. `dump_sink_clear()` sets `DumpPresent` == 0. `POST16(0xDE4C)` on entry. Test on: QEMU WHPX + TCG; bare metal.

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

**Test checkpoint:** After simulated crash + reboot, `X:\Crash\*.dmp` exists. Registry `CrashPending` == 1. Dialog shows crash path. Archive rotation deletes 6th file when 5 exist. `POST16(0xDE4E)` on entry. Test on: QEMU WHPX + TCG; bare metal.

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
    CPU: Intel(R) Core(TM) i7-1165G7 × 8
    RAM: 16384 MiB
  ```
- [ ] Stack trace: read thread stack pages from the dump's `MemoryListStream`; simulate the RBP chain walk the same way `panic.c` does; look up each frame in `ModuleListStream` + the embedded `symtab` (load `kernel.sym` if it exists on disk, or read symbol names from `ImpossibleOSInfoStream`)

- [ ] The `.dmp` file produced by the dump writer (§5) uses the standard MDMP format (`MDMP` magic, standard stream types, standard CONTEXT layout)
  -- a Windows developer can open it directly in WinDbg 10+ and run `!analyze -v`; module names and addresses from `ModuleListStream` appear in the module list; the `CONTEXT` record allows stack unwinding via PDB symbols if available
- [ ] Note: WinDbg needs matching PDB symbols for deep analysis; for kernel-only debugging, the MDMP alone is sufficient for `!analyze -v`

- [ ] `dmpanalyze /compare <dump1.dmp> <dump2.dmp>` -- diff two crash dumps:
  - Compare bugcheck code, faulting module, top-5 stack frames
  - Output: `SAME_STOP_CODE`, `SAME_FAULTING_MODULE`, `LIKELY_SAME_BUG` / `DIFFERENT_CRASH` classification
  - Useful for identifying duplicate crashes in a fleet of machines

- [ ] Commit: `"apps/dmpanalyze: MDMP parser, !analyze output, WinDbg-compatible dump, /compare"`

**Test checkpoint:** `dmpanalyze.exe test.dmp` produces output with STOP code, faulting function, and >= 3 stack frames. `/compare` on two identical dumps reports `LIKELY_SAME_BUG`. WinDbg opens the .dmp without format errors. Test on: QEMU WHPX + TCG; bare metal.

---

## OS Comparison

| ⭐ | Feature                                   | 🪟 Win11                            | 🐧 Linux                              | 🚀 Impossible OS             |
|----|-------------------------------------------|----------------------------------|------------------------------------|---------------------------|
| 💎 | Text crash log on panic                   | ✅ Event Viewer                  | ✅ `dmesg` / `journalctl`          | ✅ Done -- `crashdump.log` |
| 💎 | Windows STOP code taxonomy                | ✅ Full (`0xXXXXXXXX`)           | ❌ Kernel OOPS (different)         | ⬜ §1                     |
| 💎 | FPU/XMM/XSAVE state in dump               | ✅ Full CONTEXT record           | ✅ `PTRACE_GETFPREGS` in coredump  | ⬜ §2                     |
| 💎 | Loaded module list                        | ✅ Full (`lm` in WinDbg)         | ✅ `/proc/modules`, `kcore`        | ⬜ §3                     |
| 💎 | Binary MDMP minidump format               | ✅ Full WER/WinDbg compatible    | ⚠️ ELF coredump (different format) | ⬜ §4–§5                  |
| 💎 | Kernel dump                               | ✅ `%SystemRoot%\MEMORY.DMP`     | ✅ `makedumpfile -d 31`            | ⬜ §6                     |
| 💎 | Full memory dump                          | ✅ Full memory dump option       | ✅ `makedumpfile -d 0`             | ⬜ §6                     |
| 💎 | VFS-bypass raw partition dump sink        | ✅ Writes to pagefile before VFS | ✅ Kdump to dedicated partition    | ⬜ §7                     |
| 💎 | Post-boot dump recovery & notification    | ✅ WER dialog                    | ✅ `apport` / `systemd-coredump`   | ⬜ §8                     |
| 💎 | Crash dump archive rotation               | ✅ `%Minidump%\*.dmp` (last 5)   | ⚠️ manual / distro-specific        | ⬜ §8 -- .3                |
| 💎 | WinDbg-compatible dump file               | ✅ Native                        | ❌ ELF coredump (not WinDbg)       | ⬜ §4–§5                  |
| 💎 | LZ4 dump compression                      | ⚠️ Xpress only (no LZ4)          | ✅ `makedumpfile` LZO/snappy       | ⬜ §6 -- (LZ4)             |
| ⭐ | Custom `ImpossibleOSInfoStream` in MDMP   | ❌ No vendor extension           | ❌ No equivalent                   | ⬜ §4 -- .4 🚀             |
| ⭐ | `dmpanalyze /compare` crash deduplication | ❌ Needs WER portal              | ❌ Not available                   | ⬜ §9 -- .4 🚀             |
| ⭐ | Built-in `dmpanalyze.exe` on-device       | ❌ Requires WinDbg install       | ❌ Requires `crash` tool install   | ⬜ §9 -- 🚀                |

After §1–8, Impossible OS reaches full Windows 11 crash-dump parity: WinDbg-compatible binary MDMP files, VFS-bypass raw-partition sink, post-boot recovery dialog, and archive rotation. Linux's ELF coredump format is not WinDbg-compatible and requires external `makedumpfile`/`crash` tooling. The built-in `dmpanalyze.exe` (§9), the custom `ImpossibleOSInfoStream` with OS-specific metadata, and the `/compare` deduplication command are exclusive features that make post-mortem crash analysis practical without requiring an external debugger.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_crashdump()` (→ XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_crashdump.c` with:
  - Minidump header: `minidump_init()` produces valid MDMP signature (`"MDMP"`)
  - Thread list stream: current thread's RIP/RSP captured in dump
  - Module list stream: kernel.exe appears with correct base address
  - Memory region: stack region included in memory dump
  - Exception record: `EXCEPTION_RECORD` fields populated from test fault context
  - Symbol resolution: `symtab_resolve(kernel_main)` returns correct name
  - Black-box recorder: `bbr_write("test")` → `bbr_read()` returns `"test"` after simulated restart
  - Dump file write: `crashdump_write()` produces non-empty file on disk (mock VFS)
- [ ] Register in `test_runner_init()`: `test_register_crashdump()`
- [ ] Commit: `"test: add crash dump generation test suite"`

---

## Verification

- [ ] **STOP code**: trigger `KeBugCheckEx(BUGCHECK_MANUALLY_INITIATED_CRASH, 1, 2, 3, 4)` from a shell command; BSOD screen shows `STOP: 0x000000E2 (0x1, 0x2, 0x3, 0x4)`; Registry `HKLM\SYSTEM\LastBugCheck\Code` == `0xE2`.
- [ ] **XSAVE**: verify `g_panic_xsave_buf` is non-zero after a panic (FPU was in use); confirm `CONTEXT.MxCsr` is non-default.
- [ ] **Module list**: `modules_find_by_address(kernel_entry)` returns the `kernel.exe` `LOADED_MODULE`; `modules_find_by_address(0)` returns NULL.
- [ ] **Minidump validity**: trigger a test panic; recover `.dmp` from `X:\Crash\`; open with `dmpanalyze.exe` -- output must include STOP code, faulting function name, and at least 3 stack frames.
- [ ] **WinDbg round-trip**: copy the `.dmp` to a Windows machine; open in WinDbg 10+; `!analyze -v` must complete without errors (symbol names optional -- raw addresses are sufficient for format validation).
- [ ] **VFS-bypass**: deliberately trigger a panic after VFS unmount; verify the dump partition header shows `DumpPresent = 1` at sector 0.
- [ ] **Full dump CRC**: write a full dump; `dmpanalyze.exe` reads and validates `MINIDUMP_HEADER.Checksum` CRC32C -- must match.
- [ ] **Remaining limits**: `dmpanalyze /compare` is a convenience feature; full `!analyze -v` symbol resolution requires `kernel.sym` on the analysis machine; the `[Submit Report]` WER upload path requires the HTTP client from `06-networking` to be complete before it is wired up.
- [ ] Commit: `"kernel/crashdump: KeBugCheckEx, MDMP minidump/kernel/full dump, raw-partition sink, dmpanalyze"`
