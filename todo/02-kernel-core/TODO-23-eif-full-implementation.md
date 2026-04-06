# TODO-23 -- EIF Full Implementation

> **Goal:** Complete the Executable Impossible Format (EIF) from its current basic loader to a production-quality native binary format. The basic loader (TODO-08 §5) validates headers, copies segments, and builds import dispatch tables. This TODO fills the gaps: segment permission enforcement, ASLR, API version gating, metadata parsing, LZ4 decompression, range overlap validation, module registration, and optional import stubs. When done, EIF is the fastest, most secure native binary format on any OS.

> [!IMPORTANT]
> **Current state:** `eif_load()` in `src/kernel/eif.c` loads segments via identity mapping, validates imports against SSDT range, writes a per-process dispatch table at 0x8F0000, and measures load time. Missing: segment R/W/X enforcement, ASLR, API version check, metadata parsing, LZ4 decompression, module registration, overlap validation.
> EIF spec lives at `docs/specs/eif-format.md`. Code signing is tracked separately in TODO-08 §17. The `elf2eif` converter is tracked in TODO-08 §13.

## Inputs

- [`src/kernel/eif.c`](../../src/kernel/eif.c) -- current EIF loader (basic segments + imports)
- [`include/kernel/eif.h`](../../include/kernel/eif.h) -- EIF structures and constants
- [`docs/specs/eif-format.md`](../../docs/specs/eif-format.md) -- normative EIF specification
- [`src/kernel/exec.c`](../../src/kernel/exec.c) -- exec dispatcher, module registration API
- [`include/kernel/exec.h`](../../include/kernel/exec.h) -- `loaded_module_t`, `exec_register_module()`
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) -- `vmm_map_page()`, `vmm_protect()`
- [`src/kernel/mm/pmm.c`](../../src/kernel/mm/pmm.c) -- `pmm_alloc_frame()`
- -> XREF: `TODO-08-binary-system.md §5` -- basic EIF loader (FOUNDATION, complete)
- -> XREF: `TODO-08-binary-system.md §13` -- elf2eif converter (COMPLEMENT, not started)
- -> XREF: `TODO-08-binary-system.md §17` -- EIF code signing (COMPLEMENT, not started)
- -> XREF: `TODO-08-binary-system.md §15` -- ASLR for all formats (COMPLEMENT, not started)
- -> XREF: `TODO-08-binary-system.md §6` -- module list registration (FOUNDATION, complete)
- -> XREF: `TODO-20-kernel-libraries.md` -- LZ4 decompressor (FOUNDATION, needed for §6)
- -> XREF: `TODO-05-native-api-ssdt.md` -- SSDT service table (FOUNDATION, partial)

## Outcome

- EIF segments mapped with correct R/W/X permissions via PTE flags (NX on data, RO on rodata).
- EIF binaries with `load_base=0` get ASLR-randomized load addresses.
- `api_version` field checked against running OS API version; rejects too-new binaries.
- Metadata section parsed; `"name"`, `"version"`, `"min_os"` exposed to process info.
- LZ4-compressed segments (`EIF_FLAG_COMPRESSED`) transparently decompressed at load time.
- File range overlap validation per spec normative rule 2.
- Every loaded EIF registered via `exec_register_module()` for crash dumps and debugger.
- Optional imports resolved to deterministic `STATUS_NOT_IMPLEMENTED` stubs.
- Per-process dispatch table properly isolated when per-process page tables exist.

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On              | Status |
| --- | :---: | ---------------------------------------------- | ----------------------- | :----: |
| ⭐  |   1   | Range overlap validation (normative rule 2)    | --                      |  [ ]   |
| 💎  |   2   | Segment permission enforcement (R/W/X PTE)     | §1                      |  [ ]   |
| ⭐  |   3   | Module registration for EIF                    | T08 §6                  |  [ ]   |
| ⭐  |   4   | API version gating                             | §1                      |  [ ]   |
| ⭐  |   5   | Metadata section parser                        | §1                      |  [ ]   |
| ⭐  |   6   | LZ4 compressed segments                        | §2, T20                 |  [ ]   |
| ⭐  |   7   | Optional import stubs                          | §3                      |  [ ]   |
| ⭐  |   8   | EIF ASLR (load_base=0 randomization)           | §2, T08 §15             |  [ ]   |
| ⭐  |   9   | Per-process dispatch table isolation            | §3                      |  [ ]   |

> 💎 = parity -- matches a capability Windows PE and Linux ELF both have.
> ⭐ = exclusive -- Impossible OS native format superiority.

---

## 1. Range Overlap Validation (Normative Rule 2)

