# TODO-08 — Binary Format System (exec_load / ELF / PE32+ / EIF)

> **Goal:** Build the multi-format executable loader that every user-mode program depends on. Three formats must work: ELF (existing basic loader upgraded), PE32+ (Windows-compatible, imports wired to Win32 API), and EIF (Impossible OS native — 64-byte header, syscall-ID import table, <10 µs load time). A single `exec_load()` dispatcher auto-detects format by magic bytes and routes to the correct loader. ASLR and EIF code signing close out the security story.

> [!IMPORTANT]
> **Current state:** A basic ELF loader exists in `src/kernel/elf.c` (143 lines). It loads `PT_LOAD` segments via identity mapping and is called directly from `task.c`. No format dispatcher, no PE32+ support, no EIF format, no proper VMM-backed user-space mapping.

## Inputs

- [`src/kernel/elf.c`](../../src/kernel/elf.c), [`include/kernel/elf.h`](../../include/kernel/elf.h) — existing basic ELF loader
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) — direct `elf_load()` call site (line 635) to be replaced
- [`user/user.ld`](../../user/user.ld) — user-mode linker script (base `0x800000`)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) — `vmm_map_pages()` for user-space page allocation
- [`src/kernel/fs/vfs.c`](../../src/kernel/fs/vfs.c) — `vfs_read()` for file I/O in loaders
- → XREF: `TODO-04-peb-teb-user-abi.md §6–§7` — TEB allocation (§6) and initial user stack frame (§7) are populated after `exec_load()` hands control to ring 3
- → XREF: `TODO-05-native-api-layer.md §5` — `NtXxx` SSDT entries must exist before §7 (import resolver) maps DLL function names to SSDT indices
- → XREF: `TODO-03-object-manager.md §5` — process object registered in Ob namespace at `exec_load()` time
- → XREF: `11-user-platform-sdk/INDEX.md` — EIF spec doc lives there; `elf2eif` tool and SDK integration wire back to §9

## Outcome

- `exec_load(path, proc)` in `src/kernel/exec.c` replaces the direct `elf_load()` call; auto-detects format by magic and dispatches.
- ELF loader uses VMM-backed user pages with correct R/W/X permissions; supports PIE.
- EIF format is specified in `docs/specs/eif-format.md`; kernel loads EIF in <10 µs.
- PE32+ parser loads sections and resolves imports against the kernel-provided Win32 export tables.
- `tools/elf2eif` converts standard ELF64 output to EIF — no custom compiler needed.
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

| ⭐  | Order | Deliverable                                  | Depends On              | Status |
| --- | :---: | -------------------------------------------- | ----------------------- | :----: |
| 💎  |   1   | `exec_load()` multi-format dispatcher        | VMM, VFS, sched         |  [ ]   |
| 💎  |   2   | Enhanced ELF loader (VMM-backed, PIE)        | 1                       |  [ ]   |
| ⭐  |   3   | EIF format specification                     | —                       |  [ ]   |
| ⭐  |   4   | EIF kernel loader                            | 1, 3                    |  [ ]   |
| 💎  |   5   | PE32+ header parser                          | 1                       |  [ ]   |
| 💎  |   6   | PE32+ section loader                         | 5                       |  [ ]   |
| 💎  |   7   | PE32+ import table resolver (Win32 dispatch) | 6, TODO-05 §5           |  [ ]   |
| 💎  |   8   | PE32+ base relocation                        | 6                       |  [ ]   |
| ⭐  |   9   | `elf2eif` host-side converter                | 3                       |  [ ]   |
| 💎  |  10   | ELF dynamic linker (shared libraries)        | 2                       |  [ ]   |
| 💎  |  11   | ASLR for all three formats                   | 2, 4, 6                 |  [ ]   |
| ⭐  |  12   | EIF code signing                             | 4                       |  [ ]   |

> 💎 = parity — Windows NT (PE32+) and Linux (ELF) both provide these capabilities.
> ⭐ = exclusive — triple-format support, EIF native format, syscall-ID imports, and mandatory code signing are Impossible OS only.

---

## 1. `exec_load()` Multi-Format Dispatcher `[Opus]`

Replace the direct `elf_load()` call in `task.c:635` with a generic dispatcher.

- [ ] Create `src/kernel/exec.c` + `include/kernel/exec.h`
- [ ] Define `exec_format_t` — `{ magic[4], magic_len, name, loader_fn }`
- [ ] Implement `exec_load(const char *path, process_t *proc)`:
  - Read first 4 bytes via VFS; match registered format handlers
  - Unknown magic → `ENOEXEC`; missing file → `ENOENT`
  - Allocate user-space page tables via VMM before calling format loader
  - Set up user-mode stack (top of user address range), push argc/argv
  - Set Ring 3 return context: RIP = entry, RSP = user stack, CS/SS = user segments
