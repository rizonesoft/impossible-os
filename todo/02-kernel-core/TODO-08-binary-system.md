# TODO-08 -- Binary Format System (exec_load / ELF / PE32+ / EIF)

> **Goal:** Build the multi-format executable loader that every user-mode program depends on. Three formats must work: ELF (existing basic loader upgraded), PE32+ (Windows-compatible, imports wired to Win32 API), and EIF (Impossible OS native -- 64-byte header, syscall-ID import table, <10 µs load time). A single `exec_load()` dispatcher auto-detects format by magic bytes and routes to the correct loader. ASLR and EIF code signing close out the security story.

> [!IMPORTANT]
> **Current state:** A basic ELF loader exists in `src/kernel/elf.c` (143 lines). It loads `PT_LOAD` segments via identity mapping and is called directly from `task.c`. No format dispatcher, no PE32+ support, no EIF format, no proper VMM-backed user-space mapping.

## Inputs

- [`src/kernel/elf.c`](../../src/kernel/elf.c), [`include/kernel/elf.h`](../../include/kernel/elf.h) -- existing basic ELF loader
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- direct `elf_load()` call site (line 635) to be replaced
- [`user/user.ld`](../../user/user.ld) -- user-mode linker script (base `0x800000`)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) -- `vmm_map_page()` for page mapping; `vmm_map_pages()` (multi-page wrapper) must be added in §2
- [`src/kernel/fs/vfs.c`](../../src/kernel/fs/vfs.c) -- `vfs_read()` for file I/O in loaders
- → XREF: `TODO-04-peb-teb-user-abi.md §6–§7` -- TEB allocation (§6) and initial user stack frame (§7) are populated after `exec_load()` hands control to ring 3
- → XREF: `TODO-05-native-api-ssdt.md §5` -- `NtXxx` SSDT entries must exist before §9 (import resolver) maps DLL function names to SSDT indices
- → XREF: `TODO-03-object-manager.md §5` -- process object registered in Ob namespace at `exec_load()` time
- → XREF: `TODO-10-exception-dispatch-seh.md §6` -- PE32+ `.pdata` section must be registered for loaded modules before `RtlLookupFunctionEntry` can find unwind data; §8 of this TODO registers `.pdata` with the unwind table registry
- → XREF: `TODO-16-crash-dump-generation.md §3` -- `LOADED_MODULE` registry consumes module base/size/name from `exec_load()`; §6 of this TODO populates the module list
- → XREF: `TODO-17-kernel-security-hardening.md §6,§7` -- PE Load Config Directory (§12) exposes CFG bitmap and CET flags; kernel CET shadow stack (§6) and CET IBT (§7) enforcement lives in TODO-17
- → XREF: `10-services-security/TODO-07-win32-pe-loader.md` -- scope overlap: TODO-10/07 covers Win32 subsystem-level PE execution (SYSCALL/SYSRET setup, Win32 ABI, user CRT); this TODO covers the kernel-level binary format infrastructure (loaders, format dispatcher, ASLR). PE header structs and loader core are authoritative HERE; Win32 subsystem wiring is authoritative THERE.
- → XREF: `12-user-platform-sdk/INDEX.md` -- EIF spec doc lives there; `elf2eif` tool and SDK integration wire back to §13
- → XREF: `TODO-09-process-model-extensions.md §3` -- `exec_load()` (§1) must set `task->program_break` to end of BSS so brk/sbrk (TODO-09 §3) can extend from the correct address

## Outcome

- `exec_load(path, proc)` in `src/kernel/exec.c` replaces the direct `elf_load()` call; auto-detects format by magic and dispatches.
- ELF loader uses VMM-backed user pages with correct R/W/X permissions; supports PIE; honours `PT_GNU_STACK` (NX stack), `PT_GNU_RELRO` (read-only GOT after relocation), and `PT_GNU_PROPERTY` (CET/IBT feature flags).
- EIF format is specified in `docs/specs/eif-format.md`; kernel loads EIF in <10 µs.
- PE32+ parser loads sections, resolves imports, processes TLS directory, and registers `.pdata` unwind tables.
- Every loaded module (ELF, PE32+, EIF) is registered in the `LDR_DATA_TABLE_ENTRY` module list and the `LOADED_MODULE` crash dump registry.
- PE Load Config Directory is parsed for CFG bitmap and CET shadow stack flags; enforcement deferred to TODO-17.
- Script/shebang (`#!`) files are auto-detected and dispatched to the named interpreter.
- `tools/elf2eif` converts standard ELF64 output to EIF -- no custom compiler needed.
- ASLR randomises load base for PIE ELF, EIF, and PE32+.
- EIF binaries with `SIGNED` flag are signature-verified before entry.

## Format Quick Reference

