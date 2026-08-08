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
> **Current state (updated 2026-07-04):** TODO-12 §23 shipped the GLOBAL atom table + global LCID/LANGID storage + `NtAddAtom`/`NtFindAtom`/`NtDeleteAtom`/`NtQueryInformationAtom` and `NtQueryDefaultLocale`/`NtSetDefaultLocale`/`NtQueryDefaultUILanguage`/`NtSetDefaultUILanguage` in `src/kernel/nt/nt_misc.c` with an ASCII-only fold. THIS TODO then shipped the rest: the single Unicode case-fold authority (§2), NLS table loader (§4), code-page providers (§5), LCID/locale metadata (§6), sort keys + normalization (§7), NLS/MUI query syscalls (§8), and the OB/registry/atom consumer retrofit (§9). Per-process LOCAL atom tables remain user-mode; the full-BMP fold corpus + real-table boot path are §10-deferred.

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

| ⭐  | Order | Deliverable                        | Depends On    | Status |
| --- | :---: | ---------------------------------- | ------------- | :----: |
| 💎  |   1   | Unicode string primitive layer     | --            |  [x]   |
| 💎  |   2   | Case folding and invariant compare | §1            |  [x]   |
| 💎  |   3   | Global and local atom tables       | T12           |  [x]   |
| 💎  |   4   | NLS table file format and loader   | VFS, T03      |  [x]   |
| 💎  |   5   | Code page conversion providers     | §4            |  [x]   |
| 💎  |   6   | Locale and LCID metadata           | §4            |  [/]   |
| 💎  |   7   | Sort keys and normalization policy | §2, §6        |  [x]   |
| 💎  |   8   | Native atom/NLS/locale syscalls    | T12           |  [/]   |
| ⭐  |   9   | Retrofit kernel consumers          | T05, T14, T22 |  [/]   |
| 💎  |  10   | Tests and compatibility corpus     | §1..§9        |  [/]   |

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
> **Accepted:** [M] `copy_from_user` copies byte-at-a-time (kernel-wide implementation in `cpu_security.c`, not §1 code -- §1 issues one `copy_from_user` for Length bytes) -> XREF: 03-memory-concurrency/TODO-02-memory-security.md §4 (item: "Bulk-copy path in `copy_from_user`/`copy_to_user`" at line 134)
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

- [x] Per-process LOCAL atom tables are USER-MODE per Win32 parity (ntdll `RtlAtomTable`); kernel Nt atom syscalls stay GLOBAL-only. Owner: `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md` §9 (item: "RtlCreateAtomTable"). (§3 design)
- [x] Retrofit the global atom table onto the NLS case-folding authority (§2), replacing the ASCII fold (`atom_name_eq` folds via `rtl_upcase_char`; `w_fold` removed).
- [x] Bound the locked atom lookup: per-slot case-folded FNV hash (`atom_name_hash`, computed outside `s_atom_lock`) gates the full name compare in `atom_find_slot_locked`, so a miss costs one word compare per slot, not up to 255 folds.
- [x] Commit: `"kernel: nls -- global atom NLS-authority retrofit + bounded lookup"`

> **Note:** The GLOBAL atom table (16-bit IDs 0xC000+, string interning, refcount, add/find/delete/query APIs, integer/string range split) shipped in TODO-12 §23 (`src/kernel/nt/nt_misc.c`) with an ASCII-only case fold. This section retrofits it onto the §2 fold authority and bounds its lookup; LOCAL atom tables are user-mode (owner above).

**Test checkpoint:** Global `nt_atom_find` locates an atom case-insensitively via the §2 authority (added `"foo"`, found `"FOO"`, plus a Latin-1 fold pair). The bounded lookup resolves a hit without a full-name compare on every slot (per-slot folded-hash prefilter). Local-atom cross-process isolation is validated in the user-mode owner (TODO-04 §9), not here.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | atom suites 0 failures

> **Notes:**
> - **What shipped:** `nt_misc.c` global atom retrofit -- `atom_name_eq` folds through `rtl_upcase_char` (the §2 authority; `w_fold` removed) plus a per-slot case-folded FNV hash (`atom_name_hash`) prefilter in `atom_find_slot_locked`.
> - **How it integrates:** the query hash is computed OUTSIDE `s_atom_lock` and reused on insert, so the irqsave hold no longer covers the per-char fold; a miss costs one word compare per slot. New `test_nt_misc.c` Latin-1 fold test.
> - **Downstream effects:** global atom case-insensitivity now agrees with the §2 `RtlEqualUnicodeString` fold; the `oa_name` retrofit is §9, full-BMP fold is §4.
> - **Canonical doc:** [`src/kernel/nt/nt_misc.c`](../../src/kernel/nt/nt_misc.c) (atom table section).
> - **Scope boundary:** per-process LOCAL atom tables are user-mode (owner `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md` §9); kernel `NtAddAtom`/`NtFindAtom`/`NtDeleteAtom` stay global-only; full-BMP/locale casing is §4.
> **Verified:** 2026-07-03 | commit `730353cd` | 4/4 items | build OK | tests 13758 kernel + 16 user PASS | lint 0 errors
> **Quality reviewed:** 2026-07-03 | Codex 5x (design, adversarial, consistency, perf, re-adversarial) | design deferred local atoms to user-mode (`D12T04 §9`); fixed: stale ASCII header comment (L), hash-under-lock + double-compute (M) | scope: kernel-code-quality

