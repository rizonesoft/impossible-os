---
schema_version: 1
id: elf-reloc-kernel-modules
domain: 12-user-platform-sdk
status: active
title: "TODO-03 -- ELF Relocations & Kernel Module System"
---

# TODO-03 -- ELF Relocations & Kernel Module System

> **Goal:** Build the ELF relocation engine powering two subsystems: loadable kernel
> modules (`.kmod` drivers) and the Linux ELF compatibility layer's `dlopen`/`dlsym`.
> The native user-mode binary format is PE (`.exe`/`.dll`) -- `LoadLibrary`/`GetProcAddress`
> is the native DLL path (→ XREF `10-platform-services/TODO-08 §7`). ELF supports only
> ring-0 `.kmod` drivers and Linux compat `.so` shared libraries.

> [!IMPORTANT]
> `include/kernel/elf.h` already defines `ET_EXEC`, `ET_DYN`, `EM_X86_64`,
> `struct elf64_header`, and `struct elf64_phdr` for `task_exec`. This TODO extends
> `elf.h` with the section-header types (`ET_REL`, `SHT_SYMTAB`, `SHT_RELA`, `Elf64_Sym`,
> `Elf64_Rela`) and all `R_X86_64_*` relocation constants -- do not duplicate the existing
> definitions.
>
> **dlopen / dlsym** (§5): the Linux ELF compat stretch goal is specced in
> `10-platform-services/TODO-10 §9`; this TODO provides the kernel-side relocation and
> GOT/PLT infrastructure that TODO-10 §13 depends on.
>
> **`pmm_alloc_contiguous(count)`** must be used for all module image buffers > 4 KB.
> `kmalloc` is only for small structs (≤ 4 KB). Violating this silently corrupts the 2 MiB
> heap.
>
> **Security**: kernel modules execute ring-0 code. §3 must validate ELF magic, class,
> machine, and type before applying any relocations or calling `init_fn`.

---

## Inputs

