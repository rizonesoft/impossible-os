/* ============================================================================
 * nt_rtlstr.c -- Invariant UTF-16 case fold + Rtl*UnicodeString compare authority
 *
 * See include/kernel/nt/nt_rtlstr.h for the contract. Stateless and re-entrant:
 * no shared mutable state, so no locking is required on any CPU. All inputs are
 * kernel-resident (callers snapshot/validate user pointers via nt_unicode.h
 * first), so nothing here probes or copies user memory. As a central name
 * authority these helpers still defend the canonical UNICODE_STRING validity
 * rules (even Length, Length <= MaximumLength, non-NULL Buffer for non-empty
 * strings) so a malformed counted string cannot overread or false-compare.
 * ============================================================================ */

#include "kernel/nt/nt_rtlstr.h"
#include "kernel/types.h"

uint16_t rtl_upcase_char(uint16_t c)
{
    if (c >= 0x61u && c <= 0x7Au)                  /* ASCII a-z: the hot case, first */
        return (uint16_t)(c - 0x20u);
    if (c < 0x80u)                                  /* any other ASCII never folds */
        return c;
    if (c >= 0xE0u && c <= 0xFEu && c != 0xF7u)    /* Latin-1 lowercase (skip 0xF7 division) */
        return (uint16_t)(c - 0x20u);
    if (c == 0xFFu)                                 /* small y with diaeresis -> U+0178 */
        return 0x178u;
    return c;                                       /* unchanged (U+00DF and >= U+0100 pending table) */
}

/* Canonical UNICODE_STRING validity: even byte Length, Length within the
 * declared buffer capacity, and a backing Buffer for any non-empty string.
 * Mirrors nt_unicode_string_validate's field rules (minus the user probe, which
 * callers already performed). Returns 1 when the counted string is well-formed. */
static int rtl_us_valid(const UNICODE_STRING *us)
{
    if (!us)
        return 0;
    if (us->Length & 1u)                    /* must be WCHAR-aligned */
        return 0;
    if (us->Length > us->MaximumLength)     /* content cannot exceed the buffer */
        return 0;
    if (us->Length != 0u && !us->Buffer)    /* non-empty needs a backing buffer */
        return 0;
    return 1;
}

/* Overread-safe readable WCHAR count: 0 for a NULL Buffer, Length clamped to the
 * declared MaximumLength and floored to an even byte count, then halved. For a
 * well-formed string (even Length <= MaximumLength) this is exactly Length/2, so
 * the comparator below stays a proper total order even if a malformed name ever
 * reaches this defensive path. */
static uint32_t rtl_us_safe_wchars(const UNICODE_STRING *us)
{
    uint16_t len;

    if (!us || !us->Buffer)
        return 0u;
    len = us->Length;
    if (len > us->MaximumLength)
        len = us->MaximumLength;
    return (uint32_t)(len & 0xFFFEu) / 2u;   /* even-floor, then WCHARs */
}

NTSTATUS RtlUpcaseUnicodeString(UNICODE_STRING *dst, const UNICODE_STRING *src,
                                int allocate_destination)
{
    uint32_t wchars, i;

    if (!dst || !src)
        return STATUS_INVALID_PARAMETER;
    if (allocate_destination)                       /* pool alloc is a user-mode ntdll concern */
        return STATUS_INVALID_PARAMETER;
    if (!rtl_us_valid(src))                          /* defend the canonical validity rules */
        return STATUS_INVALID_PARAMETER;
    if (src->Length != 0u && !dst->Buffer)
        return STATUS_INVALID_PARAMETER;
    if (src->Length > dst->MaximumLength)           /* upcase is 1:1: bytes-out == bytes-in */
        return STATUS_BUFFER_TOO_SMALL;

    wchars = (uint32_t)src->Length / 2u;
    for (i = 0; i < wchars; i++)
        dst->Buffer[i] = rtl_upcase_char(src->Buffer[i]);
    dst->Length = src->Length;
    return STATUS_SUCCESS;
}

int RtlEqualUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b,
                          int case_insensitive)
{
    uint32_t wchars, i;

    if (!rtl_us_valid(a) || !rtl_us_valid(b))
        return 0;
    if (a->Length != b->Length)
        return 0;
    wchars = (uint32_t)a->Length / 2u;
    for (i = 0; i < wchars; i++) {
        uint16_t ca = a->Buffer[i];
        uint16_t cb = b->Buffer[i];
        if (ca == cb)                               /* exact match: skip the fold */
            continue;
        if (!case_insensitive)
            return 0;
        if (rtl_upcase_char(ca) != rtl_upcase_char(cb))
            return 0;
    }
    return 1;
}

int RtlCompareUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b,
                            int case_insensitive)
{
    /* Compare over the overread-safe readable length of each operand so the
     * result is a proper total order (reflexive + antisymmetric) even for a
     * malformed operand -- forcing a non-equal sentinel here would break sorted
     * lookups. The "malformed is never equal" guarantee lives in the boolean
     * RtlEqualUnicodeString predicate, which carries no comparator contract. */
    uint32_t na = rtl_us_safe_wchars(a);
    uint32_t nb = rtl_us_safe_wchars(b);
    uint32_t n = (na < nb) ? na : nb;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint16_t ca = a->Buffer[i];
        uint16_t cb = b->Buffer[i];
        if (ca != cb) {                             /* fold only on a raw mismatch */
            if (case_insensitive) {
                ca = rtl_upcase_char(ca);
                cb = rtl_upcase_char(cb);
            }
            if (ca != cb)
                return (ca < cb) ? -1 : 1;
        }
    }
    if (na == nb)
        return 0;
    return (na < nb) ? -1 : 1;
}

int CompareStringOrdinal(const uint16_t *a, int32_t a_wchars,
                         const uint16_t *b, int32_t b_wchars,
                         int case_insensitive)
{
    uint32_t i;

    if (!a || !b)
        return NT_CSTR_ERROR;
    if (a_wchars < -1 || b_wchars < -1)             /* only -1 is the NUL-terminated sentinel */
        return NT_CSTR_ERROR;

    /* Single pass: stop at the first mismatch or the shorter operand's end, so a
     * leading difference never forces a full pre-scan of NUL-terminated inputs.
     * For a counted operand the end is index-based; for -1 it is the first
     * U+0000, tested before the code unit is used as content. */
    for (i = 0u; ; i++) {
        int a_end = (a_wchars >= 0) ? (i >= (uint32_t)a_wchars) : (a[i] == 0u);
        int b_end = (b_wchars >= 0) ? (i >= (uint32_t)b_wchars) : (b[i] == 0u);
        if (a_end || b_end) {
            if (a_end && b_end)
                return NT_CSTR_EQUAL;
            return a_end ? NT_CSTR_LESS_THAN : NT_CSTR_GREATER_THAN;
        }
        {
            uint16_t ca = a[i];
            uint16_t cb = b[i];
            if (ca != cb) {                         /* fold only on a raw mismatch */
                if (case_insensitive) {
                    ca = rtl_upcase_char(ca);
                    cb = rtl_upcase_char(cb);
                }
                if (ca != cb)
                    return (ca < cb) ? NT_CSTR_LESS_THAN : NT_CSTR_GREATER_THAN;
            }
        }
    }
}
