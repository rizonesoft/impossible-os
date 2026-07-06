---
schema_version: 1
id: binary-system
domain: 02-kernel-core
status: active
title: "TODO-17 -- Binary Format System (exec_load / ELF / PE32+ / EIF)"
---

# TODO-17 -- Binary Format System (exec_load / ELF / PE32+ / EIF)

> **Validated:** 2026-07-06 | validate-todo-file clean (structure / IO table / XREF drift fixed §7,§8->§9,§10 / §8 [x]->[/] open items / graph 8/8)
>
> **Gap-audited:** 2026-07-06 | Win11/PE + Linux/ELF loader parity (parity-research-analyst todo-plan + Codex gap-audit red-team). Filed: §20 new (IFUNC/IRELATIVE, DT_INIT/FINI_ARRAY, DT_VERSYM/VERNEED versioning, RUNPATH/RPATH search order, DT_FLAGS_1); §6 ELF .eh_frame unwind; §18 ELF TLS ABI (TCB/DTV/FS-base); §19 PE export-forwarder chains. Reciprocal: TODO-21 §14 module-deregister + fini-array on exit. Rejected Codex ownership-split claim (TODO-03 §5/§6 are crypto/JSON, TODO-10 §11 is heap-hardening -- none own ELF dynamic linking). Deferred: SEC_IMAGE COW -> TODO-05 Section objects (blocked on per-process PML4 §8); PE Authenticode -> TODO-05 driver signing; page-in code-sign = refinement.

> **Goal:** Build the multi-format executable loader that every user-mode program depends on. Three formats must work: ELF (existing basic loader upgraded), PE32+ (Windows-compatible, imports wired to Win32 API), and EIF (Impossible OS native -- 64-byte header, syscall-ID import table, <10 µs load time). A single `exec_load()` dispatcher auto-detects format by magic bytes and routes to the correct loader. ASLR and EIF code signing close out the security story.

> [!IMPORTANT]
> **Current state:** A basic ELF loader exists in `src/kernel/elf.c`. It loads `PT_LOAD` segments via identity mapping and is called directly from `task_exec()` in `src/kernel/sched/task.c`. No format dispatcher, no PE32+ support, no EIF format, no proper VMM-backed user-space mapping.

## Inputs

- [`src/kernel/elf.c`](../../src/kernel/elf.c), [`include/kernel/elf.h`](../../include/kernel/elf.h) -- existing basic ELF loader
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- direct `elf_load()` call site in `task_exec()` (currently around line 1141) to be replaced
- [`user/user.ld`](../../user/user.ld) -- user-mode linker script (base `0x800000`)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) -- `vmm_map_page()` for page mapping; `vmm_map_pages()` (multi-page wrapper) must be added in §2
- [`src/kernel/fs/vfs.c`](../../src/kernel/fs/vfs.c) -- `vfs_read()` for file I/O in loaders
- → XREF: `TODO-11-peb-teb-user-abi.md §1–§2` -- TEB allocation (§1) and initial user stack frame (§2) are populated after `exec_load()` hands control to ring 3
- → XREF: `TODO-12-native-api-ssdt.md §6` -- `NtXxx` SSDT entries must exist before §9 (import resolver) maps DLL function names to SSDT indices
- → XREF: `TODO-05-object-manager.md §5` -- process object registered in Ob namespace at `exec_load()` time
- → XREF: `TODO-23-exception-dispatch-seh.md §6` -- PE32+ `.pdata` section must be registered for loaded modules before `RtlLookupFunctionEntry` can find unwind data; §8 of this TODO registers `.pdata` with the unwind table registry
- → XREF: `TODO-27-crash-dump-generation.md §3` -- `LOADED_MODULE` registry consumes module base/size/name from `exec_load()`; §6 of this TODO populates the module list
- → XREF: `TODO-10-kernel-security-hardening.md §9,§10` -- PE Load Config Directory (§11) exposes CFG bitmap and CET flags; kernel CET shadow stack (§9) and CET IBT (§10) enforcement lives in TODO-23
- → XREF: `10-platform-services/TODO-07-win32-pe-loader.md` -- scope overlap: TODO-23/07 covers Win32 subsystem-level PE execution (SYSCALL/SYSRET setup, Win32 ABI, user CRT); this TODO covers the kernel-level binary format infrastructure (loaders, format dispatcher, ASLR). PE header structs and loader core are authoritative HERE; Win32 subsystem wiring is authoritative THERE.
- → XREF: `12-user-platform-sdk/INDEX.md` -- EIF spec doc lives there; `elf2eif` tool and SDK integration wire back to §13
- → XREF: `TODO-21-process-model-extensions.md §3` -- `exec_load()` (§1) must set `task->program_break` to end of BSS so brk/sbrk (TODO-21 §3) can extend from the correct address
- → XREF: `TODO-20-eif-full-implementation.md` -- completes EIF beyond §5 basic loader: segment permissions, ASLR, API version gating, metadata parsing, LZ4 decompression, module registration, import stubs. §13 (elf2eif) and §17 (code signing) remain here.
- → XREF: `TODO-28-bsod-ux-enhancements.md` §12: F2 last-driver deferral consumes module registry and `exec_find_module_by_pc()` from §6
- → XREF: `00-infrastructure/TODO-04-usermode-test-framework.md §5, §8, §9` -- `test_syscall.exe` / `test_process.exe` / `test_fileio.exe` exercise `exec_load()` (§1) + ELF loader (§2) + EIF kernel loader (§5) + PE delay-load (§19) from user mode

## Outcome

- `exec_load(path, proc)` in `src/kernel/exec.c` replaces the direct `elf_load()` call; auto-detects format by magic and dispatches.
- ELF loader uses VMM-backed user pages with correct R/W/X permissions; supports PIE; honours `PT_GNU_STACK` (NX stack), `PT_GNU_RELRO` (read-only GOT after relocation), and `PT_GNU_PROPERTY` (CET/IBT feature flags).
- EIF format is specified in `specs/eif-format.md`; kernel loads EIF in <10 µs.
- PE32+ parser loads sections, resolves imports, processes TLS directory, and registers `.pdata` unwind tables.
- PE32+ import compatibility covers API-set contract DLL names and delay-load/bound-import metadata for modern Win11 binaries.
- Every loaded module (ELF, PE32+, EIF) is registered in the `LDR_DATA_TABLE_ENTRY` module list and the `LOADED_MODULE` crash dump registry.
- ELF `PT_TLS` templates are loaded and cloned per thread so Linux-compat ELF binaries using `__thread` work correctly.
- PE Load Config Directory is parsed for CFG bitmap and CET shadow stack flags; enforcement deferred to TODO-23.
- Script/shebang (`#!`) files are auto-detected and dispatched to the named interpreter.
- `tools/elf2eif` converts standard ELF64 output to EIF -- no custom compiler needed.
- ASLR randomises load base for PIE ELF, EIF, and PE32+.
- EIF binaries with `SIGNED` flag are signature-verified before entry.

## Format Quick Reference

| Feature      | ELF                      | PE32+                          | EIF (native)                    |
| ------------ | ------------------------ | ------------------------------ | ------------------------------- |
| Magic        | `\x7FELF`                | `MZ`                           | `EIF!`                          |
| Header size  | 64 bytes                 | ~256 bytes (DOS+PE+Optional)   | 64 bytes                        |
| Import model | PLT/GOT (string symbols) | IAT (DLL name + function name) | Syscall ID table (integer only) |
| API surface  | POSIX / Linux compat     | Win32 (`kernel32.dll` stubs)   | Win32 (syscall IDs direct)      |
| Toolchain    | `clang-19 + ld.lld`      | `clang-19 + lld-link`          | `clang-19 + ld.lld + elf2eif`   |
| Primary use  | Dev tools, Linux compat  | Windows app compat             | All native OS apps              |

