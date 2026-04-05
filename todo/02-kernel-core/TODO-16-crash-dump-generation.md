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
- → XREF: `TODO-02-system-logging.md §9` -- crash-persistent ring buffer capture to reserved physical memory; complementary to binary dump -- §9 captures text log, this TODO captures CPU/memory state
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

| ⭐  | Order | Deliverable                                            | Depends On              | Status |
| --- | :---: | ------------------------------------------------------ | ----------------------- | :----: |
| 💎  |   1   | Bugcheck codes & `KeBugCheckEx`                        | --                       |  [ ]   |
| 💎  |   2   | FPU/XMM/XSAVE state capture                           | 1                       |  [ ]   |
| 💎  |   3   | Kernel module registry (`LOADED_MODULE` list)          | TODO-08-binary-system.md §1              |  [ ]   |
| 💎  |   4   | MDMP binary format: header, directory & stream types   | 1, 2, 3, TODO-10-exception-dispatch-seh.md §1     |  [ ]   |
| 💎  |   5   | Minidump writer (crashing thread + small memory)       | 4                       |  [ ]   |
| 💎  |   6   | Kernel dump & full dump variants                       | 5                       |  [ ]   |
| 💎  |   7   | Raw-partition dump sink (VFS bypass)                   | 5, TODO-15-power-management.md §4           |  [ ]   |
| 💎  |   8   | Post-boot crash recovery: copy dump + "unexpected shutdown" dialog | 7, TODO-01-kernel-init-sequencing.md §4 |  [ ]   |
| ⭐  |   9   | `dmpanalyze.exe` crash analyzer                        | 4, 8                    |  [ ]   |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.

---

## 1. Bugcheck Codes & `KeBugCheckEx` `[Sonnet]`

### 1.1 STOP code table

- [ ] Define in `include/kernel/bugcheck.h`:
  ```c
  typedef uint32_t BUGCHECK_CODE;

  /* Subset of Windows-compatible STOP codes */
  #define BUGCHECK_IRQL_NOT_LESS_OR_EQUAL     0x0000000A
  #define BUGCHECK_KMODE_EXCEPTION_NOT_HANDLED 0x0000001E
  #define BUGCHECK_PAGE_FAULT_IN_NONPAGED_AREA 0x00000050
  #define BUGCHECK_SYSTEM_SERVICE_EXCEPTION    0x0000003B
  #define BUGCHECK_KERNEL_STACK_INPAGE_ERROR   0x00000077
  #define BUGCHECK_KERNEL_DATA_INPAGE_ERROR    0x0000007A
  #define BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE 0x00000139
  #define BUGCHECK_CRITICAL_PROCESS_DIED       0x000000EF
  #define BUGCHECK_HEAP_CORRUPTION             0x000000C5
  #define BUGCHECK_DRIVER_IRQL_NOT_LESS_OR_EQUAL 0x000000D1
  #define BUGCHECK_MANUALLY_INITIATED_CRASH    0x000000E2
  /* Impossible OS exclusive codes (0xE0000001–0xE00000FF) */
  #define BUGCHECK_IMPOSSIBLE_HEAP_GUARD       0xE0000001
  #define BUGCHECK_IMPOSSIBLE_PMM_DOUBLE_FREE  0xE0000002
  #define BUGCHECK_IMPOSSIBLE_VMM_CORRUPT      0xE0000003
  #define BUGCHECK_IMPOSSIBLE_HIBERNATE_CORRUPT 0xE0000004
  ```
- [ ] `const char *bugcheck_name(BUGCHECK_CODE code)` -- returns string like `"PAGE_FAULT_IN_NONPAGED_AREA"` for display on BSOD and in dump analysis

### 1.2 KeBugCheckEx