The spec mandates `segment_offset < import_offset < signature_offset < metadata_offset` and that all ranges must be non-overlapping. The current loader checks individual bounds but not inter-range ordering or overlap.

- [ ] In `eif_validate()`: after individual bounds checks, verify ordering: `segment_table_end <= import_offset` (when imports exist), `import_table_end <= signature_offset` (when signed), `signature_end <= metadata_offset` (when metadata exists)
- [ ] Verify no segment data range overlaps another segment's data range
- [ ] Verify segment data ranges don't overlap the segment table, import table, signature, or metadata
- [ ] Reject files violating any overlap constraint with `ENOEXEC` and descriptive log message
- [ ] Commit: `"kernel: eif -- normative range overlap validation"`

**Test checkpoint:** An EIF with overlapping segment data ranges is rejected with `"eif: range overlap"` in serial log. An EIF with import_offset < segment_table_end is rejected. Valid EIF with correct ordering still loads. `POST16(0xDE20)` on entry, `POST16(0xDE21)` after validation. Test on: QEMU WHPX + TCG; bare metal.

## 2. Segment Permission Enforcement (R/W/X via PTE)

The current loader copies segment data but does not set page permissions. All pages remain kernel RW. Segments should have proper NX (no-execute) on data and RO (read-only) on rodata.

- [ ] After segment copy, compute page-aligned range for each segment
- [ ] For `EIF_SEG_READ` only (rodata): `vmm_protect_range(vaddr, mem_size, VMM_USER_RO | VMM_FLAG_NX)`
- [ ] For `EIF_SEG_READ | EIF_SEG_WRITE` (data/BSS): `vmm_protect_range(vaddr, mem_size, VMM_USER_RW | VMM_FLAG_NX)`
- [ ] For `EIF_SEG_READ | EIF_SEG_EXEC` (text): `vmm_protect_range(vaddr, mem_size, VMM_USER_RO)` (executable, not writable)
- [ ] Reject segments with `EIF_SEG_WRITE | EIF_SEG_EXEC` (W+X) -- security policy: no writable+executable pages
- [ ] Commit: `"kernel: eif -- segment permission enforcement via PTE flags"`

**Test checkpoint:** EIF `.text` segment is executable but not writable. EIF `.data` segment is writable but not executable. Attempt to execute from `.data` causes #PF. Attempt to write to `.text` causes #PF. Serial log shows `"eif: segment 0: R-X"`, `"eif: segment 1: RW-"`. `POST16(0xDE22)` on entry, `POST16(0xDE23)` after permissions set. Test on: QEMU WHPX + TCG; bare metal.

**Regression risk:** Changing page permissions after segment copy may break if segments span 2 MiB page boundaries that aren't split yet. `vmm_protect_range` handles splitting, but verify on bare metal. If broken, revert permission changes and keep segments RW until per-process VMM is complete.

## 3. Module Registration for EIF

Every loaded binary must be registered in the crash registry and module list (TODO-08 §6). The current EIF loader does not call `exec_register_module()`.

- [ ] After successful segment load, build a `loaded_module_t` with: `base_address = load_base` (or first segment vaddr), `size_of_image` = span from lowest to highest segment, `entry_point = load_base + entry_point`, `format = EXEC_FMT_EIF`, `name` from metadata `"name"` key (if parsed) or task name
- [ ] Call `exec_register_module(NULL, &mod)` from `eif_load()`
- [ ] Remove the EIF-specific module registration in `task_exec()` (it currently registers all formats with the ELF range -- EIF should register itself with its actual load range)
- [ ] Commit: `"kernel: eif -- module registration via exec_register_module"`

**Test checkpoint:** After loading an EIF, `exec_find_module_by_pc(entry_va)` returns the EIF module with correct base and `EXEC_FMT_EIF`. Serial log shows `"exec: registered module '<name>' at 0x<base>"`. `POST16(0xDE24)` on entry, `POST16(0xDE25)` after registration. Test on: QEMU WHPX + TCG; bare metal.

## 4. API Version Gating

The `api_version` field in the EIF header specifies the minimum OS API version required. Currently parsed but never checked.

- [ ] Define `EIF_CURRENT_API_VERSION` in `eif.h` (start at 1)
- [ ] In `eif_validate()`: if `hdr->api_version > EIF_CURRENT_API_VERSION`, reject with `"eif: binary requires API version %u, OS provides %u"`
- [ ] Log accepted API version at DEBUG level
- [ ] Commit: `"kernel: eif -- API version gating"`

**Test checkpoint:** EIF with `api_version=1` loads successfully. EIF with `api_version=99` is rejected with `"eif: binary requires API version 99, OS provides 1"` in serial log. `POST16(0xDE26)` on entry. Test on: QEMU WHPX + TCG; bare metal.