---

## 4. NLS Table File Format and Loader

- [x] `nls_table_v1` format in `include/kernel/nt/nls.h`: 32-byte header (magic/version/lcid/code_page/nls_version/total_size/crc32/chunk_count) + `nls_chunk_desc[]` directory + bodies; offsets pinned by `_Static_assert`.
- [x] `nls_init()` loads `C:\Impossible\System\NLS\invariant.nls` at Phase 2 (`SUBSYS_NLS`, POST16 `0x20D0/1`, after registry); header read first, body into a `pmm_alloc_contiguous` blob (mirrors `hive_load`).
- [x] Compiled invariant fallback when no valid table: `nls_upcase_char` delegates < U+0100 to the §2 `rtl_upcase_char` authority (disk never overrides that range); ASCII+Latin-1 CTYPE1 classifier. UTF-8/UTF-16 transcoding is §5.
- [x] `nls_table_parse` (pure, no VFS) validates before publish: magic, version, `total_size == len` and `<= MAX`, chunk-count bound, CRC32, every chunk in-bounds (overflow-safe), even offset/size, no duplicate type.
- [x] Format chunks so §7/§8 need no break: separate `CTYPE1/2/3` (Codex fix: one uint16 can't hold 3 flag namespaces), `UPCASE`, reserved `FOLD_*`, header `nls_version`; accessors guard `cp < count`. (gap-audit)
- [x] Commit: `"kernel: nls -- nls_table_v1 format + loader with compiled fallback"`

**Test checkpoint:** A `nls_table_v1` blob with a bad magic or a mismatched CRC is rejected (`STATUS_INVALID_IMAGE_FORMAT`) and the compiled fallback is used. A chunk whose offset+size exceeds `total_size`, a duplicate chunk type, and a `total_size != len` are each rejected before publish. With no active table, `nls_upcase_char(>= U+0100)` passes through unchanged and `nls_get_version()` reports the compiled default -- the state `nls_init()` leaves on a missing NLS directory.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | 42 suites, 0 failures

> **Notes:**
> - **What shipped:** `nls.h` + `nls.c` -- the `nls_table_v1` format, the pure `nls_table_parse` validator (CRC32 + bounds + dup/size), the Phase-2 `nls_init` loader, a compiled ASCII+Latin-1 fallback, and the query accessors.
> - **How it integrates:** `nls_init` runs after registry in `boot_storage.c`; the table publishes with a release store + acquire-load read (SMP live at Phase 2); load/parse failure logs degraded and serves the fallback.
> - **Downstream effects:** S5 reuses the format for code-page tables; S7 owns the reserved FOLD_* chunks; S8 consumes CTYPE/version; S9 retrofits OB/registry onto `nls_upcase_char`. Real full-BMP data + generator is owned by S10.
> - **Canonical doc:** [`include/kernel/nt/nls.h`](../../include/kernel/nt/nls.h).
> - **Scope boundary:** S4 owns format + loader + validation + fallback + accessors. GetStringTypeW/GetNLSVersionEx are S8; fold/collation payload S7; transcoding S5; consumer retrofit S9. CRC32 detects accidental corruption only.
> **Verified:** 2026-07-04 | commit `3487af07` | 6/6 items | build OK | nls 53/53 PASS | smoke PASS
> **Quality reviewed:** 2026-07-04 | Codex 10x (design, adversarial, re-adversarial, test-coverage, consistency, perf) | 6M fixed, 1H+1M accepted-XREF | scope: kernel-code-quality

---

## 5. Code Page Conversion Providers

- [x] UTF-8 <-> UTF-16LE algorithmic codec in `nls_cp.c` (`nls_cp_utf8_to_utf16`/`nls_cp_utf16_to_utf8`); rejects overlong/lone-surrogate/>U+10FFFF on decode and lone/invalid surrogates on encode; sizing pass via `dst==NULL`.
- [x] CP437/CP850/Windows-1252 <-> UTF-16LE as compiled 256-entry tables; CP1252 undefined bytes (0x81/0x8D/0x8F/0x90/0x9D) carry an `NLS_CP_UNDEFINED` sentinel.
- [x] `NLS_CP_STRICT` (fail) + `NLS_CP_REPLACE` (U+FFFD decode / default byte encode) modes across UTF-8 and SBCS.
- [x] `NLS_CP_BESTFIT` mode: a 1-byte best-fit map (smart quotes/dashes/bullet/nbsp -> ASCII, keeping SBCS `max_char_size==1`); opt-out = not passing BESTFIT (equiv `WC_NO_BEST_FIT_CHARS`). (gap-audit)
- [x] `nls_cp_get_provider(cp)` lookup; resolves pseudo pages (ACP/OEMCP/THREAD_ACP); unknown id -> NULL.
- [x] `GetACP`/`GetOEMCP` read `HKLM\SYSTEM\Nls` ACP(1252)/OEMCP(437) via `RegGetDword` (compiled fallback); `CP_THREAD_ACP` resolves to system ACP (per-thread override -> §6). (gap-audit)
- [x] `nls_cp_get_info` (CPINFO/CPINFOEX: max_char_size, default_char, name) + `nls_cp_is_dbcs_lead_byte` (SBCS -> always false). (gap-audit)
- [x] `nls_cp_is_valid` + `nls_cp_enum` (walks the provider array). (gap-audit)
- [x] Commit: `"kernel: nls -- code page conversion providers (UTF-8/CP437/CP850/1252)"`

**Test checkpoint:** UTF-8 round-trips a 3-byte BMP sequence and a surrogate-pair 4-byte sequence through UTF-16LE. An invalid UTF-8 lead byte yields U+FFFD in replacement mode and an error in strict mode. CP1252 0x80 maps to U+20AC (Euro). Provider lookup by unknown code page ID returns NULL.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | 71 suites, 0 failures

> **Notes:**
> - **What shipped:** `nls_cp.h` + `nls_cp.c` -- UTF-8<->UTF-16LE codec, compiled CP437/CP850/CP1252 tables, strict/replace/best-fit modes, provider registry, GetACP/GetOEMCP policy, CPINFO/IsDBCSLeadByte/IsValidCodePage/EnumSystemCodePages.
> - **How it integrates:** pure kernel-resident functions (no boot init); `nls_cp_register_defaults()` seeds `HKLM\SYSTEM\Nls` from `registry_populate_defaults`; conversions with explicit code pages never touch the registry.
> - **Downstream effects:** consumable by VFS/console/future syscalls; the Win32 MultiByteToWideChar/WideCharToMultiByte/GetACP export + SSDT surface is owned by S8. Full per-thread CP_THREAD_ACP (SetThreadLocale) is owned by S6.
> - **Canonical doc:** [`include/kernel/nt/nls_cp.h`](../../include/kernel/nt/nls_cp.h).
> - **Scope boundary:** S5 owns UTF-8 + the three SBCS providers + policy + metadata. DBCS providers, best-fit corpus beyond the ASCII lookalikes, and Win32/SSDT exposure are out of scope. UTF-7 not implemented.
> **Verified:** 2026-07-04 | commit `fbe79801` | 9/9 items | build OK | nls 71/71 PASS
> **Accepted:** [M] ACP/OEMCP cache snapshots at populate-defaults time, before any hive load (no persisted-policy load exists today) -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §6 (item: "registry_load_hives" at line 194)
> **Quality reviewed:** 2026-07-04 | Codex 12x (design, adversarial, re-adversarial, consistency, perf) | 3H+9M fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 6. Locale and LCID Metadata

- [x] Locale records in `nls_locale.c` (LCID, BCP-47, language/region, ANSI/OEM code page, separators, date/time formats, currency, first-day); invariant + en-US/en-GB/de-DE/fr-FR/es-ES; `by_lcid`/`by_bcp47` round-trip.
- [x] Kernel stores invariant + installed-locale metadata (compiled `s_locales[]`); user-mode libraries format UI strings over it.
- [/] Registry policy: `HKLM\SYSTEM\Nls` SystemLocale/UserLocale/UILanguage seeded + boot-cached; `NtQueryDefaultLocale`/`NtQueryDefaultUILanguage` bound to it. Privileged `NtSetDefault*` stays fail-closed pending SRM.
- [/] Locale change notification: WM_SETTINGCHANGE (lParam "intl") via 08-graphics-ui/TODO-15-win32k-shadow-ssdt.md §25 (item: "NtUserBroadcastSystemMessage"). BLOCKED: TODO-15 §25 unshipped.
- [x] 3-way split (system / user / UI language) stored + independently queryable via registry policy (per-user HKCU routing deferred until per-user hives). (gap-audit)
- [x] Ordered MUI UI-language fallback chain `nls_locale_ui_fallback` (specific -> neutral primary -> en-US backstop, de-duped); mirrors `SetThreadPreferredUILanguages`. (gap-audit)
- [/] Locale-formatted timezone DISPLAY NAMES over the tz table owned by `08-graphics-ui/TODO-12-clock-time.md` (§6 owns only the display-string layer). BLOCKED: clock-time §5 tz table unshipped. (gap-audit)
- [/] Per-thread `CP_THREAD_ACP`: wire `SetThreadLocale`->locale->code-page so `nls_cp_resolve(CP_THREAD_ACP)` uses the thread locale's ANSI page (schema ready: `nls_locale_t.ansi_code_page`); BLOCKED: needs per-thread locale storage. (from §5)
- [/] When `registry_load_hives()` is wired (persisted policy), refresh the NLS ACP/OEMCP + locale boot caches after hive load; §5/§6 cache at populate-defaults time. BLOCKED: `registry_load_hives()` unwired. (Codex §5)
- [x] Commit: `"kernel: nls -- locale/LCID metadata records + registry policy binding"`

**Test checkpoint:** `NtQueryDefaultLocale` returns the registry-selected LCID; the invariant locale (0x007F) is always queryable. A BCP-47 name round-trips to its LCID. `NtSetDefaultLocale` from an unprivileged caller fails closed until the §6 policy binding lands.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | 75 suites, 0 failures

> **Notes:**
> - **What shipped:** `nls_locale.h` + `nls_locale.c` -- compiled locale records (6 locales, ANSI/OEM code page + formats), `by_lcid`/`by_bcp47` round-trip, MUI fallback chain, and the registry-backed system/user/UI locale policy.
> - **How it integrates:** `nls_locale_register_defaults()` seeds `HKLM\SYSTEM\Nls` (boot-cached) from `registry_populate_defaults` and syncs the `nt_misc.c` locale globals; `NtQueryDefaultLocale` honors the UserProfile flag (user vs system).
> - **Downstream effects:** gives the TODO-12 §23 query handlers registry backing; the privileged `NtSetDefault*` setter stays fail-closed pending the SRM privilege check.
> - **Canonical doc:** [`include/kernel/nt/nls_locale.h`](../../include/kernel/nt/nls_locale.h).
> - **Scope boundary:** §6 owns records + registry-policy query binding + MUI fallback. Privileged setter -> SRM; WM_SETTINGCHANGE -> TODO-15 §25; tz display -> clock-time §5; CP_THREAD_ACP + hive-refresh deferred (blocked).
> **Verified:** 2026-07-04 | commit `cea10cbc` | 4/9 items | build OK | nls 75/75 PASS
> **Accepted:** [M] WM_SETTINGCHANGE(intl) locale-change broadcast -> XREF: 08-graphics-ui/TODO-15-win32k-shadow-ssdt.md §25 (item: "NtUserBroadcastSystemMessage" at line 447)
> **Accepted:** [M] locale-formatted timezone display names -> XREF: 08-graphics-ui/TODO-12-clock-time.md §5 (item: "Timezone database" at line 49)
> **Deferred:** [M] privileged NtSetDefaultLocale/UILanguage binding stays fail-closed pending the SRM privilege check -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §6 (item: "Registry policy" at line 188)
> **Deferred:** [M] per-thread CP_THREAD_ACP wiring needs per-thread locale storage -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §6 (item: "Per-thread `CP_THREAD_ACP`" at line 193)
> **Deferred:** [L] NLS cache refresh after registry_load_hives -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §6 (item: "registry_load_hives" at line 194)
> **Quality reviewed:** 2026-07-04 | Codex 7x (design, adversarial, re-adversarial, consistency, perf) | 1H+2M fixed | scope: kernel-code-quality

---

## 7. Sort Keys and Normalization Policy

- [x] `nls_sort_key` (invariant collation) + `nls_sort_key_binary` (ordinal) + `nls_sort_key_binary_compare` (length-aware comparator that ships with the terminator-less binary key) in `nls_sort.c`; case-insensitive via the `ignore_case` band drop.
- [x] Culture-aware collation deferred to user-mode: the kernel ships only the invariant sort key (no per-locale collation weights).
- [x] Normalization policy: OB/registry compare stays code-unit-exact (no silent normalize); `nls_normalize` is the explicit opt-in helper.
- [x] Composed/decomposed comparison test (`test_nls_no_silent_normalization`): precomposed != decomposed under `RtlEqualUnicodeString`; explicit NFC makes them equal.
- [x] Forms named: NFC + NFD over ASCII+Latin-1 now; NFKC/NFKD + full UAX #15 deferred to user-mode; `nls_normalize` returns `NLS_NORM_ERR_UNSUPPORTED` for code units >= U+0100 (honest coverage). (gap-audit)
- [x] `nls_fold_string` (FoldStringW): MAP_FOLDDIGITS + MAP_FOLDCZONE compiled ranges; non-covered units pass through unchanged (narrow coverage, documented in the header). Full Nd/compat fold owned by §8 public export + §10 corpus. (gap-audit)
- [x] `LCMAP_SORTKEY` opaque byte format: base-254 primary band (bytes >= 0x02) + `0x01` separator + case band + `0x00` terminator, so memcmp is a correct total order (prefix sorts first). (gap-audit)
- [x] Commit: `"kernel: nls -- invariant sort keys + normalization policy"`

**Test checkpoint:** Case-insensitive sort keys order "apple" < "Banana" < "cherry". The kernel does NOT fold a composed vs decomposed object name to equal (no silent normalization); the explicit normalization helper does when asked. A binary sort key is stable and comparison-consistent.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | 87 suites, 0 failures

> **Notes:**
> - **What shipped:** `nls_sort.h` + `nls_sort.c` -- invariant + ordinal sort keys (LCMAP_SORTKEY base-254 memcmp-order format), the explicit ASCII+Latin-1 `nls_normalize` (NFD/NFC round-trip), and `nls_fold_string` (FoldStringW digit/width/compat).
> - **How it integrates:** stateless/re-entrant kernel functions (no boot init, no locks); sort keys fold via the §2/§4 `nls_upcase_char` authority; normalization + fold are pure over compiled tables.
> - **Downstream effects:** §8 owns the Win32 LCMapStringEx/FoldStringW/CompareStringEx export + SSDT surface; wiring FoldStringW to consult the §4 FOLD_* chunks lands with the §10 fold-data corpus.
> - **Canonical doc:** [`include/kernel/nt/nls_sort.h`](../../include/kernel/nt/nls_sort.h).
> - **Scope boundary:** §7 owns the invariant sort key + narrow normalization + compiled FoldStringW. Culture-aware collation, NFKC/NFKD, and full UAX #15 are user-mode; disk fold-table data is §10.

> **Verified:** 2026-07-04 | commit `9df3044b` | 7/7 items | build OK | tests 253/253 PASS
> **Accepted:** [M] §7's 2-band sort key forecloses `NORM_IGNORENONSPACE`/`IGNORESYMBOLS` + word-sort flags (diacritic/case weight is baked into the primary band) -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §8 (item: "Surface `LCMapStringEx`/`CompareStringEx` over §7 sort keys" at line 248)
> **Quality reviewed:** 2026-07-04 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 3M+2L fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 8. Native Atom/NLS/Locale Syscalls

- [ ] Wire `NtGetNlsSectionPtr` as a read-only per-process-mapped NLS SECTION. DEFERRED: needs SECTION objects + per-process address space (user pages share kernel frames today). (gap-audit)
- [x] Add `SystemNlsInformation` (class 0x1001) to `NtQuerySystemInformation`: `nt_query_nls_information` snapshots ACP/OEMCP/LCIDs/langids/NLS version, two-pass length contract. (`nls_syscall_info.h`)
- [x] Wire the orphaned MUI SSDT trio: `NtIsUILanguageComitted` (0x0264 query), `NtFlushInstallUILanguage` (0x0265 fail-closed until SRM), `NtGetMUIRegistryInfo` (0x0263 in/out-size marshaller, simplified locale-snapshot blob). (gap-audit)
- [ ] Full `NtGetMUIRegistryInfo` (Flags-driven null-delimited preferred-UI-language multi-string) + `NtIsUILanguageComitted` installed-vs-active semantics: both need a language-pack/MUI-install subsystem. DEFERRED. (parity §8)
- [ ] Unify the UI-language store: settable `nt_locale` default vs `nls_locale` registry policy sync only at boot; a runtime setter diverges the query surfaces. Fold to one backing when the SRM SET path un-gates. (review §8)
- [/] `GetNLSVersionEx` version via `SystemNlsInformation.NlsVersion` (`nls_get_version()`). DEFERRED: full `NLSVERSIONINFOEX` (DefinedVersion/EffectiveId/GuidCustomVersion) needs the NLS ABI to carry them. (gap-audit)
- [ ] Public `FoldStringW` folds the full Nd digit set + compatibility zone (§4 FOLD_* / §10 corpus), failing closed on uncovered fold-relevant units so success never masks a partial fold. (Codex §7)
- [ ] Surface `LCMapStringEx`/`CompareStringEx` over §7 sort keys; the public key needs droppable diacritic + case tiers so `NORM_IGNORENONSPACE`/`IGNORESYMBOLS` + word-sort flags are honorable (§7 key is 2-band ordinal). (parity §7)
- [ ] Commit: `"kernel: nls -- NtGetNlsSectionPtr + SystemNlsInformation + MUI/version query APIs"`

> **Note:** `NtAddAtom`/`NtFindAtom`/`NtDeleteAtom`/`NtQueryInformationAtom` and `NtQueryDefaultLocale`/`NtSetDefaultLocale`/`NtQueryDefaultUILanguage`/`NtSetDefaultUILanguage` are already wired in TODO-12 §23 (`src/kernel/nt/nt_misc.c`, SSDT 0x00D8-0x00E2) over a global atom table + global LCID/LANGID storage. `SystemNlsInformation` + the orphaned MUI trio ship here; `NtGetNlsSectionPtr` (per-process mapped section) is deferred on SECTION/per-process-VA infra.

**Test checkpoint:** `NtQuerySystemInformation(SystemNlsInformation)` fills the caller buffer with the ACP/OEMCP/LCID/langid/NLS-version snapshot or returns `STATUS_INFO_LENGTH_MISMATCH` with the required length. `NtGetMUIRegistryInfo` proves the caller's in/out capacity before copying and rewrites the required size on every outcome. `NtFlushInstallUILanguage` returns `STATUS_PRIVILEGE_NOT_HELD`. `NtIsUILanguageComitted` reports TRUE only for the active UI language.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | 91 suites, 0 failures

> **Notes:**
> - **What shipped:** `nls_syscall_info.h` (SYSTEM_NLS_INFORMATION + MUI_REGISTRY_INFO ABI) + `nt_query_nls_information` in `nt_syscall.c` (NtQuerySystemInformation class 0x1001) + the MUI SSDT trio 0x0263-0x0265 in `nt_misc.c`.
> - **How it integrates:** all handlers snapshot already-published NLS state (boot-cached ACP/OEMCP, atomic locale caches, immutable NLS version) so no lock is held; probes/copies go through ProbeFor*IfUser + copy_{to,from}_user.
> - **Downstream effects:** GetNLSVersionEx callers read the version via SystemNlsInformation; the write path (NtFlushInstallUILanguage) stays fail-closed until the SRM privilege check exists.
> - **Canonical doc:** [`include/kernel/nt/nls_syscall_info.h`](../../include/kernel/nt/nls_syscall_info.h).
> - **Scope boundary:** §8 ships the read-only NLS/MUI query syscalls; NtGetNlsSectionPtr (section infra), full public FoldStringW (§10 corpus), and LCMapStringEx/CompareStringEx (multi-tier key) are deferred to their owners.

> **Verified:** 2026-07-04 | commit `196cc251` | 2/8 items ([/] partial) | build OK | tests 335/335 PASS
> **Accepted:** [H] `ProbeFor*IfUser` skips validation on KernelMode but `ssdt_previous_mode()` resolves via a global (not per-CPU) cursor -- systemic SSDT trust-boundary gap on every user-copying handler -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (item: "Per-CPU current-thread cursor" at line 120)
> **Deferred:** [H] `NtGetMUIRegistryInfo` real contract is a Flags-driven null-delimited preferred-UI-language multi-string, not the shipped fixed blob -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §8 (item: "Full `NtGetMUIRegistryInfo`" at line 250)
> **Deferred:** [M] `nt_locale`/`nls_locale` UI-language stores diverge after a runtime SET -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §8 (item: "Unify the UI-language store" at line 251)
> **Quality reviewed:** 2026-07-04 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 4M+1L fixed, 1H accepted-XREF, 1H+1M deferred | scope: kernel-code-quality

---

## 9. Retrofit Kernel Consumers

- [x] OB namespace lookup is case-insensitive via the compiled fold: `dir_name_eq` (`ob_ns.c`) folds each name byte through `rtl_upcase_char_inline`, so `\BaseNamedObjects\Foo` and `...\foo` resolve to the same object.
- [x] Disk-fold trust decision (DECIDED): OB/registry/atom SECURITY compares stay on the compiled `rtl_upcase_char`/`_inline`, NEVER the disk-backed `nls_upcase_char`; the `nls.h` contract records it. (Codex §4)
- [x] Hot name-compare/hash loops fold through inline `rtl_upcase_char_inline` (ASCII fast path; non-ASCII delegates to the one authority): OB `dir_name_eq`, registry `reg_stricmp`/`reg_fnv1a`, atom `atom_name_eq`/`atom_name_hash`. (Codex §4 perf)
- [x] Registry key/value lookup folds through the canonical `rtl_upcase_char_inline` (`reg_stricmp` + `reg_fnv1a`) and preserves original casing (stored name unchanged; only compare/hash fold).
- [x] Environment-variable matching (`reg_lookup_env_var` over HKLM\System\Environment) is case-insensitive via the same registry `reg_stricmp` canonical fold.
- [/] PE import lookup keeps its ASCII fast path (PE/COFF names are 8-bit ASCII, correct as-is). UTF-16 module-path input DEFERRED: no `LoadLibraryW`/wide-`CreateProcess` caller exists yet. (parity §9)
- [/] File-name APIs fail closed on non-ASCII/NUL/malformed via the shared `nt_wname_to_ascii` (`NtDeleteFile`/`NtQueryAttributesFile`/`FileRenameInformation`; snapshot-once, no TOCTOU). `NtCreateFile` decode + full UTF-16 ABI DEFERRED. (Codex §9)
- [ ] `NtCreateFile`/`NtOpenFile` name decode: `oa_extract_path` casts UTF-16 `Buffer` to `char*` with NO decode (reads 1 char of a real UTF-16 name); route through `nt_wname_to_ascii`. (parity §9)
- [ ] Thread `OBJ_CASE_INSENSITIVE` through `dir_find`/`dir_name_eq` so a caller can request case-SENSITIVE OB lookup (NT default for named objects); OB is unconditionally case-insensitive today. (parity §9)
- [ ] OB/registry full-BMP (U+0100+) case-insensitivity needs a COMPILE-TIME BMP fold table (the disk `nls_upcase_char` is barred from security compares); ASCII+Latin-1 only today. (parity §9)
- [ ] Commit: `"kernel: nls -- retrofit OB/registry/env/PE/file consumers onto canonical compare"`

> **Scope boundary (gap-audit):** The USER window-message atom pool (`RegisterWindowMessage`/`RegisterClass`/`RegisterClipboardFormat`, distinct from the global object-atom table) is owned by the win32k/desktop compositor TODO -> XREF that consumer, not §3. The console-vs-ANSI code-page split (`CHCP`) is owned by the shell TODO. IXFS filesystem-layer opt-in casefold/normalize (ext4 `EXT4_CASEFOLD_FL` analog, disk-persisted charset) is owned by the filesystem/IXFS TODO. OUT OF SCOPE for this kernel NLS TODO: IDN/punycode (`IdnToAscii`/`IdnToUnicode` -- user-mode library on Windows, no NT syscall) and non-Gregorian calendar metadata (`LOCALE_ICALENDARTYPE`); revisit only if a kernel consumer proves need.

**Test checkpoint:** Creating `\BaseNamedObjects\Foo` then opening `\BaseNamedObjects\foo` resolves the same object via the canonical compare. A registry key stored as "Software" opens case-insensitively but reports its original casing. `PATH` and `Path` resolve to the same environment variable.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) + `run-ob-tests.bat` (SUITE=ob) + `run-abi-tests.bat` (SUITE=abi) | 0 failures