| Feature             | ELF                        | PE32+                          | EIF (native)                     |
| ------------------- | -------------------------- | ------------------------------ | -------------------------------- |
| Magic               | `\x7FELF`                  | `MZ`                           | `EIF!`                           |
| Header size         | 64 bytes                   | ~256 bytes (DOS+PE+Optional)   | 64 bytes                         |
| Import model        | PLT/GOT (string symbols)   | IAT (DLL name + function name) | Syscall ID table (integer only)  |
| API surface         | POSIX / Linux compat        | Win32 (`kernel32.dll` stubs)  | Win32 (syscall IDs direct)       |
| Toolchain           | `clang-19 + ld.lld`        | `clang-19 + lld-link`          | `clang-19 + ld.lld + elf2eif`    |
| Primary use         | Dev tools, Linux compat    | Windows app compat             | All native OS apps               |

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On              | Status |
| --- | :---: | ---------------------------------------------- | ----------------------- | :----: |
| 💎  |   1   | `exec_load()` multi-format dispatcher          | VMM, VFS, sched         |  [ ]   |
| 💎  |   2   | Enhanced ELF loader (VMM-backed, PIE)          | 1                       |  [ ]   |
| 💎  |   3   | ELF security segments (GNU_STACK, RELRO, PROP) | 2                       |  [ ]   |
| ⭐  |   4   | EIF format specification                       | --                       |  [ ]   |
| ⭐  |   5   | EIF kernel loader                              | 1, 4                    |  [ ]   |
| 💎  |   6   | Module list registration (LDR_DATA_TABLE)      | 1, TODO-04 §8           |  [ ]   |
| 💎  |   7   | PE32+ header parser                            | 1                       |  [ ]   |
| 💎  |   8   | PE32+ section loader + `.pdata` registration   | 7                       |  [ ]   |
| 💎  |   9   | PE32+ import table resolver (Win32 dispatch)   | 8, TODO-05 §5           |  [ ]   |
| 💎  |  10   | PE32+ base relocation                          | 8                       |  [ ]   |
| 💎  |  11   | PE32+ TLS directory processing                 | 8, TODO-04 §9           |  [ ]   |
| 💎  |  12   | PE32+ Load Config and CFG bitmap               | 8                       |  [ ]   |
| ⭐  |  13   | `elf2eif` host-side converter                  | 4                       |  [ ]   |
| 💎  |  14   | ELF dynamic linker (shared libraries)          | 2                       |  [ ]   |
| 💎  |  15   | ASLR for all three formats                     | 2, 5, 8                 |  [ ]   |
| ⭐  |  16   | Script/shebang interpreter support             | 1                       |  [ ]   |
| ⭐  |  17   | EIF code signing                               | 5                       |  [ ]   |

> 💎 = parity -- Windows NT (PE32+) and Linux (ELF) both provide these capabilities.
> ⭐ = exclusive -- triple-format support, EIF native format, syscall-ID imports, shebang dispatch, and mandatory code signing are Impossible OS only.

---

## 1. `exec_load()` Multi-Format Dispatcher
Replace the direct `elf_load()` call in `task.c:635` with a generic dispatcher.

> [!WARNING]
> **Missing prerequisites:** `ENOEXEC` / `ENOENT` error constants are not defined anywhere in headers. Define them in `include/kernel/errno.h` (create if needed) or as enum values in `exec.h`. The existing codebase uses negative integer error codes -- pick a convention and document it.

- [ ] Create `src/kernel/exec.c` + `include/kernel/exec.h`
- [ ] Define `exec_format_t` -- `{ magic[4], magic_len, name, loader_fn }`
- [ ] Implement `exec_load(const char *path, process_t *proc)`:
  - Read first 4 bytes via VFS; match registered format handlers
  - Unknown magic → `ENOEXEC`; missing file → `ENOENT`
  - Allocate user-space page tables via VMM before calling format loader
  - Set up user-mode stack (top of user address range), push argc/argv
  - Set Ring 3 return context: RIP = entry, RSP = user stack, CS/SS = user segments
- [ ] `exec_register_format(exec_format_t *)` -- register formats at boot
- [ ] Register ELF format in `kernel_main()` init; PE32+ and EIF at their respective init points
- [ ] Replace the direct `elf_load()` call in `task.c:635` with `exec_load()`
- [ ] Commit: `"kernel: exec -- multi-format exec dispatcher"`

**Test checkpoint:** Serial log shows `"exec: registered format ELF (\x7fELF)"`. `exec_load("nonexistent")` returns `ENOENT` (or negative error code if `ENOENT` not yet defined -- define in §1). `exec_load` on a file with magic `0xDEADBEEF` returns error. `POST16(0xD801)` on entry, `POST16(0xD802)` after format table init. Test on: QEMU WHPX + TCG.

## 2. Enhanced ELF Loader
Upgrade `elf_load()` to use VMM-backed user pages with correct permissions and PIE support.

> [!WARNING]
> **Missing prerequisite:** `vmm_map_pages(pml4, vaddr, ...)` does not exist -- only `vmm_map_page(virt, phys, flags)` (singular, no process context). This section must either: (a) implement a `vmm_map_pages()` wrapper that allocates multiple physical pages and maps them into a per-process PML4, or (b) loop over `vmm_map_page()` with the process PML4 set as CR3. Option (a) is preferred -- create it in `vmm.c` as part of this section.
> Also: `process_t` is not a type -- use `process_object` from `include/kernel/ob/ob_process.h`, or `typedef process_object process_t` in `exec.h`.