## Implementation Order

| ⭐   | Order | Deliverable                                    | Depends On      | Status |
| --- | :---: | ---------------------------------------------- | --------------- | :----: |
| 💎   |   1   | `exec_load()` multi-format dispatcher          | VMM, VFS, sched |  [x]   |
| 💎   |   2   | Enhanced ELF loader (VMM-backed, PIE)          | §1              |  [/]   |
| 💎   |   3   | ELF security segments (GNU_STACK, RELRO, PROP) | §2              |  [/]   |
| ⭐   |   4   | EIF format specification                       | --              |  [x]   |
| ⭐   |   5   | EIF kernel loader                              | §1, §4          |  [x]   |
| 💎   |   6   | Module list registration (LDR_DATA_TABLE)      | §1, TODO-11 §4  |  [/]   |
| 💎   |   7   | PE32+ header parser                            | §1              |  [x]   |
| 💎   |   8   | PE32+ section loader + `.pdata` registration   | §7              |  [/]   |
| 💎   |   9   | PE32+ import table resolver (Win32 dispatch)   | §8, TODO-12 §6  |  [x]   |
| 💎   |  10   | PE32+ base relocation                          | §8              |  [ ]   |
| 💎   |  11   | PE32+ TLS directory processing                 | §8, TODO-11 §3  |  [ ]   |
| 💎   |  12   | PE32+ Load Config and CFG bitmap               | §8              |  [ ]   |
| ⭐   |  13   | `elf2eif` host-side converter                  | §4              |  [ ]   |
| 💎   |  14   | ELF dynamic linker (shared libraries)          | §2              |  [ ]   |
| 💎   |  15   | ASLR for all three formats                     | §2, §5, §8      |  [ ]   |
| ⭐   |  16   | Script/shebang interpreter support             | §1              |  [ ]   |
| ⭐   |  17   | EIF code signing                               | §5              |  [ ]   |
| 💎   |  18   | ELF `PT_TLS` template loading                  | §2              |  [ ]   |
| 💎   |  19   | PE API-set + delay-load/bound import support   | §9              |  [ ]   |
| 💎   |  20   | ELF dynamic linker advanced (IFUNC, init/fini) | §14             |  [ ]   |

> 💎 = parity -- Windows NT (PE32+) and Linux (ELF) both provide these capabilities.
> ⭐ = exclusive -- triple-format support, EIF native format, syscall-ID imports, shebang dispatch, and mandatory code signing are Impossible OS only.

---

## 1. `exec_load()` Multi-Format Dispatcher
Replace the direct `elf_load()` call in `task_exec()` (`src/kernel/sched/task.c`) with a generic dispatcher.

> [!NOTE]
> **Resolved:** `ENOEXEC` / `ENOENT` defined in `include/kernel/errno.h` (20 POSIX error constants). Convention: positive constants, used via `*err` out-parameter in exec API or `-EFOO` returns elsewhere.

- [x] Create `src/kernel/exec.c` + `include/kernel/exec.h` -- dispatcher with `exec_load()`, `exec_load_path()`, `exec_register_format()`, `exec_init()`
- [x] Define `ENOEXEC` / `ENOENT` error contract in `include/kernel/errno.h` (new file, 20 POSIX error constants)
- [x] Define `process_t` alias -- `typedef PROCESS_OBJECT process_t` in `include/kernel/exec.h` with `#include "kernel/ob/ob_process.h"`
- [x] Define `exec_format_t` -- `{ magic[4], magic_len, name, loader_fn }` in exec.h; up to 8 formats
- [x] Implement `exec_load(data, size, *err)` -- matches magic bytes against registered formats, dispatches to loader, returns entry point; unknown magic -> ENOEXEC; also `exec_load_path(path, *err)` reads via VFS with ENOENT support
- [x] `exec_register_format(exec_format_t *)` -- register formats at boot (max 8)
- [x] Register ELF format in `exec_init()` called from `boot_desktop.c` Phase 3; PE32+/EIF registration points ready
- [x] Replace the direct `elf_load()` call in `task_exec()` (`task.c`) with `exec_load_fmt(data, size, &err, &fmt_name)` -- the format-name-returning superset that `exec_load()` wraps; user stack/PML4/context setup unchanged
- [x] 3 unit tests: bad magic (ENOEXEC), null data (ENOEXEC), sub-magic-length buffer (ENOEXEC)
- [x] Commit: `"kernel: exec -- multi-format exec dispatcher"`

**Test checkpoint:** Serial log shows `"exec: Registered format: ELF (magic 4 bytes)"` and `"exec: Exec subsystem initialized (1 format(s))"`. Unit tests: `exec_load` on `0xDEADBEEF` magic returns 0 + ENOEXEC; NULL data returns 0 + ENOEXEC; sub-magic-length (3-byte) buffer returns 0 + ENOEXEC. Adversarial review: 7 findings (2C/2H/1M/2L) all resolved -- SMP barrier, TOCTOU fix, buffer leak fix, input validation, alignment fix.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 86 suites, 0 failures
>
> **Notes:**
> - Multi-format magic dispatcher: `exec_load`/`exec_load_fmt` route by magic to the ELF/EIF/PE loaders, registered at Phase 3 `exec_init()`; `task_exec()` drives it.
> - Path loads (`exec_load_path`) stage via PMM contiguous frames for images > 4 KiB with a fail-closed short-read guard; kmalloc kept for the small tail; format registry sealed after boot.
> - Downstream: `exec_load_fmt` publishes the matched format name to the task struct; the module registry (§6) and per-format loaders (§2/§5/§7) build on this dispatcher.
> - Review hardening (commit `347c5b2d`): PMM staging + short-read fail-closed, post-boot registry seal, removed the `size < 4` floor that masked PE's 2-byte magic, replaced an empty errno test stub with a real behavioral test.
> - Scope: `exec_load_path` integration is exercised end-to-end at boot (task_exec loads the shell/desktop) and by usermode `test_process.exe` (D00 T04 §8); kernel unit tests cover magic routing + the ENOEXEC paths.
>
> **Verified:** 2026-07-06 | commit `347c5b2d` | 9/9 items | build OK | exec tests 616/616 PASS
> **Quality reviewed:** 2026-07-06 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+2M fixed | scope: kernel-code-quality

## 2. Enhanced ELF Loader
Upgrade `elf_load()` to use VMM-backed user pages with correct permissions and PIE support.

> [!NOTE]
> **Partially resolved:** The loader validates segment ranges and accepts ET_DYN (PIE). VMM-backed per-process page mapping (`vmm_map_pages`) deferred -- current path uses identity mapping + post-load PML4 setup in `task_exec()`. Per-segment R/W/X permissions logged but enforcement via PTE bits deferred until vmm_map_pages exists.

- [x] ELF loader enhanced: accepts `ET_DYN` (PIE) in addition to `ET_EXEC` -- `elf_validate()` updated
- [/] Refactor `elf_load()` → `elf_exec(path, proc)` registered with the dispatcher -- blocked: needs `vmm_map_pages()` and process_t; current flow via `exec_load()` wrapper works
- [/] Add `vmm_map_pages()` helper in `vmm.c`/`vmm.h` for multi-page per-process user mappings -- architectural prereq for full §2
- [x] Read entire ELF file into kernel buffer via VFS; validate ELF64 header -- done in existing `elf_load()`, buffer read by `exec_load_path()` (§1)
- [x] For each `PT_LOAD` segment: copy data, zero BSS, track load range -- done; permissions logged per-segment (R/W/X flags)
  - [/] VMM-backed allocation via `vmm_map_pages(proc->pml4, vaddr, ...)` -- blocked: needs vmm_map_pages
  - [/] Set page permissions from `p_flags` via PTE bits -- blocked: needs vmm_map_pages to set per-page flags