> **Notes:**
> - **What shipped:** `rtl_upcase_char_inline` (`nt_rtlstr.h` inline ASCII fast path) + retrofitted consumers OB `dir_name_eq`, registry `reg_stricmp`/`reg_fnv1a`, atom `atom_name_*`, plus file-name fail-closed on non-ASCII (`nt_file.c`).
> - **How it integrates:** every security name compare/hash folds through the COMPILED authority (ASCII inline, Latin-1 out-of-line; U+0100+ unchanged today -- full-BMP needs a compile-time table); the disk-backed `nls_upcase_char` is never on a security path (trust boundary recorded in `nls.h`).
> - **Downstream effects:** closes the §4 accepted disk-fold-trust [H] + inline-fast-path [M]; OB/registry are now Latin-1 case-insensitive, not just ASCII.
> - **Canonical doc:** [`include/kernel/nt/nt_rtlstr.h`](../../include/kernel/nt/nt_rtlstr.h).
> - **Scope boundary:** §9 folds the existing byte-name consumers; the systemic UTF-16 `OBJECT_ATTRIBUTES` ABI (real wide file names, `LoadLibraryW` wide paths) is deferred to its future owner.

> **Verified:** 2026-07-04 | commit `ab27e7c2` | 5/10 items ([/] partial) | build OK | nls 345 + ob 429 PASS
> **Accepted:** [H] file-name handlers cast a1 to OBJECT_ATTRIBUTES* without ProbeForRead (systemic ssdt previous_mode probe-gating gap) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (item: "Per-CPU current-thread cursor" at line 120)
> **Deferred:** [H] `NtCreateFile`/`NtOpenFile` `oa_extract_path` casts UTF-16 to char* with no decode -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §9 (item: "`NtCreateFile`/`NtOpenFile` name decode" at line 285)
> **Deferred:** [H] OB lookup unconditionally case-insensitive; OBJ_CASE_INSENSITIVE unread -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §9 (item: "Thread `OBJ_CASE_INSENSITIVE`" at line 286)
> **Deferred:** [M] OB/registry full-BMP case-insensitivity foreclosed on the compiled fold -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §9 (item: "OB/registry full-BMP" at line 287)
> **Quality reviewed:** 2026-07-04 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1M+1L fixed, 2H+1M deferred, 1H accepted-XREF | scope: kernel-code-quality