- [ ] Refactor `elf_load()` → `elf_exec(path, proc)` registered with the dispatcher
- [ ] Read entire ELF file into a kernel buffer via VFS; validate ELF64 header
- [ ] For each `PT_LOAD` segment:
  - Allocate user pages via `vmm_map_pages(proc->pml4, vaddr, ...)`
  - Copy segment data; zero BSS (`memsz > filesz`) in allocated pages
  - Set page permissions from `p_flags`: `PF_X` → executable, `PF_W` → writable, `PF_R` → readable
- [ ] Support `ET_DYN` (PIE): compute random load base + vaddr (ASLR in §15)
- [ ] Set up auxiliary vector for dynamic linker if `PT_INTERP` present (§14)
- [ ] Reject segments outside the defined user address range
- [ ] Free kernel buffer after all segments are loaded
- [ ] Commit: `"kernel: elf -- enhanced ELF loader with VMM mapping and PIE"`

**Test checkpoint:** Serial log shows `"elf: loaded <N> PT_LOAD segments at 0x<user_addr>"`. Entry point is within user address range (not identity-mapped kernel address). BSS region reads as all zeros. `POST16(0xD803)` on entry, `POST16(0xD804)` after all segments mapped. Test on: QEMU WHPX + TCG; VirtualBox.

## 3. ELF Security Segments (GNU_STACK, RELRO, GNU_PROPERTY)

> [!WARNING]
> **Missing prerequisite:** `vmm_protect()` does not exist. Implement as a direct PTE permission update + `invlpg` flush in `vmm.c`. Must support at minimum: change page permissions from RW to R (for RELRO), and remove X from stack pages (for NX stack).

Modern ELF binaries carry security metadata in dedicated program headers. `PT_GNU_STACK` controls whether the stack is executable (NX enforcement). `PT_GNU_RELRO` marks the GOT and relocation data as read-only after relocation completes, blocking GOT-overwrite attacks. `PT_GNU_PROPERTY` carries CET IBT/SHSTK feature flags that the kernel must check before enabling hardware enforcement. All three are standard on Linux; without them, ELF binaries run with weaker security than they were compiled for.

- [ ] Parse `PT_GNU_STACK` (type `0x6474e551`): if `p_flags` lacks `PF_X`, ensure the user stack pages are mapped without execute permission; if `PF_X` is present, log a warning and allow (legacy compat)
- [ ] Parse `PT_GNU_RELRO` (type `0x6474e552`): after all relocations in §14, call `vmm_protect(proc->pml4, relro_start, relro_size, VMM_READ)` to make the RELRO range read-only
- [ ] Parse `PT_GNU_PROPERTY` (type `0x6474e553`): extract `GNU_PROPERTY_X86_FEATURE_1_AND` -- check for `IBT` (bit 0) and `SHSTK` (bit 1) flags; store in process metadata for TODO-17 CET enforcement
- [ ] If `PT_GNU_STACK` is absent, default to NX stack (safe default, matching Linux 6.x behavior)
- [ ] Commit: `"kernel: elf -- PT_GNU_STACK NX enforcement, PT_GNU_RELRO, PT_GNU_PROPERTY CET flags"`

**Test checkpoint:** Serial log shows `"elf: NX stack enforced"` for binaries without `PF_X` in `PT_GNU_STACK`. Serial log shows `"elf: RELRO range 0x<start>-0x<end> locked"` after relocation. `POST16(0xD805)` on entry, `POST16(0xD806)` after RELRO applied. Test on: QEMU WHPX + TCG. Note: `vmm_protect()` does not exist yet -- implement as direct PTE write + `invlpg` until `03-memory-concurrency/TODO-01` lands.

## 4. EIF Format Specification
Design the Executable Impossible Format -- minimal parsing, native OS metadata, syscall-ID imports.

- [ ] Write `docs/specs/eif-format.md` defining `eif_header_t` (64 bytes):
  - `0x00` magic `"EIF!"` · `0x04` version · `0x06` arch (`1` = x86_64) · `0x08` flags (`GUI=1 | CONSOLE=2 | DRIVER=4 | SIGNED=8`)
  - `0x0C` api_version · `0x10` entry_point · `0x18` load_base · `0x20` segment_count · `0x24` import_count
  - `0x28` segment_offset · `0x2C` import_offset · `0x30` signature_offset · `0x38` metadata_offset
- [ ] Define `eif_segment_t` (32 bytes): `vaddr`, `file_offset`, `file_size`, `mem_size`, `flags (R=1|W=2|X=4)`, reserved
- [ ] Define `eif_import_t` (8 bytes): `syscall_id` (Impossible OS SSDT number), `flags` (`0` = required, `1` = optional)
  - Import resolution is integer-only -- no string lookup at load time
- [ ] Commit: `"docs: EIF format specification"`

**Test checkpoint:** `docs/specs/eif-format.md` exists and contains `eif_header_t`, `eif_segment_t`, `eif_import_t` definitions with byte offsets. Header totals 64 bytes. No runtime test -- spec document only.

## 5. EIF Kernel Loader

- [ ] Create `src/kernel/eif.c` + `include/kernel/eif.h`
- [ ] Implement `eif_exec(path, proc)`:
  - Read 64-byte header; validate magic + arch + version
  - Read segment table; for each segment: allocate user pages, copy data, set permissions
  - Read import table; for each entry: map `syscall_id` → kernel SSDT handler pointer; write to per-process dispatch table at a well-known user-space address
  - If `SIGNED` flag: verify digital signature (§17) before mapping any segment
  - Return entry point
