---
schema_version: 1
id: kernel-image-module-registry
domain: 02-kernel-core
status: active
title: "TODO-18 -- Kernel Image & Module Registry"
---

# TODO-18 -- Kernel Image & Module Registry

> **Validated:** 2026-07-10 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Goal:** Create the canonical loaded-image registry for the whole OS: kernel image, boot modules, drivers, kernel modules, user images, DLLs, EIF modules, PE sections, symbol tables, unwind metadata, code-integrity decisions, and provenance. Crash dumps, KD, stack walking, hot-patching, code integrity, ETW, and process introspection must all read the same source of truth.

> [!IMPORTANT]
> **Current state:** Crash dump TODO has module-enumeration helpers; binary loader TODO has LDR module list work; EIF has module registration; user-platform SDK owns ELF relocatable `.kmod` loader. These are separate plans. There is no central image registry object, no global address-to-image lookup, no canonical symbol/unwind provider, no provenance record, and no loader-independent image notification.

## Inputs

- [`src/kernel/exec.c`](../../src/kernel/exec.c)
- [`src/kernel/symtab.c`](../../src/kernel/symtab.c)
- [`src/kernel/pe.c`](../../src/kernel/pe.c)
- [`src/kernel/eif.c`](../../src/kernel/eif.c)
- → XREF: [`TODO-17-binary-system.md`](./TODO-17-binary-system.md) -- image loaders and LDR lists
- → XREF: [`TODO-23-exception-dispatch-seh.md`](./TODO-23-exception-dispatch-seh.md) -- unwind and stack walking
- → XREF: [`TODO-27-crash-dump-generation.md`](./TODO-27-crash-dump-generation.md) -- dump module streams
- → XREF: [`TODO-19-code-integrity-trust-policy.md`](./TODO-19-code-integrity-trust-policy.md) -- CI decisions stored here
- → XREF: [`12-user-platform-sdk/TODO-03-elf-reloc-kernel-modules.md`](../12-user-platform-sdk/TODO-03-elf-reloc-kernel-modules.md) -- `.kmod` loader provider

## Outcome

- Every loaded executable image has one `KIMAGE_ENTRY`.
- Address-to-image and address-to-symbol lookup are fast and lock-safe.
- Crash dumps, KD, ETW, exception dispatch, CI, and process APIs enumerate the same image list.
- Image load/unload notifications are delivered through Executive callbacks and kernel notifications.
- Unwind metadata is registered once and reused by SEH, KD, and crash analyzer.
- Provenance includes source path, hash, signer, loader, trust decision, load time, and owning process/session.

## Implementation Order

| ⭐   | Order | Deliverable                             | Depends On        | Status |
| --- | :---: | --------------------------------------- | ----------------- | :----: |
| 💎   |   1   | `KIMAGE_ENTRY` data model               | --                |  [ ]   |
| 💎   |   2   | Global and per-process image registries | T05, T17          |  [ ]   |
| 💎   |   3   | Address range index                     | Ex generic table  |  [ ]   |
| 💎   |   4   | Symbol provider abstraction             | symtab            |  [ ]   |
| 💎   |   5   | Unwind metadata registry                | T23               |  [ ]   |
| 💎   |   6   | Loader integration                      | T17, T20, SDK T05 |  [ ]   |
| ⭐   |   7   | Image notifications and callbacks       | T06, T16          |  [ ]   |
| 💎   |   8   | KD/crash dump integration               | T27, T29          |  [ ]   |
| ⭐   |   9   | Provenance, CI, and hotpatch metadata   | T19               |  [ ]   |
| 💎   |  10   | Tests and consistency verifier          | §1..§9            |  [ ]   |

## 1. `KIMAGE_ENTRY` Data Model

- [ ] Define `KIMAGE_ENTRY`: base, size, type, format, flags, name, full path, section count, timestamp, checksum/hash, signer, CI decision, owner PID, load order, refcount.
- [ ] Include optional pointers for symbols, unwind ranges, exports, imports, relocation info, and debug info.
- [ ] Image types: kernel, HAL/platform, boot module, driver, kmod, process main image, DLL/shared library, EIF module, synthetic/stub.
- [ ] Commit: `"kernel: kimage -- KIMAGE_ENTRY data model"`

**Test checkpoint:** `sizeof(KIMAGE_ENTRY)` matches the `_Static_assert`; a populated entry round-trips every field. Unit test asserts type/format enums are distinct and the flags bitmask is non-overlapping. Test on: QEMU WHPX + TCG; bare metal.

---

## 2. Global and Per-Process Image Registries

- [ ] Global registry owns kernel, boot, driver, and kmod images.
- [ ] Per-process registry owns main image and DLL/shared libraries.
- [ ] Expose lock-safe iteration API with rundown protection.
- [ ] Reconcile PEB Ldr list updates with kernel registry updates.
- [ ] Commit: `"kernel: kimage -- global and per-process registries"`

