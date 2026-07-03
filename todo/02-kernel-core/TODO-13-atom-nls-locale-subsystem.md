---
schema_version: 1
id: atom-nls-locale-subsystem
domain: 02-kernel-core
status: active
title: "TODO-13 -- Atom, NLS & Locale Subsystem"
---

# TODO-13 -- Atom, NLS & Locale Subsystem

> **Validated:** 2026-07-03 | validate-todo-file clean (structure / IO table / XREF / test wiring)
> **Gap-audited:** 2026-07-03 | gap-audit + codex-gap-audit; 4 findings filed (MUI syscalls §8 + SSDT-A owner repoint, nls_table_v1 char-type/fold/version §4, code-page metadata+DBCS §5, current-state rewrite) + parity items (CompareStringOrdinal/GetStringType §2, 3-way locale + MUI fallback + tz display names §6, NFC/NFD forms + FoldString + LCMapString §7, NtGetNlsSectionPtr mapped-section §8); USER-atom/CHCP/IXFS cross-TODO + IDN/calendar out-of-scope noted

> **Goal:** Complete the kernel-owned atom and NLS/locale layer required by NT Native API, Win32 compatibility, registry case-insensitivity, object namespace comparisons, environment lookup, filesystem interop, and user-mode locale APIs. This covers global/local atom tables, Unicode case mapping, code page conversion, locale identifiers, sort keys, normalization policy, timezone display names, and native syscalls.

> [!IMPORTANT]
> **Current state:** TODO-12 §23 shipped the GLOBAL atom table + global LCID/LANGID storage + `NtAddAtom`/`NtFindAtom`/`NtDeleteAtom`/`NtQueryInformationAtom` and `NtQueryDefaultLocale`/`NtSetDefaultLocale`/`NtQueryDefaultUILanguage`/`NtSetDefaultUILanguage` in `src/kernel/nt/nt_misc.c` -- but with an ASCII-only case fold and global-only scope. Still missing: per-process LOCAL atom tables, a single Unicode case-folding authority, the NLS table loader, code-page conversion providers, full LCID/locale metadata records, and a kernel/user contract for locale-sensitive comparison. Existing string handling is mostly ASCII.

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
| 💎 | 1 | Unicode string primitive layer | -- | [x] |
| 💎 | 2 | Case folding and invariant compare | §1 | [x] |
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

- [x] Safe helpers for `UNICODE_STRING` + counted UTF-16LE + bounded copy: `nt_unicode_string_validate` (snapshot-copy, validate Length even/<=MaximumLength/<=`NT_UNICODE_MAX_BYTES`/non-NULL Buffer) in `nt_unicode.c`.
- [x] Reject unterminated/oversized buffers: `nt_decode_unicode_string` copies into a caller kernel buffer bounded by capacity, NUL-terminates, `STATUS_BUFFER_TOO_SMALL` if too small (handler retrofit is §9).
- [x] Canonical `nt_decode_unicode_string` + `nt_encode_unicode_string` in `nt_unicode.{h,c}`; plus lossless ASCII-narrow bridge `nt_unicode_to_ascii` (rejects non-ASCII + embedded NUL).
- [x] Overflow-safe length arithmetic: `nt_unicode_wchars_to_bytes` + `>=`-form capacity checks (no `+1` wrap).
- [x] Commit: `"kernel: nls -- UNICODE_STRING primitive layer with bounded conversion"`

**Test checkpoint:** `nt_decode_unicode_string` on a `UNICODE_STRING` with `Length` past the buffer returns an error, not an overread. A counted UTF-16LE buffer with an odd byte length is rejected. Length arithmetic on `0xFFFF`-scale `MaximumLength` does not wrap.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | 17 suites, 0 failures