- [ ] `KeBugCheckEx(BUGCHECK_CODE code, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4)` in `src/kernel/panic.c`:
  - Stores `{code, p1, p2, p3, p4}` in a static global `g_last_bugcheck` (accessible without heap allocation)
  - Updates the BSOD screen stop code line: `"STOP: 0x00000050 (0x..., 0x..., 0x..., 0x...)"`
  - Existing `panic_screen()` call becomes a thin wrapper that populates `g_last_bugcheck` then calls the dump pipeline (§5)
- [ ] Existing `PANIC(msg)` and `PANIC_IF(cond, msg)` macros updated to call `KeBugCheckEx(BUGCHECK_MANUALLY_INITIATED_CRASH, ...)` internally
- [ ] Registry persistence: `HKLM\SYSTEM\LastBugCheck\Code`, `HKLM\SYSTEM\LastBugCheck\Param1`–`Param4`, `Timestamp` written early in the crash path before VFS state is uncertain (→ XREF `TODO-13-registry-completion.md §4`)

### 1.3 Commit

- [ ] Commit: `"kernel/panic: KeBugCheckEx, STOP code table, bugcheck Registry persistence"`

---

## 2. FPU/XMM/XSAVE State Capture `[Sonnet]`

### 2.1 XSAVE area

- [ ] Allocate a single static `xsave_area_t` scratch buffer in `panic.c` at compile time -- 4 KiB is sufficient for `XSAVE` up to AVX-512:
  ```c
  static uint8_t g_panic_xsave_buf[4096] __attribute__((aligned(64)));
  ```
- [ ] `panic_capture_fpu_state()` -- called at the very top of `panic_screen()` before any other work:
  ```c
  __asm__ volatile (
      "xsave64 %0"
      : "=m"(g_panic_xsave_buf)
      : "a"(0xFFFFFFFF), "d"(0xFFFFFFFF)
      : "memory"
  );
  ```
- [ ] If XSAVE is not supported (check `cpuid` XSAVE bit), fall back to `fxsave64 %0`

### 2.2 CONTEXT record (x64)

- [ ] Extend `struct interrupt_frame` → `CONTEXT` mapping in `include/kernel/panic.h`:
  ```c
  typedef struct {
      uint64_t P1Home, P2Home, P3Home, P4Home, P5Home, P6Home; /* reserved */
      uint32_t ContextFlags;  /* CONTEXT_AMD64 = 0x100000 | flags */
      uint32_t MxCsr;
      uint16_t SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
      uint32_t EFlags;
      uint64_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
      uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
      uint64_t R8, R9, R10, R11, R12, R13, R14, R15;
      uint64_t Rip;
      /* XMM / XSAVE area (512 bytes FXSAVE_FORMAT) */
      uint8_t  FltSave[512];
      uint64_t VectorRegister[52]; /* YMM high halves */
      uint64_t VectorControl;
      uint64_t DebugControl, LastBranchToRip, LastBranchFromRip;
      uint64_t LastExceptionToRip, LastExceptionFromRip;
  } CONTEXT;
  ```
- [ ] `panic_build_context(frame, ctx)` -- populate `CONTEXT` from `interrupt_frame` + `g_panic_xsave_buf`; `ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT`

### 2.3 Commit

- [ ] Commit: `"kernel/panic: XSAVE FPU capture, CONTEXT record population"`

---

## 3. Kernel Module Registry `[Sonnet]`

### 3.1 LOADED_MODULE type

- [ ] Define in `include/kernel/modules.h`:
  ```c
  #define MODULE_NAME_MAX 64
  typedef struct {
      uint64_t image_base;       /* load address */
      uint64_t image_size;       /* total mapped size */
      uint64_t entry_point;      /* entry function VA */
      uint32_t checksum;         /* PE/ELF optional checksum */
      uint32_t flags;            /* MODULE_FLAG_KERNEL=1, MODULE_FLAG_DRIVER=2 */
      char     name[MODULE_NAME_MAX]; /* e.g. "kernel.exe", "ahci.kmod" */
      char     path[256];             /* full path */
  } LOADED_MODULE;

  #define MODULE_LIST_MAX 64
  ```