- [x] Accept `ET_DYN` (PIE) type in elf_validate -- ASLR random base deferred to §15
- [/] Set up auxiliary vector for dynamic linker if `PT_INTERP` present -- deferred to §14
- [x] Reject segments outside user address range (`USER_ELF_BASE..USER_ELF_END`) with error log
- [x] Free kernel buffer after all segments loaded -- done in `exec_load_path()` (§1 adversarial fix)
- [x] Per-segment permission logging: R/W/X flags printed for each PT_LOAD segment
- [x] Malformed-ELF hardening: two-pass loader (validate all phdrs then copy) so a rejected image never mutates user frames; wrap-safe bounds, `p_filesz<=p_memsz`, `e_phnum<=64`, no PT_LOAD overlap, `e_entry` in exec segment; 9 tests
- [x] Replace byte-loop `elf_memcpy`/`elf_memzero` with kernel scalar `memcpy`/`memset` (perf: cold path but dominant per-byte work as images grow; kept scalar, no SIMD/FPU)
- [/] Commit: `"kernel: elf -- enhanced ELF loader with VMM mapping and PIE"` -- deferred with the VMM-backed core; the hardening subset shipped as `ebc2bf55`

**Test checkpoint:** Serial log shows `"elf: Loaded N PT_LOAD segments at 0x800000-0xNNNNNN, entry=0xNNNNNN"` with per-segment R/W/X flags. Segments outside user range rejected. ET_DYN accepted. BSS region zeroed.

> **Deferred:** VMM-backed per-process page mapping (`vmm_map_pages` + process_t, per-PTE R/W/X perms, `elf_exec` refactor, PT_INTERP auxv) blocked on an architectural prerequisite -- current path uses identity mapping + post-load PML4 setup in `task_exec()`. Implementable subset SHIPPED + REVIEWED: `ET_DYN`/PIE, per-segment load + R/W/X logging, user-range rejection, and full malformed/hostile-ELF hardening (two-pass atomic loader, wrap-safe bounds, `e_phnum<=64`, no PT_LOAD overlap, `e_entry` in exec segment, `PT_GNU_PROPERTY` payload cap + uint64 note-advance) with 9 rejection tests. -> XREF: `vmm_map_pages` architectural prereq tracked as the two `[/]` items above (item: "Add `vmm_map_pages()` helper" at line 135).
> **Reviewed:** 2026-07-06 | commit `ebc2bf55` | Codex 5 rounds (adversarial, consistency, perf, re-adversarial) -- 5 distinct real defects fixed (atomicity, phdr O(n^2) DoS, GNU-property CPU amplifier, GNU-property inner-loop hang, NX overclaim); round 5 all approve | build OK | exec 625 kernel + 16 user PASS | scope: kernel-code-quality

## 3. ELF Security Segments (GNU_STACK, RELRO, GNU_PROPERTY)

> [!NOTE]
> **Resolved:** `vmm_protect()` and `vmm_protect_range()` implemented in vmm.c/vmm.h. Operates on kernel PML4 (identity-mapped user range). Per-process PML4 support deferred to vmm_map_pages prerequisite.

Modern ELF binaries carry security metadata in dedicated program headers. `PT_GNU_STACK` records whether the stack should be non-executable; the loader parses it into `result.nx_stack`, but PTE-level NX enforcement is still pending (the security fields are dropped by `elf_exec_wrapper` -- see the threading item). `PT_GNU_RELRO` marks the GOT and relocation data as read-only after relocation completes, blocking GOT-overwrite attacks. `PT_GNU_PROPERTY` carries CET IBT/SHSTK feature flags that the kernel must check before enabling hardware enforcement. All three are standard on Linux; without them, ELF binaries run with weaker security than they were compiled for.

- [x] Parse `PT_GNU_STACK` (type `0x6474E551`): if `p_flags` lacks `PF_X`, set `result.nx_stack = 1`; if `PF_X` present, log warning "Executable stack requested (legacy binary)"
- [x] Implement `vmm_protect(virt, new_flags)` + `vmm_protect_range(addr, size, flags)` in vmm.c/vmm.h -- PTE flag update preserving physical address + `invlpg` flush; operates on kernel PML4
- [x] Parse `PT_GNU_RELRO` (type `0x6474E552`): record `relro_start`/`relro_size` in `elf_load_result`
  - [/] Enforce RELRO via `vmm_protect_range(relro_start, relro_size, VMM_KERNEL_RO)` -- blocked: must be called after §14 relocations complete
- [x] Parse `PT_GNU_PROPERTY` (type `0x6474E553`): full ELF note parsing with byte reads (no type-pun UB); extracts `GNU_PROPERTY_X86_FEATURE_1_AND` IBT/SHSTK flags into `result.cet_ibt`/`result.cet_shstk`
  - [x] Bound the note walk: skip payloads over `ELF_GNU_PROPERTY_MAX` (4 KiB), so a crafted 16 MiB payload times up to 64 property headers cannot amplify exec CPU
  - [/] CET enforcement -- deferred to `TODO-10-kernel-security-hardening` §9 (shadow stack), §10 (IBT)
- [/] Thread `elf_load_result` security fields through the exec dispatcher + `task_exec()` so RELRO/NX/CET is not a silent no-op -- `elf_exec_wrapper` drops them (NX log softened to "requested"; stack PTE NX still unset)
- [x] If `PT_GNU_STACK` is absent, default to NX stack (`result.nx_stack = 1`, log "No PT_GNU_STACK -- defaulting to NX stack")
- [x] Extended `elf_load_result` with `nx_stack`, `has_relro`, `cet_ibt`, `cet_shstk`, `relro_start`, `relro_size` fields
- [x] Added `PT_GNU_STACK`, `PT_GNU_RELRO`, `PT_GNU_PROPERTY`, `PT_INTERP`, `ET_DYN`, CET property constants to elf.h
- [x] Adversarial review: F01 High (unaligned note reads) fixed with byte shifts; F02 Medium (kernel PML4 only) accepted; F03 Low (fragile initializer) fixed with explicit zero + default
- [x] Commit: `"kernel: elf -- PT_GNU_STACK NX enforcement, PT_GNU_RELRO, PT_GNU_PROPERTY CET flags"` (commit title as shipped; NX/RELRO/CET are parsed metadata -- PTE/hw enforcement still pending)

**Test checkpoint:** Serial log shows `"elf: NX stack requested (enforcement pending)"` or `"elf: No PT_GNU_STACK -- defaulting to NX stack"`. RELRO range logged when present. CET flags parsed and logged. (Log says "requested" not "enforced" until the security fields are threaded through `task_exec` and stack PTE NX is set.)

> **Deferred:** PARSING shipped + REVIEWED (`PT_GNU_STACK`/`PT_GNU_RELRO`/`PT_GNU_PROPERTY` note parse, `vmm_protect`/`vmm_protect_range`, `elf_load_result` security fields, plus the hardened `PT_GNU_PROPERTY` note walk -- 4 KiB payload cap + uint64 advance, shipped in `ebc2bf55`). ENFORCEMENT deferred: RELRO write-protect needs `§14` relocations first; CET IBT/SHSTK enforcement is TODO-23; and NX/RELRO/CET are silent no-ops until the security fields are threaded through the exec dispatcher (same `vmm_map_pages`/per-process-PTE prerequisite as §2). -> XREF: RELRO enforce (item: "Enforce RELRO via `vmm_protect_range`" at line 171); CET -> `TODO-10-kernel-security-hardening` §9 (shadow stack), §10 (IBT); field threading (item: "Thread `elf_load_result` security fields" at line 174).
> **Reviewed:** 2026-07-06 | commit `ebc2bf55` | `PT_GNU_PROPERTY` hardening covered by the same Codex 5-round loop as §2 (all approve) | build OK | scope: kernel-code-quality

