---
schema_version: 1
id: atom-nls-locale-subsystem
domain: 02-kernel-core
status: active
title: "TODO-13 -- Atom, NLS & Locale Subsystem"
---

# TODO-13 -- Atom, NLS & Locale Subsystem

> **Validated:** 2026-07-03 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Goal:** Complete the kernel-owned atom and NLS/locale layer required by NT Native API, Win32 compatibility, registry case-insensitivity, object namespace comparisons, environment lookup, filesystem interop, and user-mode locale APIs. This covers global/local atom tables, Unicode case mapping, code page conversion, locale identifiers, sort keys, normalization policy, timezone display names, and native syscalls.

> [!IMPORTANT]
> **Current state:** TODO-12 reserves atom/locale/miscellaneous syscall space, but there is no atom table implementation, no NLS table loader, no Unicode case-folding authority, no code-page conversion provider, no LCID/locale metadata, and no kernel/user contract for locale-sensitive comparison. Existing code mostly treats strings as ASCII.

## Inputs

- [`src/kernel/nt`](../../src/kernel/nt/)
- [`src/kernel/ob/ob_ns.c`](../../src/kernel/ob/ob_ns.c)
- [`src/kernel/registry.c`](../../src/kernel/registry.c)
- → XREF: [`TODO-12-native-api-ssdt.md`](./TODO-12-native-api-ssdt.md) §23 -- atom, locale, miscellaneous syscalls
- → XREF: [`TODO-14-registry-completion.md`](./TODO-14-registry-completion.md) -- case-insensitive key names and Unicode strings
- → XREF: [`TODO-22-environment-variables.md`](./TODO-22-environment-variables.md) -- case-insensitive environment blocks
- → XREF: [`TODO-03-kernel-libraries.md`](./TODO-03-kernel-libraries.md) -- decompression/parser providers for table loading

## Outcome

- Global and per-session atom tables support Win32/NT semantics.
- Kernel has a single Unicode case-folding and string-compare authority.
- NLS tables load at boot with a safe compiled fallback.
- Code page conversions support UTF-8, UTF-16LE, CP437, CP850, and Windows-1252 first, then extensible tables.
- Locale metadata is queryable through native APIs and user-mode wrappers.
- Registry/object/environment comparisons stop depending on ad-hoc ASCII-only behavior.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| -- | :---: | ----------- | ---------- | :----: |
| 💎 | 1 | Unicode string primitive layer | -- | [ ] |
| 💎 | 2 | Case folding and invariant compare | §1 | [ ] |
| 💎 | 3 | Global and local atom tables | T12 | [ ] |
| 💎 | 4 | NLS table file format and loader | VFS, T03 | [ ] |
| 💎 | 5 | Code page conversion providers | §4 | [ ] |
| 💎 | 6 | Locale and LCID metadata | §4 | [ ] |
| 💎 | 7 | Sort keys and normalization policy | §2, §6 | [ ] |
| 💎 | 8 | Native atom/NLS/locale syscalls | T12 | [ ] |
| ⭐ | 9 | Retrofit kernel consumers | T05, T14, T22 | [ ] |
| 💎 | 10 | Tests and compatibility corpus | §1..§9 | [ ] |

---

## 1. Unicode String Primitive Layer

- [ ] Define safe helpers for `UNICODE_STRING`, counted UTF-16LE buffers, and bounded conversion.
- [ ] Reject unterminated buffer assumptions in Native API handlers.
- [ ] Provide `nt_decode_unicode_string` and `nt_encode_unicode_string` as canonical helpers.
- [ ] Add overflow-safe length arithmetic.
- [ ] Commit: `"kernel: nls -- UNICODE_STRING primitive layer with bounded conversion"`

**Test checkpoint:** `nt_decode_unicode_string` on a `UNICODE_STRING` with `Length` past the buffer returns an error, not an overread. A counted UTF-16LE buffer with an odd byte length is rejected. Length arithmetic on `0xFFFF`-scale `MaximumLength` does not wrap.

---

## 2. Case Folding and Invariant Compare