- [ ] `exec_register_format(exec_format_t *)` — register formats at boot
- [ ] Register ELF format in `kernel_main()` init; PE32+ and EIF at their respective init points
- [ ] Replace the direct `elf_load()` call in `task.c:635` with `exec_load()`
- [ ] Commit: `"kernel: exec — multi-format exec dispatcher"`

## 2. Enhanced ELF Loader `[Sonnet]`

Upgrade `elf_load()` to use VMM-backed user pages with correct permissions and PIE support.

- [ ] Refactor `elf_load()` → `elf_exec(path, proc)` registered with the dispatcher
- [ ] Read entire ELF file into a kernel buffer via VFS; validate ELF64 header
- [ ] For each `PT_LOAD` segment:
  - Allocate user pages via `vmm_map_pages(proc->pml4, vaddr, ...)`
  - Copy segment data; zero BSS (`memsz > filesz`) in allocated pages
  - Set page permissions from `p_flags`: `PF_X` → executable, `PF_W` → writable, `PF_R` → readable
- [ ] Support `ET_DYN` (PIE): compute random load base + vaddr (ASLR in §11)
- [ ] Set up auxiliary vector for dynamic linker if `PT_INTERP` present (§10)
- [ ] Reject segments outside the defined user address range
- [ ] Free kernel buffer after all segments are loaded
- [ ] Commit: `"kernel: elf — enhanced ELF loader with VMM mapping and PIE"`

## 3. EIF Format Specification `[Opus]`

Design the Executable Impossible Format — minimal parsing, native OS metadata, syscall-ID imports.

- [ ] Write `docs/specs/eif-format.md` defining `eif_header_t` (64 bytes):
  - `0x00` magic `"EIF!"` · `0x04` version · `0x06` arch (`1` = x86_64) · `0x08` flags (`GUI=1 | CONSOLE=2 | DRIVER=4 | SIGNED=8`)
  - `0x0C` api_version · `0x10` entry_point · `0x18` load_base · `0x20` segment_count · `0x24` import_count
  - `0x28` segment_offset · `0x2C` import_offset · `0x30` signature_offset · `0x38` metadata_offset
- [ ] Define `eif_segment_t` (32 bytes): `vaddr`, `file_offset`, `file_size`, `mem_size`, `flags (R=1|W=2|X=4)`, reserved
- [ ] Define `eif_import_t` (8 bytes): `syscall_id` (Impossible OS SSDT number), `flags` (`0` = required, `1` = optional)
  - Import resolution is integer-only — no string lookup at load time
- [ ] Commit: `"docs: EIF format specification"`

## 4. EIF Kernel Loader `[Sonnet]`

- [ ] Create `src/kernel/eif.c` + `include/kernel/eif.h`
- [ ] Implement `eif_exec(path, proc)`:
  - Read 64-byte header; validate magic + arch + version
  - Read segment table; for each segment: allocate user pages, copy data, set permissions
  - Read import table; for each entry: map `syscall_id` → kernel SSDT handler pointer; write to per-process dispatch table at a well-known user-space address
  - If `SIGNED` flag: verify digital signature (§12) before mapping any segment
  - Return entry point
- [ ] Performance target: < 10 µs for a typical 64 KB binary
- [ ] Commit: `"kernel: eif — EIF loader"`

## 5. PE32+ Header Parser `[Sonnet]`

- [ ] Create `src/kernel/pe.c` + `include/kernel/pe.h`
- [ ] Define PE structures: `pe_dos_header_t` (MZ magic, `e_lfanew`), PE signature `0x00004550`, `pe_coff_header_t` (Machine = `0x8664`, NumberOfSections), `pe_optional_header64_t` (Magic = `0x20B`, AddressOfEntryPoint, ImageBase, SizeOfImage, SizeOfHeaders, DataDirectory), `pe_section_header_t`
- [ ] Implement `pe_validate(data, size)`:
  - Check MZ magic at offset 0; PE signature at `e_lfanew`; Machine == `0x8664`; Optional Header Magic == `0x20B`
  - Reject 32-bit PE (Magic == `0x10B`) with `ENOEXEC`
- [ ] Commit: `"kernel: pe — PE32+ header parser"`

## 6. PE32+ Section Loader `[Sonnet]`