- [ ] Performance target: < 10 µs for a typical 64 KB binary
- [ ] Commit: `"kernel: eif -- EIF loader"`

**Test checkpoint:** Serial log shows `"eif: loaded in <N> µs"` where `<N>` < 10. Invalid magic rejected with error. `SIGNED` flag without signature returns error. `POST16(0xD807)` on entry, `POST16(0xD808)` after segments mapped. Test on: QEMU WHPX + TCG.

## 6. Module List Registration (LDR_DATA_TABLE_ENTRY)

Every loaded executable and shared library must be registered in the per-process module list so that the debugger (TODO-18), crash dump generator (TODO-16), SEH unwind logic (TODO-10), and `PEB->Ldr` module walks all work. Win11 maintains three linked lists (`InLoadOrder`, `InMemoryOrder`, `InInitializationOrder`) in `PEB_LDR_DATA`. Linux maintains `struct link_map` for `dl_iterate_phdr()`. Impossible OS must populate both for dual-format compat.

- [ ] Define `loaded_module_t` in `include/kernel/exec.h`: `{ base_address, size_of_image, entry_point, full_path, name, format (ELF/PE/EIF), pdata_base, pdata_size }`
- [ ] `exec_register_module(process_t *proc, loaded_module_t *mod)` -- inserts into per-process module list and global `LOADED_MODULE` crash registry (→ XREF TODO-16 §3)
- [ ] For PE32+ modules: insert `LDR_DATA_TABLE_ENTRY` into `PEB->Ldr` lists (→ XREF TODO-04 §8)
- [ ] For ELF modules: insert into a process-local `link_map` chain for `dl_iterate_phdr()` compat
- [ ] `exec_find_module_by_pc(uint64_t rip)` -- binary search module list by address range; returns module for stack traces and `.pdata` lookup
- [ ] Commit: `"kernel: exec -- module list registration for Ldr, crash dump, and debugger"`

**Test checkpoint:** After loading a binary, `exec_find_module_by_pc(entry_va)` returns non-NULL with correct `name` and `base_address`. Serial log shows `"exec: registered module '<name>' at 0x<base> (size=<N>)"`. `POST16(0xD809)` on entry, `POST16(0xD80A)` after module inserted. Test on: QEMU WHPX + TCG.

## 7. PE32+ Header Parser

- [ ] Create `src/kernel/pe.c` + `include/kernel/pe.h`
- [ ] Define PE structures: `pe_dos_header_t` (MZ magic, `e_lfanew`), PE signature `0x00004550`, `pe_coff_header_t` (Machine = `0x8664`, NumberOfSections), `pe_optional_header64_t` (Magic = `0x20B`, AddressOfEntryPoint, ImageBase, SizeOfImage, SizeOfHeaders, DataDirectory), `pe_section_header_t`
- [ ] Implement `pe_validate(data, size)`:
  - Check MZ magic at offset 0; PE signature at `e_lfanew`; Machine == `0x8664`; Optional Header Magic == `0x20B`
  - Reject 32-bit PE (Magic == `0x10B`) with `ENOEXEC`
- [ ] Commit: `"kernel: pe -- PE32+ header parser"`

**Test checkpoint:** `pe_validate()` returns success for a valid PE32+ header (Machine `0x8664`, Magic `0x20B`). Returns error for 32-bit PE (`0x10B`). Returns error for truncated file. `POST16(0xD80B)` on entry. Test on: QEMU WHPX + TCG.

## 8. PE32+ Section Loader + `.pdata` Registration

- [ ] Parse section headers; for each section:
  - Allocate user pages at `ImageBase + VirtualAddress`
  - Copy `SizeOfRawData` bytes from `PointerToRawData`; zero-fill `VirtualSize - SizeOfRawData`
  - Set permissions from `Characteristics`: `IMAGE_SCN_MEM_EXECUTE` / `IMAGE_SCN_MEM_WRITE` / `IMAGE_SCN_MEM_READ`
- [ ] Map PE headers (`SizeOfHeaders`) at ImageBase for runtime introspection
- [ ] Parse DataDirectory entry 3 (`IMAGE_DIRECTORY_ENTRY_EXCEPTION`): locate `.pdata` section containing `RUNTIME_FUNCTION` entries; register `{ pdata_base, pdata_size, image_base }` with the module list (§6) so `RtlLookupFunctionEntry` (→ XREF TODO-10 §6) can find unwind data
- [ ] Register loaded module via `exec_register_module()` (§6) with base, size, entry, and `.pdata` info
- [ ] Commit: `"kernel: pe -- PE32+ section loader with .pdata registration"`

**Test checkpoint:** Serial log shows `"pe: mapped <N> sections, .pdata at 0x<addr> (<M> entries)"`. Section permissions match `Characteristics`. BSS-only sections are zero-filled. `POST16(0xD80C)` on entry, `POST16(0xD80D)` after `.pdata` registered. Test on: QEMU WHPX + TCG.

## 9. PE32+ Import Table Resolver (Win32 Dispatch)