## 4. EIF Format Specification
Design the Executable Impossible Format -- minimal parsing, native OS metadata, syscall-ID imports.

- [x] Write `specs/eif-format.md` -- complete spec with design goals, comparison table, file layout, byte-offset tables
- [x] Define `eif_header_t` (64 bytes): magic `"EIF!"` (0x45494621), version, arch (x86_64/AArch64), flags (GUI/CONSOLE/DRIVER/SIGNED/COMPRESSED/DEBUG), api_version, entry_point, load_base, segment/import counts and offsets, signature/metadata offsets. Static assert on size.
- [x] Define `eif_segment_t` (32 bytes): vaddr, file_offset, file_size, mem_size, flags (READ/WRITE/EXEC), reserved. Static assert on size.
- [x] Define `eif_import_t` (8 bytes): syscall_id (SSDT service number), flags (OPTIONAL bit). Static assert on size. Import resolution is integer-only -- no string lookup.
- [x] Signature format spec: Ed25519 or RSA-2048-SHA256, covers `[0, signature_offset)`, verification flow documented
- [x] Metadata section spec: key-value pairs (name, version, author, icon, min_os)
- [x] C struct definitions with `__attribute__((packed))` and `_Static_assert` provided for `include/kernel/eif.h`
- [x] elf2eif conversion flow documented (section 13 prereq)
- [x] Commit: `"docs: EIF format specification"`

**Test checkpoint:** `specs/eif-format.md` exists and contains `eif_header_t`, `eif_segment_t`, `eif_import_t` definitions with byte offsets. Header totals 64 bytes. No runtime test -- spec document only.

## 5. EIF Kernel Loader

- [x] Create `src/kernel/eif.c` + `include/kernel/eif.h` -- structs from §4 spec, static asserts, loader API
- [x] Implement `eif_load(data, size)`:
  - [x] Read 64-byte header; validate magic (`EIF!`), arch (x86_64), version (1), bounds checks on segment/import tables
  - [x] Load segments: copy file data to vaddr (identity-mapped), zero BSS, validate user range, reserve PMM pages, per-segment R/W/X logging
  - [x] Validate imports: range-check each syscall_id (0x0000-0x03FF); reject required out-of-range imports (side-effect free -- no dispatch)
  - [x] Write per-process dispatch table at user-space address 0x8F0000 -- `eif_dispatch_entry_t` array with `{syscall_id, available}` pairs; two-pass validation (validate all imports first, then write atomically); segment overlap protection; max 1024 entries
  - [x] If `SIGNED` flag set but no verification: reject with warning (verification deferred to §17)
  - [x] Return `load_base + entry_point`
- [x] Performance measurement: uptime_ns() delta logged in microseconds
- [x] Registered as "EIF" format in `exec_init()` with magic `{'E','I','F','!'}`
- [x] Commit: `"kernel: eif -- EIF loader"`

**Test checkpoint:** Serial log shows `"eif: loaded in <N> µs"` where `<N>` < 10. Invalid magic rejected with error. `SIGNED` flag without signature returns error. `POST16(0xD807)` on entry, `POST16(0xD808)` after segments mapped. Test on: QEMU WHPX + TCG; bare metal.

## 6. Module List Registration (LDR_DATA_TABLE_ENTRY)

Every loaded executable and shared library must be registered in the per-process module list so that the debugger (TODO-29), crash dump generator (TODO-27), SEH unwind logic (TODO-23), and `PEB->Ldr` module walks all work. Win11 maintains three linked lists (`InLoadOrder`, `InMemoryOrder`, `InInitializationOrder`) in `PEB_LDR_DATA`. Linux maintains `struct link_map` for `dl_iterate_phdr()`. Impossible OS must populate both for dual-format compat.

- [x] Define `loaded_module_t` in `include/kernel/exec.h`: `{ base_address, size_of_image, entry_point, full_path, name, format (ELF/PE/EIF), pdata_base, pdata_size }` -- 368 bytes, static assert, spinlock-protected global registry (max 64), sorted by base_address
- [x] `exec_register_module(process_t *proc, loaded_module_t *mod)` -- inserts into per-process module list and global `LOADED_MODULE` crash registry (→ XREF TODO-27 §3) -- SMP-safe via irqsave spinlock, sorted insertion, duplicate detection
- [x] For PE32+ modules: insert `LDR_DATA_TABLE_ENTRY` into `PEB->Ldr` lists (→ XREF TODO-11 §4) -- allocates entry + UTF-16 strings, inserts into all 3 circular lists (InLoadOrder, InMemoryOrder, InInitializationOrder)
- [/] For ELF modules: insert into a process-local `link_map` chain for `dl_iterate_phdr()` compat -- registered in global crash registry; per-process link_map deferred to dynamic linker (§14) when process-private address spaces exist
- [ ] Extend module identity beyond `name`: reserve image-ID storage for EIF `build_id` metadata, PE CodeView GUID / Debug Directory identity, and ELF `.note.gnu.build-id` when available (-> XREF `TODO-20-eif-full-implementation.md §5`, `TODO-27-crash-dump-generation.md §3`)
- [ ] Register ELF `.eh_frame_hdr` unwind data (symmetric to PE `.pdata`): add fields to `loaded_module_t`, parse `PT_GNU_EH_FRAME` (`0x6474E550`) so the unwinder gets DWARF CFI -> XREF: `TODO-23-exception-dispatch-seh.md §6`
- [x] `exec_find_module_by_pc(uint64_t rip)` -- binary search module list by address range; returns module for stack traces and `.pdata` lookup -- O(log n) binary search, ISR-safe (irqsave spinlock)
- [x] Commit: `"kernel: exec -- module list registration for Ldr, crash dump, and debugger"`

**Test checkpoint:** After loading a binary, `exec_find_module_by_pc(entry_va)` returns non-NULL with correct `name` and `base_address`. Serial log shows `"exec: registered module '<name>' at 0x<base> (size=<N>)"`. `POST16(0xD809)` on entry, `POST16(0xD80A)` after module inserted. Test on: QEMU WHPX + TCG; bare metal.

## 7. PE32+ Header Parser

- [x] Create `src/kernel/pe.c` + `include/kernel/pe.h` -- PE32+ structures with static asserts at exact Windows offsets, unaligned-safe read helpers, registered as `PE32+` format in exec dispatcher (MZ 2-byte magic)
- [x] Define PE structures: `pe_dos_header_t` (64 bytes), `pe_coff_header_t` (20 bytes), `pe_optional_header64_t` (240 bytes, 16 DataDirectory entries), `pe_section_header_t` (40 bytes) -- all with offset/size static asserts
- [x] Implement `pe_validate(data, size)` returning `pe_validate_result_t` with zero-copy pointers into data buffer:
  - Check MZ magic at offset 0; e_lfanew bounds; PE signature at e_lfanew; Machine == `0x8664`; Optional Header Magic == `0x20B`; SizeOfOptionalHeader >= 240; section headers within bounds
  - Reject 32-bit PE (Magic == `0x10B`) with `ENOEXEC`; reject i386 Machine with `ENOEXEC`
- [x] `pe_load()` registered in exec dispatcher -- validates and returns 0 until section loader (§8) is implemented
- [x] 7 unit tests in `test_exec.c`: struct sizes, constants, valid PE32+, 32-bit rejection, truncated, bad magic, NULL
- [x] Commit: `"kernel: pe -- PE32+ header parser"`

**Test checkpoint:** `pe_validate()` returns success for a valid PE32+ header (Machine `0x8664`, Magic `0x20B`). Returns error for 32-bit PE (`0x10B`). Returns error for truncated file. `POST16(0xD80B)` on entry. Test on: QEMU WHPX + TCG; bare metal.

