/* ============================================================================
 * nt_misc.h -- Atom table, locale, and miscellaneous NT syscalls (SSDT)
 *
 * Registers the "catch-all" native syscalls at SSDT 0x00D8-0x00E2:
 *   - Global atom table:  NtAddAtom / NtFindAtom / NtDeleteAtom /
 *                         NtQueryInformationAtom       (0x00DF-0x00E2)
 *   - Locale / UI lang:   NtQueryDefaultLocale / NtSetDefaultLocale /
 *                         NtQueryDefaultUILanguage / NtSetDefaultUILanguage /
 *                         NtQueryInstallUILanguage     (0x00DA-0x00DE)
 *   - Misc:               NtDisplayString (0x00D8), NtRaiseHardError (0x00D9)
 *
 * The atom table follows Win32 global-atom semantics (RtlAddAtomToAtomTable):
 * string atoms are refcounted and share IDs in the 0xC000-0xFFFF range;
 * integer atoms (MAKEINTATOM, 0x0001-0xBFFF) pass through without a table
 * entry. Names are case-insensitive (ASCII fold) and capped at 255 chars.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/ntstatus.h"

/* ---- Atom table constants (Win32 winbase.h parity) ----------------------- */

/* Integer atoms occupy 0x0001-0xBFFF; string atoms start here. A name
 * pointer numerically below MAXINTATOM is a MAKEINTATOM integer atom, not a
 * real address. */
#define NT_MAXINTATOM              0xC000u
/* First ID handed out to a string atom. */
#define NT_STRING_ATOM_BASE        0xC000u
/* Win32 RTL_MAXIMUM_ATOM_LENGTH: 255 wide chars, not counting the NUL. */
#define NT_MAX_ATOM_LEN            255u
/* Fixed capacity of the global string-atom table (compatibility cap, not a
 * correctness bound -- linear scan under one lock). */
#define NT_ATOM_TABLE_CAP          512u

/* ATOM_INFORMATION_CLASS (NtQueryInformationAtom). */
#define AtomBasicInformation       0u
#define AtomTableInformation       1u

/* ATOM_BASIC_INFORMATION -- layout matches Windows SDK ntifs.h.
 *   offset 0:  USHORT UsageCount;   (refcount)
 *   offset 2:  USHORT Flags;        (0 = string atom; 1 = integer/pinned)
 *   offset 4:  USHORT NameLength;   (byte count, excluding NUL; UNICODE_STRING convention)
 *   offset 6:  WCHAR  Name[1];      (NUL-terminated wide name; flexible)
 */
typedef struct _ATOM_BASIC_INFORMATION {
    uint16_t UsageCount;   /* 0x00 */
    uint16_t Flags;        /* 0x02 */
    uint16_t NameLength;   /* 0x04 */
    uint16_t Name[1];      /* 0x06 */
} ATOM_BASIC_INFORMATION;
_Static_assert(__builtin_offsetof(ATOM_BASIC_INFORMATION, Flags) == 2,
               "ATOM_BASIC_INFORMATION.Flags must be at offset 2 (Win ABI)");
_Static_assert(__builtin_offsetof(ATOM_BASIC_INFORMATION, NameLength) == 4,
               "ATOM_BASIC_INFORMATION.NameLength must be at offset 4 (Win ABI)");
_Static_assert(__builtin_offsetof(ATOM_BASIC_INFORMATION, Name) == 6,
               "ATOM_BASIC_INFORMATION.Name must be at offset 6 (Win ABI)");

/* ---- Locale defaults ----------------------------------------------------- */

/* en-US -- the single bootstrap locale/UI-language until a per-user hive
 * exists (UserProfile arg is accepted and ignored, see nt_misc.c). */
#define NT_DEFAULT_LCID            0x00000409u
#define NT_DEFAULT_LANGID          0x0409u

/* ---- HARDERROR_RESPONSE_OPTION (NtRaiseHardError ValidResponseOptions) ---- */

#define OptionAbortRetryIgnore     0u
#define OptionOk                   1u
#define OptionOkCancel             2u
#define OptionRetryCancel          3u
#define OptionYesNo                4u
#define OptionYesNoCancel          5u
#define OptionShutdownSystem       6u
#define OptionOkNoWait             7u
#define OptionCancelTryContinue    8u

/* ---- HARDERROR_RESPONSE (NtRaiseHardError *Response out) ------------------ */

#define ResponseReturnToCaller     0u
#define ResponseNotHandled         1u
#define ResponseAbort              2u
#define ResponseCancel             3u
#define ResponseIgnore             4u
#define ResponseNo                 5u
#define ResponseOk                 6u
#define ResponseRetry              7u
#define ResponseYes                8u
#define ResponseTryAgain           9u
#define ResponseContinue          10u

/* ---- SSDT registration --------------------------------------------------- */

/* Allocate + zero the global atom table. MUST be called once, from a
 * single-CPU boot boundary, BEFORE nt_misc_register_ssdt() publishes the atom
 * syscall handlers. Returns STATUS_INSUFFICIENT_RESOURCES if the table could
 * not be backed, in which case the atom syscalls stay registered but every one
 * of them fails closed with that status. Idempotent. */
NTSTATUS nt_misc_atoms_init(void);

void nt_misc_register_ssdt(void);

/* ---- Pure helpers exposed for unit tests --------------------------------- *
 * These operate on kernel-side wide strings (name = uint16_t*, len = chars)
 * so tests can exercise atom/locale logic without the syscall marshalling
 * path. The syscall handlers marshal user args then call these. */

/* Add or bump-refcount a string atom. Returns STATUS_SUCCESS + *out_atom on
 * success. name/len must already be validated (<= NT_MAX_ATOM_LEN chars). */
NTSTATUS nt_atom_add(const uint16_t *name, uint32_t len_chars, uint16_t *out_atom);
/* Find a string atom without changing its refcount. */
NTSTATUS nt_atom_find(const uint16_t *name, uint32_t len_chars, uint16_t *out_atom);
/* Decrement refcount; frees the slot at zero. No-op success for integer atoms. */
NTSTATUS nt_atom_delete(uint16_t atom);
/* Snapshot an atom's usage count + name into a caller buffer (chars). */
NTSTATUS nt_atom_query_basic(uint16_t atom, uint16_t *out_usage,
                             uint16_t *name_out, uint32_t name_cap_chars,
                             uint32_t *out_name_len_chars);
/* Reset the atom table to empty -- test-only fixture teardown. */
void nt_atom_reset_for_test(void);

/* Locale get/set accessors (kernel-side; the syscalls wrap these). */
uint32_t nt_locale_get_default(void);
void     nt_locale_set_default(uint32_t lcid);
uint16_t nt_locale_get_ui_language(void);
void     nt_locale_set_ui_language(uint16_t langid);
uint16_t nt_locale_get_install_ui_language(void);