This bridges PE executables to the Impossible OS Win32 API -- every `CreateFile`, `ReadFile`, and `WriteFile` call in a PE binary flows through here.

> [!IMPORTANT]
> **Soft dependency on TODO-05 §5** (Nt/Zw naming + syscall migration, currently `[ ]`). §9 is NOT fully blocked -- implement the import resolution machinery and export table structure now. Use stub thunks that return `STATUS_NOT_IMPLEMENTED` for unmigrated SSDT entries. Real SSDT indices can be wired later when TODO-05 §5 lands. This allows PE binaries to load and run; calls to unimplemented APIs return clean errors instead of crashing.

- [ ] Parse Import Directory Table (DataDirectory entry 1):
  - Walk `IMAGE_IMPORT_DESCRIPTOR` array; get DLL name string and INT/IAT entry pairs
  - Resolve by name (`IMAGE_IMPORT_BY_NAME`) or by ordinal
- [ ] Maintain kernel-side export tables:
  - `impossible_kernel32_exports[]`, `impossible_ntdll_exports[]`, `impossible_user32_exports[]`, `impossible_advapi32_exports[]`
  - Each entry: `{ function_name, ssdt_index }` -- write SSDT thunk address into IAT
- [ ] Unknown DLL names → stub that returns `STATUS_NOT_IMPLEMENTED` (do not crash)
- [ ] Export table lookup: sorted array + binary search or FNV hash table
- [ ] Commit: `"kernel: pe -- PE32+ import table resolver"`

**Test checkpoint:** Serial log shows `"pe: resolved <N> imports from <DLL>"` for each DLL. `kernel32.dll!ExitProcess` resolves to a valid SSDT thunk. Unknown DLL produces warning log, not crash. `POST16(0xD80E)` on entry, `POST16(0xD80F)` after IAT patched. Test on: QEMU WHPX + TCG.

## 10. PE32+ Base Relocation

- [ ] Parse `.reloc` section (DataDirectory entry 5):
  - Walk base relocation blocks (page RVA + array of `type | offset` entries)
  - `IMAGE_REL_BASED_DIR64` (10): add `delta = actual_base - ImageBase` to 64-bit value at offset
  - `IMAGE_REL_BASED_ABSOLUTE` (0): skip (padding)
- [ ] Apply delta to all type-10 entries after section loading
- [ ] Commit: `"kernel: pe -- PE32+ base relocation"`

**Test checkpoint:** Serial log shows `"pe: relocated <N> entries, delta=0x<delta>"`. A PE loaded at non-preferred base calls a function pointer without crash. `POST16(0xD810)` on entry, `POST16(0xD811)` after relocations applied. Test on: QEMU WHPX + TCG.

## 11. PE32+ TLS Directory Processing

PE binaries using `__declspec(thread)` or C11 `_Thread_local` store TLS templates and callbacks in the TLS directory (DataDirectory entry 9). The loader must allocate per-thread TLS data from the template, assign a TLS index, and invoke TLS callbacks (`DLL_PROCESS_ATTACH`) before the entry point runs. Without this, any Win32 binary using thread-local storage will crash on first TLS access.

- [ ] Parse `IMAGE_TLS_DIRECTORY64` from DataDirectory[9]: `StartAddressOfRawData`, `EndAddressOfRawData`, `AddressOfIndex`, `AddressOfCallBacks`, `SizeOfZeroFill`, `Characteristics`
- [ ] Allocate per-thread TLS block: copy raw data range `[Start..End)` to a fresh allocation; append `SizeOfZeroFill` zero bytes; store pointer in TEB `TlsSlots[assigned_index]`
- [ ] Write assigned TLS index to `AddressOfIndex` in the mapped image
- [ ] Invoke TLS callbacks (if `AddressOfCallBacks` is non-NULL): walk null-terminated function pointer array; call each with `(hModule, DLL_PROCESS_ATTACH, NULL)` -- callbacks run in ring 3 before `main()`
- [ ] On thread creation: allocate fresh TLS block from template; call callbacks with `DLL_THREAD_ATTACH`
- [ ] Commit: `"kernel: pe -- TLS directory processing and callbacks"`

**Test checkpoint:** Serial log shows `"pe: TLS index=<N>, raw data <size> bytes, <M> callbacks"`. TLS callback log: `"pe: TLS callback DLL_PROCESS_ATTACH at 0x<addr>"`. Thread-local variable read returns initialized value. `POST16(0xD812)` on entry, `POST16(0xD813)` after callbacks invoked. Test on: QEMU WHPX + TCG.

## 12. PE32+ Load Config and CFG Bitmap

Modern PE binaries carry an `IMAGE_LOAD_CONFIG_DIRECTORY64` (DataDirectory entry 10) containing security metadata: Control Flow Guard (CFG) function table, CET shadow stack compatibility flags, and security cookie location. The kernel must parse this to enable hardware-assisted control flow integrity.

> [!NOTE]
> → XREF: `TODO-17-kernel-security-hardening.md §6,§7` -- CET shadow stack (§6) and CET IBT (§7) enforcement is authoritative in TODO-17. This section parses the PE metadata and stores it in the process/module record; TODO-17 acts on it. CFG bitmap enforcement also requires a dedicated CFG section in TODO-17.