## 8. PE32+ Section Loader + `.pdata` Registration

- [x] Parse section headers; for each section: allocate physical frames via `pmm_alloc_frame()`, map at `ImageBase + VirtualAddress` via `vmm_map_page()`, copy `SizeOfRawData` bytes via identity map, zero-fill remainder. Permissions from Characteristics: `MEM_WRITE` -> `VMM_FLAG_WRITABLE`, `!MEM_EXECUTE` -> `VMM_FLAG_NX`. ImageBase validated >= 0x1000000, overflow checked.
- [x] Map PE headers (`SizeOfHeaders`) at ImageBase for runtime introspection -- allocated pages, copied via identity map, mapped read-only with User bit
- [x] Parse DataDirectory entry 3 (`IMAGE_DIRECTORY_ENTRY_EXCEPTION`): extract `.pdata` RVA and size, compute VA as `ImageBase + pdata_rva`, register with module list (§6). `RtlLookupFunctionEntry` (→ XREF TODO-23 §6) can find unwind data via `exec_find_module_by_pc()`.
- [x] Register loaded module via `exec_register_module()` (§6) with base, size, entry, and `.pdata` info -- PE loader calls this directly (task_exec detects via module list, no duplicate registration)
- [x] task_exec made format-agnostic: uses `exec_find_module_by_pc(entry)` to determine user page range instead of hardcoding ELF range. Works for PE, ELF, and EIF.
- [x] 3 unit tests: pe_load returns correct entry VA, module registered with correct base/format, rejects low ImageBase
- [x] Commit: `"kernel: pe -- PE32+ section loader with .pdata registration"`
- [ ] `NtQuerySection(..., SectionImageInformation)` for `SEC_IMAGE` sections: after `pe_load()` maps a PE, persist optional-header fields (ImageBase, AddressOfEntryPoint, SizeOfStackReserve, SizeOfStackCommit) on the backing `SECTION_OBJECT` (or a PE-side hook in `ob_section.c` / `nt_section.c`) so `ObQuerySectionObject` class 1 returns real values instead of all-zero placeholders. Validation: unit test loads a tiny PE fixture, maps as image section, queries class 1, asserts entry and stack fields match the fixture header. -> XREF: `TODO-12-native-api-ssdt.md` section 18 Accepted (SectionImageInformation deferred until PE loader exposes headers to section layer).
- [ ] Map PE images into a per-process PML4 instead of the shared `kernel_pml4`. Current `pe_load()` calls `vmm_map_page(image_base + offset, ..., flags | VMM_FLAG_USER)` against the shared kernel address space (src/kernel/pe.c:743,793); `vmm.c get_or_create_table()` OR's the User bit into every intermediate PML4/PDPT/PD entry on the way down, and `pe_rollback()` only clears leaf PTEs -- upper-table User mutations persist even on validate failure, leaving the shared kernel PML4 in a slightly more permissive state than it started. Fix: extend `task_exec()`'s per-process cr3 build-out to happen BEFORE `exec_load()` so PE sections map into `tasks[pid].cr3` (not `kernel_pml4`), OR teach `pe_rollback()` to track and undo upper-level User promotions so rollback returns the kernel PML4 to its original state. Validation: grep `src/kernel/pe.c` for `vmm_map_page(image_base` -- must target a per-process PML4; unit test that calls `pe_load()` then inspects PML4[1]/PDPT[x]/PD[x] User bits before and after a forced-validation-failure, asserts no delta in the shared kernel table. Consumer: `task_exec()` launcher-exec + forked-exec branches in `src/kernel/sched/task.c`. Raised by TODO-04 §15 Codex [M] finding.

**Test checkpoint:** Serial log shows `"pe: mapped <N> sections, .pdata at 0x<addr> (<M> entries)"`. Section permissions match `Characteristics`. BSS-only sections are zero-filled. `POST16(0xD80C)` on entry, `POST16(0xD80D)` after `.pdata` registered. Test on: QEMU WHPX + TCG; bare metal.

## 9. PE32+ Import Table Resolver (Win32 Dispatch)

This bridges PE executables to the Impossible OS Win32 API -- every `CreateFile`, `ReadFile`, and `WriteFile` call in a PE binary flows through here.

> [!IMPORTANT]
> **Soft dependency on TODO-12 §6** (Nt/Zw naming + syscall migration, now marked `[x]`). §9 is still not fully blocked by remaining API coverage: implement the import-resolution machinery and export-table structure now. Keep stub thunks that return `STATUS_NOT_IMPLEMENTED` for unmigrated SSDT entries so PE binaries load and run while missing APIs fail cleanly instead of crashing.

- [x] Parse Import Directory Table (DataDirectory entry 1): walk `IMAGE_IMPORT_DESCRIPTOR` array, extract DLL name + INT/IAT thunk pairs, resolve by name (`IMAGE_IMPORT_BY_NAME`); ordinal imports logged and stubbed. Max 64 DLLs, 4096 thunks per DLL.
- [x] Kernel-side export tables: `s_kernel32_exports[]` (8 entries: CloseHandle, CreateFileA/W, ExitProcess, GetLastError, ReadFile, SetLastError, WriteFile), `s_ntdll_exports[]` (5 entries: NtClose, NtCreateFile, NtReadFile, NtTerminateProcess, NtWriteFile) -- sorted by name, binary search lookup. New DLLs added via `s_dll_tables[]` registry.
- [x] Unknown DLL names: imports stubbed with NULL thunk (faults on call); unknown functions within known DLLs also stubbed. No crash on unrecognized DLL.
- [x] Export table lookup: sorted `pe_export_entry_t` array + binary search (`pe_lookup_export`). Case-insensitive DLL name matching (`pe_stricmp`).
- [x] Import structures in pe.h: `pe_import_descriptor_t` (20 bytes), `pe_import_by_name_t`, `PE_ORDINAL_FLAG64`, `pe_export_entry_t`, `pe_dll_exports_t`
- [x] Commit: `"kernel: pe -- PE32+ import table resolver"`

**Test checkpoint:** Serial log shows `"pe: resolved <N> imports from <DLL>"` for each DLL. `kernel32.dll!ExitProcess` resolves to a valid SSDT thunk. Unknown DLL produces warning log, not crash. `POST16(0xD80E)` on entry, `POST16(0xD80F)` after IAT patched. Test on: QEMU WHPX + TCG; bare metal.

## 10. PE32+ Base Relocation

- [ ] Parse `.reloc` section (DataDirectory entry 5):
  - Walk base relocation blocks (page RVA + array of `type | offset` entries)
  - `IMAGE_REL_BASED_DIR64` (10): add `delta = actual_base - ImageBase` to 64-bit value at offset
  - `IMAGE_REL_BASED_ABSOLUTE` (0): skip (padding)
- [ ] Apply delta to all type-10 entries after section loading
- [ ] Commit: `"kernel: pe -- PE32+ base relocation"`

**Test checkpoint:** Serial log shows `"pe: relocated <N> entries, delta=0x<delta>"`. A PE loaded at non-preferred base calls a function pointer without crash. `POST16(0xD810)` on entry, `POST16(0xD811)` after relocations applied. Test on: QEMU WHPX + TCG; bare metal.

## 11. PE32+ TLS Directory Processing

PE binaries using `__declspec(thread)` or C11 `_Thread_local` store TLS templates and callbacks in the TLS directory (DataDirectory entry 9). The loader must allocate per-thread TLS data from the template, assign a TLS index, and invoke TLS callbacks (`DLL_PROCESS_ATTACH`) before the entry point runs. Without this, any Win32 binary using thread-local storage will crash on first TLS access.

