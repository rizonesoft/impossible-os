# P0108 — ELF Dynamic Linker & Kernel Modules

> **Goal:** ELF relocation engine for **kernel modules** and the **Linux compatibility
> layer** (TODO-540). The native user-space binary format is PE (.exe/.dll) —
> see `TODO-510-Native-Win32.md`. ELF support exists for: (1) kernel `.kmod`
> drivers, and (2) running Linux ELF binaries via the compat layer.

> [!IMPORTANT]
> **Impossible OS is natively Win32/PE.** The kernel is ELF (loaded by UEFI),
> and kernel modules use ELF relocations, but all user-space programs use PE.
> `dlopen`/`dlsym` are provided for the Linux compatibility layer; the native
> equivalents are `LoadLibrary`/`GetProcAddress` (PE DLL loading).
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. ELF Relocation Engine & Dynamic Linker

**Prompt:** The ELF relocation engine is used by two subsystems: (1) **kernel module loading** (`.kmod` files in `TODO-063-Drivers.md`) which uses ELF relocations to link driver code against kernel symbols, and (2) the **Linux compatibility layer** (`TODO-540-Linux.md`) which runs ELF binaries and loads `.so` shared libraries. Implement: ELF symbol table scanning, relocation types (`R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_PLT32`, `R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`), GOT/PLT patching for lazy binding, and `dlopen`/`dlsym`/`dlclose` API for the Linux compat layer. The native PE equivalents (`LoadLibrary`/`GetProcAddress`) are implemented in `TODO-510-Native-Win32.md §4`. Start by implementing the relocation engine for kernel modules, then extend to user-mode ELF compat. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: ELF relocation engine + dlopen"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> **Native PE equivalent:** `LoadLibrary` / `GetProcAddress` / `FreeLibrary` — the
> primary DLL loading API (see `TODO-510-Native-Win32.md §4.4`).
> `dlopen`/`dlsym` are provided for Linux compat only.
> **Syscall numbers:** Add `SYS_DLOPEN`, `SYS_DLSYM`, `SYS_DLCLOSE` to `syscall.h` (Linux compat layer syscalls).
- [ ] Implement ELF `.symtab`/`.dynsym` symbol table scanner
- [ ] Implement ELF section loader: `.text`, `.data`, `.bss`, `.rodata` into process address space
- [ ] Implement ELF relocations (`R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_PLT32`, `R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`) — **this engine is reused by kernel modules**
- [ ] Implement GOT/PLT patching for lazy binding (PLT stubs call dynamic linker on first call)
- [ ] Implement `dlopen(path, flags)` — load `.so` into memory, apply relocations
- [ ] Implement `dlsym(handle, symbol)` — look up exported symbol address
- [ ] Implement `dlclose(handle)` — decrement ref count, unload if zero
- [ ] Track loaded libraries per-process (ref-counted handles)
- [ ] Define library search path: app dir → `C:\Impossible\System\` → `C:\Impossible\System\Drivers\`
- [ ] Add `SYS_DLOPEN`, `SYS_DLSYM`, `SYS_DLCLOSE` syscalls
- [ ] Test: build a minimal `.so` with one exported function, `dlopen` it, `dlsym` the function, call it
- [ ] Commit: `"kernel: ELF relocation engine + dlopen"`

---

## 2. Kernel Modules — See TODO-063-Drivers.md

> **Implemented in [TODO-063-Drivers.md §1](../060-Hardware-Drivers/TODO-063-Drivers.md)** — Loadable `.kmod` drivers,
> `EXPORT_SYMBOL`, `module_init`/`module_cleanup`, PCI auto-load, RTL8139 as PoC.
>
> **Dependency:** The ELF relocation engine from §1 above is shared — implement §1 first,
> then reuse the `R_X86_64_*` relocation handler in the kernel module loader.
> Kernel modules are implemented after the Drivers phase when the kernel ABI is stable.

---

## 3. Symbol Versioning

**Prompt:** Without symbol versioning, upgrading a shared library can silently break existing binaries if a function's ABI changes. Linux uses GNU Symbol Versioning (`.gnu.version`, `.gnu.version_d`, `.gnu.version_r`); Windows uses DLL version manifests (`<dependentAssembly>` in SxS). Impossible OS shared libraries should embed a version tag per exported symbol: `symbol@LIBNAME_1.0`. The dynamic linker checks the required version matches the provided version at bind time. This protects against ABI breakage as system libraries evolve. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: ELF symbol versioning"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> **Prerequisite:** §1 dynamic linker must be complete.
- [ ] Parse `.gnu.version_d` (version definitions in .so) and `.gnu.version_r` (required versions in executable)
- [ ] At bind time: check required version tag matches provided tag for each symbol
- [ ] On version mismatch: fail load with clear error (`"libfoo.so: symbol bar@LIBFOO_2.0 not found, need LIBFOO_1.0"`)
- [ ] `nm --dynamic` equivalent shell command: list exported symbols with versions from a `.so`
- [ ] Commit: `"kernel: ELF symbol versioning"`

---

## 4. Library Cache (`ldcache`)

**Prompt:** Scanning the entire library search path on every `dlopen` is slow. Linux's `ldconfig` pre-builds `/etc/ld.so.cache` — a sorted list of all library names and their paths. Impossible OS should maintain `C:\Impossible\System\ldcache.bin` — a flat binary index of (library name → path) pairs. The dynamic linker checks this cache first before scanning directories. `ldcache --rebuild` regenerates it by scanning the library dirs. Automatically regenerated when `install` or `uninstall` drops/removes a `.so` file. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: dynamic linker cache"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> **Beats:** Windows DLL lookup is PATH-based with no cache — slow on cold start. Linux ldcache requires root to rebuild. Impossible OS can rebuild per-user.
- [ ] Define `ldcache.bin` format: `[count][name_len][name][path_len][path]...` entries
- [ ] Dynamic linker: check cache before directory scan
- [ ] Implement `ldcache_lookup(name, out_path)` — O(log n) binary search after sort
- [ ] `ldcache --rebuild` shell command: scan library dirs, write sorted cache file
- [ ] Call `ldcache --rebuild` automatically after install/uninstall
- [ ] Commit: `"kernel: dynamic linker cache"`

---

## Priority Order

| Priority | Section                        | Reason                                          |
|----------|--------------------------------|-------------------------------------------------|
| 🔴 P0    | 1. ELF symbol table + relocs   | Engine needed by kernel modules (TODO-080)      |
| 🟠 P1    | 1. GOT/PLT patching            | Required for shared library dispatch (Linux compat) |
| 🟡 P2    | 1. dlopen/dlsym/dlclose        | Linux compat layer (TODO-540) — PE `LoadLibrary` is native |
| 🟡 P2    | 2. Kernel modules              | See TODO-063-Drivers.md — after Drivers phase   |
| 🟡 P2    | 3. Symbol versioning           | ABI stability for system libraries              |
| 🟡 P2    | 4. Library cache               | Performance — fast dlopen cold start            |

---

## OS Comparison

| Feature                    | 🪟 Windows 11 (DLL)     | 🐧 Linux (ELF .so)        | 🚀 Impossible OS               |
| -------------------------- | ---------------------- | ------------------------ | ----------------------------- |
| Runtime dynamic loading    | ✅ `LoadLibrary`        | ✅ `dlopen`               | ⬜ §1 P0                       |
| Symbol lookup              | ✅ `GetProcAddress`     | ✅ `dlsym`                | ⬜ §1 P0                       |
| GOT / PLT lazy binding     | ✅ Import Address Table | ✅ GOT/PLT                | ⬜ §1 P1                       |
| Kernel module loading      | ✅ Kernel driver (WDM)  | ✅ `insmod`/`depmod`      | ⬜ §2 (TODO-080)               |
| Symbol versioning          | ✅ SxS manifests        | ✅ GNU symbol versioning  | ⬜ §3 P2                       |
| Library path cache         | ❌ PATH scan            | ✅ `/etc/ld.so.cache`     | ⬜ §4 P2 — **user-rebuilable** |
| **Per-user library cache** | ❌                      | ❌ Root only (`ldconfig`) | ⬜ **§4 — Impossible OS only** |
