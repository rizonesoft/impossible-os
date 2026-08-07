---
schema_version: 1
id: win32-pe-loader
domain: 10-platform-services
status: active
title: "TODO-07 -- Native Win32 Execution & PE Loader"
---

# TODO-07 -- Native Win32 Execution & PE Loader

> **Goal:** Impossible OS is natively Win32 -- PE32+ is the native binary format and Win32 is the native API. This TODO fixes ring-3 execution (current ABI is `INT 0x80` / Linux-style, no STAR/LSTAR setup), migrates to Windows x64 syscall ABI (`SYSCALL` + RCX/RDX/R8/R9), and delivers the full PE loader stack that makes Win32 user-mode programs work natively.

> [!IMPORTANT]
> **Already exists**: GDT user segments `GDT_USER_CODE=0x18`, `GDT_USER_DATA=0x20` defined; `gdt_set_tss_rsp0()` exists. `task_create_user()` + `user_stack_base` in `struct task`. `task_exec()` in `task.h`. `elf_load(data, size)` + `struct elf_load_result` in `elf.h`. `vfs_open/read/close()`. `user/lib/crt0.asm` (ELF `_start` + `INT 0x80`), `user/include/syscall.h` (Linux-style `RDI/RSI/RDX` ABI). **Current ABI is `INT 0x80`** -- no `MSR_STAR/LSTAR/SFMASK` setup, no `syscall_entry` handler for `SYSCALL` instruction. **GDT note**: current selectors 0x18/0x20; STAR MSR encoding for SYSRET requires: `STAR[63:48]` = `GDT_USER_CODE - 16` so SYSRET can add 16 to get user CS; existing GDT layout must be verified/adjusted. **Existing kernel syscall numbers** (`SYS_EXIT=3`, `SYS_GETPROCS=10`, `SYS_KILL=11`, etc.) are stable -- Windows x64 ABI just changes the calling convention, not these numbers. **No PE structs** anywhere in codebase.

## Inputs