- [ ] Parse `IMAGE_TLS_DIRECTORY64` from DataDirectory[9]: `StartAddressOfRawData`, `EndAddressOfRawData`, `AddressOfIndex`, `AddressOfCallBacks`, `SizeOfZeroFill`, `Characteristics`
- [ ] Allocate per-thread TLS block: copy raw data range `[Start..End)` to a fresh allocation; append `SizeOfZeroFill` zero bytes; store pointer in TEB `TlsSlots[assigned_index]`
- [ ] Write assigned TLS index to `AddressOfIndex` in the mapped image
- [ ] Invoke TLS callbacks (if `AddressOfCallBacks` is non-NULL): walk null-terminated function pointer array; call each with `(hModule, DLL_PROCESS_ATTACH, NULL)` -- callbacks run in ring 3 before `main()`
- [ ] On thread creation: allocate fresh TLS block from template; call callbacks with `DLL_THREAD_ATTACH`
- [ ] Commit: `"kernel: pe -- TLS directory processing and callbacks"`

**Test checkpoint:** Serial log shows `"pe: TLS index=<N>, raw data <size> bytes, <M> callbacks"`. TLS callback log: `"pe: TLS callback DLL_PROCESS_ATTACH at 0x<addr>"`. Thread-local variable read returns initialized value. `POST16(0xD812)` on entry, `POST16(0xD813)` after callbacks invoked. Test on: QEMU WHPX + TCG; bare metal.

## 12. PE32+ Load Config and CFG Bitmap

Modern PE binaries carry an `IMAGE_LOAD_CONFIG_DIRECTORY64` (DataDirectory entry 10) containing security metadata: Control Flow Guard (CFG) function table, CET shadow stack compatibility flags, and security cookie location. The kernel must parse this to enable hardware-assisted control flow integrity.

> [!NOTE]
> → XREF: `TODO-10-kernel-security-hardening.md §9,§10` -- CET shadow stack (§9) and CET IBT (§10) enforcement is authoritative in TODO-23. This section parses the PE metadata and stores it in the process/module record; TODO-23 acts on it. CFG bitmap enforcement also requires a dedicated CFG section in TODO-23.

- [ ] Parse `IMAGE_LOAD_CONFIG_DIRECTORY64` from DataDirectory[10]: extract `GuardCFFunctionTable`, `GuardCFFunctionCount`, `GuardFlags`
- [ ] If `IMAGE_GUARD_CF_INSTRUMENTED` and `IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT`: allocate a per-process CFG bitmap; populate valid call targets from the GFIDS table; store in process metadata
- [ ] Extract `SecurityCookie` address and initialise with `RDRAND` value (overwrite the linker default)
- [ ] Parse CET fields: `GuardAddressTakenIatEntryTable`, `GuardEHContinuationTable` -- store for TODO-23 CET activation
- [ ] If Load Config is absent or has zero size: treat as legacy binary (no CFG, no CET) -- log warning
- [ ] Commit: `"kernel: pe -- Load Config directory, CFG bitmap, CET metadata"`

**Test checkpoint:** Serial log shows `"pe: CFG bitmap: <N> valid targets"` for CFG-instrumented PE. Shows `"pe: security cookie initialized"`. Legacy PE without Load Config shows `"pe: no Load Config -- legacy binary"`. `POST16(0xD814)` on entry, `POST16(0xD815)` after CFG bitmap populated. Test on: QEMU WHPX + TCG; bare metal.

## 13. `elf2eif` Host-Side Converter

Standard developer workflow: `clang-19 → ld.lld → elf2eif` -- no custom compiler required.

> [!NOTE]
> This section owns producer-side emission for `TODO-20-eif-full-implementation.md` §5-§8: metadata, `api_version`, PIC `load_base=0`, compressed segment encoding, and the final optional-import call ABI.

- [ ] Create `tools/elf2eif.c` (compiled with host `gcc`)
- [ ] Add `tools/syscall_map.h` (or generated header) mapping Win32 symbol names to SSDT service numbers sourced from `include/kernel/nt/service_numbers.h`
- [ ] Read input ELF64; validate headers; extract `PT_LOAD` segments → EIF `eif_segment_t` entries; derive PIC vs fixed-base output (`load_base=0` for PIC EIF) and preserve caller-selected `api_version`
- [ ] Generate import table:
  - Scan ELF `.dynsym` or custom `.eif_imports` section for Win32 API symbols
  - Map function names → SSDT service numbers via `syscall_map.h`
  - Emit the chosen optional-import call ABI expected by `TODO-20` §7 (data-only dispatch entries plus deterministic user thunks unless the EIF spec is widened first)
- [ ] Write EIF output: header + segments + import table + metadata section; populate `metadata_offset`, optional `build_id`, and compressed `file_size` / `EIF_FLAG_COMPRESSED` fields when enabled
- [ ] Add CLI knobs for advanced EIF features: `--api-version`, `--pic`, `--lz4`, and metadata key/value input consumed by the header + metadata writer
- [ ] Optional: embed digital signature via `tools/eifsign`
- [ ] Add `make elf2eif` target; integrate auto-conversion into user-mode build rules
- [ ] Commit: `"tools: elf2eif converter"`

**Test checkpoint:** `make elf2eif` builds successfully. `tools/elf2eif hello.elf hello.eif` produces output with `EIF!` magic at offset 0. CLI options can emit metadata, `api_version`, PIC `load_base=0`, and compressed segments with headers matching the spec. Kernel loads the resulting EIF and reaches entry point. No runtime POST codes -- host-side tool.

## 14. ELF Dynamic Linker

Shared library support for the Linux compatibility layer and future ELF apps.

> [!NOTE]
> A dedicated ELF shared library TODO has not yet been filed. This section implements only the minimum kernel-side pieces needed for the Linux compatibility layer. File a new TODO under `02-kernel-core` if dynamic linking scope expands beyond the items listed here. Advanced semantics (IFUNC/IRELATIVE, `DT_INIT_ARRAY`/`DT_FINI_ARRAY`, symbol versioning, library search order, `DT_FLAGS`/`DT_FLAGS_1`) are owned by §20; ELF TLS ABI (TCB/DTV/FS-base) by §18.

- [ ] Parse `PT_DYNAMIC` segment for `DT_NEEDED`, `DT_STRTAB`, `DT_SYMTAB`, `DT_HASH`/`DT_GNU_HASH`
- [ ] Load `.so` files from `C:\Impossible\System\lib\` via VFS
- [ ] Symbol resolution: `DT_GNU_HASH` lookup → `DT_SYMTAB` match
- [ ] Apply relocations: `R_X86_64_JUMP_SLOT` (PLT), `R_X86_64_GLOB_DAT` (GOT), `R_X86_64_RELATIVE`
- [ ] Lazy PLT binding: stubs resolve on first call
- [ ] After relocations complete: apply `PT_GNU_RELRO` protection (§3) to make GOT read-only
- [ ] Register each loaded `.so` via `exec_register_module()` (§6)
- [ ] Commit: `"kernel: elf -- dynamic linker and shared library loading"`

**Test checkpoint:** Serial log shows `"elf: loaded shared library '<name>.so' at 0x<addr>"` for each `DT_NEEDED`. PLT stub call resolves on first invocation (lazy binding). `PT_GNU_RELRO` applied after relocations -- GOT write attempt faults. `POST16(0xD816)` on entry, `POST16(0xD817)` after all `.so` loaded. Test on: QEMU WHPX + TCG; bare metal.

## 15. ASLR -- Address Space Layout Randomization

- [ ] Random base for PIE ELF (`ET_DYN`): choose base within user range; add to all `PT_LOAD` vaddrs
- [ ] Random base for EIF PIC images (`load_base=0`): choose a randomized base within user range; keep non-PIC EIF at its preferred base until relocations are specified (-> XREF `TODO-20-eif-full-implementation.md §8`)
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

**Test checkpoint:** Script with `#!/C:\Impossible\System\shell.exe` dispatches to shell -- serial log shows `"exec: shebang -> /C:\Impossible\System\shell.exe"`. Circular shebang rejected with error. Missing interpreter → error. `POST16(0xD81A)` on entry, `POST16(0xD81B)` after interpreter resolved. Test on: QEMU WHPX + TCG; bare metal.