> **Notes:**
> - **What shipped:** `nt_unicode.h` + `nt_unicode.c` -- the canonical UNICODE_STRING validate/decode/encode primitive + `nt_unicode_to_ascii` lossless narrow bridge; `NT_UNICODE_MAX_BYTES`=65534 ceiling, caller-buffer capacity is the effective cap.
> - **How it integrates:** stateless/re-entrant (no locking); TOCTOU-safe via snapshot-copy of the struct then the buffer through `copy_from_user` when previous-mode is UserMode; `test_nls.c` under new `TEST_CAT_NLS` (14 suites, kernel-mode).
> - **Downstream effects:** the single primitive §2 (case fold), §5 (code page), and §9 (consumer retrofit of `oa_name`/`oa_probe_ascii_name`) build on; retrofit of char*-casting handlers is deferred to §9.
> - **Canonical doc:** [`include/kernel/nt/nt_unicode.h`](../../include/kernel/nt/nt_unicode.h).
> - **Scope boundary:** UTF-16 <-> UTF-8/code-page transcoding is §5, not §1; §1 provides validated UTF-16 + an ASCII-range narrow bridge only.
> **Verified:** 2026-07-03 | commit `0584317f` | 4/4 items | build OK | tests 13719 kernel + 16 user PASS | smoke PASS
> **Accepted:** [M] `copy_from_user` copies byte-at-a-time (kernel-wide implementation in `cpu_security.c`, not §1 code -- §1 issues one `copy_from_user` for Length bytes) -> XREF: 03-memory-concurrency/TODO-02-memory-security.md §4 (item: "Bulk-copy path in `copy_from_user`/`copy_to_user`" at line 128)
> **Quality reviewed:** 2026-07-03 | Codex 6x (design, adversarial, consistency, perf, re-adversarial x2) | 4H fixed (probe keyed to explicit prev_mode, `_pad` leak, encode capacity vs overstated MaximumLength, validate now probes Buffer), 1M accepted-XREF | scope: kernel-code-quality

---

## 2. Case Folding and Invariant Compare

- [x] Implement invariant uppercase mapping for ASCII a-z + Latin-1 Supplement (0xE0-0xFE except 0xF7, and 0xFF->0x178); Latin-Extended + full-BMP algorithmic fold land in §4's table loader. (§2 design)
- [x] Add `RtlEqualUnicodeString`, `RtlCompareUnicodeString`, `RtlUpcaseUnicodeString` (caller-provided dst buffer; no pool alloc at kernel Rtl layer).
- [x] Support case-sensitive and case-insensitive modes.
- [x] Provide the single compare authority the Object Manager, Registry, and atom table retrofit onto; consumer retrofit is owned by §3 (atom `w_fold`) and §9 (`oa_name`/`oa_probe_ascii_name`), not §2.
- [x] Add `CompareStringOrdinal` (pure code-unit compare, no locale) as an explicit named deliverable over `RtlCompareUnicodeString` (was only in the §10 test list). (gap-audit)
- [x] Scope note: supplementary-plane + locale special-casing (Turkish dotless-i, German ss expansion) is explicitly OUT of the kernel `Rtl*` layer (mirrors NT; user-mode `LCMapStringEx` owns it). (gap-audit)
- [x] Public `GetStringTypeW`/`GetStringTypeEx` (C1/C2/C3 over the full BMP table) is owned by §4, not §2 -- shipping it over only ASCII+Latin-1 here would misclassify BMP chars. (§2 design)
- [x] Commit: `"kernel: nls -- invariant case folding + RtlUnicodeString compare authority"`

**Test checkpoint:** `RtlEqualUnicodeString("File", "file", CaseInsensitive=TRUE)` returns TRUE; with `FALSE` returns FALSE. `RtlUpcaseUnicodeString` folds ASCII a-z and BMP Latin-1 supplement. Case-sensitive compare distinguishes U+0041 from U+0061.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | 33 suites, 0 failures

