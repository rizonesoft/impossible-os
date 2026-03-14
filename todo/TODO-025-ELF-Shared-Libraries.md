# P0108 — ELF Dynamic Linker & Kernel Modules

> **Goal:** User-space ELF dynamic linking (`dlopen`/`dlsym`) and the shared ELF relocation
> engine that also underpins kernel modules in `TODO-080-Drivers.md`.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. ELF Shared Libraries (User-Space)

**Prompt:** ELF shared libraries (.so) are loaded at runtime and shared between processes — study the ELF spec sections on PT_DYNAMIC, DT_NEEDED, .dynsym/.dynstr, and GOT/PLT. The dynamic linker parses the executable's DT_NEEDED list, searches the library path (app dir → `C:\Impossible\System\` → `C:\Impossible\System\Drivers\`), loads each .so into memory, applies R_X86_64_64 and R_X86_64_PC32 relocations, and patches the GOT. The `dlopen`/`dlsym`/`dlclose` API enables runtime plugin loading. This feature directly supports Phase 10's compatibility layer where Win32 DLL stubs are loaded similarly. Start by implementing symbol resolution for the current statically-linked kernel, then extend to user-mode binaries. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: dynamic linker (dlopen/dlsym)"`. Update `README.md` if it contains stale or incorrect references to dynamic linking. Create or update documentation in `docs/` covering the dynamic linker, ELF relocation types, GOT/PLT patching, and library search path.

> **Win32 equivalent:** `LoadLibrary` / `GetProcAddress` / `FreeLibrary` — user-mode only.
> **Syscall numbers:** Add `SYS_DLOPEN`, `SYS_DLSYM`, `SYS_DLCLOSE` to `syscall.h` (assign next available numbers after existing syscalls).


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
- [ ] Commit: `"kernel: dynamic linker (dlopen/dlsym)"`

---

## 2. Kernel Modules — See TODO-080-Drivers.md

> **Implemented in [TODO-080-Drivers.md §1](TODO-080-Drivers.md)** — Loadable `.kmod` drivers,
> `EXPORT_SYMBOL`, `module_init`/`module_cleanup`, PCI auto-load, RTL8139 as PoC.
>
> **Dependency:** The ELF relocation engine from §1 above is shared — implement §1 first,
> then reuse the `R_X86_64_*` relocation handler in the kernel module loader.
> Kernel modules are implemented after the Drivers phase when the kernel ABI is stable.

---

## Priority Order

| Priority | Section                        | Reason                                         |
|----------|--------------------------------|------------------------------------------------|
| 🔴 P0     | 1. ELF symbol table + relocs   | Engine needed by both dlopen and kernel modules|
| 🟠 P1     | 1. GOT/PLT patching            | Required for real shared library call dispatch |
| 🟠 P1     | 1. dlopen/dlsym/dlclose        | User-mode plugin loading, Win32 compat layer   |
| 🟡 P2     | 2. Kernel modules              | See TODO-080-Drivers.md — after Drivers phase  |