- [ ] Parse section headers; for each section:
  - Allocate user pages at `ImageBase + VirtualAddress`
  - Copy `SizeOfRawData` bytes from `PointerToRawData`; zero-fill `VirtualSize - SizeOfRawData`
  - Set permissions from `Characteristics`: `IMAGE_SCN_MEM_EXECUTE` / `IMAGE_SCN_MEM_WRITE` / `IMAGE_SCN_MEM_READ`
- [ ] Map PE headers (`SizeOfHeaders`) at ImageBase for runtime introspection
- [ ] Commit: `"kernel: pe — PE32+ section loader"`

## 7. PE32+ Import Table Resolver (Win32 Dispatch) `[Opus]`

This bridges PE executables to the Impossible OS Win32 API — every `CreateFile`, `ReadFile`, and `WriteFile` call in a PE binary flows through here. Requires TODO-05 §5 SSDT entries to exist.

- [ ] Parse Import Directory Table (DataDirectory entry 1):
  - Walk `IMAGE_IMPORT_DESCRIPTOR` array; get DLL name string and INT/IAT entry pairs
  - Resolve by name (`IMAGE_IMPORT_BY_NAME`) or by ordinal
- [ ] Maintain kernel-side export tables:
  - `impossible_kernel32_exports[]`, `impossible_ntdll_exports[]`, `impossible_user32_exports[]`, `impossible_advapi32_exports[]`
  - Each entry: `{ function_name, ssdt_index }` — write SSDT thunk address into IAT
- [ ] Unknown DLL names → stub that returns `STATUS_NOT_IMPLEMENTED` (do not crash)
- [ ] Export table lookup: sorted array + binary search or FNV hash table
- [ ] Commit: `"kernel: pe — PE32+ import table resolver"`

## 8. PE32+ Base Relocation `[Sonnet]`

- [ ] Parse `.reloc` section (DataDirectory entry 5):
  - Walk base relocation blocks (page RVA + array of `type | offset` entries)
  - `IMAGE_REL_BASED_DIR64` (10): add `delta = actual_base - ImageBase` to 64-bit value at offset
  - `IMAGE_REL_BASED_ABSOLUTE` (0): skip (padding)
- [ ] Apply delta to all type-10 entries after section loading
- [ ] Commit: `"kernel: pe — PE32+ base relocation"`

## 9. `elf2eif` Host-Side Converter `[Sonnet]`

Standard developer workflow: `clang-19 → ld.lld → elf2eif` — no custom compiler required.

- [ ] Create `tools/elf2eif.c` (compiled with host `gcc`)
- [ ] Read input ELF64; validate headers; extract `PT_LOAD` segments → EIF `eif_segment_t` entries
- [ ] Generate import table:
  - Scan ELF `.dynsym` or custom `.eif_imports` section for Win32 API symbols
  - Map function names → SSDT service numbers via `syscall_map.h`
- [ ] Write EIF output: header + segments + import table
- [ ] Optional: embed digital signature via `tools/eifsign`
- [ ] Add `make elf2eif` target; integrate auto-conversion into user-mode build rules
- [ ] Commit: `"tools: elf2eif converter"`

## 10. ELF Dynamic Linker `[Opus]`

Shared library support for the Linux compatibility layer and future ELF apps.

> [!NOTE]
> A dedicated ELF shared library TODO has not yet been filed. This section implements only the minimum kernel-side pieces needed for the Linux compatibility layer. File a new TODO under `02-kernel-core` if dynamic linking scope expands beyond the items listed here.