- [ ] Static `LOADED_MODULE g_module_list[MODULE_LIST_MAX]` and `uint32_t g_module_count`; no dynamic allocation -- written during boot and driver load

### 3.2 Registration API

- [ ] `modules_register(base, size, entry, name, path, flags)` -- fills next free slot; returns `MODULE_HANDLE` (slot index + 1, 0=error)
- [ ] `modules_unregister(handle)` -- zeroes the slot (for driver hot-unload)
- [ ] `modules_find_by_address(va)` → `LOADED_MODULE*` -- scan list for entry where `base ≤ va < base + size`; used by `symtab_resolve` and the dump writer to annotate stack frames with module names

### 3.3 Boot-time registration

- [ ] In `kernel_main()` (Phase 1): call `modules_register(KERNEL_IMAGE_BASE, kernel_image_size, kernel_entry, "kernel.exe", "\\boot\\kernel.exe", MODULE_FLAG_KERNEL)`; `kernel_image_size` derived from `__kernel_end - __kernel_start` linker symbols
- [ ] In the binary loader (→ XREF `TODO-08-binary-system.md §1`): call `modules_register()` for every loaded ELF/PE image at its mapped base

### 3.4 Commit

- [ ] Commit: `"kernel/modules: LOADED_MODULE registry, register/unregister, boot-time kernel entry"`

---

## 4. MDMP Binary Format: Header, Directory & Stream Types `[Opus]`

### 4.1 MDMP header

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

### 4.2 Stream type codes

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

### 4.3 Stream data structures

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

### 4.4 ImpossibleOSInfoStream (exclusive)

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

### 4.5 Commit

- [ ] Commit: `"kernel/crashdump: MDMP header, directory, all stream type structs, ImpossibleOSInfo"`

---

## 5. Minidump Writer `[Opus]`

### 5.1 Dump layout plan

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

### 5.2 Memory range selection for minidump

- [ ] Crashing thread's kernel stack: 8 pages (32 KiB) around `RSP`
- [ ] User-space stack (if crash was in user context): 16 pages around user RSP from `interrupt_frame.rsp`
- [ ] Code page at `RIP`: 1 page
- [ ] Any memory addresses appearing as arguments in the top 8 stack frames (heuristic: values in `[0x1000, KERNEL_BASE)` for user, or `[KERNEL_BASE, KERNEL_END)` for kernel) -- include the page they point to

### 5.3 Minidump write engine

- [ ] `crashdump_write_minidump(frame, ctx)`:
  1. Allocate two contiguous scratch pages (`pmm_alloc_contiguous(2)`) as the write buffer; the dump writer fills these pages and flushes via the raw sink (§7)
  2. Build `MINIDUMP_HEADER` + `MINIDUMP_DIRECTORY` array in scratch buffer
  3. Serialise each stream in order; compute RVA offsets as bytes from file start; update the corresponding `MINIDUMP_DIRECTORY` entry
  4. Append all raw memory page data (§5.2) at the end; update `MemoryListStream` descriptors with their final RVAs
  5. Compute CRC32C of the entire dump; write into `MINIDUMP_HEADER.Checksum`
  6. Call `dump_sink_write(buf, total_size)` (§7) to flush to disk

### 5.4 Commit

- [ ] Commit: `"kernel/crashdump: minidump writer, stream serialisation, CRC32C, memory page selection"`

---

## 6. Kernel Dump & Full Dump Variants `[Opus]`

### 6.1 Memory classification

- [ ] `dump_classify_page(pa)` → `PAGE_CLASS`:
  - `PAGE_CLASS_KERNEL`: physical page mapped in the kernel's address range (upper half, `va ≥ 0xFFFF800000000000`)
  - `PAGE_CLASS_USER`: mapped in user space (`va < 0x0000800000000000`)
  - `PAGE_CLASS_HARDWARE`: MMIO / framebuffer / LAPIC ranges -- skip in kernel dump, include description in `ImpossibleOSInfoStream`
  - `PAGE_CLASS_FREE`: not in PMM used list -- skip always