---

## 10. Tests and Compatibility Corpus

- [x] Unit tests: UTF-8 invalid/overlong/surrogate rejection, surrogate round-trip, case-insensitive compare, atom refcounts (test_nt_misc.c), CP437 + CP1252 round trip -- all under TEST_CAT_NLS/ABI.
- [x] Win32 atom APIs via SSDT dispatch (`test_nls_atom_syscall_roundtrip`: NtAddAtom/NtFindAtom/NtDeleteAtom) + pure nt_atom_* helpers + `CompareStringOrdinal` (test_nls.c).
- [x] `test_nls_fuzz_counted_strings`: deterministic 512-iter LCG fuzz over the validator/decode/RtlEqual/sort-key/normalize/fold helpers, per-target contracts + dst canary (never crash/overread/overrun).
- [x] Missing-NLS invariant fallback: `test_nls_missing_dir_fallback` (no active table -> compiled fallback); boot degraded-state marker via the smoke path.
- [ ] Compatibility corpus (full-BMP): host tool emits a real invariant.nls (UPCASE + CTYPE1/2/3, §4 format); unblocks §7 full-FoldStringW + §9 full-BMP. DEFERRED (host tooling + build wiring). (design §10)
- [ ] Minimal real-table boot fixture: a valid invariant.nls in sysroot + a boot/test assertion that nls_init loaded it (version != fallback), proving the disk-load path not just fallback. (design §10)
- [x] Tests for `GetStringTypeW` C1/C2/C3 (`nls_char_type`), `FoldStringW`, `LCMapStringEx` sort keys, `GetNLSVersionEx`, `GetCPInfoEx`/`IsDBCSLeadByte` (SBCS), `CompareStringOrdinal`. (gap-audit)
- [x] `TEST_CAT_NLS` wired: enum (test.h), labels (test_runner.c), `make test-nls` (Makefile), bootx64.c parser (test_suite=nls), run-nls-tests.bat.
- [ ] Commit: `"kernel: nls -- unit tests + compatibility corpus (TEST_CAT_NLS)"`