- `include/kernel/gdt.h` -- `GDT_USER_CODE=0x18`, `GDT_USER_DATA=0x20`, `GDT_TSS_SEG=0x28`; `gdt_set_tss_rsp0()` -- §1 STAR MSR encoding; GDT layout verification
- `include/kernel/sched/task.h` -- `task_create_user()`, `user_stack_base`, `kernel_rsp`, `task_exec()` -- §7 PE execution process setup
- `include/kernel/elf.h` -- `elf_load(data, size)`, `struct elf_load_result` -- §8 format precedence; PE tried first, ELF fallback
- `include/kernel/sched/syscall.h` -- existing `SYS_*` constants -- §2 ABI extension; new Win32-named constants added alongside
- `include/kernel/fs/vfs.h` -- `vfs_open/read/stat()` -- §7 PE file load from VFS
- `include/kernel/mm/vmm.h` -- `vmm_map_user()`, `vmm_alloc_user()` -- §4 section mapping at `ImageBase`
- `user/lib/crt0.asm` -- existing ELF `_start` (replace for PE; keep ELF version as `crt0_elf.asm`) -- §9
- `user/include/syscall.h` -- existing Linux-style ABI (extend with Windows x64 ABI wrapper macros) -- §2, §9
- → XREF: `02-kernel-core/TODO-15-security-reference-monitor.md` -- privilege model; §1 SYSRET ring transition is the security boundary
- → XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md` -- SMEP/SMAP; §1 ring-3 execution must respect kernel page protections
- → XREF: `02-kernel-core/TODO-17-binary-system.md` -- scope overlap: PE header structs, section loader, import resolver (including API-set + delay-load/bound import handling), and base relocation are authoritative in TODO-08 (kernel-level binary format infrastructure). This TODO focuses on Win32 subsystem integration: SYSCALL/SYSRET ABI, user CRT, and `pe_exec()` process launch. §1–§6 here should consume the kernel PE loader from TODO-08 §7–§10,§19 rather than re-implementing.
- → XREF: `10-platform-services/TODO-08` (next) -- Win32 API stubs (CreateFile/ReadFile/CreateProcess/CreateWindow); §6 IAT resolution depends on those stubs being present

## Outcome

- STAR/LSTAR/SFMASK MSRs set; `syscall_entry` saves/restores all registers; `SWAPGS` kernel ↔ user GS; ring-3 test stub works.
- `include/kernel/sched/abi.h`: Windows x64 syscall numbers + calling convention.
- `include/pe.h`: all PE32+ structs (DOS header, COFF, optional header, section, data dir, import/reloc descriptors).
- `pe_load(data, size)`: validate → map sections → return entry VA.
- `pe_apply_relocations()`: walk `BASE_RELOCATION` blocks, apply `DIR64` delta.
- `pe_resolve_imports()`: walk IAT, lookup by name/ordinal in built-in DLL stub tables.
- `pe_exec(path)`: VFS load → PE load → TEB/PEB setup → ring-3 launch at entry point.
- `exec_load()` tries PE first, then ELF; file associations use correct loader.
- `user/lib/crt0_pe.asm`: PE `_start` (Windows `__start` convention) + `ExitProcess`; `kernel32_stub.c`: `CreateFile/ReadFile/WriteFile/ExitProcess` thin wrappers.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                            | Depends On                                                                                 | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------ | :----: |
| ⭐  |   1   | §1 Fix ring-3 -- STAR/LSTAR/SFMASK MSRs, `syscall_entry` asm, `SWAPGS`, minimal ring-3 test           | `GDT_USER_CODE/DATA` (exists); `gdt_set_tss_rsp0()` (exists); QEMU `-d int` debug        |  [ ]   |
| 💎  |   2   | §2 Win x64 ABI -- `include/kernel/sched/abi.h`; stable `SYS_*` naming; RCX/RDX/R8/R9 dispatch       | §1 SYSCALL handler (arguments land in correct registers post-SYSCALL)                     |  [ ]   |
| 💎  |   3   | §3 PE header structs -- `include/pe.h`; DOS/COFF/optional/section/data-dir/import/reloc structs        | PE spec; no runtime deps                                                                   |  [ ]   |
| 💎  |   4   | §4 PE loader core -- `pe_load(data, size)`: validate → map sections; return entry VA                  | §3; `vmm_alloc_user()`; PMM                                                                |  [ ]   |
| 💎  |   5   | §5 Base relocations -- `pe_apply_relocations()`: walk `BASE_RELOCATION`, apply `DIR64` delta           | §4                                                                                         |  [ ]   |
| 💎  |   6   | §6 Import resolution -- `pe_resolve_imports()`: IAT walk, name/ordinal lookup, DLL stub tables         | §5; built-in DLL stub tables (`kernel32.dll`, `user32.dll`, `gdi32.dll`, `ntdll.dll`)    |  [ ]   |
| ⭐  |   7   | §7 PE execution -- `pe_exec(path)`: TEB/PEB setup, ring-3 switch at PE entry point                    | §4+§5+§6; `task_create_user()` (exists); VFS file load                                   |  [ ]   |
| 💎  |   8   | §8 Format precedence -- `exec_load()` tries PE first, ELF fallback; file assoc routing                 | §7; `elf_load()` (exists); `task_exec()`                                                  |  [ ]   |
| 💎  |   9   | §9 User CRT -- `crt0_pe.asm` Windows `_start`; `kernel32_stub.c` thin wrappers; `libcrt.a`            | §2 ABI; §7 working PE execution; `user/lib/` (exists)                                    |  [ ]   |

---

## 1. Fix Ring-3 User-Mode Execution `[Opus]`

Set `MSR_STAR` (SYSCALL/SYSRET CS selectors), `MSR_LSTAR` (`syscall_entry` address), `MSR_SFMASK` (clear IF on entry). Write `syscall_entry` in NASM: `SWAPGS`, save all registers (including R12-R15, RBX, RBP), dispatch by RAX, restore + `SYSRETQ`. Minimal ring-3 test: `SYSRET` to stub calling `SYS_EXIT(0)`.

**Files:** `src/kernel/syscall_entry.asm` (new), extend `src/kernel/sched/syscall.c`

> [!NOTE]
> **GDT layout for STAR**: STAR MSR `[47:32]` = kernel CS (used on `SYSCALL`); STAR `[63:48]` = user CS - 16 (SYSRET sets `CS = STAR[63:48] + 16`, `SS = STAR[63:48] + 8`). Current: `GDT_USER_CODE=0x18` (index 3, RPL=0). For SYSRET to work: need user CS with RPL=3 = `0x1B`; user SS with RPL=3 = `0x23`. STAR `[63:48]` must be `0x1B - 0x10 = 0x0B` which is odd -- not a valid selector. **Fix**: need to reorder GDT so that user code and data selectors satisfy: `user_ss = user_cs + 8` (SYSRET constraint). Common layout: `[0]=null`, `[1]=kernel_code(0x08)`, `[2]=kernel_data(0x10)`, `[3]=user_data(0x18, RPL=3→0x1B)`, `[4]=user_code(0x20, RPL=3→0x23)`, `[5..6]=TSS(0x28)`. With STAR `[63:48]=0x13`: SYSRET sets `CS=0x13+0x10=0x23` (user code ✅), `SS=0x13+0x08=0x1B` (user data ✅). **Kernel CS/SS** for SYSCALL: STAR `[47:32]=0x08` (kernel code segment). **MSR setup** (in `syscall_init()`, called from kernel init): `wrmsr(MSR_STAR, ((uint64_t)0x13 << 48) | ((uint64_t)0x08 << 32))` + `wrmsr(MSR_LSTAR, (uint64_t)syscall_entry)` + `wrmsr(MSR_SFMASK, 0x200)` (clear IF). Enable SCE in EFER: `wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1)`. **`syscall_entry` (NASM)**: `swapgs`; save RSP to per-CPU `cpu_data.user_rsp`; load kernel RSP from `cpu_data.kernel_rsp`; push all callee-saved (RBX, RBP, R12–R15) + RCX (user RIP), R11 (user RFLAGS); call `syscall_dispatch(rax, rcx, rdx, r8, r9, r10)`; restore; `swapgs`; `sysretq`. **Minimal test**: compile `user/test_ring3.asm` (NASM): `mov rax, SYS_EXIT; mov rcx, 0; syscall`; `pe_exec()` (or direct map); verify via serial output `klog("ring3: returned from SYS_EXIT")`.

- [ ] Verify/reorder GDT so `user_code+8 = user_data` for SYSRET constraint; update `gdt.h` constants
- [ ] `syscall_init()` in `src/kernel/sched/syscall.c`: `wrmsr(MSR_EFER, ... | SCE_BIT)`, `wrmsr(MSR_STAR, ...)`, `wrmsr(MSR_LSTAR, syscall_entry)`, `wrmsr(MSR_SFMASK, 0x200)`
- [ ] `src/kernel/syscall_entry.asm`: `swapgs`; per-CPU kernel RSP load; push callee-saved; dispatch; restore; `swapgs`; `sysretq`
- [ ] `syscall_dispatch(rax, rcx, rdx, r8, r9, r10)` in C: switch on `rax` → existing syscall handlers; note: `rcx` = user RIP (clobbered by SYSCALL), `r11` = user RFLAGS
- [ ] QEMU `-d int,cpu_reset` + `klog(LOG_INFO, "ring3", "syscall: rax=%llu")` for debugging
- [ ] Minimal ring-3 test: map stub at user VA; `SYSRET` to it; stub calls `syscall` with `SYS_EXIT=3`; kernel receives + logs "ring3 test OK"
- [ ] Update existing `INT 0x80` handler to forward to same `syscall_dispatch()` (keep for ELF compat during transition)
- [ ] Commit: `"kernel: ring3 fix -- STAR/LSTAR/SFMASK MSRs, syscall_entry SWAPGS, per-CPU RSP, ring-3 test OK"`

## 2. Windows x64 Syscall ABI `[Sonnet]`

`include/kernel/sched/abi.h`: Win32-named `SYS_*` constants mapping to existing numeric IDs. Windows x64 calling convention: args in RCX, RDX, R8, R9 (then stack), syscall number in RAX. New file I/O, process, memory, and UI syscall numbers.

**Files:** `include/kernel/sched/abi.h` (new), extend `include/kernel/sched/syscall.h`

> [!NOTE]
> `abi.h` maps Win32 names to the existing stable kernel numbers AND adds new ones: `SYS_CREATEFILE=60` (→ open/create file via VFS), `SYS_READFILE=61` (map to VFS read with HANDLE), `SYS_WRITEFILE=62`, `SYS_CLOSEHANDLE=63`, `SYS_SETFILEPOINTER=64`, `SYS_EXITPROCESS=3` (alias `SYS_EXIT`), `SYS_CREATEPROCESS=65`, `SYS_GETCOMMANDLINE=66`, `SYS_GETCURRENTPID=67`, `SYS_VIRTUALALLOC=68`, `SYS_VIRTUALFREE=69`, `SYS_VIRTUALPROTECT=70`, `SYS_MSGBOX=71`, `SYS_CREATEWINDOW=72`, `SYS_POSTMESSAGE=73`. Note: numbers 60–59 chosen to follow after `SYS_PRIVILEGE_REQUEST=59`. **Handle table**: user-mode file operations need a kernel handle table; `HANDLE` is an index into `task.handle_table[]` -- `struct kernel_handle { int type; union { vfs_node_t *node; } u; }`. Add `handle_table[64]` to `struct task`. **Dispatch**: `syscall_dispatch()` extended with new cases; WIN32-named cases just translate HANDLE→node and call existing VFS functions.

- [ ] `include/kernel/sched/abi.h`: Win32-named `SYS_*` constants; numbering from 60 up; all Win32 calling convention documented in comments
- [ ] `struct kernel_handle { int type; vfs_node_t *node; }` + `handle_table[64]` added to `struct task`
- [ ] `handle_alloc(task, node)` → returns HANDLE index; `handle_resolve(task, handle)` → `vfs_node_t *`
- [ ] `SYS_CREATEFILE` handler: `vfs_open(path)` → `handle_alloc()` → return HANDLE to user
- [ ] `SYS_READFILE/WRITEFILE/CLOSEHANDLE` handlers using `handle_resolve()`
- [ ] `SYS_VIRTUALALLOC/FREE/PROTECT` handlers: `vmm_alloc_user()` + page table flags
- [ ] `SYS_GETCURRENTPID`: return `task_current()->pid`
- [ ] `SYS_MSGBOX` stub: enqueue message box request to compositor
- [ ] Commit: `"kernel: win32 abi -- SYS_CREATEFILE/READFILE/WRITEFILE/CLOSEHANDLE/VIRTUALALLOC, handle table"`

## 3. PE Header Structures `[Sonnet]`

`include/pe.h`: `pe_dos_header`, `pe_coff_header`, `pe_optional_header64`, `pe_section_header`, `pe_data_directory`, `pe_import_descriptor`, `pe_thunk_data64`, `pe_base_reloc_block`, `pe_base_reloc_entry`. Constants: `PE_DOS_MAGIC`, `PE_SIGNATURE`, `PE_MACHINE_AMD64`, `PE32PLUS_MAGIC`.

**Files:** `include/pe.h` (new)

> [!NOTE]
> All structs use `__attribute__((packed))`. Key fields: `pe_dos_header { uint16_t e_magic; uint8_t pad[58]; uint32_t e_lfanew; }`. `pe_coff_header { uint32_t signature; uint16_t machine; uint16_t num_sections; uint32_t timestamp; uint32_t sym_table_off; uint32_t num_syms; uint16_t optional_header_size; uint16_t characteristics; }`. `pe_optional_header64 { uint16_t magic; uint8_t major_linker; uint8_t minor_linker; uint32_t code_size; uint32_t init_data_size; uint32_t uninit_data_size; uint32_t entry_point_rva; uint32_t code_base; uint64_t image_base; uint32_t section_align; uint32_t file_align; ... uint32_t size_of_image; uint32_t size_of_headers; ... uint16_t num_data_dirs; pe_data_directory data_dirs[16]; }`. `pe_section_header { char name[8]; uint32_t virtual_size; uint32_t virtual_address; uint32_t raw_size; uint32_t raw_offset; ...; uint32_t characteristics; }`. `pe_import_descriptor { uint32_t ilt_rva; uint32_t timestamp; uint32_t forwarder_chain; uint32_t name_rva; uint32_t iat_rva; }`. `pe_thunk_data64 { uint64_t ordinal_or_hint_rva; }` (bit 63 = ordinal flag). `pe_base_reloc_block { uint32_t page_rva; uint32_t block_size; }` followed by `uint16_t entries[]` (type in bits [15:12], offset in [11:0]).

- [ ] `include/pe.h` with all structs `__attribute__((packed))`
- [ ] Constants: `PE_DOS_MAGIC=0x5A4D`, `PE_SIGNATURE=0x4550`, `PE_MACHINE_AMD64=0x8664`, `PE32PLUS_MAGIC=0x020B`
- [ ] Data directory indices: `PE_DIR_EXPORT=0`, `PE_DIR_IMPORT=1`, `PE_DIR_RESOURCE=2`, `PE_DIR_BASE_RELOC=5`, `PE_DIR_DEBUG=6`, `PE_DIR_TLS=9`, `PE_DIR_IAT=12`
- [ ] Relocation types: `IMAGE_REL_BASED_ABSOLUTE=0`, `IMAGE_REL_BASED_DIR64=10`
- [ ] Section characteristics: `PE_SCN_CNT_CODE=0x20`, `PE_SCN_MEM_EXECUTE=0x20000000`, `PE_SCN_MEM_READ=0x40000000`, `PE_SCN_MEM_WRITE=0x80000000`
- [ ] Import hint/name table entry struct: `pe_import_by_name { uint16_t hint; char name[]; }`
- [ ] Commit: `"kernel: pe.h -- PE32+ struct definitions, constants, section/import/reloc descriptors"`

## 4. PE Loader Core `[Sonnet]`

`pe_load(data, size, base_out)`: validate `MZ`→`e_lfanew`→`PE\0\0`→COFF Machine==AMD64→PE32+ magic. Allocate `SizeOfImage` bytes at preferred `ImageBase` (fall back if taken). Map each section via `memcpy` raw→VA, zero BSS. Return entry VA.

**Files:** `src/kernel/pe.c` (new), `include/kernel/pe_loader.h` (new)

> [!NOTE]
> **VMM prerequisite (2026-04-04):** `vmm_set_user_page()` auto-splits 2 MiB huge pages on demand, so PE sections can be mapped at any `ImageBase` address. `vmm_destroy_user_pml4()` frees all dynamically split PT frames on process exit. However, per-process PML4s still share physical frames (identity-mapped clones) -- true process isolation requires unique physical backing per process (D3/T1§3).

> [!NOTE]
> `pe_load(const uint8_t *data, uint64_t size, uint64_t *entry_va_out)`: (1) validate: `dos->e_magic == PE_DOS_MAGIC`; `*(uint32_t*)(data + dos->e_lfanew) == PE_SIGNATURE`; `coff->machine == PE_MACHINE_AMD64`; `opt64->magic == PE32PLUS_MAGIC`; return `PE_ERR_*` on any failure. (2) `alloc_base = vmm_alloc_user(opt->size_of_image)` -- try preferred `opt->image_base` first; if `vmm_map_user(opt->image_base, ...)` fails → fall back to `vmm_alloc_user(opt->size_of_image)` at any address. (3) per section: `dest = alloc_base + sec->virtual_address`; `src = data + sec->raw_offset`; `kmemcpy(dest, src, min(sec->raw_size, sec->virtual_size))`; if `sec->virtual_size > sec->raw_size`: `kmemset(dest + sec->raw_size, 0, sec->virtual_size - sec->raw_size)` (BSS zero-fill). (4) `*entry_va_out = alloc_base + opt->entry_point_rva`. **Error codes**: `PE_OK=0`, `PE_ERR_BAD_DOS=-1`, `PE_ERR_BAD_SIG=-2`, `PE_ERR_NOT_AMD64=-3`, `PE_ERR_NOT_PE32PLUS=-4`, `PE_ERR_ALLOC=-5`. Log each error via `klog_err("pe_load: %s", pe_strerror(err))`.

- [ ] `include/kernel/pe_loader.h`: `pe_load()`, `pe_apply_relocations()`, `pe_resolve_imports()`, `pe_exec()` prototypes; `PE_OK/PE_ERR_*` codes
- [ ] `pe_load()`: header validation chain; `vmm_alloc_user()` at preferred or any base; per-section `kmemcpy` + BSS zero
- [ ] `pe_strerror(int err)` -- human-readable error string for klog
- [ ] Handle `SizeOfImage` > VMM user space guard: `klog_err()` + return `PE_ERR_ALLOC`
- [ ] Handle section `raw_size=0` (BSS-only sections): just zero `virtual_size` bytes
- [ ] Test: load `hello.exe` PE32+ binary → `entry_va_out` points within mapped image; no crash on validation
- [ ] Commit: `"kernel: pe_load -- header validation, section mapping, BSS zero, vmm_alloc_user at ImageBase"`

## 5. Base Relocations `[Sonnet]`

`pe_apply_relocations(base, data, delta)`: parse DataDirectory[5] (BASE_RELOCATION). Walk relocation blocks. For each `IMAGE_REL_BASED_DIR64` entry: add `delta` to the 64-bit value at `base + page_rva + entry_offset`. Skip type=0 (padding).

**Files:** extend `src/kernel/pe.c`

> [!NOTE]
> `pe_apply_relocations(uint8_t *base, const uint8_t *file_data, uint64_t delta)`: skip if `delta == 0` (loaded at preferred base -- no work needed). `reloc_dir = &opt->data_dirs[PE_DIR_BASE_RELOC]`; if `reloc_dir->size == 0` → return `PE_OK` (no relocations). Iterate: `block = (pe_base_reloc_block *)(base + reloc_dir->rva)`; while `block->page_rva != 0`: `num_entries = (block->block_size - 8) / 2`; for each `entry`: `type = entry >> 12`; `offset = entry & 0xFFF`; if `type == IMAGE_REL_BASED_DIR64`: `*(uint64_t *)(base + block->page_rva + offset) += delta`; if `type == IMAGE_REL_BASED_ABSOLUTE`: skip (padding); else: `klog_warn("pe_reloc: unknown type %u", type)` + continue. Advance `block = (next block after block->block_size bytes)`. Return `PE_OK`.

- [ ] `pe_apply_relocations(uint8_t *image_base, const uint8_t *file_data, uint64_t delta)` -- skip if `delta==0`
- [ ] Block walk: `block_size` bytes advancement; `num_entries = (block_size - 8) / 2`
- [ ] `IMAGE_REL_BASED_DIR64`: `*(uint64_t*)(image_base + page_rva + offset) += delta`
- [ ] `IMAGE_REL_BASED_ABSOLUTE`: skip silently (it's padding)
- [ ] Unknown types: `klog_warn()` + continue (don't abort -- robustness)
- [ ] Call `pe_apply_relocations()` from `pe_load()` after section mapping, passing `actual_base - preferred_base` as delta
- [ ] Commit: `"kernel: pe_apply_relocations -- DIR64 delta patch, block walk, skip ABSOLUTE padding"`

## 6. Import Resolution `[Sonnet]`

`pe_resolve_imports(base, data)`: parse DataDirectory[1] (IMPORT_TABLE). For each `IMAGE_IMPORT_DESCRIPTOR`, find matching built-in DLL stub table. For each `IMAGE_THUNK_DATA`: by name → name hash lookup; by ordinal → ordinal lookup. Write resolved function pointer to IAT. Fail with detailed klog on missing symbols.

**Files:** extend `src/kernel/pe.c`; `src/kernel/win32/dll_stubs.c` (new)

> [!NOTE]
> → XREF: `10-platform-services/TODO-08` -- Win32 API stubs (`CreateFile`, `ReadFile`, `CreateProcess`, `MessageBoxA`, etc.); §6 here wires the import resolver to find them; actual stub implementations live in TODO-08. For now: stubs return `PE_ERR_MISSING_IMPORT` with `klog_err("pe_import: unresolved: %s!%s", dll_name, func_name)` for any symbol not yet implemented. **Built-in DLL tables**: `typedef struct { const char *name; void *func; } dll_export_t`. Four tables: `kernel32_exports[]`, `user32_exports[]`, `gdi32_exports[]`, `ntdll_exports[]` -- each is a `{name, NULL}` stub array initially. `dll_find(name)` searches all four tables. **IAT write**: `pe_resolve_imports()`: `import_dir = &opt->data_dirs[PE_DIR_IMPORT]`; iterate `pe_import_descriptor` (stop at all-zero entry); `dll_name = base + desc->name_rva`; ILT/IAT walk: if `thunk.ordinal_or_hint_rva & (1ULL << 63)` → ordinal = low word → `dll_find_ordinal(dll, ordinal)`; else → `hint_name = (pe_import_by_name*)(base + thunk.ordinal_or_hint_rva)`; `dll_find_by_name(dll, hint_name->name)` → if found: `*(uint64_t*)(base + iat_rva + i*8) = func_addr`; else: `klog_err()` + `iat_entry = (uint64_t)pe_missing_import_stub` (stub that logs + calls `SYS_EXIT(1)`).

- [ ] `dll_export_t kernel32_exports[]` stub array (name=func_name, func=NULL for unimplemented); `user32_exports[]`, `gdi32_exports[]`, `ntdll_exports[]`
- [ ] `void *dll_find(const char *dll_name, const char *func_name)` -- case-insensitive DLL name match; scan export table
- [ ] `void *dll_find_ordinal(const char *dll_name, uint16_t ordinal)` -- ordinal lookup
- [ ] `pe_resolve_imports(uint8_t *base, const uint8_t *file_data)` -- ILT walk → IAT write
- [ ] `pe_missing_import_stub()` -- `klog_err("PE: missing import called")` + `syscall SYS_EXIT(1)` (in ASM or via C)
- [ ] Wire `pe_resolve_imports()` into `pe_load()` after relocations
- [ ] **Harden `pe_resolve_dll_imports` RVA bounds** (`pe.c`): the `name_rva+2`/`hint_rva+3` checks add in 32-bit and wrap near `UINT32_MAX` -> OOB read on a malformed PE; use non-wrapping/64-bit math + malformed-PE tests. (TODO-12 §31 review)
- [ ] Commit: `"kernel: pe_resolve_imports -- IAT walk, dll_find name/ordinal, stub tables, missing import trap"`

## 7. PE Execution `[Opus]`

`pe_exec(path)`: VFS load → `pe_load()` → TEB/PEB minimal setup (`PEB.ImageBaseAddress`, `TEB.StackBase/StackLimit`, `PEB.ProcessParameters.CommandLine`) → switch to ring-3 at PE entry point. Process exit via `SYS_EXITPROCESS` → kernel reclaims memory.

**Files:** extend `src/kernel/pe.c`, extend `src/kernel/sched/task.c`

> [!NOTE]
> `pe_exec(const char *path)`: VFS open → `vfs_stat()` for size → `pmm_alloc_contiguous(ceil(size/4096))` → `vfs_read()` → `pe_load(data, size, &entry_va)` → `pe_apply_relocations()` → `pe_resolve_imports()`. **User stack**: `stack_base = vmm_alloc_user(8192)`; set `RSP = stack_base + 8192 - 8` (align 16 - 8 for call convention). **TEB setup**: allocate 1 page at user VA; minimal `TEB { void *stack_base; void *stack_limit; void *TEB_self; }` at `FS`-relative base (set `FS_BASE` MSR to TEB VA). **PEB setup**: allocate 1 page; minimal `PEB { uint64_t image_base; void *process_parameters; }` at `TEB.PEB_ptr`. **`ProcessParameters`**: allocate 256 bytes; embed null-terminated command-line buffer at `cmd_line` field. **Ring-3 switch**: `task_create_user_with_entry(entry_va, name)` -- or direct: push user CS/SS/RFLAGS/RSP/entry on kernel stack + `iretq`. Set `RCX = entry_va` (SYSRET approach: not applicable here since we jump directly). Use `iretq` for initial launch: push `user_data|3`, `user_rsp`, `0x200` (RFLAGS IF), `user_code|3`, `entry_va`; `iretq`. **On exit**: `SYS_EXITPROCESS` or `SYS_EXIT` handler → `vmm_free_user(image_base, size_of_image)` + `vmm_free_user(stack_base, 8192)` + TEB/PEB free + `task_exit()`.

- [ ] `pe_exec(const char *path)` -- VFS load; `pmm_alloc_contiguous()`; full `pe_load/reloc/import` pipeline
- [ ] User stack: `vmm_alloc_user(8192)` + initial RSP setup
- [ ] TEB: 1-page user VA; `FS_BASE` MSR set to TEB VA; minimal `stack_base/stack_limit/self`
- [ ] PEB: 1-page user VA; `image_base` + `process_parameters` ptr; `CommandLine` buffer
- [ ] `iretq` ring-3 launch: kernel stack frame: `[user_data|3, user_rsp, 0x200, user_code|3, entry_va]` + `iretq`
- [ ] `SYS_EXITPROCESS` handler: `vmm_free_user(image_base, size)` + stack + TEB + PEB + `task_exit()`
- [ ] Test: `pe_exec("C:\\Users\\Default\\hello.exe")` → serial log "Hello from ring 3!" + clean exit
- [ ] Commit: `"kernel: pe_exec -- TEB/PEB setup, iretq ring-3 launch, ExitProcess memory reclaim"`

## 8. EXE Format Precedence `[Sonnet]`

`exec_load(path)`: try PE (first 2 bytes == `MZ` + valid PE signature) → if valid: `pe_exec(path)`. Else try ELF (`\x7FELF`) → `task_exec()` existing. Else error. `file_assoc_open` uses `exec_load` for `.exe` files.

**Files:** `src/kernel/exec.c` (new or extend `task.c`)

> [!NOTE]
> `exec_load(const char *path)`: read first 512 bytes (header probe) → if `*(uint16_t *)header == PE_DOS_MAGIC && valid_pe_sig(header)` → `pe_exec(path)`. Else if `*(uint32_t *)header == ELF_MAGIC` → `elf_exec(path)` (VFS load + `elf_load()` + ring-3 via existing path). Else `klog_err("exec_load: unknown format: %s", path)` + return error. **`valid_pe_sig`**: check `dos->e_lfanew < 512` && `*(uint32_t*)(header + e_lfanew) == PE_SIGNATURE`. **File association**: register `.exe` → `exec_load` in `file_assoc` default table (TODO-02 §1). The existing `task_exec()` called from shell commands continues to work for ELF binaries. **Format logging**: `klog(LOG_INFO, "exec", "Loading %s: format=%s", path, fmt_str)` for debuggability.

- [ ] `exec_load(const char *path)` -- 512-byte header probe; PE check first; ELF fallback; unknown = error
- [ ] `valid_pe_sig(const uint8_t *header512)` inline helper -- `e_magic + e_lfanew range + PE\0\0` check
- [ ] Shell `exec` command + `file_assoc_open(".exe", ...)` → route to `exec_load()`
- [ ] `klog(LOG_INFO, "exec", "format=PE32+/ELF/unknown")` for each launch
- [ ] Commit: `"kernel: exec_load -- PE32+ first, ELF fallback, format probe, .exe file assoc"`

## 9. Minimal User-Mode Runtime `[Sonnet]`

`user/lib/crt0_pe.asm`: Windows `_start` entry (RCX=PEB ptr), sets up stack, calls `main(argc, argv)`, calls `ExitProcess(ret)`. `user/lib/kernel32_stub.c`: thin `CreateFile/ReadFile/WriteFile/ExitProcess/GetStdHandle` wrappers using `SYSCALL` instruction + Windows x64 calling convention. Compile to `user/lib/libcrt.a`.

**Files:** `user/lib/crt0_pe.asm` (new), `user/lib/kernel32_stub.c` (new), extend `user/include/windows.h` (new)

> [!NOTE]
> **`crt0_pe.asm`**: `section .text; global _start; extern main; extern ExitProcess`. `_start:` -- Windows PE entry point: `RCX = PEB ptr` on entry. Clear `RBP`. Parse `PEB->ProcessParameters->CommandLine` (UTF-16LE) → convert to `argc/argv` via `crt_parse_cmdline()`; push `argv`, `argc`; `call main`; `mov ecx, eax; call ExitProcess`. **`kernel32_stub.c`**: each function uses NASM-style inline asm or a `.asm` macro: `CreateFile(path, access, ...)` → `mov rax, SYS_CREATEFILE; mov rcx, path; syscall; ret`. Use `user/include/windows.h` type aliases (`HANDLE=uint64_t`, `DWORD=uint32_t`, `BOOL=int`, `LPCSTR=const char *`, `INVALID_HANDLE_VALUE=(HANDLE)(-1)`). **`GetStdHandle`**: return pre-assigned handles (stdin=0, stdout=1, stderr=2 -- same as kernel `SYS_WRITE` fd). **`WriteFile(handle, buf, n, written, NULL)`** → `mov rax, SYS_WRITEFILE; syscall`. **Build**: `Makefile` adds `user/lib/crt0_pe.asm` + `user/lib/kernel32_stub.c` to `libcrt.a`; PE user programs link with `-lcrt`.

- [ ] `user/include/windows.h`: `HANDLE`, `DWORD`, `BOOL`, `LPCSTR`, `LPVOID`, `INVALID_HANDLE_VALUE`, `GENERIC_READ/WRITE`, `OPEN_EXISTING/CREATE_ALWAYS`
- [ ] `user/lib/crt0_pe.asm`: `_start`; `RCX=PEB` entry; `crt_parse_cmdline()` stub; call `main(argc, argv)`; call `ExitProcess(ret)`
- [ ] `user/lib/kernel32_stub.c`: `CreateFile/ReadFile/WriteFile/CloseHandle/ExitProcess/GetStdHandle` using `SYSCALL` + Windows ABI
- [ ] `crt_parse_cmdline()` in `crt0_pe.asm` or `kernel32_stub.c`: parse `argv[0]` from PEB CommandLine string
- [ ] Add `crt0_pe.o` + `kernel32_stub.o` to `user/lib/Makefile` → `libcrt.a`
- [ ] Update `user/hello.c` to use `#include <windows.h>` + `WriteFile(GetStdHandle(-11), "Hello\n", 6, NULL, NULL)`; compile as PE
- [ ] Commit: `"user: crt0_pe + kernel32_stub -- PE _start, ExitProcess, CreateFile/ReadFile/WriteFile stubs"`