- [ ] Parse `IMAGE_LOAD_CONFIG_DIRECTORY64` from DataDirectory[10]: extract `GuardCFFunctionTable`, `GuardCFFunctionCount`, `GuardFlags`
- [ ] If `IMAGE_GUARD_CF_INSTRUMENTED` and `IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT`: allocate a per-process CFG bitmap; populate valid call targets from the GFIDS table; store in process metadata
- [ ] Extract `SecurityCookie` address and initialise with `RDRAND` value (overwrite the linker default)
- [ ] Parse CET fields: `GuardAddressTakenIatEntryTable`, `GuardEHContinuationTable` -- store for TODO-17 CET activation
- [ ] If Load Config is absent or has zero size: treat as legacy binary (no CFG, no CET) -- log warning
- [ ] Commit: `"kernel: pe -- Load Config directory, CFG bitmap, CET metadata"`

**Test checkpoint:** Serial log shows `"pe: CFG bitmap: <N> valid targets"` for CFG-instrumented PE. Shows `"pe: security cookie initialized"`. Legacy PE without Load Config shows `"pe: no Load Config -- legacy binary"`. `POST16(0xD814)` on entry, `POST16(0xD815)` after CFG bitmap populated. Test on: QEMU WHPX + TCG.

## 13. `elf2eif` Host-Side Converter

Standard developer workflow: `clang-19 → ld.lld → elf2eif` -- no custom compiler required.

- [ ] Create `tools/elf2eif.c` (compiled with host `gcc`)
- [ ] Read input ELF64; validate headers; extract `PT_LOAD` segments → EIF `eif_segment_t` entries
- [ ] Generate import table:
  - Scan ELF `.dynsym` or custom `.eif_imports` section for Win32 API symbols
  - Map function names → SSDT service numbers via `syscall_map.h`
- [ ] Write EIF output: header + segments + import table
- [ ] Optional: embed digital signature via `tools/eifsign`
- [ ] Add `make elf2eif` target; integrate auto-conversion into user-mode build rules
- [ ] Commit: `"tools: elf2eif converter"`

**Test checkpoint:** `make elf2eif` builds successfully. `tools/elf2eif hello.elf hello.eif` produces output with `EIF!` magic at offset 0. Kernel loads the resulting EIF and reaches entry point. No runtime POST codes -- host-side tool.

## 14. ELF Dynamic Linker

Shared library support for the Linux compatibility layer and future ELF apps.

> [!NOTE]
> A dedicated ELF shared library TODO has not yet been filed. This section implements only the minimum kernel-side pieces needed for the Linux compatibility layer. File a new TODO under `02-kernel-core` if dynamic linking scope expands beyond the items listed here.

- [ ] Parse `PT_DYNAMIC` segment for `DT_NEEDED`, `DT_STRTAB`, `DT_SYMTAB`, `DT_HASH`/`DT_GNU_HASH`
- [ ] Load `.so` files from `C:\Impossible\System\lib\` via VFS
- [ ] Symbol resolution: `DT_GNU_HASH` lookup → `DT_SYMTAB` match
- [ ] Apply relocations: `R_X86_64_JUMP_SLOT` (PLT), `R_X86_64_GLOB_DAT` (GOT), `R_X86_64_RELATIVE`
- [ ] Lazy PLT binding: stubs resolve on first call
- [ ] After relocations complete: apply `PT_GNU_RELRO` protection (§3) to make GOT read-only
- [ ] Register each loaded `.so` via `exec_register_module()` (§6)
- [ ] Commit: `"kernel: elf -- dynamic linker and shared library loading"`

**Test checkpoint:** Serial log shows `"elf: loaded shared library '<name>.so' at 0x<addr>"` for each `DT_NEEDED`. PLT stub call resolves on first invocation (lazy binding). `PT_GNU_RELRO` applied after relocations -- GOT write attempt faults. `POST16(0xD816)` on entry, `POST16(0xD817)` after all `.so` loaded. Test on: QEMU WHPX + TCG.

## 15. ASLR -- Address Space Layout Randomization

- [ ] Random base for PIE ELF (`ET_DYN`): choose base within user range; add to all `PT_LOAD` vaddrs
- [ ] Random base for EIF: add random offset to `load_base` if non-zero
- [ ] Random base for PE32+: pick base ≠ `ImageBase`; apply base relocation (§10) with new delta
- [ ] Randomize user stack base
- [ ] Randomize heap base
- [ ] PRNG: use `RDRAND` instruction if `CPU_FEATURE_RDRAND` available; fallback to TSC-seeded LCG
- [ ] Commit: `"kernel: exec -- ASLR for ELF, EIF, and PE32+"`

**Test checkpoint:** Load same PIE ELF twice in two processes -- serial log shows different base addresses. PE loaded at address ≠ `ImageBase`. Stack base differs between processes. `POST16(0xD818)` on entry, `POST16(0xD819)` after base selected. Test on: QEMU WHPX + TCG; VirtualBox; bare metal. Verify `RDRAND` works on bare metal.

## 16. Script/Shebang Interpreter Support

Auto-detect `#!` (shebang) lines in text files and dispatch to the named interpreter. Linux has `binfmt_script` in the kernel; Windows has file-extension associations only. Impossible OS handles both -- magic-based shebang dispatch works even without a file extension, and the exec dispatcher routes it transparently.