**Test checkpoint:** `test_nls.c` registers under `TEST_CAT_NLS`; the suite covers UTF-8 invalid-sequence rejection, surrogate-pair round trip, case-insensitive compare, atom refcount add/find/delete, and CP437/CP1252 round trips, all with concrete expected values. Boot with a missing NLS dir passes with a logged degraded-state marker.

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | 96 suites, 0 failures

> **Notes:**
> - **What shipped:** §10 test surface -- `test_nls_fuzz_counted_strings` (512-iter LCG fuzz), `test_nls_cp_roundtrip` (CP437/CP1252), `test_nls_atom_syscall_roundtrip` (atom SSDT dispatch), + the `make test-nls` target; 96 nls suites.
> - **How it integrates:** all under TEST_CAT_NLS via `test_register_nls`; the fuzz asserts every counted-string helper returns a defined/bounded result + dst canary intact for random malformed inputs.
> - **Downstream effects:** items 1/2/3/4/6/7 close the §1-§9 test coverage; the disk-load NLS corpus is the only open piece.
> - **Canonical doc:** [`src/kernel/test/test_nls.c`](../../src/kernel/test/test_nls.c).
> - **Scope boundary:** the full-BMP `invariant.nls` corpus (host tool + build wiring) and the minimal real-table boot fixture are deferred `[ ]` items; they also own the deferred §7 full-FoldStringW + §9 full-BMP.