**Test checkpoint:** Registering N images and iterating returns exactly N under a spinlock/rundown; concurrent register/unregister on two CPUs leaves the count consistent. Serial log shows `"kimage: registered '<name>' (global|pid=<N>)"`. Test on: QEMU WHPX + TCG; bare metal.

---

## 3. Address Range Index

- [ ] Use Executive generic table or interval tree keyed by `[base, end)`.
- [ ] Support `image_lookup_by_address(addr)`.
- [ ] Handle overlapping ranges as fatal loader bugs unless explicitly marked alias mapping.
- [ ] Cache last lookup per CPU for stack walking hot path.
- [ ] Commit: `"kernel: kimage -- address range index and lookup"`

**Test checkpoint:** `image_lookup_by_address(base)` and `(end-1)` return the entry; `(end)` and `(base-1)` return NULL; an overlapping insert is rejected/faults. Per-CPU cache hit on repeated lookup. Test on: QEMU WHPX + TCG; bare metal.

---

## 4. Symbol Provider Abstraction

- [ ] Providers: kernel symtab file, PE export/PDB-lite info, ELF symtab/dynsym, EIF metadata, synthetic stubs.
- [ ] API: `ksym_lookup_addr`, `ksym_lookup_name`, `ksym_iterate_module`.
- [ ] Return symbol name, displacement, source file/line when available.
- [ ] Mark stripped images clearly for crash output.
- [ ] Commit: `"kernel: kimage -- symbol provider abstraction"`

**Test checkpoint:** `ksym_lookup_addr(known_sym+4)` returns the symbol name with displacement 4; a stripped image reports `<no symbols>`; `ksym_lookup_name` round-trips. Test on: QEMU WHPX + TCG; bare metal.

---

## 5. Unwind Metadata Registry

- [ ] Register PE `.pdata`, ELF `.eh_frame`/frame info when available, and EIF unwind metadata.
- [ ] Provide range lookup for the exception-dispatch unwind engine (T23).
- [ ] Validate unwind ranges are inside executable image sections.
- [ ] Include unwind table hash in image provenance.
- [ ] Commit: `"kernel: kimage -- unwind metadata registry"`

**Test checkpoint:** An unwind range lookup for an address inside a registered image returns the `.pdata`/`.eh_frame` entry; a range outside executable sections is rejected at registration. Test on: QEMU WHPX + TCG; bare metal.

---

## 6. Loader Integration

- [ ] `exec_load`, PE loader, ELF loader, EIF loader, and kmod loader call `kimage_register`.
- [ ] Unload paths call `kimage_unregister` after rundown.
- [ ] Existing crash module enumeration migrates to this registry.
- [ ] Load failures must not leave partial image entries.
- [ ] Commit: `"kernel: kimage -- loader integration"`

**Test checkpoint:** Loading an EIF/PE image registers exactly one `KIMAGE_ENTRY`; a failed load leaves zero entries (atomic); the migrated crash-module enumeration returns the same set. Serial log shows `"kimage: registered '<name>'"` on load. Test on: QEMU WHPX + TCG; bare metal.

---

## 7. Image Notifications and Callbacks

- [ ] Publish `ImageLoad` and `ImageUnload` through Executive callback object.
- [ ] Publish notification state through the crash-dump/diagnostics path (T27) for service/user consumers.
- [ ] Include CI decision and trust level.
- [ ] Ensure callbacks run after registry insertion but before user entrypoint for user images.
- [ ] Commit: `"kernel: kimage -- image load/unload notifications"`

**Test checkpoint:** A registered callback fires on image load with the correct base/name/CI-decision, after insertion (lookup succeeds inside the callback) and before the user entrypoint. Unload fires the ImageUnload callback. Test on: QEMU WHPX + TCG; bare metal.

---

## 8. KD/Crash Dump Integration

- [ ] KD module list reads global/per-process registry.
- [ ] Crash dumps emit module stream from `KIMAGE_ENTRY`.
- [ ] Panic screen "what failed" uses address-to-image lookup.
- [ ] `dmpanalyze.exe` consumes same serialized format.
- [ ] Commit: `"kernel: kimage -- KD and crash dump integration"`

**Test checkpoint:** The crash-dump module stream lists every registered image once; the panic screen resolves a faulting RIP to `<image>+<offset>` via `image_lookup_by_address`. Test on: QEMU WHPX + TCG; bare metal.

---

## 9. Provenance, CI, and Hotpatch Metadata

- [ ] Store load source: bootloader, VFS path, memory buffer, generated/stub.
- [ ] Store CI hash, signer, policy mode, decision, and audit ID.
- [ ] Reserve hotpatch metadata fields: patchable functions, original bytes hash, active patch ID.
- [ ] Refuse unload while active stack frames reference image unless forced crash-only teardown.
- [ ] Commit: `"kernel: kimage -- provenance, CI, and hotpatch metadata"`