- [ ] Parse `PT_DYNAMIC` segment for `DT_NEEDED`, `DT_STRTAB`, `DT_SYMTAB`, `DT_HASH`/`DT_GNU_HASH`
- [ ] Load `.so` files from `C:\Impossible\System\lib\` via VFS
- [ ] Symbol resolution: `DT_GNU_HASH` lookup → `DT_SYMTAB` match
- [ ] Apply relocations: `R_X86_64_JUMP_SLOT` (PLT), `R_X86_64_GLOB_DAT` (GOT), `R_X86_64_RELATIVE`
- [ ] Lazy PLT binding: stubs resolve on first call
- [ ] Commit: `"kernel: elf — dynamic linker and shared library loading"`

## 11. ASLR — Address Space Layout Randomization `[Sonnet]`

- [ ] Random base for PIE ELF (`ET_DYN`): choose base within user range; add to all `PT_LOAD` vaddrs
- [ ] Random base for EIF: add random offset to `load_base` if non-zero
- [ ] Random base for PE32+: pick base ≠ `ImageBase`; apply base relocation (§8) with new delta
- [ ] Randomize user stack base
- [ ] PRNG: use `RDRAND` instruction if `CPU_FEATURE_RDRAND` available; fallback to TSC-seeded LCG
- [ ] Commit: `"kernel: exec — ASLR for ELF, EIF, and PE32+"`

## 12. EIF Code Signing `[Opus]`

EIF binaries with the `SIGNED` flag must pass signature verification before any segment is mapped.

> [!IMPORTANT]
> → XREF: `TODO-20-kernel-libraries.md §5` — Monocypher provides `crypto_eddsa_check()` (Ed25519) and `crypto_blake2b()` (SHA-256 substitute); §5 must be integrated before this section can be implemented.

- [ ] Read signature from `signature_offset` in EIF header
- [ ] Compute SHA-256 over header + all segment data in file order
- [ ] Verify signature against OS-embedded trusted public key
- [ ] Policy: `SIGNED` flag → reject on invalid/missing signature; unsigned binaries run with reduced capabilities (no raw disk I/O, no driver-level access)
- [ ] Create `tools/eifsign.c` — host-side signing tool
- [ ] Commit: `"kernel: eif — EIF code signing"`

---

## OS Comparison

| ⭐  | Feature                               | 🪟 Windows 11                     | 🐧 Linux                          | 🚀 Impossible OS                                         |
| --- | ------------------------------------- | ---------------------------------- | --------------------------------- | --------------------------------------------------------- |
| 💎  | Native binary format                  | ✅ PE32+                          | ✅ ELF                            | ⬜ Planned — §3–§4 (EIF native)                          |
| 💎  | ELF loading                           | ⚠️ WSL only                       | ✅ Native                         | ⚠️ Partial — basic identity-map loader; §2 upgrades it   |
| 💎  | PE32+ loading                         | ✅ Native                         | ⚠️ Wine only                      | ⬜ Planned — §5–§8 (kernel-level)                        |
| 💎  | Dynamic linking / shared libs         | ✅ DLL loading                    | ✅ `ld.so`                        | ⬜ Planned — §10                                         |
| 💎  | ASLR                                  | ✅ Mandatory since Vista          | ✅ PIE + kernel ASLR              | ⬜ Planned — §11                                         |
| ⭐  | Triple format support (ELF+PE32++EIF) | ❌ PE32+ only natively            | ❌ ELF only natively              | ⬜ **Planned — §1–§8 — all three**                       |
| ⭐  | < 10 µs load time (native format)     | ❌ ~50 µs PE parsing              | ❌ ~30 µs ELF + dynamic link      | ⬜ **Planned — §4 — EIF instant load**                   |
| ⭐  | Syscall-ID import resolution          | ❌ String-based DLL imports       | ❌ String-based symbol resolution | ⬜ **Planned — §3–§4 — integer-only, no string lookup**  |
| ⭐  | Standard toolchain → native binary    | ❌ Requires PE linker             | ⚠️ clang produces ELF directly    | ⬜ **Planned — §9 — `clang + ld.lld + elf2eif`**         |
| ⭐  | Mandatory per-binary code signing     | ⚠️ Authenticode (optional)        | ❌ No built-in per-binary signing | ⬜ **Planned — §12 — mandatory for EIF**                 |

> **After §1–§4:** Impossible OS has a triple-format dispatcher and the world's fastest native loader (EIF).
> **After §5–§8:** Full kernel-level PE32+ loading — run Windows-compiled executables natively without a compatibility layer.
> **§12** makes EIF code signing mandatory, not optional — providing stronger integrity guarantees than Windows Authenticode.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `exec_load("hello.elf", ...)` executes and reaches user-mode entry point without a GPF
- [ ] ELF segments land at VMM-allocated user pages with correct R/W/X; no identity-map address
- [ ] `exec_load("hello.eif", ...)` loads and enters; serial log shows < 10 µs load time
- [ ] `exec_load("hello.exe", ...)` (PE32+) loads sections; IAT entries point to kernel Win32 stubs
- [ ] PE32+ import from `kernel32.dll!ExitProcess` resolves to the correct `NtTerminateProcess` thunk
- [ ] PE32+ with non-preferred `ImageBase`: base relocations applied correctly; no GPF on first call
- [ ] PIE ELF loaded at two different addresses in two processes; no virtual address collision
- [ ] EIF with `SIGNED` flag: reject with `ENOEXEC` if signature is missing or invalid
- [ ] `elf2eif hello.elf hello.eif` produces a valid EIF that the kernel loads correctly
- [ ] Commit: `"kernel: exec — binary format system complete"`