> **Verified:** 2026-07-04 | commit `e877efe5` | 6/8 items ([/] partial) | build OK | 96 nls suites PASS
> **Deferred:** [H] boot still only exercises the fallback NLS table; the real disk-load path is unproven -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §10 (item: "Minimal real-table boot fixture" at line 319)
> **Quality reviewed:** 2026-07-04 | Codex 6x (adversarial, consistency, perf, re-adversarial x3) | 4M+1L fixed | scope: kernel-code-quality (test-only)

---

## OS Comparison

| ⭐  | Feature                        | 🪟 Win11                                          | 🐧 Linux                         | 🚀 Impossible OS                                                                |
| --- | ------------------------------ | ------------------------------------------------- | -------------------------------- | ------------------------------------------------------------------------------- |
| 💎  | Global + local atom tables     | ✅ Global/Local `AddAtom`                         | ⚠️ no direct equivalent          | 🔄 global NLS-folded + bounded (§3); local user-mode (ntdll, see §3)            |
| 💎  | Unicode case-fold authority    | ✅ `RtlUpcaseUnicodeString` NLS                   | ✅ ICU / glibc `towupper`        | 🔄 invariant ASCII+Latin-1 fold + Rtl compare (§2/§4); full-BMP §10-deferred    |
| 💎  | Code page conversion           | ✅ `MultiByteToWideChar` NLS                      | ✅ `iconv`                       | ✅ UTF-8/CP437/CP850/1252 strict/replace/best-fit (§5)                          |
| 💎  | LCID / locale metadata         | ✅ `GetLocaleInfoEx` LCID                         | ✅ `setlocale` / `nl_langinfo`   | 🔄 records + registry system/user/UI policy (§6); privileged setter -> SRM      |
| 💎  | Sort keys / collation          | ✅ `CompareStringEx` linguistic                   | ✅ ICU collation, `strcoll`      | ✅ invariant + case-insensitive `nls_sort_key` (§7); culture-aware -> user-mode |
| 💎  | NLS table loading + fallback   | ✅ `l_intl.nls` at boot                           | ✅ locale archive                | ✅ `nls_table_v1` loader + CRC/bounds + compiled fallback (§4)                  |
| 💎  | Object-name Unicode compare    | ✅ OB case-insensitive NLS                        | ⚠️ VFS bytewise (case-sensitive) | 🔄 OB/registry/atom case-insensitive fold (§9); full-BMP needs a compiled table |
| ⭐  | Normalization policy           | ✅ `NormalizeString` NFC/NFD                      | ✅ ICU normalizer                | 🔄 no-silent-normalize + ASCII/Latin-1 NFC/NFD (§7); full UAX #15 user-mode     |
| 💎  | Char-type + folding APIs       | ✅ `GetStringType`/`FoldString`                   | ✅ ICU `u_charType`              | 🔄 compiled `FoldStringW` (§7); `nls_char_type` C1-C3; GetStringTypeW deferred  |
| 💎  | Code-page metadata / DBCS      | ✅ `GetCPInfoEx`/`IsDBCSLeadByte`                 | ✅ `nl_langinfo` / iconv         | 🔄 CPINFO/ACP/OEMCP (§5); DBCS providers deferred                               |
| ⭐  | MUI UI-language fallback chain | ✅ `SetThreadPreferredUILanguages`                | ✅ gettext `LANGUAGE` list       | ✅ `nls_locale_ui_fallback` ordered chain (§6)                                  |
| 💎  | NLS/MUI native query syscalls  | ✅ `NtQuerySystemInformation` / `GetNLSVersionEx` | ⚠️ `/proc` + `localedef`         | 🔄 SystemNlsInformation + MUI trio (§8); section-ptr deferred                   |

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

> **Test runner:** `scripts\debug\kernel\run-nls-tests.bat` (SUITE=nls) | 96 suites, 0 failures (2026-07-04 WSL2 TCG)