> [!TIP]
> Neither Windows nor Linux handles this at the kernel binary-loader level with format-agnostic dispatch. Windows relies on `cmd.exe` file associations; Linux has `binfmt_script` but it's a separate module from `binfmt_elf`. Impossible OS unifies all formats -- ELF, PE32+, EIF, and scripts -- in a single `exec_load()` dispatcher.

- [ ] Register `#!` (bytes `0x23 0x21`) as a format in the exec dispatcher (§1)
- [ ] Parse shebang line: extract interpreter path and optional argument (max 256 bytes, stop at `\n`)
- [ ] Validate interpreter path exists via VFS; reject circular shebangs (interpreter itself has `#!`)
- [ ] Re-invoke `exec_load()` with interpreter as the binary and original script path as argv[1]
- [ ] Handle edge cases: missing interpreter → `ENOENT`; empty shebang → `ENOEXEC`; shebang longer than 256 bytes → truncate with warning
- [ ] Commit: `"kernel: exec -- shebang (#!) interpreter support"`

**Test checkpoint:** Script with `#!/C:\Impossible\System\shell.exe` dispatches to shell -- serial log shows `"exec: shebang -> /C:\Impossible\System\shell.exe"`. Circular shebang rejected with error. Missing interpreter → error. `POST16(0xD81A)` on entry, `POST16(0xD81B)` after interpreter resolved. Test on: QEMU WHPX + TCG.

## 17. EIF Code Signing

EIF binaries with the `SIGNED` flag must pass signature verification before any segment is mapped.

> [!IMPORTANT]
> → XREF: `TODO-20-kernel-libraries.md §5` -- Monocypher provides `crypto_eddsa_check()` (Ed25519) and `crypto_blake2b()` (SHA-256 substitute); §5 must be integrated before this section can be implemented.

- [ ] Read signature from `signature_offset` in EIF header
- [ ] Compute SHA-256 over header + all segment data in file order
- [ ] Verify signature against OS-embedded trusted public key
- [ ] Policy: `SIGNED` flag → reject on invalid/missing signature; unsigned binaries run with reduced capabilities (no raw disk I/O, no driver-level access)
- [ ] Create `tools/eifsign.c` -- host-side signing tool
- [ ] Commit: `"kernel: eif -- EIF code signing"`

**Test checkpoint:** EIF with valid signature loads successfully -- serial log shows `"eif: signature verified"`. EIF with `SIGNED` flag but missing/invalid signature rejected -- serial log shows `"eif: signature verification FAILED"`. Unsigned EIF (no `SIGNED` flag) loads but with reduced capabilities logged. `POST16(0xD81C)` on entry, `POST16(0xD81D)` after signature check. Test on: QEMU WHPX + TCG.

---

## OS Comparison

| ⭐ | Feature                     | 🪟 Win11                    | 🐧 Linux                    | 🚀 Impossible OS               |
|----|-----------------------------|--------------------------|--------------------------|------------------------------|
| 💎 | Native binary format        | ✅ PE32+                 | ✅ ELF                   | ⬜ §4–§5 EIF native          |
| 💎 | ELF loading                 | ⚠️ WSL only              | ✅ Native                | ⚠️ §2 basic loader           |
| 💎 | PE32+ loading               | ✅ Native                | ⚠️ Wine only             | ⬜ §7–§10 kernel-level       |
| 💎 | Dynamic linking             | ✅ DLL loading           | ✅ ld.so                 | ⬜ §14                       |
| 💎 | ASLR                        | ✅ Mandatory             | ✅ PIE + kernel           | ⬜ §15                       |
| 💎 | NX stack                    | ✅ DEP default           | ✅ PT_GNU_STACK           | ⬜ §3                        |
| 💎 | RELRO (read-only GOT)       | ⚠️ N/A (PE IAT)         | ✅ PT_GNU_RELRO           | ⬜ §3                        |
| 💎 | CET/IBT binary flags        | ✅ Load Config           | ✅ GNU_PROPERTY           | ⬜ §3 ELF, §12 PE           |
| 💎 | Module list                  | ✅ PEB->Ldr              | ✅ link_map               | ⬜ §6                        |
| 💎 | PE .pdata unwind             | ✅ Kernel + ntdll        | ❌ N/A                   | ⬜ §8                        |
| 💎 | PE TLS + callbacks           | ✅ Full support          | ❌ N/A (ELF PT_TLS)      | ⬜ §11                       |
| 💎 | PE Load Config / CFG         | ✅ CFG mandatory         | ❌ N/A                   | ⬜ §12                       |
| ⭐ | Triple format support        | ❌ PE32+ only            | ❌ ELF only              | ⬜ §1–§10 all three          |
| ⭐ | < 10 µs load time            | ❌ ~50 µs                | ❌ ~30 µs                | ⬜ §5 EIF instant            |
| ⭐ | Syscall-ID imports           | ❌ String-based          | ❌ String-based          | ⬜ §4–§5 integer-only        |
| ⭐ | Standard toolchain → native  | ❌ Needs PE linker       | ⚠️ ELF only              | ⬜ §13 elf2eif               |
| ⭐ | Shebang dispatch             | ❌ File extension        | ⚠️ Separate module       | ⬜ §16 unified               |
| ⭐ | Mandatory code signing       | ⚠️ Optional Authenticode | ❌ None built-in         | ⬜ §17 mandatory EIF         |