- `include/kernel/elf.h` -- extend with `ET_REL`, `SHT_SYMTAB`, `SHT_RELA`, `Elf64_Shdr`, `Elf64_Sym`, `Elf64_Rela`, `R_X86_64_*` constants -- §1
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous(count)` -- §1 §3 module image allocation
- `include/kernel/mm/vmm.h` -- `vmm_map_page(virt, phys, flags)` -- §3 exec+rw page mapping
- `include/kernel/fs/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_stat` -- §3 §5 kmod load from disk
- `include/kernel/sched/task.h` -- `task_exec`, `struct task` -- §5 per-process loaded-module list
- `include/kernel/syscall.h` -- add `SYS_DLOPEN`/`SYS_DLSYM`/`SYS_DLCLOSE` -- §5
- `src/kernel/elf.c` (existing `task_exec` ELF loader) -- reuse section-scan helpers -- §1
- `10-platform-services/TODO-08-win32-api-surface.md §7` (→ XREF) -- PE DLL `LoadLibrary` (native path; not ELF)
- `10-platform-services/TODO-10-linux-compat.md §13` (→ XREF) -- dynamic ELF `dlopen` consumer
- `scripts/linker-kernel.ld` (kernel linker script) -- add `__ksymtab` section -- §4

---

## Outcome

`kmod_load("C:\\Impossible\\System\\Drivers\\foo.kmod")` validates, relocates, and
calls `init_fn` of a `.kmod` ELF relocatable object. `ksym_lookup("pmm_alloc_contiguous")`
returns the kernel function address. `EXPORT_SYMBOL(fn)` populates the `__ksymtab` section
at link time. `lsmod`/`insmod`/`rmmod`/`modprobe` shell commands work. The GOT/PLT lazy-
binding engine is available for the Linux ELF compat `dlopen` path.

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                             |
| ---- | ---------------------------------------- | ----- | -------------------------------------- |
| 1    | ELF Relocation Engine                    | 💎    | Extend `elf.h`; `pmm_alloc_contiguous` |
| 2    | Kernel Module Format `.kmod`             | ⭐    | §1 relocation engine                   |
| 3    | Kernel Module Loader                     | ⭐    | §1 §2; `vmm_map_page`; `vfs_read`      |
| 4    | Kernel Symbol Export (`EXPORT_SYMBOL`)   | ⭐    | §3; linker script `__ksymtab` section  |
| 5    | `dlopen` / `dlsym` for Linux Compat      | 💎    | §1 §6; `D10T10 §9` prerequisite        |
| 6    | GOT / PLT Lazy Binding                   | 💎    | §1 relocation engine; §5 consumer      |
| 7    | Module Hot-swap (Stretch)                | ⭐    | §3 stable; clean `exit_fn` protocol    |
| 8    | `lsmod` / `insmod` / `rmmod` / `modprobe` | 💎    | §3 §4                                  |

---

## 1. ELF Relocation Engine `[Opus]`

> Novel architecture: applies CPU-specific `R_X86_64_*` relocation types against
> live kernel address space. No prior Impossible OS relocation engine exists.

**Source file:** `src/kernel/elf_reloc.c`; extend `include/kernel/elf.h`

- [ ] **Extend `include/kernel/elf.h`** with missing ELF section structures:
  ```c
  #define ET_REL          1      /* relocatable object */
  #define SHT_NULL        0
  #define SHT_PROGBITS    1
  #define SHT_SYMTAB      2
  #define SHT_STRTAB      3
  #define SHT_RELA        4      /* relocation with addend */
  #define SHT_REL         9      /* relocation without addend */
  #define SHF_ALLOC       0x2
  #define SHF_EXECINSTR   0x4

  typedef struct { uint64_t sh_name; uint32_t sh_type; uint64_t sh_flags;
      uint64_t sh_addr; uint64_t sh_offset; uint64_t sh_size; uint32_t sh_link;
      uint32_t sh_info; uint64_t sh_addralign; uint64_t sh_entsize; } Elf64_Shdr;

  typedef struct { uint32_t st_name; uint8_t st_info; uint8_t st_other;
      uint16_t st_shndx; uint64_t st_value; uint64_t st_size; } Elf64_Sym;

  typedef struct { uint64_t r_offset; uint64_t r_info; int64_t  r_addend; } Elf64_Rela;

  /* R_INFO accessors */
  #define ELF64_R_SYM(i)  ((i) >> 32)
  #define ELF64_R_TYPE(i) ((i) & 0xFFFFFFFF)

  /* x86-64 relocation types */
  #define R_X86_64_NONE       0
  #define R_X86_64_64         1   /* S + A (absolute 64-bit) */
  #define R_X86_64_PC32       2   /* S + A - P (PC-relative 32-bit) */
  #define R_X86_64_PLT32      4   /* PLT entry + A - P */
  #define R_X86_64_GLOB_DAT   6   /* GOT entry; S */
  #define R_X86_64_JUMP_SLOT  7   /* PLT slot; S */
  #define R_X86_64_RELATIVE   8   /* B + A (base-relative) */
  #define R_X86_64_32         10  /* S + A (zero-extend to 32-bit) */
  #define R_X86_64_32S        11  /* S + A (sign-extend to 32-bit) */
  ```
- [ ] **`elf_resolve_symbol(name)`**: search kernel `__ksymtab` (§4) first; then walk `g_loaded_kmods[]` `.dynsym` tables; return address or 0 if not found; log missing symbol at `WARN` level
- [ ] **`elf_apply_rela(section_base, rela_table, rela_count, symtab, strtab, load_bias)`**:
  - For each `Elf64_Rela` entry: `sym_idx = ELF64_R_SYM(r_info)`; `type = ELF64_R_TYPE(r_info)`
  - `S` = symbol value: if `st_shndx == SHN_UNDEF` → `elf_resolve_symbol(name)`; else section base + `st_value` + `load_bias`
  - `P` = relocation site virtual address (`section_base + r_offset`)
  - `A` = `r_addend`
  - Apply per type:
    - `R_X86_64_64`: `*((uint64_t*)P) = S + A`
    - `R_X86_64_PC32` / `R_X86_64_PLT32`: `int32_t v = (int32_t)(S + A - P)`; check fits in 32-bit signed; `*((int32_t*)P) = v`
    - `R_X86_64_GLOB_DAT`: `*((uint64_t*)P) = S`
    - `R_X86_64_JUMP_SLOT`: `*((uint64_t*)P) = S` (eager) or PLT trampoline (lazy, §6)
    - `R_X86_64_RELATIVE`: `*((uint64_t*)P) = load_bias + A`
    - `R_X86_64_32` / `R_X86_64_32S`: truncate/sign-extend check; apply 32-bit
    - Unknown type: `klog(ERROR, "elf", "unknown reloc type %u", type)`; return -1
- [ ] **`elf_apply_rel(section_base, rel_table, rel_count, symtab, strtab, load_bias)`**: same logic with implicit `A = 0` (no addend field)

---

## 2. Kernel Module Format `.kmod` `[Sonnet]`

**Header:** `include/kernel/kmod.h`

- [ ] **`.kmod` ELF relocatable object** (`ET_REL`, compiled with `clang-19 --target=x86_64-elf -r`):
  - Mandatory section: `.kmod_info` containing exactly one `struct kmod_info` at offset 0
  - Optional sections: `.text`, `.data`, `.bss`, `.rodata`; standard section naming
  - No `PT_INTERP`; no dynamic linker needed; symbols resolved against kernel `__ksymtab`
- [ ] **`struct kmod_info`** in `include/kernel/kmod.h`:
  ```c
  #define KMOD_NAME_MAX    32
  #define KMOD_VER_MAX     16
  #define KMOD_DEP_MAX      8

  typedef struct {
      char     magic[8];               /* "KMOD\0\0\0\0" */
      char     name[KMOD_NAME_MAX];    /* short name, e.g. "rtl8139" */
      char     version[KMOD_VER_MAX];  /* e.g. "1.0.0" */
      void   (*init_fn)(void);         /* called by kmod_load() after reloc */
      void   (*exit_fn)(void);         /* called by kmod_unload() */
      char     depends[KMOD_DEP_MAX][KMOD_NAME_MAX]; /* dependency module names */
  } kmod_info_t;
  ```
- [ ] **Build recipe** for a `.kmod` driver:
  ```
  clang-19 --target=x86_64-elf -ffreestanding -nostdlib -nostdinc \
      -fno-pie -mno-red-zone -mcmodel=kernel \
      -r -o mydriver.kmod mydriver.c
  ```
- [ ] **Magic validation**: `kmod_load` checks `magic[0..3] == "KMOD"` before touching `init_fn`; reject with `klog(ERROR)` if mismatched
- [ ] **Version format**: `major.minor.patch` string; checked by `kmod_load` for compatibility (major must match kernel's expected module ABI version stored in `HKLM\SYSTEM\KmodABI`)

---

## 3. Kernel Module Loader `[Opus]`

> Ring-0 code loaded from disk into kernel address space. Novel; security-sensitive.

**Source file:** `src/kernel/kmod.c`; header `include/kernel/kmod.h`

- [ ] **`struct loaded_kmod`** in `include/kernel/kmod.h`:
  ```c
  typedef struct {
      char         name[KMOD_NAME_MAX];
      uintptr_t    load_base;     /* physical base of allocated pages */
      uint64_t     load_size;     /* total allocation in bytes */
      kmod_info_t *info;          /* pointer into loaded image */
      uint32_t     refcount;
  } loaded_kmod_t;

  extern loaded_kmod_t g_loaded_kmods[64];
  extern int           g_kmod_count;
  ```
- [ ] **`int kmod_load(const char *path)`**:
  1. `vfs_open(path)` → `vfs_read` full file into `pmm_alloc_contiguous` buffer (size / PAGE_SIZE + 1 pages)
  2. Validate: `ELF_MAGIC`, `ELFCLASS64`, `ELFDATA2LSB`, `e_type == ET_REL`, `e_machine == EM_X86_64`; return `-EINVAL` on any mismatch
  3. Parse section headers (`Elf64_Shdr`); compute total `SHF_ALLOC` size; allocate second buffer (exec+rw via `pmm_alloc_contiguous` + `vmm_map_page` with `PAGE_RW | PAGE_EXEC`)
  4. Copy `SHF_ALLOC` sections to load buffer at their aligned offsets; record each section's load address
  5. Find `.kmod_info` section; validate `struct kmod_info` magic; read `depends[]` -- recursively call `kmod_load` for any unloaded dependencies
  6. Find `SHT_RELA`/`SHT_REL` sections; call `elf_apply_rela/rel` for each; use `elf_resolve_symbol` for `SHN_UNDEF` symbols
  7. If any symbol unresolved → `klog(ERROR, "kmod", "unresolved symbol: %s in %s", name, path)`; free buffers; return `-ENOSYM`
  8. Add to `g_loaded_kmods[]`; call `info->init_fn()`
  9. Return 0 on success; negative errno on failure
- [ ] **`int kmod_unload(const char *name)`**:
  - Find in `g_loaded_kmods[]` by name; check `refcount == 1`; call `info->exit_fn()`; `pmm_free` pages; remove from array
  - If `refcount > 1`: return `-EBUSY` + log `"module {name} still in use (refcount={n})"`
- [ ] **`void kmod_list(void)`**: for each `g_loaded_kmods[]` entry, `klog(INFO, "kmod", "[%d] %s v%s @ 0x%llx",...)`
- [ ] **Auto-load at boot**: in `kernel_main`, after VFS is mounted: scan `C:\Impossible\System\Drivers\*.kmod`; call `kmod_load` for each; skip if kernel cmdline contains `nokmod={name}`
- [ ] **Dependency resolution order**: topological sort via `depends[]` array before loading; detect cycles → `klog(ERROR)` + skip

---

## 4. Kernel Symbol Export (`EXPORT_SYMBOL`) `[Opus]`

> Novel linker-script + macro infrastructure. No prior Impossible OS `__ksymtab` exists.

**Header:** `include/kernel/export.h`; linker script: `scripts/linker-kernel.ld`

- [ ] **`EXPORT_SYMBOL` macro** in `include/kernel/export.h`:
  ```c
  typedef struct { const char *name; void *addr; } kmod_export_t;

  #define EXPORT_SYMBOL(fn) \
      static kmod_export_t __ksym_##fn \
          __attribute__((used, section("__ksymtab"))) = { #fn, (void *)&fn }
  ```
- [ ] **Linker script addition** in `scripts/linker-kernel.ld`:
  ```
  __ksymtab_start = .;
  KEEP(*(__ksymtab))
  __ksymtab_end = .;
  ```
- [ ] **`ksym_lookup(const char *name)`** in `src/kernel/ksym.c`:
  - Binary search `__ksymtab_start`…`__ksymtab_end` (entries must be sorted at link time)
  - Sort requirement: the linker naturally places entries in link order; add a **post-build sort** in `scripts/build.sh` using `llvm-nm-19` to emit a sorted `__ksymtab` section replacement (or implement a one-time `ksym_sort_once()` at boot using insertion sort -- O(n²) is acceptable for ≤ 512 exports)
  - Return `addr` on match; `NULL` on miss
- [ ] **Export key kernel APIs** (`EXPORT_SYMBOL` annotations in their `.c` files):
  - `pmm_alloc_contiguous`, `pmm_free_contiguous`
  - `kmalloc`, `kfree`
  - `vmm_map_page`
  - `vfs_open`, `vfs_read`, `vfs_write`, `vfs_close`, `vfs_readdir`, `vfs_stat`
  - `klog`
  - `pci_find_device`, `pci_read_config`, `pci_write_config`
  - `task_create`, `task_get_by_pid`
  - `reg_get_string`, `reg_set_string`
  - `notify_send`
- [ ] **Security**: modules can only call `EXPORT_SYMBOL`-marked functions; no arbitrary kernel address is callable; `elf_resolve_symbol` returns 0 for any name not in `__ksymtab` or loaded module `.dynsym`

---

## 5. `dlopen` / `dlsym` for Linux Compat `[Sonnet]`

> Implements the kernel-side infrastructure for `10-platform-services/TODO-10 §9`.
> The Linux compat layer calls these; the native PE equivalent is
> `LoadLibrary`/`GetProcAddress` (→ `D10T08 §7`).

**Source file:** `src/kernel/elf_dyn.c`; header `include/kernel/elf_dyn.h`

- [ ] **Per-process loaded-ELF list** in `struct task`:
  ```c
  struct elf_module *elf_modules[32];  /* NULL-terminated; [0] = main exe */
  ```
- [ ] **`void *dlopen(const char *path, int flags)`** (`RTLD_LAZY=1`, `RTLD_NOW=2`, `RTLD_DEFAULT=0`):
  - If `path == NULL`: return `RTLD_DEFAULT` handle (searches all loaded modules)
  - Check `task->elf_modules[]` for already-loaded path → increment refcount; return handle
  - `vfs_open(path)` + `vfs_read` into `pmm_alloc_contiguous` buffer
  - Validate: `e_type == ET_DYN`, `EM_X86_64`, `ELFCLASS64`
  - Map `PT_LOAD` segments at chosen `load_bias` (ASLR: pick random page-aligned base; simple fallback: use `pmm_alloc_contiguous` address)
  - Call `elf_apply_rela` for all `SHT_RELA` sections; `elf_resolve_symbol` for undefined syms (search kernel `__ksymtab` + `RTLD_DEFAULT` loaded modules)
  - If `RTLD_NOW`: resolve all PLT slots eagerly; if `RTLD_LAZY`: install PLT trampolines (§6)
  - Allocate `struct elf_module`; store in `task->elf_modules[]`; return handle (pointer to `struct elf_module`)
- [ ] **`void *dlsym(void *handle, const char *name)`**:
  - `handle == RTLD_DEFAULT`: search all `task->elf_modules[]` `.dynsym` tables in order
  - else: search specified module's `.dynsym` only; scan `Elf64_Sym` table for `st_name` match; return `st_value + load_bias` or `NULL`
- [ ] **`int dlclose(void *handle)`**: decrement refcount; if 0: call `.fini_array` entries (if present); unmap pages; free `struct elf_module`; remove from `task->elf_modules[]`
- [ ] **Syscall wiring**: add to `include/kernel/syscall.h`: `SYS_DLOPEN`, `SYS_DLSYM`, `SYS_DLCLOSE` with next available numbers; dispatch in `syscall.c` to `dlopen/dlsym/dlclose` via user-pointer validation + `probe_user_read`

---

## 6. GOT / PLT Lazy Binding `[Opus]`

> Novel lazy-binding PLT trampoline patching GOT entries. No prior Impossible OS precedent.

**Source file:** `src/kernel/elf_plt.c`

- [ ] **PLT stub structure** (per-symbol, 16 bytes each):
  ```
  PLT[n]:  jmp qword [GOT + n*8]   ; 6 bytes: FF 25 + 32-bit GOT offset
           push n                   ; 5 bytes: 68 + uint32_t index
           jmp PLT[0]               ; 5 bytes: E9 + 32-bit relative to PLT[0]
  PLT[0]:  push qword [GOT+8]       ; resolver data
           jmp qword [GOT+16]       ; jump to elf_plt_resolve
  ```
- [ ] **Initial GOT setup** for `RTLD_LAZY`:
  - `GOT[0]` = pointer to `Elf64_Dyn` dynamic section (standard)
  - `GOT[1]` = pointer to `struct elf_module` (resolver data)
  - `GOT[2]` = pointer to `elf_plt_resolve`
  - Each `JUMP_SLOT` GOT entry initialised to `PLT[n] + 6` (points to the `push n` instruction)
- [ ] **`uint64_t elf_plt_resolve(struct elf_module *mod, uint32_t slot_idx)`**:
  - Look up symbol name from `.rela.plt` entry `slot_idx` → symbol name from `.dynsym` + `.dynstr`
  - Call `dlsym(RTLD_DEFAULT, name)`; if NULL: `klog(ERROR)` + return 0
  - Patch `mod->got[3 + slot_idx] = resolved_addr` (atomically with `__atomic_store_n`)
  - Return `resolved_addr` -- the PLT trampoline returns here and jumps to it
- [ ] **`RTLD_NOW`** path: at `dlopen` time, iterate all `SHT_RELA` `.rela.plt` entries; call `dlsym` for each; write resolved address directly into GOT; no PLT trampoline needed

---

## 7. Module Hot-Swap (Stretch) `[Sonnet]`

**Source file:** extension of `src/kernel/kmod.c`

- [ ] **`int kmod_reload(const char *name)`**:
  - `kmod_unload(name)` → if `-EBUSY`, refuse + log `"cannot reload: module in use"`
  - `kmod_load` with same path (re-read from VFS, allowing the file to have been updated on disk)
  - On success: log `"[kmod] hot-swap complete: {name}"`; on failure: log error, do **not** leave system without the module
- [ ] **Driver exit_fn contract**: every kmod `exit_fn` must:
  - Deregister all IRQ handlers (call `irq_free_handler(irq, fn)`)
  - Release all DMA buffers and unmap MMIO
  - Call `pci_release_device()` if applicable
  - Return promptly (< 100 ms); blocking in `exit_fn` is a bug
- [ ] **`kmod reload <name>`** shell command: calls `kmod_reload(name)`; prints result

---

## 8. `lsmod` / `insmod` / `rmmod` / `modprobe` Shell Commands `[Sonnet]`

**Source file:** `src/shell/cmd_kmod.c`

- [ ] **`lsmod`**: iterate `g_loaded_kmods[]`; print table:
  ```
  Name          Version   Load Addr         Refcnt  Dependencies
  rtl8139       1.0.0     0xFFFF800001230000  1       (none)
  virtio_blk    1.0.0     0xFFFF800001250000  1       (none)
  ```
- [ ] **`insmod <path.kmod>`**: `kmod_load(path)`; print `"Loaded: {name} v{version}"` on success; print error on failure
- [ ] **`rmmod <name>`**: `kmod_unload(name)`; print `"Unloaded: {name}"` or error
- [ ] **`modprobe <name>`**:
  - Search `C:\Impossible\System\Drivers\{name}.kmod` first; if found: `kmod_load`
  - If not found: search `C:\Impossible\System\Drivers\` for any `.kmod` whose `struct kmod_info.name` matches; load that
  - Recursively resolve `depends[]` array (depth-first, detect cycles)
  - Print all loaded modules in dependency order
- [ ] **Error messages**: POSIX-style: `"insmod: {path}: no such file"`, `"rmmod: {name}: module not loaded"`, `"modprobe: {name}: dependency {dep} not found"`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                | 🐧 Linux                               | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | --------------------------------------- | -------------------------------------- | ---------------------------------------- |
| 💎  | ELF `R_X86_64_*` relocation engine       | ❌ PE/COFF only in ntoskrnl             | ✅ `arch/x86/kernel/module.c`          | ⬜ §1 -- `elf_apply_rela/rel` for all 9 reloc |
| ⭐  | `.kmod` ELF relocatable kernel modules   | ✅ Windows drivers (PE `.sys`)          | ✅ Linux `ko` (ELF `ET_REL`)           | ⬜ §2 -- §3; ELF format + `kmod_load/unload` |
| 💎  | Kernel symbol export                     | ✅ `ntoskrnl.exe` export table          | ✅ `EXPORT_SYMBOL` / `kallsyms`        | ⬜ §4 -- linker-script `__ksymtab` section + `ksym_lookup` |
| 💎  | `dlopen`/`dlsym`/`dlclose` for Linux compat `.so` | ❌ Not applicable (PE native)           | ✅ glibc `ld-linux.so`                 | ⬜ §5 -- kernel-side ELF dynamic loader for |
| 💎  | GOT/PLT lazy binding on first call       | ❌ PE import by address (no             | ✅ ld.so lazy PLT binding              | ⬜ §6 -- PLT trampoline → GOT patch      |
| ⭐  | Module hot-swap without reboot           | ✅ Partial (driver update via WU,       | ✅ `rmmod`+`insmod` (limited)          | ⬜ §7 -- (Stretch) -- ; `kmod_reload` with |
| 💎  | `lsmod`/`insmod`/`rmmod`/`modprobe` shell commands | ✅ `sc.exe` / `devcon.exe` (PE drivers) | ✅ `lsmod`/`insmod`/`rmmod`/`modprobe` | ⬜ §8 -- full command suite with dependency |

Impossible OS kernel modules use **ELF `ET_REL` format** (the same format the Linux kernel
uses for `.ko` files), giving access to the entire LLVM/Clang toolchain for driver
development, while the native user-mode binary format remains PE for Win32 compatibility.
The `EXPORT_SYMBOL` + `__ksymtab` model explicitly bounds the attack surface: a malicious
module cannot call arbitrary kernel addresses -- only the kernel's published symbol table.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Extend elf.h**: build succeeds; existing `task_exec` still compiles with extended header
- [ ] **Reloc engine**: build a minimal `test.kmod` with one `extern` reference to `klog`; `kmod_load` resolves it and calls `init_fn`; `klog` output visible in serial log
- [ ] **kmod_info magic**: load a file with corrupted magic → `kmod_load` returns `-EINVAL` + log `"invalid kmod magic"`
- [ ] **Missing symbol**: `kmod` that references `nonexistent_fn` → `kmod_load` returns `-ENOSYM` + log names the missing symbol
- [ ] **EXPORT_SYMBOL**: `ksym_lookup("pmm_alloc_contiguous")` returns a non-zero address; `ksym_lookup("not_exported")` returns NULL
- [ ] **lsmod**: after loading a `.kmod`, `lsmod` shows its name, version, and address
- [ ] **insmod / rmmod**: `insmod C:\Impossible\System\Drivers\virtio_blk.kmod` → driver init fires; `rmmod virtio_blk` → exit fires; `lsmod` no longer shows it
- [ ] **modprobe**: `modprobe rtl8139` → finds `rtl8139.kmod` in drivers dir; loads it; shows in `lsmod`
- [ ] **dlopen** (when TODO-10 §13 is ready): `dlopen("libfoo.so", RTLD_LAZY)` → returns handle; `dlsym(handle, "foo_fn")` → returns address; call it → correct result; `dlclose` → no crash
- [ ] **PLT lazy binding**: first call to imported symbol via PLT → GOT patched; second call goes direct (verify by checking GOT entry before and after first call)
- [ ] Commit: `"kernel: ELF reloc engine, .kmod loader, EXPORT_SYMBOL, dlopen/PLT, lsmod/insmod/rmmod/modprobe"`