**Test checkpoint:** A registered image records its load source + CI decision + signer; unregister is refused (returns busy) while the refcount indicates active frames, and succeeds after rundown. Test on: QEMU WHPX + TCG; bare metal.

---

## 10. Tests and Consistency Verifier

- [ ] Unit tests: register/unregister, overlap reject, addr lookup, symbol lookup, unwind lookup, per-process iteration.
- [ ] Boot verifier: kernel image, symtab, and loaded drivers all appear exactly once.
- [ ] Fuzz malformed image metadata; no registry leaks.
- [ ] Bulletproofing: PEB Ldr list count matches kernel per-process registry for each process.
- [ ] Commit: `"kernel: kimage -- tests and consistency verifier"`

**Test checkpoint:** All `test_kimage` assertions pass (register/unregister, overlap reject, addr/symbol/unwind lookup, per-process iteration); the boot verifier confirms kernel + symtab + drivers appear exactly once and the PEB-Ldr-vs-registry count matches per process. Test on: QEMU WHPX + TCG; bare metal.

---

## OS Comparison

| ⭐   | Feature                    | 🪟 Win11                       | 🐧 Linux                    | 🚀 Impossible OS                     |
| --- | -------------------------- | ----------------------------- | -------------------------- | ----------------------------------- |
| 💎   | Loaded-image registry      | ✅ PsLoadedModuleList          | ✅ /proc/modules + vmap     | 🔄 §1-§2 KIMAGE_ENTRY registries     |
| 💎   | Address-to-image lookup    | ✅ KLDR range tables           | ✅ __module_address         | 🔄 §3 interval index + per-CPU cache |
| 💎   | Symbol resolution          | ✅ DbgHelp/PDB                 | ✅ kallsyms                 | 🔄 §4 provider abstraction           |
| 💎   | Unwind metadata registry   | ✅ RtlLookupFunctionEntry      | ⚠️ .eh_frame per-object    | 🔄 §5 unified .pdata/.eh_frame/EIF   |
| ⭐   | Format-agnostic image list | ⚠️ PE only                    | ⚠️ ELF only                | 🔄 §1-§6 PE + ELF + EIF + kmod       |
| 💎   | Image load/unload notify   | ✅ PsSetLoadImageNotifyRoutine | ⚠️ module notifier chain   | 🔄 §7 Executive callbacks            |
| 💎   | Crash-dump module stream   | ✅ MINIDUMP module list        | ✅ ELF core NT_FILE         | 🔄 §8 KIMAGE_ENTRY module stream     |
| ⭐   | Image provenance + CI      | ⚠️ CI.dll separate            | ⚠️ IMA/module sig separate | 🔄 §9 unified provenance record      |
| ⭐   | Hotpatch metadata slot     | ✅ hotpatch pointers           | ⚠️ livepatch separate      | ⬜ §9 reserved fields                |

> **After §1-§6:** a single loader-independent image registry (PE/ELF/EIF/kmod) with fast address/symbol/unwind lookup that every subsystem shares.
> **After §7-§9:** load notifications, crash-dump/KD module streams, and unified provenance/CI/hotpatch metadata -- one source of truth Windows and Linux split across several subsystems.

---

## Unit Tests

Create `src/kernel/test/test_kimage.c`, register via `test_register_kimage()` in `test_runner.c` under `TEST_CAT_EXEC` (the registry is populated by the exec/loader path). Concrete assertions:

- `test_kimage_entry_size`: `sizeof(KIMAGE_ENTRY)` == static-assert value; type/format enums distinct.
- `test_kimage_register_iterate`: register 3 images, iterate returns 3; unregister 1, iterate returns 2.
- `test_kimage_addr_lookup`: `image_lookup_by_address(base)` and `(end-1)` return the entry; `(end)`/`(base-1)` return NULL.
- `test_kimage_overlap_reject`: inserting an overlapping `[base,end)` returns an error (no silent replace).
- `test_kimage_symbol_lookup`: `ksym_lookup_addr(sym+4)` returns name + displacement 4; stripped image returns `<no symbols>`.
- `test_kimage_unwind_lookup`: unwind range lookup inside a registered image returns its entry; out-of-section range rejected.
- `test_kimage_per_process_iterate`: per-process registry count matches inserted DLL count.

> **Note:** these tests use in-memory `KIMAGE_ENTRY` fixtures and pure registry helpers; they MUST NOT call live loaders (`exec_load`, `pe_load`, `eif_load`) or boot infrastructure (per the test-side-effect policy).

---

## Verification

- [ ] `bash scripts/build.sh` shows `=== BUILD OK ===`.
- [ ] `bash scripts/test.sh SUITE=exec` passes with the new `test_kimage` assertions.
- [ ] Boot verifier: serial log confirms kernel image + symtab + each loaded driver appears exactly once in the registry, and the PEB Ldr count matches the per-process registry.
- [ ] `bash scripts/test-smoke.sh` shows `SMOKE TEST PASSED` (loader-path changes).

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | N suites, 0 failures