> **Notes:**
> - **What shipped:** `nt_rtlstr.h` + `nt_rtlstr.c` -- the kernel name-compare authority: invariant UTF-16 upcase fold (`rtl_upcase_char`, ASCII a-z + Latin-1) + `RtlUpcase`/`RtlEqual`/`RtlCompareUnicodeString` + `CompareStringOrdinal`.
> - **How it integrates:** pure kernel-resident ops, no user probing (callers decode via the §1 primitive first); `build.sh` auto-picks up new `nt_*.c`; `TEST_CAT_NLS` now 33 suites (16 added for §2).
> - **Defense:** the authority self-defends the canonical validity rules -- `rtl_us_valid` rejects malformed input in Equal/Upcase, `rtl_us_safe_wchars` keeps Compare a reflexive+antisymmetric total order.
> - **Downstream effects:** §3 (atom `w_fold`) + §9 (`oa_name`) retrofit consumers onto this authority; full-BMP fold + public `GetStringTypeW`/`GetStringTypeEx` are owned by §4's NLS table loader.
> - **Canonical doc:** [`include/kernel/nt/nt_rtlstr.h`](../../include/kernel/nt/nt_rtlstr.h).
> - **Scope boundary:** locale special-casing (Turkish dotless-i, German ss expansion) and supplementary planes are OUT of the kernel `Rtl*` layer (mirrors NT; user-mode `LCMapStringEx` owns them).
> **Verified:** 2026-07-03 | commit `19737af6` | 7/7 items | build OK | tests 13755 kernel + 16 user PASS | lint 0 errors
> **Quality reviewed:** 2026-07-03 | Codex 6x (design, adversarial, consistency, perf, re-adversarial x2) | fixed: bad-negative ordinal count NUL scan (H), malformed-input overread self-defense (H), non-antisymmetric malformed compare (M); perf: ASCII fast path + raw-compare-first fold + one-pass ordinal compare | scope: kernel-code-quality

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
- [ ] Add `nls_table_v1` chunks for character-type data (`GetStringTypeW` C1/C2/C3), folding tables (upcase + compatibility/digit/width), and an NLS/collation version ID (`GetNLSVersionEx`) so later consumers do not force a format break. (gap-audit)
- [ ] Commit: `"kernel: nls -- nls_table_v1 format + loader with compiled fallback"`

**Test checkpoint:** A `nls_table_v1` blob with a bad magic or a mismatched hash is rejected and the compiled fallback is used. A table whose offsets exceed the blob size is rejected before publish. Boot with an empty NLS directory logs degraded state and serves the invariant fallback.

---

## 5. Code Page Conversion Providers

- [ ] Implement UTF-8 <-> UTF-16LE.
- [ ] Implement CP437, CP850, and Windows-1252 <-> UTF-16LE.
- [ ] Add replacement-character and strict-failure modes.
- [ ] Add best-fit substitution mode (Windows `WideCharToMultiByte` default) distinct from strict/replacement; opt-out equals `WC_NO_BEST_FIT_CHARS`. (gap-audit)
- [ ] Expose provider lookup by code page ID.
- [ ] Add `GetACP`/`GetOEMCP`/`CP_THREAD_ACP` default-ANSI/OEM code-page policy bound to registry (system + per-thread). (gap-audit)
- [ ] Add `GetCPInfoEx`-style metadata per provider (`CPINFO`/`CPINFOEX`: max char size, default char, DBCS lead-byte ranges) + `IsDBCSLeadByte`. (gap-audit)
- [ ] Add `IsValidCodePage` + `EnumSystemCodePages` (installed vs supported). (gap-audit)
- [ ] Commit: `"kernel: nls -- code page conversion providers (UTF-8/CP437/CP850/1252)"`

**Test checkpoint:** UTF-8 round-trips a 3-byte BMP sequence and a surrogate-pair 4-byte sequence through UTF-16LE. An invalid UTF-8 lead byte yields U+FFFD in replacement mode and an error in strict mode. CP1252 0x80 maps to U+20AC (Euro). Provider lookup by unknown code page ID returns NULL.

---

## 6. Locale and LCID Metadata

- [ ] Define locale records: LCID, BCP-47 name, language, region, decimal separator, date/time formats, currency metadata, first day of week.
- [ ] Kernel stores invariant and installed-locale metadata; user-mode libraries format UI strings.
- [ ] Registry policy chooses system locale and user locale; owns the privileged/per-user `NtSetDefaultLocale`/`NtSetDefaultUILanguage` (fail closed in TODO-12 §23 `nt_misc.c` pending this).
- [ ] Publish locale change notification through TODO-27.
- [ ] Itemize the 3-way split: independent system locale (non-Unicode program default), user locale, and UI language -- each separately settable via registry policy. (gap-audit)
- [ ] Add an ordered MUI UI-language fallback chain (e.g. es-MX -> es -> en-US), not just a single LANGID; mirrors `SetThreadPreferredUILanguages` / gettext `LANGUAGE`. (gap-audit)
- [ ] Provide locale-formatted timezone DISPLAY NAMES over the tz table + DST rules owned by `08-graphics-ui/TODO-12-clock-time.md` (this section owns only the localized display-string layer). (gap-audit)
- [ ] Commit: `"kernel: nls -- locale/LCID metadata records + registry policy binding"`