## 5. Metadata Section Parser

The spec defines a key-value metadata section (name, version, author, icon, min_os). Currently the `metadata_offset` field is parsed but the section is never read.

- [ ] Define `eif_metadata_t` struct in `eif.h`: `{ char name[64]; char version[32]; char author[64]; char min_os[16]; }` -- parsed from the key-value pairs
- [ ] Implement `eif_parse_metadata(data, size, metadata_offset)`: walk key-value pairs (`key_len + key + val_len + val`, terminated by `key_len==0`), populate `eif_metadata_t`
- [ ] Bounds-check every key_len and val_len against remaining file size
- [ ] Log parsed metadata: `"eif: name='%s' version='%s' author='%s'"`
- [ ] Store metadata in `loaded_module_t.name` field for crash registry visibility
- [ ] Commit: `"kernel: eif -- metadata section parser"`

**Test checkpoint:** EIF with metadata section shows `"eif: name='hello' version='1.0'"` in serial log. EIF with truncated metadata (key_len points past EOF) is rejected. EIF without metadata (`metadata_offset=0`) loads normally. `POST16(0xDE28)` on entry, `POST16(0xDE29)` after parse. Test on: QEMU WHPX + TCG; bare metal.

## 6. LZ4 Compressed Segments

The spec reserves `EIF_FLAG_COMPRESSED` for LZ4-compressed segment data. This enables smaller binaries on disk with transparent decompression at load time.

- [ ] Depends on: LZ4 decompressor in kernel (-> XREF TODO-20)
- [ ] If `EIF_FLAG_COMPRESSED` is set: for each segment, `file_size` is the compressed size; `mem_size` is the uncompressed size
- [ ] Allocate a temporary kernel buffer (`kmalloc(file_size)`), copy compressed data, decompress into target vaddr, free temp buffer
- [ ] Validate decompressed size matches `mem_size`; reject on mismatch
- [ ] If LZ4 library not available: reject compressed EIF with `"eif: LZ4 decompression not available"`
- [ ] Commit: `"kernel: eif -- LZ4 compressed segment loading"`

**Test checkpoint:** An EIF with `COMPRESSED` flag and LZ4-compressed `.text` segment loads and executes correctly. Decompressed size matches `mem_size`. Serial log shows `"eif: decompressed segment 0: %u -> %u bytes"`. Corrupt compressed data is rejected. `POST16(0xDE2A)` on entry, `POST16(0xDE2B)` after decompress. Test on: QEMU WHPX + TCG; bare metal.

## 7. Optional Import Stubs

Spec normative rule 5: skipped optional imports MUST have their dispatch table entry set to a deterministic stub returning `STATUS_NOT_IMPLEMENTED`.

- [ ] In the dispatch table write pass: for optional imports where `available=0`, set the dispatch entry to point to a kernel-provided stub function that returns `STATUS_NOT_IMPLEMENTED` (0xC0000002)
- [ ] The stub address should be a well-known kernel page mapped read-only into user space
- [ ] User code calling an unavailable optional import gets a clean `STATUS_NOT_IMPLEMENTED` return instead of a crash
- [ ] Commit: `"kernel: eif -- optional import stub generation"`

**Test checkpoint:** An EIF importing an unregistered optional SSDT entry gets `STATUS_NOT_IMPLEMENTED` (0xC0000002) return value, not a crash. Serial log shows `"eif: optional import 0x%x stubbed"`. `POST16(0xDE2C)` on entry. Test on: QEMU WHPX + TCG; bare metal.

## 8. EIF ASLR (load_base=0 Randomization)

When `load_base=0`, the EIF binary is position-independent and should be loaded at a randomized address for security. This integrates with the broader ASLR work in TODO-08 §15.

- [ ] If `load_base == 0`: generate a random base address using the kernel CSPRNG (-> XREF TODO-20 Monocypher/RDRAND)
- [ ] Random base must be page-aligned, within the user address range, and not overlapping existing mappings
- [ ] Update all segment vaddr calculations to use the randomized base
- [ ] Update dispatch table base address calculation
- [ ] Log the randomized base: `"eif: ASLR base=0x%x"`
- [ ] Commit: `"kernel: eif -- ASLR for position-independent binaries"`

**Test checkpoint:** Two consecutive loads of the same PIC EIF binary (`load_base=0`) produce different base addresses. Serial log shows different `"eif: ASLR base=0x%x"` values. Non-PIC EIF (`load_base!=0`) loads at its preferred address. `POST16(0xDE2E)` on entry. Test on: QEMU WHPX + TCG; bare metal.

## 9. Per-Process Dispatch Table Isolation