- [ ] Implement invariant uppercase/lowercase mapping for ASCII plus core Unicode BMP.
- [ ] Add `RtlEqualUnicodeString`, `RtlCompareUnicodeString`, `RtlUpcaseUnicodeString`.
- [ ] Support case-sensitive and case-insensitive modes.
- [ ] Registry and Object Manager use this layer for name comparisons.
- [ ] Commit: `"kernel: nls -- invariant case folding + RtlUnicodeString compare authority"`

**Test checkpoint:** `RtlEqualUnicodeString("File", "file", CaseInsensitive=TRUE)` returns TRUE; with `FALSE` returns FALSE. `RtlUpcaseUnicodeString` folds ASCII a-z and BMP Latin-1 supplement. Case-sensitive compare distinguishes U+0041 from U+0061.

---

## 3. Global and Local Atom Tables

- [ ] Implement per-process or per-session local atom tables for Win32 compatibility.
- [ ] Retrofit the global atom table onto the NLS case-folding authority (§2), replacing the ASCII fold.
- [ ] Bound the locked atom lookup (`nt_misc.c` `atom_find_slot_locked` scans O(512x255) under the irqsave `s_atom_lock`): add a per-slot case-folded hash / bucket index or a thread-context mutex.
- [ ] Commit: `"kernel: nls -- local atom tables + NLS-authority global atom retrofit"`

> **Note:** The GLOBAL atom table (16-bit IDs 0xC000+, string interning, refcount, add/find/delete/query APIs, integer/string range split) shipped in TODO-12 §23 (`src/kernel/nt/nt_misc.c`) with an ASCII-only case fold. This section still owns per-process/per-session LOCAL atom tables and the NLS-authority retrofit above.

**Test checkpoint:** A local atom added in one process is not visible in another; a global atom is. Case-insensitive lookup finds "Foo" when added as "foo" via the §2 authority. The bounded lookup resolves a hit without the O(512x255) linear scan.

---

## 4. NLS Table File Format and Loader