**Test checkpoint:** `NtQueryDefaultLocale` returns the registry-selected LCID; the invariant locale (0x007F) is always queryable. A BCP-47 name round-trips to its LCID. `NtSetDefaultLocale` from an unprivileged caller fails closed until the §6 policy binding lands.

---

## 7. Sort Keys and Normalization Policy

- [ ] Implement invariant binary sort and case-insensitive sort keys.
- [ ] Defer full culture-aware collation to user-mode unless a kernel consumer proves it needs it.
- [ ] Define normalization policy: kernel accepts valid UTF-16, does not silently normalize object names, and exposes helper for explicit normalization.
- [ ] Add path/registry comparison tests for composed/decomposed tricky cases.
- [ ] Name the normalization forms the explicit helper implements: NFC + NFD (FS/name interop) now; NFKC/NFKD flagged as user-mode/deferred; state completeness (narrow FS-interop vs full UAX #15). (gap-audit)
- [ ] Add `FoldStringW` transformations (MAP_FOLDCZONE compatibility, MAP_FOLDDIGITS, width fold) over the §4 folding tables. (gap-audit)
- [ ] Define the `LCMapStringEx`+`LCMAP_SORTKEY` opaque sort-key byte format (primary/diacritic/case weight bands, `0x01` separators); Impossible's format need not match Windows' internal layout. (gap-audit)
- [ ] Commit: `"kernel: nls -- invariant sort keys + normalization policy"`

**Test checkpoint:** Case-insensitive sort keys order "apple" < "Banana" < "cherry". The kernel does NOT fold a composed vs decomposed object name to equal (no silent normalization); the explicit normalization helper does when asked. A binary sort key is stable and comparison-consistent.

---

## 8. Native Atom/NLS/Locale Syscalls

- [ ] Wire `NtGetNlsSectionPtr` equivalent as a safe table-query API. Implement it as a read-only SECTION mapped into each process (amortized zero-syscall user-mode `RtlUpcaseUnicodeString`), NOT a per-call syscall. (gap-audit)
- [ ] Add `SystemNlsInformation` to `NtQuerySystemInformation`.
- [ ] Add `NtGetMUIRegistryInfo` (SSDT 0x0263), `NtIsUILanguageComitted` (0x0264), `NtFlushInstallUILanguage` (0x0265) -- orphaned in the SSDT master table after T12 §23 closed; this section owns them. (gap-audit)
- [ ] Expose `GetNLSVersionEx` (NLS/collation version query) over the §4 version chunk. (gap-audit)
- [ ] Commit: `"kernel: nls -- NtGetNlsSectionPtr + SystemNlsInformation + MUI/version query APIs"`

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

> **Scope boundary (gap-audit):** The USER window-message atom pool (`RegisterWindowMessage`/`RegisterClass`/`RegisterClipboardFormat`, distinct from the global object-atom table) is owned by the win32k/desktop compositor TODO -> XREF that consumer, not §3. The console-vs-ANSI code-page split (`CHCP`) is owned by the shell TODO. IXFS filesystem-layer opt-in casefold/normalize (ext4 `EXT4_CASEFOLD_FL` analog, disk-persisted charset) is owned by the filesystem/IXFS TODO. OUT OF SCOPE for this kernel NLS TODO: IDN/punycode (`IdnToAscii`/`IdnToUnicode` -- user-mode library on Windows, no NT syscall) and non-Gregorian calendar metadata (`LOCALE_ICALENDARTYPE`); revisit only if a kernel consumer proves need.

**Test checkpoint:** Creating `\BaseNamedObjects\Foo` then opening `\BaseNamedObjects\foo` resolves the same object via the canonical compare. A registry key stored as "Software" opens case-insensitively but reports its original casing. `PATH` and `Path` resolve to the same environment variable.

---

## 10. Tests and Compatibility Corpus

- [ ] Unit tests: UTF-8 invalid sequences, UTF-16 surrogate pairs, case-insensitive compare, atom refcounts, CP437/1252 round trips.
- [ ] Compatibility tests for Win32 atom APIs and `CompareStringOrdinal`.
- [ ] Fuzz counted string inputs to every Native API helper.
- [ ] Boot test with missing NLS files uses invariant fallback and logs degraded state.
- [ ] Tests for `GetStringTypeW` C1/C2/C3 classification, `FoldStringW`, `LCMapStringEx` sort-key ordering, `GetNLSVersionEx`, `GetCPInfoEx`/`IsDBCSLeadByte` metadata, and `CompareStringOrdinal`. (gap-audit)
- [ ] Create `TEST_CAT_NLS` (enum in `test.h`, label in `test_runner.c`, `make test-nls` target, `bootx64.c` parser, `scripts\debug\kernel\run-nls-tests.bat`).
- [ ] Commit: `"kernel: nls -- unit tests + compatibility corpus (TEST_CAT_NLS)"`

**Test checkpoint:** `test_nls.c` registers under `TEST_CAT_NLS`; the suite covers UTF-8 invalid-sequence rejection, surrogate-pair round trip, case-insensitive compare, atom refcount add/find/delete, and CP437/CP1252 round trips, all with concrete expected values. Boot with a missing NLS dir passes with a logged degraded-state marker.

---

## OS Comparison

| ⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS |
|----|---------|----------|----------|------------------|
| 💎 | Global + local atom tables | ✅ Global/Local `AddAtom` | ⚠️ no direct equivalent | 🔄 global shipped (T12 §23); §3 local + retrofit |
| 💎 | Unicode case-fold authority | ✅ `RtlUpcaseUnicodeString` NLS | ✅ ICU / glibc `towupper` | 🔄 invariant fold + Rtl compare (§2); full-BMP §4 |
| 💎 | Code page conversion | ✅ `MultiByteToWideChar` NLS | ✅ `iconv` | ⬜ §5 UTF-8/CP437/CP850/1252 |
| 💎 | LCID / locale metadata | ✅ `GetLocaleInfoEx` LCID | ✅ `setlocale` / `nl_langinfo` | 🔄 default LCID (T12 §23); §6 records |
| 💎 | Sort keys / collation | ✅ `CompareStringEx` linguistic | ✅ ICU collation, `strcoll` | ⬜ §7 invariant + case-insensitive keys |
| 💎 | NLS table loading + fallback | ✅ `l_intl.nls` at boot | ✅ locale archive | ⬜ §4 `nls_table_v1` + compiled fallback |
| 💎 | Object-name Unicode compare | ✅ OB case-insensitive NLS | ⚠️ VFS bytewise (case-sensitive) | ⬜ §9 canonical OB/registry compare |
| ⭐ | Normalization policy | ✅ `NormalizeString` NFC/NFD | ✅ ICU normalizer | ⬜ §7 no-silent-normalize + explicit helper |
| 💎 | Char-type + folding APIs | ✅ `GetStringType`/`FoldString` | ✅ ICU `u_charType` | ⬜ §4/§7 C1-C3 + FoldString |
| 💎 | Code-page metadata / DBCS | ✅ `GetCPInfoEx`/`IsDBCSLeadByte` | ✅ `nl_langinfo` / iconv | ⬜ §5 CPINFO + lead-byte |
| ⭐ | MUI UI-language fallback chain | ✅ `SetThreadPreferredUILanguages` | ✅ gettext `LANGUAGE` list | ⬜ §6 ordered fallback |

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
- [ ] `test_getstringtype_classification` -- `GetStringTypeW` returns C1_ALPHA/C1_DIGIT/C1_SPACE for expected code points.
- [ ] `test_cpinfoex_dbcs_leadbyte` -- `GetCPInfoEx` reports max-char-size + default char; `IsDBCSLeadByte` for a DBCS CP lead range (once a DBCS table exists) else `TEST_PENDING`.
- [ ] `test_compare_string_ordinal` -- `CompareStringOrdinal` code-unit compare distinguishes case and orders by code unit.
- [ ] `test_nls_missing_dir_fallback` -- pure helper check of fallback-table selection (no live boot infra).

---

## Verification

Boot on QEMU WHPX + TCG, VirtualBox, and bare metal. Confirm case-insensitive object/registry/env lookups resolve, code-page round trips are lossless, and a missing NLS directory boots on the invariant fallback with a degraded-state log line. Verify on bare metal -- VM behavior differs for boot-time table loading and disk-sourced NLS files.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | N suites, 0 failures