### 6.2 Kernel dump

- [ ] `crashdump_write_kernel_dump(frame, ctx)`:
  - Same MDMP structure as minidump but `Flags |= MiniDumpWithCodeSegs`
  - `MemoryListStream` uses `Memory64ListStream` format (64-bit base/size pairs + a single bulk RVA for all page data) instead of per-range RVAs
  - Walk PMM used-page list; include all `PAGE_CLASS_KERNEL` pages
  - On a typical system this is 32–256 MiB; apply per-page LZ4 block compression: each 4 KiB page is compressed to a `{compressed_size (u16), data[compressed_size]}` record; uncompressed pages (compression ratio < 1) are stored raw with `compressed_size = 0x8000 | 4096`
- [ ] `crashdump_type` Registry value: `0`=minidump (default), `1`=kernel dump, `2`=full dump; read in `panic_screen()` to select the writer

### 6.3 Full dump

- [ ] `crashdump_write_full_dump(frame, ctx)`:
  - Walk all physical pages reported in `boot_info.memory_map` as `MEMORY_TYPE_USABLE` and `MEMORY_TYPE_KERNEL`
  - LZ4-compress each 64-page (256 KiB) chunk; write compressed chunk prefixed with `{orig_size_u32, comp_size_u32}`
  - On a 4 GiB machine: uncompressed ~4 GiB; after LZ4 typically 1–2 GiB
  - `MINIDUMP_HEADER.Flags |= MiniDumpWithFullMemory`
  - Raw-partition dump sink (§7) must be large enough; warn at boot time if dump partition < total RAM; fall back to kernel dump if too small

### 6.4 Commit

- [ ] Commit: `"kernel/crashdump: kernel dump variant, full dump with LZ4 compression, dump-type selector"`

---

## 7. Raw-Partition Dump Sink (VFS Bypass) `[Opus]`

### 7.1 Dump partition

- [ ] Dedicated GPT partition identified by type GUID `{IMPOSSIBLE-DUMP-GUID}` -- the same raw-block mechanism used by S4 hibernate (→ XREF `TODO-15-power-management.md §4`); if that GUID is present the partition doubles as the dump sink; if absent, fall back to the VFS text path
- [ ] At kernel init (Phase 1 -- before VFS): call `dump_sink_probe()` -- scan GPT via `blkdev_open_by_gpt_type(DUMP_GUID)`; cache the raw block device handle in a static `g_dump_blkdev`; this works before any filesystem driver is mounted

### 7.2 Write state machine

- [ ] `dump_sink_write(buf, size)` -- streaming write engine:
  - Maintains a 512-byte sector-aligned write position `g_dump_offset`
  - Pads the buffer to the next 512-byte boundary with zeros
  - Issues synchronous DMA writes via `blkdev_write_raw(g_dump_blkdev, g_dump_offset, buf, aligned_size)` -- must not use IRQs or wait queues (panic context); busy-poll DMA completion status register directly
  - If `g_dump_blkdev` is NULL (no dump partition): fall back to the existing VFS text-dump path from `panic.c`

### 7.3 Dump partition header at sector 0

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

### 7.4 Commit

- [ ] Commit: `"kernel/crashdump: raw partition sink, VFS-bypass DMA write, dump partition header"`

---

## 8. Post-Boot Crash Recovery & CrashDumps Archive `[Sonnet]`

### 8.1 Kernel init check for pending dump