Currently the EIF dispatch table at 0x8F0000 is identity-mapped and shared. When per-process page tables exist, each process should have its own dispatch table backed by unique physical pages.

- [ ] Allocate physical frames for the dispatch table pages via `pmm_alloc_frame()`
- [ ] Map at EIF_DISPATCH_TABLE_ADDR in the per-process PML4 (not kernel PML4)
- [ ] Write dispatch entries into the per-process pages
- [ ] Set pages as User + Read-Only (dispatch table should not be writable by user code)
- [ ] Commit: `"kernel: eif -- per-process dispatch table isolation"`

**Test checkpoint:** Two EIF processes with different imports see different dispatch table contents at 0x8F0000. Writing to the dispatch table from user mode causes #PF. Serial log shows `"eif: dispatch table mapped at 0x8F0000 (per-process)"`. `POST16(0xDE30)` on entry. Test on: QEMU WHPX + TCG; bare metal.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                | 🐧 Linux               | 🚀 Impossible OS              |
|----|----------------------------|------------------------|------------------------|----------------------------------|
| 💎 | Segment permissions        | ✅ PE section chars    | ✅ ELF p_flags         | ⬜ Planned -- §2               |
| 💎 | ASLR                       | ✅ Mandatory           | ✅ PIE + kernel        | ⬜ Planned -- §8               |
| 💎 | Code signing enforcement   | ✅ Authenticode/WHCP   | ✅ IMA/EVM             | ⬜ T08 §17                     |
| ⭐ | < 10 us load time          | ❌ ~100 us (PE IAT)    | ❌ ~50 us (ELF PLT)    | ✅ §5 done (uptime_ns)         |
| ⭐ | Integer-only imports       | ❌ String-based IAT    | ❌ String-based PLT    | ✅ §5 done (SSDT dispatch)     |
| ⭐ | API version gating         | ⚠️ Subsystem version   | ❌ No ABI version      | ⬜ Planned -- §4               |
| ⭐ | Built-in metadata          | ⚠️ RT_VERSION resource | ⚠️ .note sections      | ⬜ Planned -- §5               |
| ⭐ | LZ4 compressed segments    | ❌ Not supported       | ❌ Not supported       | ⬜ Planned -- §6               |
| ⭐ | Mandatory overlap checks   | ⚠️ Partial bounds      | ⚠️ Partial bounds      | ⬜ Planned -- §1               |
| ⭐ | Optional import stubs      | ❌ DLL not found crash | ❌ dlopen NULL crash   | ⬜ Planned -- §7               |

> **After §1-§3:** EIF has proper security (permissions + range validation) and crash-dump visibility.
> **After §4-§5:** EIF binaries carry structured metadata and version-gate against the OS API level.
> **After §6:** EIF binaries can be smaller on disk than ELF or PE with transparent LZ4 decompression.
> **After §7-§9:** EIF is fully production-ready with ASLR, import stubs, and per-process isolation.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_eif()` -- register in `src/kernel/test/test_runner.c`.
> Tests run with `debug=1` or `test=1` in boot.conf. Use `TEST_CAT_EXEC` category.

- [ ] Create `src/kernel/test/test_eif.c` with:
  - Overlap validation: overlapping segment data ranges rejected
  - Overlap validation: valid non-overlapping EIF accepted
  - API version: `api_version=1` accepted, `api_version=99` rejected
  - Metadata parser: valid key-value pairs parsed correctly, `name` field populated
  - Metadata parser: truncated metadata (key_len past EOF) rejected
  - Metadata parser: `metadata_offset=0` accepted (no metadata)
  - Struct sizes: `sizeof(eif_header_t)==64`, `sizeof(eif_segment_t)==32`, `sizeof(eif_import_t)==8`
  - Module registration: after `eif_load`, `exec_find_module_by_pc(entry)` finds module with `EXEC_FMT_EIF`
- [ ] Move existing EIF tests from `test_exec.c` into `test_eif.c` for consolidation
- [ ] Register in `test_runner_init()`: `test_register_eif()`
- [ ] Commit: `"test: add EIF full implementation test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] EIF binary loads with correct segment permissions (R-X text, RW- data)
- [ ] EIF with `api_version` > current is rejected cleanly
- [ ] EIF with metadata shows parsed name/version in serial log
- [ ] EIF module appears in crash registry via `exec_find_module_by_pc`
- [ ] Two PIC EIF loads (`load_base=0`) get different ASLR base addresses
- [ ] Optional import returns `STATUS_NOT_IMPLEMENTED`, not crash
- [ ] All POST16 codes appear in correct order on serial output
- [ ] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal
- [ ] Commit: `"kernel: eif -- full implementation complete"`