## 17. EIF Code Signing

EIF binaries with the `SIGNED` flag must pass signature verification before any segment is mapped.

> [!IMPORTANT]
> → XREF: `TODO-03-kernel-libraries.md §3` -- Monocypher provides `crypto_eddsa_check()` (Ed25519) and `crypto_blake2b()` (SHA-256 substitute); §3 must be integrated before this section can be implemented.

- [ ] Read signature from `signature_offset` in EIF header
- [ ] Compute SHA-256 over header + all segment data in file order
- [ ] Verify signature against OS-embedded trusted public key
- [ ] Policy: `SIGNED` flag → reject on invalid/missing signature; unsigned binaries run with reduced capabilities (no raw disk I/O, no driver-level access)
- [ ] Create `tools/eifsign.c` -- host-side signing tool
- [ ] Commit: `"kernel: eif -- EIF code signing"`

**Test checkpoint:** EIF with valid signature loads successfully -- serial log shows `"eif: signature verified"`. EIF with `SIGNED` flag but missing/invalid signature rejected -- serial log shows `"eif: signature verification FAILED"`. Unsigned EIF (no `SIGNED` flag) loads but with reduced capabilities logged. `POST16(0xD81C)` on entry, `POST16(0xD81D)` after signature check. Test on: QEMU WHPX + TCG; bare metal.

## 18. ELF `PT_TLS` Template Loading

Linux ELF binaries using `__thread` rely on `PT_TLS` template mapping and per-thread TLS block initialization. Without this, thread-local variables in ELF user-mode binaries are incorrect or crash on access.

- [ ] In `include/kernel/elf.h`: add `PT_TLS` support constants and `elf_tls_template_t` metadata in `elf_load_result` (template start/filesz/memsz/align)
- [ ] In `src/kernel/elf.c` (`elf_exec`/`elf_load` path): parse `PT_TLS`, capture template metadata, and map initial TLS image for the main thread
- [ ] In thread creation path (`src/kernel/sched/task.c`): allocate a fresh TLS block for each new thread from the captured `PT_TLS` template; copy initialized bytes then zero-fill tail
- [ ] Wire TLS setup into exec/thread startup sequence before user entry so ELF code sees valid TLS at first instruction
- [ ] Build the x86-64 TLS ABI runtime: allocate per-thread TCB with self-pointer at offset 0, set FS base to it (via `arch_prctl`/MSR), and lay the static TLS block at negative offsets per the variant-II model
- [ ] Allocate a DTV (dynamic thread vector) keyed by module TLS ID; assign the exec module ID 1 so dynamic shared objects (§14/§20) can add their own TLS blocks
- [ ] Apply ELF TLS relocations `R_X86_64_DTPMOD64`/`R_X86_64_DTPOFF64`/`R_X86_64_TPOFF64` for local-exec/initial-exec/general-dynamic access models
- [ ] Add unit tests in `src/kernel/test/test_exec.c`: static `__thread` variable init value, per-thread isolation, and zero-filled TLS tail behavior
- [ ] Commit: `"kernel: elf -- PT_TLS template loading and per-thread TLS setup"`

**Test checkpoint:** ELF binary with `__thread int g_tls = 7;` reads `7` in the main thread and independent values in two spawned threads. Serial log shows `"elf: PT_TLS template filesz=<N> memsz=<M>"`. `POST16(0xD81E)` on entry, `POST16(0xD81F)` after first TLS block init. Test on: QEMU WHPX + TCG; bare metal.

## 19. PE API-Set + Delay-Load/Bound Import Support

Modern Windows binaries frequently import `api-ms-win-*` / `ext-ms-*` contract DLLs and may use delay-load import tables. The loader must resolve these correctly for broad Win11 binary compatibility.

- [ ] In `include/kernel/pe.h` + `src/kernel/pe.c`: add API-set namespace structs/parsing helpers and delay-load directory structs (`IMAGE_DELAYLOAD_DESCRIPTOR`, delay IAT/BIAT metadata)
- [ ] In PE import resolver path: detect API-set contract DLL names and map them to concrete host DLLs before export lookup (`api-ms-win-*`, `ext-ms-*`)
- [ ] Parse Delay-Load Import Directory (DataDirectory 13): resolve thunks on first call path and support delay-unload/delay-bound metadata
- [ ] Parse Bound Import metadata (DataDirectory 11): validate timestamps; on mismatch/failure, fall back to normal import resolution instead of crashing
- [ ] Resolve PE export forwarder chains: an export RVA inside the export directory is a `DLL.Function`/`DLL.#Ordinal` forwarder; recursively resolve to the real target (bounded depth, reject cycles) -- needed for `kernel32`->`ntdll` layering
- [ ] Add unit tests in `src/kernel/test/test_exec.c`: API-set contract name resolution, delay-load first-call patching, and bound-import mismatch fallback path
- [ ] Commit: `"kernel: pe -- API-set contract resolution and delay-load/bound imports"`

**Test checkpoint:** A PE importing `api-ms-win-core-processthreads-l1-1-0.dll!ExitProcess` resolves through the API-set map to a concrete export and executes. A sample delay-load import resolves on first call and logs `"pe: delay import resolved <dll>!<name>"`. Bound-import timestamp mismatch logs fallback and continues. `POST16(0xD820)` on entry, `POST16(0xD821)` after delay-IAT patching. Test on: QEMU WHPX + TCG; bare metal.

---

## 20. ELF Dynamic Linker -- Advanced (IFUNC, Init/Fini, Versioning, Search Order)

§14 lands the core dynamic linker (PT_DYNAMIC, DT_NEEDED, hash lookup, JUMP_SLOT/GLOB_DAT/RELATIVE relocations, lazy PLT, RELRO). Real glibc-linked shared objects also depend on the semantics below; without them the loader skips C++ constructors, jumps to unresolved IFUNC slots, binds the wrong symbol ABI version, or searches the wrong library path. This section is split out of §14 to keep each section within the checklist-size cap.

> [!NOTE]
> §14 owns the exec-time ELF dynamic linker; there is no separate ELF-shared-library TODO. dlopen/dlsym runtime-load API and per-process module unload are process-model concerns -> XREF: `TODO-21-process-model-extensions.md §14` (process-exit destructor + module deregistration).

- [ ] GNU IFUNC resolution: after RELATIVE/GLOB_DAT relocations, walk `R_X86_64_IRELATIVE` entries, call each resolver, write the result into the slot (IRELATIVE strictly last -- resolvers read already-relocated data)
- [ ] Constructor/destructor: call `DT_PREINIT_ARRAY` (main exe only) then `DT_INIT`/`DT_INIT_ARRAY` in dependency order after relocations; register `DT_FINI`/`DT_FINI_ARRAY` for teardown (-> XREF `TODO-21 §14`)
- [ ] ELF symbol versioning: parse `DT_VERSYM`/`DT_VERNEED`/`DT_VERDEF` and bind each undefined symbol to the requested version so glibc-versioned symbols resolve to the correct ABI rather than the first match
- [ ] Library search order: honour `DT_RUNPATH`/`DT_RPATH`, an `LD_LIBRARY_PATH`-equivalent, then the default system lib dir; filter environment paths in secure mode so untrusted paths cannot inject libraries
- [ ] `DT_FLAGS`/`DT_FLAGS_1` policy: honour `DF_BIND_NOW`/`DF_1_NOW` (eager binding, disable lazy PLT), `DF_1_NODELETE` (pin module across unload), and `DF_1_PIE`
- [ ] Add unit tests in `src/kernel/test/test_exec.c`: an IRELATIVE slot points at the resolver's returned address; an init-array constructor runs before entry; a versioned symbol binds to the requested version; BIND_NOW disables lazy binding
- [ ] Commit: `"kernel: elf -- IFUNC, init/fini arrays, symbol versioning, search order, DT_FLAGS"`