---

## OS Comparison


| ⭐  | Feature                                                              | 🪟 Win11                                                                         | 🐧 Linux                                                                   | 🚀 Impossible OS                                                              |
| --- | -------------------------------------------------------------------- | -------------------------------------------------------------------------------- | -------------------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| ⭐  | Ring-3 execution via `SYSCALL`/`SYSRET` with correct STAR/LSTAR MSRs | ✅ `SYSCALL`/`SYSRET`; `KiSystemCall64` entry; `SWAPGS`; per-CPU                 | ✅ `SYSCALL`/`SYSRET`; `entry_SYSCALL_64`; per-CPU `rsp_scratch`; `swapgs` | ⬜ §1 -- `[Opus]` STAR reorder + `SWAPGS`                                     |
| ⭐  | Windows x64 syscall ABI                                              | ✅ `NtUserCall*`; `NtCreateFile`; HANDLE objects; kernel                         | ✅ `int 0x80`/`SYSCALL`; RDI/RSI/RDX/RCX; fd integers                      | ⬜ §2 -- `⭐` native Win32 ABI --                                             |
| 💎  | PE32+ header structures                                              | ✅ Native PE32+ format; ntdll `LdrLoadDll`;                                      | ⚠️ No native PE; Wine `pe_loader.c`;                                       | ⬜ §3 -- `include/pe.h` complete struct set; `__attribute__((packed))`        |
| 💎  | PE loader                                                            | ✅ `ntdll!LdrpMapViewOfSection`; ASLR; NX sections; signed-image                 | ⚠️ Wine `pe_loader.c`; `ld-linux` for ELF;                                 | ⬜ §4 -- `pe_load()` in kernel; `vmm_alloc_user()` at                         |
| 💎  | Base relocations                                                     | ✅ ASLR always randomizes; full reloc                                            | ⚠️ Wine handles PE relocs; ELF                                             | ⬜ §5 -- `pe_apply_relocations()` DIR64 walk; skip ABSOLUTE                   |
| 💎  | Import resolution                                                    | ✅ `LdrpResolveForwarders`; delay-load; ordinal + name;                          | ⚠️ Wine `LoadLibrary` emulation; ELF uses                                  | ⬜ §6 -- `pe_resolve_imports()` + built-in `kernel32/user32/gdi32/ntdll` stub |
| ⭐  | PE execution                                                         | ✅ `ntdll!LdrInitializeThunk`; full TEB/PEB; LdrpInitialize; KiUserApcDispatcher | ⚠️ No native PE exec; Wine                                                 | ⬜ §7 -- `⭐` native kernel TEB/PEB (no                                       |
| 💎  | Format precedence                                                    | ✅ PE only (no ELF); `NTDLL`                                                     | ✅ ELF only (no PE); `binfmt_misc`                                         | ⬜ §8 -- `⭐` both PE32+ and ELF                                              |
| ⭐  | User CRT                                                             | ✅ `msvcrt.dll`; `ucrt`; `vcruntime`; full CRT                                   | ✅ `glibc`/`musl` crt0; `ld-linux` dynamic linking;                        | ⬜ §9 -- `⭐` minimal `libcrt.a` maps Win32                                   |

> **After §1–§9:** Impossible OS executes native Win32 PE32+ binaries -- not via emulation, but because PE is the native format and Win32 is the native API. The `⭐` advantages: the ring-3 transition uses the same `SYSCALL`/`SYSRET` + `SWAPGS` + per-CPU GS approach as Windows NT; TEB/PEB are set up natively in the kernel (no WINE-style userspace setup); and `libcrt.a` maps `CreateFile/ReadFile/WriteFile` directly to kernel syscall numbers (zero DLL-load overhead at startup).

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Minimal ring-3 test: `SYSRET` to stub calling `SYS_EXIT(0)` → serial log "ring3 test OK"; QEMU `-d int` shows no fault
- [ ] `pe_load()` on a hand-crafted minimal PE32+ binary → `entry_va_out` within image; all sections mapped
- [ ] Relocation test: load PE at non-preferred base → `pe_apply_relocations()` patches all DIR64 entries → no access violation
- [ ] `pe_resolve_imports()` on a binary importing `ExitProcess@kernel32` → IAT entry filled with stub address; missing import → `klog_err` + `pe_missing_import_stub` installed
- [ ] `pe_exec("C:\\hello.exe")` → runs in ring-3 → `WriteFile` output appears on serial → `ExitProcess(0)` → process cleaned up; no memory leak
- [ ] Open `hello.exe` (PE) via shell → `exec_load()` detects PE format; open `.elf` binary → ELF path taken
- [ ] `hello.c` compiled as PE32+ with `libcrt.a` → writes "Hello from Win32!" to stdout via `WriteFile`/`GetStdHandle`
- [ ] Commit: `"win32: ring3 fix, PE loader, IAT resolution, TEB/PEB, pe_exec, crt0_pe -- PE32+ execution complete"`
