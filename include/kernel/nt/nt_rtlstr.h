/* ============================================================================
 * nt_rtlstr.h -- Invariant UTF-16 case fold + Rtl*UnicodeString compare authority
 *
 * The single kernel-side authority for locale-independent (invariant) UTF-16
 * casing and counted-string comparison. The Object Manager, Registry, and the
 * atom table compare object names through these helpers instead of open-coding a
 * private ASCII fold.
 *
 * Inputs are KERNEL-RESIDENT UNICODE_STRINGs / WCHAR arrays: syscall callers must
 * first snapshot + validate user pointers via nt_unicode_string_validate() (see
 * nt_unicode.h), so nothing here probes user memory. Every function is stateless
 * and re-entrant (no shared mutable state), safe on all CPUs.
 *
 * Casing scope: the fold is INVARIANT (locale-independent) and covers ASCII a-z
 * plus the Latin-1 Supplement. The full-BMP algorithmic fold table and the
 * public GetStringTypeW/GetStringTypeEx classification API are owned by the NLS
 * table loader, not this layer. Locale special-casing (Turkish dotless-i, German
 * sharp-s expansion, supplementary-plane) is deliberately OUT of the kernel Rtl
 * layer -- it mirrors NT, where user-mode LCMapStringEx owns it.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/ob/peb.h"   /* UNICODE_STRING */

/* Win32 CompareStringOrdinal / CompareString family return codes. */
#define NT_CSTR_ERROR         0
#define NT_CSTR_LESS_THAN     1
#define NT_CSTR_EQUAL         2
#define NT_CSTR_GREATER_THAN  3

/*
 * rtl_upcase_char -- invariant uppercase fold of one UTF-16 code unit.
 *
 * Maps ASCII a-z (0x61-0x7A) and the Latin-1 Supplement lowercase letters
 * (0xE0-0xFE except the 0xF7 division sign) to their uppercase form, plus the
 * special case 0xFF (small y with diaeresis) -> U+0178. Every other code unit is
 * returned UNCHANGED -- including U+00DF (sharp s), whose uppercase "SS" is an
 * expansion NT's 1:1 Rtl upcase does not perform, and everything at or above
 * U+0100 (deferred to the NLS table loader's full-BMP table). Never miscases: a
 * code unit is only folded when its invariant uppercase is a single known code
 * unit.
 */
uint16_t rtl_upcase_char(uint16_t c);

/*
 * rtl_upcase_char_inline -- inline ASCII fast path for HOT name-compare/hash
 * loops (OB directory walk, registry lookup, atom table -- all fold per code
 * unit under a spinlock, so an out-of-line call per char is a real cost; this
 * build has no LTO to inline it away). ASCII (< 0x80) folds inline with no
 * call; every non-ASCII code unit delegates to the out-of-line rtl_upcase_char
 * so there is ONE fold table (no divergence). Use this in loops; use
 * rtl_upcase_char where a function pointer or a single call is needed.
 * SECURITY: this is the COMPILED invariant fold; security name compares (OB /
 * registry / atom) MUST route here or through rtl_upcase_char, NEVER through
 * the disk-backed nls_upcase_char (a tampered NLS table must not be able to
 * collapse two distinct secured names).
 */
static inline uint16_t rtl_upcase_char_inline(uint16_t c)
{
    if (c >= 0x61u && c <= 0x7Au)   /* ASCII a-z: the hot case */
        return (uint16_t)(c - 0x20u);
    if (c < 0x80u)                  /* any other ASCII never folds */
        return c;
    return rtl_upcase_char(c);      /* non-ASCII: the full compiled authority */
}

/*
 * RtlUpcaseUnicodeString -- write the invariant uppercase of src into the
 * caller-provided dst buffer. dst->Buffer / dst->MaximumLength describe the
 * destination; dst->Length is set to the byte count written on success (upcase
 * is 1:1, so it equals src->Length). No pool allocation happens at the kernel
 * Rtl layer, so allocate_destination MUST be 0 -- the AllocateDestinationString
 * TRUE convenience is a user-mode ntdll concern and a non-zero value returns
 * STATUS_INVALID_PARAMETER. Returns STATUS_BUFFER_TOO_SMALL when dst cannot hold
 * src->Length bytes, STATUS_INVALID_PARAMETER on a NULL argument or a src that
 * violates the canonical validity rules (odd Length, Length > MaximumLength,
 * non-empty with a NULL Buffer).
 */
NTSTATUS RtlUpcaseUnicodeString(UNICODE_STRING *dst, const UNICODE_STRING *src,
                                int allocate_destination);

/*
 * RtlEqualUnicodeString -- 1 when a and b have equal byte Length and equal
 * contents, else 0. When case_insensitive != 0 both sides are compared under the
 * invariant fold. Kernel-resident inputs only; a malformed operand (odd Length,
 * Length > MaximumLength, non-empty with NULL Buffer) is never reported equal.
 */
int RtlEqualUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b,
                          int case_insensitive);

/*
 * RtlCompareUnicodeString -- lexicographic code-unit compare. Returns a value
 * < 0, 0, or > 0 (memcmp convention) with the invariant fold applied to both
 * sides when case_insensitive != 0. A shorter string that is a prefix of a
 * longer one sorts first. Kernel-resident inputs only. A malformed operand is
 * compared over its overread-safe readable content (Length clamped to
 * MaximumLength, even-floored), so the result stays a proper total order
 * (reflexive + antisymmetric) usable as a sort comparator. Use
 * RtlEqualUnicodeString when malformed input must never test equal.
 */
int RtlCompareUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b,
                            int case_insensitive);

/*
 * CompareStringOrdinal -- Win32 ordinal (non-locale) compare over raw UTF-16
 * code units. a_wchars / b_wchars are WCHAR counts, or -1 for a NUL-terminated
 * string. Returns NT_CSTR_LESS_THAN / NT_CSTR_EQUAL / NT_CSTR_GREATER_THAN, or
 * NT_CSTR_ERROR (0) on a NULL pointer or a count < -1 (only -1 is the sentinel).
 * case_insensitive != 0 applies the invariant fold (full-BMP ordinal casing
 * lands with the NLS table).
 */
int CompareStringOrdinal(const uint16_t *a, int32_t a_wchars,
                         const uint16_t *b, int32_t b_wchars,
                         int case_insensitive);