**Test checkpoint:** Serial log shows `"elf: called <N> init-array constructors"` and `"elf: resolved <N> IFUNC relocations"`. A binary with a versioned undefined symbol binds to the correct `DT_VERNEED` entry. `DF_BIND_NOW` binary resolves all PLT slots at load (no lazy stub). Test on: QEMU WHPX + TCG; bare metal.

---

## OS Comparison

| ⭐   | Feature                     | 🪟 Win11                  | 🐧 Linux            | 🚀 Impossible OS                 |
| --- | --------------------------- | ------------------------ | ------------------ | ------------------------------- |
| 💎   | Native binary format        | ✅ PE32+                  | ✅ ELF              | 🔄 §4 spec + §5 loader done      |
| 💎   | ELF loading                 | ⚠️ WSL only              | ✅ Native           | ⚠️ §2 basic loader              |
| 💎   | PE32+ loading               | ✅ Native                 | ⚠️ Wine only       | 🔄 §7-§8 done; §9-§10 pending    |
| 💎   | Dynamic linking             | ✅ DLL loading            | ✅ ld.so            | ⬜ §14                           |
| 💎   | ASLR                        | ✅ Mandatory              | ✅ PIE + kernel     | ⬜ §15                           |
| 💎   | NX stack                    | ✅ DEP default            | ✅ PT_GNU_STACK     | ⬜ §3                            |
| 💎   | RELRO (read-only GOT)       | ⚠️ N/A (PE IAT)          | ✅ PT_GNU_RELRO     | ⬜ §3                            |
| 💎   | CET/IBT binary flags        | ✅ Load Config            | ✅ GNU_PROPERTY     | ⬜ §3 ELF, §12 PE                |
| 💎   | Module list                 | ✅ PEB->Ldr               | ✅ link_map         | 🔄 §6 registry + Ldr done        |
| 💎   | PE .pdata unwind            | ✅ Kernel + ntdll         | ❌ N/A              | 🔄 §8 .pdata registered          |
| 💎   | PE TLS + callbacks          | ✅ Full support           | ❌ N/A (ELF PT_TLS) | ⬜ §11                           |
| 💎   | Native TLS templates        | ✅ PE TLS                 | ✅ PT_TLS           | ⬜ §11 + §18                     |
| 💎   | PE Load Config / CFG        | ✅ CFG mandatory          | ❌ N/A              | ⬜ §12                           |
| 💎   | Contract import mapping     | ✅ API-set                | ⚠️ SONAME aliases  | ⬜ §19                           |
| 💎   | ELF symbol versioning       | ❌ N/A (PE)               | ✅ DT_VERNEED       | ⬜ §20                           |
| 💎   | GNU IFUNC dispatch          | ❌ N/A                    | ✅ IRELATIVE        | ⬜ §20                           |
| 💎   | Init/fini arrays            | ⚠️ CRT thunk             | ✅ init_array       | ⬜ §20                           |
| 💎   | ELF .eh_frame unwind        | ❌ N/A (PE .pdata)        | ✅ PT_GNU_EH_FRAME  | ⬜ §6                            |
| 💎   | PE export forwarders        | ✅ DLL.Func chains        | ❌ N/A              | ⬜ §19                           |
| ⭐   | Triple format support       | ❌ PE32+ only             | ❌ ELF only         | ⬜ §1–§10 all three              |
| ⭐   | < 10 us load time           | ❌ ~50 us                 | ❌ ~30 us           | 🔄 §5 uptime_ns measured         |
| ⭐   | Syscall-ID imports          | ❌ String-based           | ❌ String-based     | ✅ §5 dispatch table at 0x8F0000 |
| ⭐   | Standard toolchain → native | ❌ Needs PE linker        | ⚠️ ELF only        | ⬜ §13 elf2eif                   |
| ⭐   | Shebang dispatch            | ❌ File extension         | ⚠️ Separate module | ⬜ §16 unified                   |
| ⭐   | Mandatory code signing      | ⚠️ Optional Authenticode | ❌ None built-in    | ⬜ §17 mandatory EIF             |

> **After §1–§6:** Impossible OS has a triple-format dispatcher, module list infrastructure, and the world's fastest native loader (EIF).
> **After §7–§12:** Full kernel-level PE32+ loading with TLS, CFG, and .pdata unwind tables -- run Windows-compiled executables natively without a compatibility layer.
> **§3** brings ELF security to Linux parity: NX stack, RELRO, and CET property flags.
> **§18** closes Linux parity for ELF thread-local storage (`PT_TLS`) in multi-threaded user binaries.
> **§19** closes modern Win11 import compatibility by handling API-set contract DLL names and delay-load/bound import metadata.
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
  - PE API-set contract import (`api-ms-win-*`) resolves to concrete host DLL before export lookup (§19)
  - PE delay-load import thunk patches correctly on first call; bound-import mismatch falls back safely (§19)
  - PE32+ TLS: `IMAGE_TLS_DIRECTORY64` parsed; TLS index written; initial TLS data copied (§11)
  - PE32+ Load Config: `IMAGE_LOAD_CONFIG_DIRECTORY64` parsed; CFG bitmap populated for CFG-instrumented PE (§12)
  - ELF `PT_TLS`: template copied for main thread; per-thread TLS blocks are isolated (§18)
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
- [ ] ELF with `PT_GNU_STACK` (no PF_X): serial log shows `"elf: NX stack requested (enforcement pending)"`; execute-from-stack fault requires the security fields threaded + stack PTE NX (blocked, same open item as §3)
- [ ] ELF with `PT_GNU_RELRO`: GOT pages marked read-only after relocation; write attempt faults
- [ ] `exec_load("hello.eif", ...)` loads and enters; serial log shows < 10 µs load time
- [ ] `exec_load("hello.exe", ...)` (PE32+) loads sections; IAT entries point to kernel Win32 stubs
- [ ] PE32+ import from `kernel32.dll!ExitProcess` resolves to the correct `NtTerminateProcess` thunk
- [ ] PE32+ with non-preferred `ImageBase`: base relocations applied correctly; no GPF on first call
- [ ] PE32+ `.pdata` registered: `RtlLookupFunctionEntry(entry_va)` returns non-NULL `RUNTIME_FUNCTION`
- [ ] PE32+ TLS: thread-local variable access succeeds; TLS callback logged before `main()`
- [ ] PE32+ Load Config: CFG bitmap populated; serial log shows `"pe: CFG bitmap: <N> valid targets"`
- [ ] PE API-set contract import (`api-ms-win-*` / `ext-ms-*`) resolves to concrete DLL and target export
- [ ] PE delay-load import thunk resolves on first call; bound-import timestamp mismatch falls back to normal import path
- [ ] ELF with `PT_TLS`: main-thread TLS template initialized and two threads observe isolated `__thread` values
- [ ] Module list: `exec_find_module_by_pc(entry_va)` returns correct module for both ELF and PE
- [ ] PIE ELF loaded at two different addresses in two processes; no virtual address collision
- [ ] EIF with `SIGNED` flag: reject with `ENOEXEC` if signature is missing or invalid
- [ ] `elf2eif hello.elf hello.eif` produces a valid EIF that the kernel loads correctly
- [ ] Script with `#!/C:\Impossible\System\shell.exe` shebang: `exec_load()` runs interpreter with script as argument
- [ ] Commit: `"kernel: exec -- binary format system complete"`
