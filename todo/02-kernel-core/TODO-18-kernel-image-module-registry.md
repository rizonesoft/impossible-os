# TODO-18 -- Kernel Image & Module Registry

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

| ⭐ | Order | Deliverable | Depends On | Status |
| -- | :---: | ----------- | ---------- | :----: |
| 💎 | 1 | `KIMAGE_ENTRY` data model | -- | [ ] |
| 💎 | 2 | Global and per-process image registries | T05, T17 | [ ] |
| 💎 | 3 | Address range index | Ex generic table | [ ] |
| 💎 | 4 | Symbol provider abstraction | symtab | [ ] |
| 💎 | 5 | Unwind metadata registry | T23 | [ ] |
| 💎 | 6 | Loader integration | T17, T20, SDK T05 | [ ] |
| ⭐ | 7 | Image notifications and callbacks | T06, T16 | [ ] |
| 💎 | 8 | KD/crash dump integration | T27, T29 | [ ] |
| ⭐ | 9 | Provenance, CI, and hotpatch metadata | T19 | [ ] |
| 💎 | 10 | Tests and consistency verifier | §1..§9 | [ ] |

## 1. `KIMAGE_ENTRY` Data Model

- [ ] Define `KIMAGE_ENTRY`: base, size, type, format, flags, name, full path, section count, timestamp, checksum/hash, signer, CI decision, owner PID, load order, refcount.
- [ ] Include optional pointers for symbols, unwind ranges, exports, imports, relocation info, and debug info.
- [ ] Image types: kernel, HAL/platform, boot module, driver, kmod, process main image, DLL/shared library, EIF module, synthetic/stub.

## 2. Global and Per-Process Image Registries

- [ ] Global registry owns kernel, boot, driver, and kmod images.
- [ ] Per-process registry owns main image and DLL/shared libraries.
- [ ] Expose lock-safe iteration API with rundown protection.
- [ ] Reconcile PEB Ldr list updates with kernel registry updates.

## 3. Address Range Index

- [ ] Use Executive generic table or interval tree keyed by `[base, end)`.
- [ ] Support `image_lookup_by_address(addr)`.
- [ ] Handle overlapping ranges as fatal loader bugs unless explicitly marked alias mapping.
- [ ] Cache last lookup per CPU for stack walking hot path.

## 4. Symbol Provider Abstraction

- [ ] Providers: kernel symtab file, PE export/PDB-lite info, ELF symtab/dynsym, EIF metadata, synthetic stubs.
- [ ] API: `ksym_lookup_addr`, `ksym_lookup_name`, `ksym_iterate_module`.
- [ ] Return symbol name, displacement, source file/line when available.
- [ ] Mark stripped images clearly for crash output.

## 5. Unwind Metadata Registry

- [ ] Register PE `.pdata`, ELF `.eh_frame`/frame info when available, and EIF unwind metadata.
- [ ] Provide range lookup for TODO-23 unwind engine.
- [ ] Validate unwind ranges are inside executable image sections.
- [ ] Include unwind table hash in image provenance.

## 6. Loader Integration

- [ ] `exec_load`, PE loader, ELF loader, EIF loader, and kmod loader call `kimage_register`.
- [ ] Unload paths call `kimage_unregister` after rundown.
- [ ] Existing crash module enumeration migrates to this registry.
- [ ] Load failures must not leave partial image entries.

## 7. Image Notifications and Callbacks

- [ ] Publish `ImageLoad` and `ImageUnload` through Executive callback object.
- [ ] Publish notification state through TODO-27 for service/user consumers.
- [ ] Include CI decision and trust level.
- [ ] Ensure callbacks run after registry insertion but before user entrypoint for user images.

## 8. KD/Crash Dump Integration

- [ ] KD module list reads global/per-process registry.
- [ ] Crash dumps emit module stream from `KIMAGE_ENTRY`.
- [ ] Panic screen "what failed" uses address-to-image lookup.
- [ ] `dmpanalyze.exe` consumes same serialized format.

## 9. Provenance, CI, and Hotpatch Metadata

- [ ] Store load source: bootloader, VFS path, memory buffer, generated/stub.
- [ ] Store CI hash, signer, policy mode, decision, and audit ID.
- [ ] Reserve hotpatch metadata fields: patchable functions, original bytes hash, active patch ID.
- [ ] Refuse unload while active stack frames reference image unless forced crash-only teardown.

## 10. Tests and Consistency Verifier

- [ ] Unit tests: register/unregister, overlap reject, addr lookup, symbol lookup, unwind lookup, per-process iteration.
- [ ] Boot verifier: kernel image, symtab, and loaded drivers all appear exactly once.
- [ ] Fuzz malformed image metadata; no registry leaks.
- [ ] Bulletproofing: PEB Ldr list count matches kernel per-process registry for each process.