> **After §1–§6:** Impossible OS has a triple-format dispatcher, module list infrastructure, and the world's fastest native loader (EIF).
> **After §7–§12:** Full kernel-level PE32+ loading with TLS, CFG, and .pdata unwind tables -- run Windows-compiled executables natively without a compatibility layer.
> **§3** brings ELF security to Linux parity: NX stack, RELRO, and CET property flags.
> **§16** unifies shebang dispatch into `exec_load()` -- no separate kernel module needed.
> **§17** makes EIF code signing mandatory, not optional -- stronger integrity guarantees than Windows Authenticode.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_exec()` -- register in `src/kernel/test/test_runner.c`.
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_exec.c` with:
  - `exec_load` with `\x7FELF` magic routes to ELF loader (not PE32+ or EIF)
  - `exec_load` with `MZ` magic routes to PE32+ loader
  - `exec_load` with `EIF!` magic routes to EIF loader
  - `exec_load` with `#!` magic routes to shebang handler (§16)
  - `exec_load` with unknown magic returns `ENOEXEC`
  - `exec_load` on non-existent file returns `ENOENT`
  - ELF `PT_LOAD` segments are mapped at VMM user pages (address within user range, not identity-mapped)
  - ELF page permissions: `PF_X` segment has execute permission; `PF_W` segment has write permission
  - ELF BSS region (`memsz > filesz`) is zero-filled
  - ELF `PT_GNU_STACK` without `PF_X`: stack pages lack execute permission (§3)
  - ELF `PT_GNU_RELRO`: RELRO range is read-only after relocation (§3)
  - PE32+ validation: `pe_validate` accepts valid PE32+ header (Machine `0x8664`, Magic `0x20B`)
  - PE32+ validation: `pe_validate` rejects 32-bit PE (Magic `0x10B`) with `ENOEXEC`
  - PE32+ base relocation: `IMAGE_REL_BASED_DIR64` applies correct delta to 64-bit value
  - PE32+ import resolver: known `kernel32.dll!ExitProcess` maps to `NtTerminateProcess` SSDT index
  - PE32+ import resolver: unknown DLL name returns stub (not crash)
  - PE32+ TLS: `IMAGE_TLS_DIRECTORY64` parsed; TLS index written; initial TLS data copied (§11)
  - PE32+ Load Config: `IMAGE_LOAD_CONFIG_DIRECTORY64` parsed; CFG bitmap populated for CFG-instrumented PE (§12)
  - Module registration: `exec_register_module()` creates entry with correct base/size/name (§6)
  - Module lookup: `exec_find_module_by_pc(entry_va)` finds the registered module
  - EIF header validation: correct magic + arch + version accepted; wrong magic rejected
  - EIF import table: syscall ID resolves to valid SSDT handler
  - EIF `SIGNED` flag with missing signature returns `ENOEXEC`
  - Shebang: `#!/C:\Impossible\System\shell.exe` script dispatches to shell (§16)
- [ ] Register in `test_runner_init()`: `test_register_exec()`
- [ ] Commit: `"test: add binary format system test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `exec_load("hello.elf", ...)` executes and reaches user-mode entry point without a GPF
- [ ] ELF segments land at VMM-allocated user pages with correct R/W/X; no identity-map address
- [ ] ELF with `PT_GNU_STACK` (no PF_X): serial log shows `"elf: NX stack enforced"`; writing to stack works, executing from stack faults
- [ ] ELF with `PT_GNU_RELRO`: GOT pages marked read-only after relocation; write attempt faults
- [ ] `exec_load("hello.eif", ...)` loads and enters; serial log shows < 10 µs load time
- [ ] `exec_load("hello.exe", ...)` (PE32+) loads sections; IAT entries point to kernel Win32 stubs
- [ ] PE32+ import from `kernel32.dll!ExitProcess` resolves to the correct `NtTerminateProcess` thunk
- [ ] PE32+ with non-preferred `ImageBase`: base relocations applied correctly; no GPF on first call
- [ ] PE32+ `.pdata` registered: `RtlLookupFunctionEntry(entry_va)` returns non-NULL `RUNTIME_FUNCTION`
- [ ] PE32+ TLS: thread-local variable access succeeds; TLS callback logged before `main()`
- [ ] PE32+ Load Config: CFG bitmap populated; serial log shows `"pe: CFG bitmap: <N> valid targets"`
- [ ] Module list: `exec_find_module_by_pc(entry_va)` returns correct module for both ELF and PE
- [ ] PIE ELF loaded at two different addresses in two processes; no virtual address collision
- [ ] EIF with `SIGNED` flag: reject with `ENOEXEC` if signature is missing or invalid
- [ ] `elf2eif hello.elf hello.eif` produces a valid EIF that the kernel loads correctly
- [ ] Script with `#!/C:\Impossible\System\shell.exe` shebang: `exec_load()` runs interpreter with script as argument
- [ ] Commit: `"kernel: exec -- binary format system complete"`
