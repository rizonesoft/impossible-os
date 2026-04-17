# TODO-13 -- Atom, NLS & Locale Subsystem

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

## 1. Unicode String Primitive Layer

- [ ] Define safe helpers for `UNICODE_STRING`, counted UTF-16LE buffers, and bounded conversion.
- [ ] Reject unterminated buffer assumptions in Native API handlers.
- [ ] Provide `nt_decode_unicode_string` and `nt_encode_unicode_string` as canonical helpers.
- [ ] Add overflow-safe length arithmetic.

## 2. Case Folding and Invariant Compare

- [ ] Implement invariant uppercase/lowercase mapping for ASCII plus core Unicode BMP.
- [ ] Add `RtlEqualUnicodeString`, `RtlCompareUnicodeString`, `RtlUpcaseUnicodeString`.
- [ ] Support case-sensitive and case-insensitive modes.
- [ ] Registry and Object Manager use this layer for name comparisons.

## 3. Global and Local Atom Tables

- [ ] Implement global atom table with 16-bit atom IDs and string interning.
- [ ] Implement per-process or per-session local atom tables for Win32 compatibility.
- [ ] APIs: add, find, delete, query, refcount.
- [ ] Enforce reserved integer atom range and string atom range.

## 4. NLS Table File Format and Loader

- [ ] Define compact `nls_table_v1` format with magic, version, locale ID, code page ID, hash, and table offsets.
- [ ] Load tables from `C:\Impossible\System\NLS\`.
- [ ] Provide compiled fallback for invariant locale and UTF-8/UTF-16.
- [ ] Validate checksums and bounds before publishing tables.

## 5. Code Page Conversion Providers

- [ ] Implement UTF-8 <-> UTF-16LE.
- [ ] Implement CP437, CP850, and Windows-1252 <-> UTF-16LE.
- [ ] Add replacement-character and strict-failure modes.
- [ ] Expose provider lookup by code page ID.

## 6. Locale and LCID Metadata

- [ ] Define locale records: LCID, BCP-47 name, language, region, decimal separator, date/time formats, currency metadata, first day of week.
- [ ] Kernel stores invariant and installed-locale metadata; user-mode libraries format UI strings.
- [ ] Registry policy chooses system locale and user locale.
- [ ] Publish locale change notification through TODO-27.

## 7. Sort Keys and Normalization Policy

- [ ] Implement invariant binary sort and case-insensitive sort keys.
- [ ] Defer full culture-aware collation to user-mode unless a kernel consumer proves it needs it.
- [ ] Define normalization policy: kernel accepts valid UTF-16, does not silently normalize object names, and exposes helper for explicit normalization.
- [ ] Add path/registry comparison tests for composed/decomposed tricky cases.

## 8. Native Atom/NLS/Locale Syscalls

- [ ] Wire `NtAddAtom`, `NtFindAtom`, `NtDeleteAtom`, `NtQueryInformationAtom`.
- [ ] Wire `NtGetNlsSectionPtr` equivalent as a safe table-query API.
- [ ] Wire `NtQueryDefaultLocale`, `NtSetDefaultLocale`, `NtQueryDefaultUILanguage`, `NtSetDefaultUILanguage`.
- [ ] Add `SystemNlsInformation` to `NtQuerySystemInformation`.

## 9. Retrofit Kernel Consumers

- [ ] Object Manager namespace lookup uses canonical Unicode compare.
- [ ] Registry key/value lookup uses canonical Unicode compare and preserves original casing.
- [ ] Environment variables use Windows-compatible case-insensitive matching.
- [ ] PE loader import lookup preserves ASCII fast path but supports UTF-16 path inputs.
- [ ] File APIs pass code-page conversion requests to filesystem/user-mode boundary without lossy truncation.

## 10. Tests and Compatibility Corpus

- [ ] Unit tests: UTF-8 invalid sequences, UTF-16 surrogate pairs, case-insensitive compare, atom refcounts, CP437/1252 round trips.
- [ ] Compatibility tests for Win32 atom APIs and `CompareStringOrdinal`.
- [ ] Fuzz counted string inputs to every Native API helper.
- [ ] Boot test with missing NLS files uses invariant fallback and logs degraded state.