- [ ] Define compact `nls_table_v1` format with magic, version, locale ID, code page ID, hash, and table offsets.
- [ ] Load tables from `C:\Impossible\System\NLS\`.
- [ ] Provide compiled fallback for invariant locale and UTF-8/UTF-16.
- [ ] Validate checksums and bounds before publishing tables.
- [ ] Commit: `"kernel: nls -- nls_table_v1 format + loader with compiled fallback"`

**Test checkpoint:** A `nls_table_v1` blob with a bad magic or a mismatched hash is rejected and the compiled fallback is used. A table whose offsets exceed the blob size is rejected before publish. Boot with an empty NLS directory logs degraded state and serves the invariant fallback.

---

## 5. Code Page Conversion Providers

- [ ] Implement UTF-8 <-> UTF-16LE.
- [ ] Implement CP437, CP850, and Windows-1252 <-> UTF-16LE.
- [ ] Add replacement-character and strict-failure modes.
- [ ] Expose provider lookup by code page ID.
- [ ] Commit: `"kernel: nls -- code page conversion providers (UTF-8/CP437/CP850/1252)"`

**Test checkpoint:** UTF-8 round-trips a 3-byte BMP sequence and a surrogate-pair 4-byte sequence through UTF-16LE. An invalid UTF-8 lead byte yields U+FFFD in replacement mode and an error in strict mode. CP1252 0x80 maps to U+20AC (Euro). Provider lookup by unknown code page ID returns NULL.

---

## 6. Locale and LCID Metadata

- [ ] Define locale records: LCID, BCP-47 name, language, region, decimal separator, date/time formats, currency metadata, first day of week.
- [ ] Kernel stores invariant and installed-locale metadata; user-mode libraries format UI strings.
- [ ] Registry policy chooses system locale and user locale; owns the privileged/per-user `NtSetDefaultLocale`/`NtSetDefaultUILanguage` (fail closed in TODO-12 §23 `nt_misc.c` pending this).
- [ ] Publish locale change notification through TODO-27.
- [ ] Commit: `"kernel: nls -- locale/LCID metadata records + registry policy binding"`

**Test checkpoint:** `NtQueryDefaultLocale` returns the registry-selected LCID; the invariant locale (0x007F) is always queryable. A BCP-47 name round-trips to its LCID. `NtSetDefaultLocale` from an unprivileged caller fails closed until the §6 policy binding lands.

---

## 7. Sort Keys and Normalization Policy

- [ ] Implement invariant binary sort and case-insensitive sort keys.
- [ ] Defer full culture-aware collation to user-mode unless a kernel consumer proves it needs it.
- [ ] Define normalization policy: kernel accepts valid UTF-16, does not silently normalize object names, and exposes helper for explicit normalization.
- [ ] Add path/registry comparison tests for composed/decomposed tricky cases.
- [ ] Commit: `"kernel: nls -- invariant sort keys + normalization policy"`

**Test checkpoint:** Case-insensitive sort keys order "apple" < "Banana" < "cherry". The kernel does NOT fold a composed vs decomposed object name to equal (no silent normalization); the explicit normalization helper does when asked. A binary sort key is stable and comparison-consistent.

---

## 8. Native Atom/NLS/Locale Syscalls

- [ ] Wire `NtGetNlsSectionPtr` equivalent as a safe table-query API.
- [ ] Add `SystemNlsInformation` to `NtQuerySystemInformation`.
- [ ] Commit: `"kernel: nls -- NtGetNlsSectionPtr + SystemNlsInformation query APIs"`

> **Note:** `NtAddAtom`/`NtFindAtom`/`NtDeleteAtom`/`NtQueryInformationAtom` and `NtQueryDefaultLocale`/`NtSetDefaultLocale`/`NtQueryDefaultUILanguage`/`NtSetDefaultUILanguage` are already wired in TODO-12 §23 (`src/kernel/nt/nt_misc.c`, SSDT 0x00D8-0x00E2) over a global atom table + global LCID/LANGID storage. This section retrofits those handlers onto the NLS case-folding authority (§2), local atom tables (§3), and full LCID metadata (§6); `NtGetNlsSectionPtr` + `SystemNlsInformation` remain unshipped.

**Test checkpoint:** `NtGetNlsSectionPtr` returns a read-only table pointer for a valid NLS section type and `STATUS_INVALID_PARAMETER` for an unknown type. `NtQuerySystemInformation(SystemNlsInformation)` fills the caller buffer or returns `STATUS_INFO_LENGTH_MISMATCH` with the required length.

---

## 9. Retrofit Kernel Consumers

- [ ] Object Manager namespace lookup uses canonical Unicode compare.
- [ ] Registry key/value lookup uses canonical Unicode compare and preserves original casing.
- [ ] Environment variables use Windows-compatible case-insensitive matching.
- [ ] PE loader import lookup preserves ASCII fast path but supports UTF-16 path inputs.
- [ ] File APIs pass code-page conversion requests to filesystem/user-mode boundary without lossy truncation.
- [ ] Commit: `"kernel: nls -- retrofit OB/registry/env/PE/file consumers onto canonical compare"`

**Test checkpoint:** Creating `\BaseNamedObjects\Foo` then opening `\BaseNamedObjects\foo` resolves the same object via the canonical compare. A registry key stored as "Software" opens case-insensitively but reports its original casing. `PATH` and `Path` resolve to the same environment variable.

---

## 10. Tests and Compatibility Corpus

- [ ] Unit tests: UTF-8 invalid sequences, UTF-16 surrogate pairs, case-insensitive compare, atom refcounts, CP437/1252 round trips.
- [ ] Compatibility tests for Win32 atom APIs and `CompareStringOrdinal`.
- [ ] Fuzz counted string inputs to every Native API helper.
- [ ] Boot test with missing NLS files uses invariant fallback and logs degraded state.
- [ ] Create `TEST_CAT_NLS` (enum in `test.h`, label in `test_runner.c`, `make test-nls` target, `bootx64.c` parser, `scripts\debug\kernel\run-nls-tests.bat`).
- [ ] Commit: `"kernel: nls -- unit tests + compatibility corpus (TEST_CAT_NLS)"`

**Test checkpoint:** `test_nls.c` registers under `TEST_CAT_NLS`; the suite covers UTF-8 invalid-sequence rejection, surrogate-pair round trip, case-insensitive compare, atom refcount add/find/delete, and CP437/CP1252 round trips, all with concrete expected values. Boot with a missing NLS dir passes with a logged degraded-state marker.

---

## OS Comparison

| ⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS |
|----|---------|----------|----------|------------------|
| 💎 | Global + local atom tables | ✅ Global/Local `AddAtom` | ⚠️ no direct equivalent | 🔄 global shipped (T12 §23); §3 local + retrofit |
| 💎 | Unicode case-fold authority | ✅ `RtlUpcaseUnicodeString` NLS | ✅ ICU / glibc `towupper` | ⬜ §2 invariant + BMP fold |
| 💎 | Code page conversion | ✅ `MultiByteToWideChar` NLS | ✅ `iconv` | ⬜ §5 UTF-8/CP437/CP850/1252 |
| 💎 | LCID / locale metadata | ✅ `GetLocaleInfoEx` LCID | ✅ `setlocale` / `nl_langinfo` | 🔄 default LCID (T12 §23); §6 records |
| 💎 | Sort keys / collation | ✅ `CompareStringEx` linguistic | ✅ ICU collation, `strcoll` | ⬜ §7 invariant + case-insensitive keys |
| 💎 | NLS table loading + fallback | ✅ `l_intl.nls` at boot | ✅ locale archive | ⬜ §4 `nls_table_v1` + compiled fallback |
| 💎 | Object-name Unicode compare | ✅ OB case-insensitive NLS | ⚠️ VFS bytewise (case-sensitive) | ⬜ §9 canonical OB/registry compare |
| ⭐ | Normalization policy | ✅ `NormalizeString` NFC/NFD | ✅ ICU normalizer | ⬜ §7 no-silent-normalize + explicit helper |

Win11 provides the deepest NLS surface (per-locale `.nls` tables, linguistic collation, normalization) but couples case-insensitivity into the kernel object manager. Linux pushes locale to userspace (ICU/glibc) and keeps the VFS bytewise. Impossible OS centralizes one kernel case-fold authority (§2) that OB, registry, and environment all consume (§9), keeps culture-aware collation in user-mode (§7), and refuses silent normalization of object names.

---

## Unit Tests

- [ ] `test_nls.c` under `TEST_CAT_NLS` (new category; created in §10).
- [ ] `test_unicode_decode_rejects_overrun` -- `nt_decode_unicode_string` past-buffer `Length` returns error, no overread.
- [ ] `test_case_insensitive_compare` -- `RtlEqualUnicodeString("File","file",TRUE)==TRUE`, `(...,FALSE)==FALSE`.
- [ ] `test_upcase_bmp` -- `RtlUpcaseUnicodeString` folds ASCII + Latin-1 supplement to expected code points.
- [ ] `test_local_vs_global_atom_scope` -- local atom invisible cross-process; global visible; case-insensitive find.
- [ ] `test_utf8_roundtrip_and_invalid` -- 3-byte + 4-byte round trip; invalid lead byte yields U+FFFD (replacement) / error (strict).
- [ ] `test_cp1252_euro` -- CP1252 0x80 -> U+20AC.
- [ ] `test_lcid_query_invariant` -- invariant LCID 0x007F always queryable; BCP-47 name round-trips.
- [ ] `test_sortkey_order` -- case-insensitive sort keys order apple < Banana < cherry.
- [ ] `test_no_silent_normalization` -- composed vs decomposed object name not folded equal; explicit helper folds on request.
- [ ] `test_nls_missing_dir_fallback` -- pure helper check of fallback-table selection (no live boot infra).

---

## Verification

Boot on QEMU WHPX + TCG, VirtualBox, and bare metal. Confirm case-insensitive object/registry/env lookups resolve, code-page round trips are lossless, and a missing NLS directory boots on the invariant fallback with a degraded-state log line. Verify on bare metal -- VM behavior differs for boot-time table loading and disk-sourced NLS files.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | N suites, 0 failures