- [ ] In kernel init Phase 2 (→ XREF `TODO-01-kernel-init-sequencing.md §4`), after VFS mounts but before the desktop starts: call `dump_recovery_check()`:
  1. `dump_sink_probe()` -- open dump partition
  2. Read sector 0; check `DUMP_PARTITION_HEADER.DumpPresent == 1`
  3. If present: read `DumpSize` bytes; write to `X:\Crash\{timestamp}.dmp` (create directory if absent; timestamp from `DumpPartitionHeader` or system RTC if unavailable)
  4. Also write a companion `.txt` file with the text dump (same content as the current `crashdump.log` -- keep the existing text path as a supplementary artifact)
  5. Call `dump_sink_clear()` -- zero the `DumpPresent` flag
  6. Set Registry `HKLM\SYSTEM\LastCrashDump` = the `.dmp` path and `HKLM\SYSTEM\CrashPending = 1`

### 8.2 "System shut down unexpectedly" dialog

- [ ] On desktop startup: check `HKLM\SYSTEM\CrashPending == 1`; if true:
  - Display a dialog: `"Impossible OS shut down unexpectedly.\n\nCrash dump saved to {path}.\n\n[View Report]  [Submit Report]  [Close]"`
  - `[View Report]`: launch `dmpanalyze.exe {path}` (§9)
  - `[Submit Report]` (opt-in): HTTP POST the minidump to the crash server URL from `HKLM\SYSTEM\CrashReporting\ServerUrl` (default empty = disabled)
  - `[Close]`: dismiss; set `HKLM\SYSTEM\CrashPending = 0`
- [ ] Dialog does not block shell startup; shown as a non-intrusive banner in the system tray notification area

### 8.3 CrashDumps archive management

- [ ] Keep at most 5 `.dmp` files in `X:\Crash\`; oldest is deleted when the 6th would be created
- [ ] `HKLM\SYSTEM\CrashDumps\MaxFiles` (REG_DWORD, default 5) controls the limit
- [ ] `HKLM\SYSTEM\CrashDumps\LastDump` (REG_SZ) = path to most recent `.dmp`; updated on each recovery pass

### 8.4 Commit

- [ ] Commit: `"kernel/crashdump: post-boot dump recovery, CrashDumps archive, unexpected-shutdown dialog"`

---

## 9. `dmpanalyze.exe` Crash Analyzer `[Sonnet]`

### 9.1 MDMP parser

- [ ] `src/apps/dmpanalyze/dmpanalyze.c` -- standalone command-line app:
  - Open `.dmp` file; validate `MINIDUMP_HEADER.Signature == MDMP_SIGNATURE`
  - Parse `MINIDUMP_DIRECTORY` to locate all streams by `StreamType`
  - Extract `ExceptionStream`, `ModuleListStream`, `ThreadListStream`, `SystemInfoStream`, and `ImpossibleOSInfoStream`

### 9.2 `!analyze -v` equivalent output

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

### 9.3 WinDbg compatibility

- [ ] The `.dmp` file produced by the dump writer (§5) uses the standard MDMP format (`MDMP` magic, standard stream types, standard CONTEXT layout)
  -- a Windows developer can open it directly in WinDbg 10+ and run `!analyze -v`; module names and addresses from `ModuleListStream` appear in the module list; the `CONTEXT` record allows stack unwinding via PDB symbols if available
- [ ] Note: WinDbg needs matching PDB symbols for deep analysis; for kernel-only debugging, the MDMP alone is sufficient for `!analyze -v`

### 9.4 `dmpanalyze /compare` ⭐

- [ ] `dmpanalyze /compare <dump1.dmp> <dump2.dmp>` -- diff two crash dumps:
  - Compare bugcheck code, faulting module, top-5 stack frames
  - Output: `SAME_STOP_CODE`, `SAME_FAULTING_MODULE`, `LIKELY_SAME_BUG` / `DIFFERENT_CRASH` classification
  - Useful for identifying duplicate crashes in a fleet of machines

### 9.5 Commit

- [ ] Commit: `"apps/dmpanalyze: MDMP parser, !analyze output, WinDbg-compatible dump, /compare"`

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
