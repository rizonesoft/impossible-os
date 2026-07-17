/* ============================================================================
 * nt_rtlenv.c -- ntdll Rtl environment layer (UTF-16 %VAR% expansion)
 *
 * See include/kernel/nt/nt_rtlenv.h for the contract. Arch-neutral: no gdt/idt/
 * msr/cpuid includes. Stateless apart from a single per-call allocation on the
 * NULL-Environment path; no shared mutable state, so no locking is required
 * here (env_build_block_utf16 does its own snapshot under the env mutex).
 *
 * Four invariants keep this safe and consistent with the UTF-8 env layer:
 *  - every block scan is bounded by an EXTENT the caller guarantees is readable
 *    (rtl_env_expand_block), so a stripped terminator fails validation instead
 *    of reading past the allocation; RTL_ENV_BLOCK_MAX_WCHARS is a second
 *    ceiling, not the primary bound. The public ntdll entry cannot express an
 *    extent, so it serves only the NULL form and refuses the rest;
 *  - lookup work is BUDGETED per pass (RTL_ENV_EXPAND_WORK_MAX), so the
 *    O(references * block) product cannot burn unbounded kernel time;
 *  - a NULL Environment is honored via a synthesized current-process block (from
 *    the authoritative UTF-8 environ), not by rejecting the documented ABI form;
 *  - name matching folds ASCII only, matching the UTF-8 storage comparator, so
 *    both expansion paths resolve the same case-insensitive names identically.
 * ============================================================================ */

#include "kernel/nt/nt_rtlenv.h"
#include "kernel/env.h"
#include "kernel/nt/nls_cp.h"      /* nls_cp_utf16_to_utf8 / nls_cp_utf8_to_utf16 */
#include "kernel/sched/task.h"     /* task_current() */
#include "kernel/types.h"

/* UTF-16 code units for the ASCII characters we branch on. */
#define RTL_ENV_WPCT   ((uint16_t)'%')
#define RTL_ENV_WEQ    ((uint16_t)'=')

/* Saturation ceiling for the expansion output counter (see rtl_env_expand_pass).
 * A count that reaches this is NOT a length -- the public entry rejects it rather
 * than narrowing a wrapped value into a caller's required-size field. */
#define RTL_ENV_COUNT_SAT   0xFFFFFFFFFFFFFFFFull

/* Saturating +1. Every output-length increment goes through this so the counter
 * can never wrap silently, whatever the block cap is raised to later. */
static uint64_t rtl_env_count_inc(uint64_t out)
{
    return (out == RTL_ENV_COUNT_SAT) ? out : out + 1u;
}

/* The Rtl expansion path scans a supplied block up to RTL_ENV_BLOCK_MAX_WCHARS,
 * so every block env_create_block can produce must fit inside that bound or a
 * created block could be un-expandable. Only `<=` is required (equality is not):
 * pinning it here catches a future raise of the create cap past the scan cap at
 * COMPILE time rather than as a runtime STATUS_INVALID_PARAMETER (TODO-22 s19). */
_Static_assert(ENV_CREATE_BLOCK_MAX_WCHARS <= RTL_ENV_BLOCK_MAX_WCHARS,
               "a created env block must be consumable by the Rtl expansion scan");

/* THE invariant the s19 cap raise exists to establish: every environ the storage
 * layer accepts must be expandable through the NULL-Environment form. env_set
 * caps env_block_bytes_locked() (environ_bytes + 1) at ENV_BLOCK_MAX, and
 * env_build_block_utf16 builds total = 1 + environ_bytes WCHARs, so the widest
 * legal environ (all-ASCII, one UTF-8 byte -> one WCHAR) needs exactly
 * ENV_BLOCK_MAX WCHARs of scan bound. This holds today with ZERO margin
 * (1 Mi >= 1 Mi); without this assert, raising ENV_BLOCK_MAX by one byte or
 * lowering the scan cap would silently start refusing a legal environ with
 * STATUS_INVALID_PARAMETER, build green and tests green (TODO-22 s19). */
_Static_assert(RTL_ENV_BLOCK_MAX_WCHARS >= ENV_BLOCK_MAX,
               "the Rtl scan bound must cover every environ the store accepts");

/* A representable result must leave room for the WCHAR NUL inside a USHORT byte
 * count. Written as a division so the bound cannot itself wrap: the product form
 * ((n + 1) * 2 <= 0xFFFF) evaluates in 32-bit unsigned and would PASS for a
 * future n >= 0x7FFFFFFF by wrapping the product to 0. */
_Static_assert(RTL_ENV_MAX_RESULT_WCHARS <= (0xFFFFu / 2u) - 1u,
               "RTL_ENV_MAX_RESULT_WCHARS must leave room for the NUL in a USHORT");

/* The work budget must afford at least ONE full-block miss, or a single
 * legitimate `%NAME%` against a maximum-size block would be refused outright and
 * the budget would be rejecting correct programs rather than pathological ones.
 * This is the floor the RTL_ENV_EXPAND_WORK_MAX rationale rests on; pinning it
 * here catches a future cap raise (or budget trim) that inverts the relationship
 * at COMPILE time instead of as a mystery STATUS_INSUFFICIENT_RESOURCES
 * (TODO-22 s20). */
_Static_assert(RTL_ENV_EXPAND_WORK_MAX >= RTL_ENV_BLOCK_MAX_WCHARS,
               "the expansion work budget must afford at least one full-block miss");

/* The counted source ceiling must (1) be exactly representable as the uint32 the
 * inner pass indexes with, so the post-check narrowing cannot truncate, and (2)
 * admit every UNICODE_STRING (`_U`) source, whose Length is a USHORT byte count
 * (<= 32767 WCHARs). Delegating the _U forms down to the counted engine must not
 * start refusing a source the _U ABI used to accept (TODO-22 s25). */
_Static_assert(RTL_ENV_SOURCE_MAX_WCHARS <= 0xFFFFFFFFu,
               "counted source ceiling must narrow to the uint32 pass index exactly");
_Static_assert(RTL_ENV_SOURCE_MAX_WCHARS >= 0xFFFFu / 2u,
               "counted source ceiling must admit every USHORT-bounded _U source");

/* Per-PASS lookup-work accounting. `left` is the remaining budget, `used` the
 * consumed total (compared between the two passes to detect concurrent input
 * mutation), and `exhausted` a sticky flag meaning the pass is INVALID and its
 * output must not be published. Stack-local per call: no shared mutable state,
 * so the file stays lock-free and SMP-safe by construction. */
struct rtl_env_work {
    uint64_t left;
    uint64_t used;
    int      exhausted;
};

/* Charge `cost` WCHAR inspections against the budget. Returns 0 -- and marks the
 * pass exhausted -- when the charge does not FIT, in which case the lookup that
 * incurred it must be abandoned rather than accepted.
 *
 * Refusing the charge before it lands is what makes the ceiling real. Clamping a
 * too-large charge to zero instead would let the lookup that overshot still
 * return its answer, and if it were the LAST lookup of the pass nothing would
 * ever observe the drained balance -- the pass would succeed having spent up to
 * a full extra block scan beyond the ceiling. Checking here, at the point the
 * cost is known, means no path can complete work it could not afford. */
static int rtl_env_work_charge(struct rtl_env_work *w, uint32_t cost)
{
    if ((uint64_t)cost > w->left) {
        w->exhausted = 1;
        return 0;
    }
    w->left -= (uint64_t)cost;
    w->used += (uint64_t)cost;
    return 1;
}

/* ASCII lower-case fold (A-Z only), matching env.c's env_lc storage comparator.
 * Latin-1 / Unicode folding is deliberately NOT applied so the UTF-16 path and
 * the UTF-8 env_get path resolve the same names identically. */
static uint16_t rtl_env_ascii_lc(uint16_t c)
{
    return (c >= (uint16_t)'A' && c <= (uint16_t)'Z') ? (uint16_t)(c + 0x20u) : c;
}

/* Case-insensitive (ASCII) compare of two WCHAR runs of length `n`. */
static int rtl_env_name_ci_eq(const uint16_t *a, const uint16_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (rtl_env_ascii_lc(a[i]) != rtl_env_ascii_lc(b[i]))
            return 0;
    return 1;
}

/* Validate a double-NUL-terminated block lying entirely within `extent` WCHARs
 * of readable memory, and report BOTH bounds the rest of the file needs:
 *
 *   *out_bound  the terminator INDEX -- the bound the lookup walks;
 *   *out_extent the VERIFIED consumed length in WCHARs (through the terminating
 *               NUL), used for the Destination-overlap matrix.
 *
 * Returns 1 when the block is well-formed within min(extent, cap), else 0.
 *
 * `extent` is the caller's GUARANTEE of readable WCHARs at `block`, so the scan
 * is bounded by it and can never read past the allocation -- a block whose
 * terminator lies outside the extent is malformed, not an excuse to keep
 * scanning. Without an extent the scan trusted the caller's terminator and could
 * run RTL_ENV_BLOCK_MAX_WCHARS (2 MiB) past a short allocation (TODO-22 s20).
 *
 * The empty block is the two-WCHAR "\0\0" form, and both NULs are VERIFIED here
 * rather than assumed: returning only the terminator index forces the caller to
 * reconstruct the extent as `(bound == 0) ? 2 : bound + 1`, which silently
 * promotes a malformed one-WCHAR "\0" allocation into a two-WCHAR range.
 * Deriving both numbers in one place removes that reconstruction entirely. */
static int rtl_env_block_validate(const uint16_t *block, uint32_t extent,
                                  uint32_t *out_bound, uint32_t *out_extent)
{
    uint32_t cap = (extent < RTL_ENV_BLOCK_MAX_WCHARS) ? extent
                                                       : RTL_ENV_BLOCK_MAX_WCHARS;
    uint32_t p = 0;

    /* The smallest legal block is "\0\0"; a 0- or 1-WCHAR extent cannot hold one. */
    if (!block || extent < 2u)
        return 0;

    while (p < cap) {
        if (block[p] == 0) {
            /* Terminating empty entry. An EMPTY block (p == 0) must be a genuine
             * double NUL, and every block spans through this NUL. */
            if (p == 0u) {
                if (block[1] != 0)
                    return 0;
                *out_bound = 0u;
                *out_extent = 2u;
                return 1;
            }
            *out_bound = p;
            *out_extent = p + 1u;
            return 1;
        }
        while (p < cap && block[p] != 0)  /* skip this entry's content */
            p++;
        if (p < cap)
            p++;                          /* skip this entry's NUL */
    }
    return 0;                             /* no terminator within the extent */
}

/* Look up `name` (namelen WCHARs) in `block` (bounded by block_wchars). On a
 * case-insensitive ASCII match sets out_val + out_vlen to the value run and
 * returns 1; returns 0 when absent.
 *
 * `w` carries the remaining WCHAR-inspection budget. A MISS walks the whole
 * block, so that product is the expensive term the budget exists to bound; when
 * it runs out `w->exhausted` is set and the caller ABORTS the pass rather than
 * accepting the truncated lookup as a miss (which would silently expand
 * `%NAME%` to the wrong text). See RTL_ENV_EXPAND_WORK_MAX. */
static int rtl_env_block_lookup(const uint16_t *block, uint32_t block_wchars,
                                const uint16_t *name, uint32_t namelen,
                                const uint16_t **out_val, uint32_t *out_vlen,
                                struct rtl_env_work *w)
{
    uint32_t p = 0;
    while (p < block_wchars && block[p] != 0) {
        uint32_t start = p;
        uint32_t eq;
        uint32_t cost;

        /* Cheap early-out: a drained balance cannot afford even a 1-WCHAR entry,
         * so refuse without scanning this one. rtl_env_work_charge below is the
         * authoritative gate -- this only avoids the wasted scan. A lookup that
         * finishes with exactly 0 left is a real miss, not a refusal, which is
         * why the caller tests the sticky flag and never `left == 0`. */
        if (w->left == 0u) {
            w->exhausted = 1;
            return 0;
        }

        /* A hidden "=X:" drive variable's name BEGINS with '=' (entry
         * "=C:=value"), so that leading '=' is part of the NAME and the
         * separator is the NEXT one. Start the scan one WCHAR in for such an
         * entry; for an ordinary "KEY=VALUE" entry this is the plain first-'='
         * offset. Mirrors the UTF-8 storage comparator (env.c env_entry_keylen)
         * so `%=C:%` resolves identically on both expansion paths -- before this,
         * an "=C:=..." entry split at index 0 and yielded an empty key that no
         * reference could ever name (TODO-22 s19). */
        eq = (block[start] == RTL_ENV_WEQ) ? start + 1u : start;
        while (eq < block_wchars && block[eq] != 0 && block[eq] != RTL_ENV_WEQ)
            eq++;
        if (eq < block_wchars && block[eq] == RTL_ENV_WEQ) {
            uint32_t keylen = eq - start;
            if (keylen == namelen &&
                rtl_env_name_ci_eq(&block[start], name, namelen)) {
                uint32_t vstart = eq + 1;
                uint32_t vend = vstart;
                while (vend < block_wchars && block[vend] != 0)
                    vend++;
                /* Charge the hit (name scan + value scan) BEFORE accepting it: a
                 * hit returns immediately, so an unaffordable one would otherwise
                 * leave the pass with no later check to notice. */
                cost = vend - start;
                if (!rtl_env_work_charge(w, cost))
                    return 0;
                *out_val = &block[vstart];
                *out_vlen = vend - vstart;
                return 1;
            }
        }
        /* Advance to the next entry (past this entry's NUL). */
        while (p < block_wchars && block[p] != 0)
            p++;
        if (p < block_wchars)
            p++;
        /* Charge every WCHAR this entry cost to inspect. The LAST entry of a miss
         * exits the loop below, so an unaffordable charge has to be caught here
         * too, not on a next iteration that never runs. */
        cost = p - start;
        if (!rtl_env_work_charge(w, cost))
            return 0;
    }
    return 0;
}

/* One expansion pass over `src` (src_wchars). When `dst` is NULL this only
 * counts; otherwise it writes at most `dst_cap` WCHARs. Returns the number of
 * output WCHARs excluding the NUL (the full count even if a write is clamped).
 * The two passes agree over immutable, kernel-resident, NON-overlapping inputs;
 * the `dst_cap` clamp is a defensive belt so that even if a caller aliases the
 * destination with Source/Environment (which the public entry also rejects),
 * pass 2 can never write past the length pass 1 counted -- no buffer overrun.
 *
 * The count is a SATURATING uint64: every increment stops at RTL_ENV_COUNT_SAT
 * instead of wrapping. Worst case is far past any representable result -- a
 * 32767-WCHAR Source holds at most 10922 `%V%` references (3 WCHARs each), each
 * expanding to a value up to RTL_ENV_BLOCK_MAX_WCHARS (1 Mi) -> ~1.1e10 WCHARs
 * -- so a uint32 count WOULD wrap at 2^32 and hand the caller a small "required"
 * length for a huge result. (The old 64 KiWCHAR cap topped out near 7.2e8, under
 * 2^32: the raise is what makes the wider count load-bearing.) uint64 cannot wrap
 * at these magnitudes; the saturation is belt-and-braces so the invariant
 * survives any future cap raise, and the public entry rejects a saturated count
 * outright (TODO-22 s19). */
static uint64_t rtl_env_expand_pass(const uint16_t *block, uint32_t block_wchars,
                                    const uint16_t *src, uint32_t src_wchars,
                                    uint16_t *dst, uint64_t dst_cap,
                                    struct rtl_env_work *w)
{
    uint32_t i = 0;
    uint64_t out = 0;
    while (i < src_wchars) {
        /* A budget-exhausted lookup produced no usable answer, so the rest of
         * this pass would be built on it. Stop immediately; the caller turns the
         * sticky flag into STATUS_INSUFFICIENT_RESOURCES and publishes nothing. */
        if (w->exhausted)
            break;
        uint16_t c = src[i];
        if (c != RTL_ENV_WPCT) {
            if (dst && out < dst_cap)
                dst[out] = c;
            out = rtl_env_count_inc(out);
            i++;
            continue;
        }
        /* `%` begins a reference. Win32/ntdll ExpandEnvironmentStrings does NOT
         * treat `%%` as an escape: an empty name (`%%`) is an unresolved variable
         * and both percent signs are preserved verbatim, falling out of the
         * empty-name -> literal branch below. (cmd.exe's `%%` -> `%` batch escape
         * is a distinct shell mode, not this Rtl primitive.) */
        {
            uint32_t j = i + 1u;
            while (j < src_wchars && src[j] != RTL_ENV_WPCT)
                j++;
            if (j >= src_wchars) {
                /* No closing '%': copy the remainder verbatim and stop. */
                while (i < src_wchars) {
                    if (dst && out < dst_cap)
                        dst[out] = src[i];
                    out = rtl_env_count_inc(out);
                    i++;
                }
                break;
            }
            {
                uint32_t namelen = j - (i + 1u);
                const uint16_t *val = NULL;
                uint32_t vlen = 0;
                /* Same name-length limit as env_expand (env.c): a reference whose
                 * name exceeds ENV_NAME_MAX cannot name a storable variable, so it
                 * is left literal rather than looked up -- keeps the UTF-8 and
                 * UTF-16 expansion paths consistent (ENV_NAME_MAX counted in WCHARs
                 * here vs UTF-8 bytes there; env names are ASCII, so they agree). */
                if (namelen >= 1u && namelen <= ENV_NAME_MAX &&
                    rtl_env_block_lookup(block, block_wchars, &src[i + 1u],
                                         namelen, &val, &vlen, w)) {
                    uint32_t k;
                    for (k = 0; k < vlen; k++) {
                        if (dst && out < dst_cap)
                            dst[out] = val[k];
                        out = rtl_env_count_inc(out);
                    }
                } else {
                    /* Unknown or empty name: copy the literal `%NAME%`. */
                    uint32_t p;
                    for (p = i; p <= j; p++) {
                        if (dst && out < dst_cap)
                            dst[out] = src[p];
                        out = rtl_env_count_inc(out);
                    }
                }
                i = j + 1u;
            }
        }
    }
    return out;
}

/* True if the byte ranges [a, a+alen) and [b, b+blen) overlap. Empty or NULL
 * ranges never overlap. Used to reject an aliased Destination (defined behavior;
 * the pass clamp already guarantees memory safety regardless).
 *
 * Lengths are `size_t`: the counted SIZE_T forms (TODO-22 s25) can present a
 * WCHAR count whose BYTE length exceeds 4 GiB, which a uint32 parameter would
 * TRUNCATE, letting a genuinely aliased range slip past detection. Widening the
 * type removes the truncation but not the endpoint hazard: a base near
 * UINTPTR_MAX plus even a small length wraps `a0 + alen`, so `a0 < b1 && b0 < a1`
 * could read false for an overlapping pair. A range whose end wraps is malformed
 * regardless, so treat it as OVERLAPPING (return 1) -- the only caller rejects on
 * overlap, so a wrapped range is refused rather than trusted, matching how
 * ProbeForRead treats base+len overflow. */
static int rtl_env_ranges_overlap(const void *a, size_t alen,
                                  const void *b, size_t blen)
{
    uintptr_t a0, a1, b0, b1;
    if (!a || !b || alen == 0u || blen == 0u)
        return 0;
    a0 = (uintptr_t)a;
    b0 = (uintptr_t)b;
    /* Reject (as overlapping) any range whose end address wraps the pointer
     * width; the endpoint arithmetic below is only meaningful when it does not. */
    if (alen > (uintptr_t)(~(uintptr_t)0) - a0 ||
        blen > (uintptr_t)(~(uintptr_t)0) - b0)
        return 1;
    a1 = a0 + (uintptr_t)alen;
    b1 = b0 + (uintptr_t)blen;
    return a0 < b1 && b0 < a1;
}

/* Canonical UNICODE_STRING validity for a kernel-resident counted string: even
 * byte Length, Length within MaximumLength, backing Buffer for a non-empty
 * string. Mirrors nt_rtlstr's rtl_us_valid (callers already probed user memory). */
static int rtl_env_us_valid(const UNICODE_STRING *us)
{
    if (!us)
        return 0;
    if (us->Length & 1u)
        return 0;
    if (us->Length > us->MaximumLength)
        return 0;
    if (us->Length != 0u && !us->Buffer)
        return 0;
    return 1;
}

/* The SIZE_T-safe counted expansion engine. Takes the block's bound + extent
 * ALREADY established (either verified by rtl_env_block_validate for a caller-
 * supplied block, or known by construction for a builder-produced one), a RAW
 * (ptr + WCHAR count) Source and Destination rather than a UNICODE_STRING, and
 * the work `budget` to spend per pass. All lengths are WCHAR counts.
 *
 * This is the primitive both ABIs sit on: the counted RtlExpandEnvironmentStrings
 * calls it directly, and the UNICODE_STRING rtl_env_expand_core wraps it (adding
 * the descriptor-overlap checks and the USHORT result ceiling its ABI needs).
 * Keeping the engine free of any UNICODE_STRING ceiling is what lets the SIZE_T
 * form report a result larger than a USHORT can hold; the wrapper re-imposes the
 * ceiling for its own callers (TODO-22 s25).
 *
 * `*ret_wchars` is the required buffer size in WCHARs INCLUDING the terminating
 * NUL, set on BOTH success and STATUS_BUFFER_TOO_SMALL; it is 0 on every other
 * status. On success `dst` holds `*ret_wchars - 1` content WCHARs plus a NUL. */
static NTSTATUS rtl_env_expand_counted(const uint16_t *block, uint32_t block_wchars,
                                       uint32_t verified_extent,
                                       const uint16_t *src, uint64_t src_wchars_in,
                                       uint16_t *dst, uint64_t dst_wchars,
                                       uint64_t *ret_wchars, uint64_t budget)
{
    uint32_t src_wchars;              /* narrowed AFTER the source ceiling check */
    uint64_t src_bytes, dst_bytes;
    uint64_t block_bytes;
    uint64_t required;                /* output WCHARs excluding NUL (saturating) */
    uint64_t needed_wchars;           /* required + NUL, wide (avoids wrap) */
    uint64_t pass1_work;              /* lookup work pass 1 consumed */
    struct rtl_env_work work;

    /* NOTHING is stored through ret_wchars until the overlap matrix below has
     * PROVEN it aliases neither the Source data nor the block: a caller may aim
     * ReturnLength into its own Source, and an early "*ret_wchars = 0" default
     * would corrupt that input before the alias is even detected. Every ret store
     * therefore lives past the matrix. */
    if (src_wchars_in != 0u && !src)
        return STATUS_INVALID_PARAMETER;
    if (dst_wchars != 0u && !dst)
        return STATUS_INVALID_PARAMETER;

    /* Resource-policy source ceiling BEFORE the narrowing to the uint32 pass
     * index: the UNICODE_STRING forms are implicitly bounded by a USHORT Length,
     * the counted SIZE_T form is not, and the outer template scan is uncharged
     * work. A template past the ceiling is declined as a resource, not rejected
     * as malformed. The ceiling keeps src_wchars <= UINT32_MAX so the cast is
     * exact. */
    if (src_wchars_in > (uint64_t)RTL_ENV_SOURCE_MAX_WCHARS)
        return STATUS_INSUFFICIENT_RESOURCES;
    src_wchars = (uint32_t)src_wchars_in;
    src_bytes = (uint64_t)src_wchars * 2u;

    /* A WCHAR count whose byte length would overflow the range math is a buffer
     * no caller can really own; reject it rather than wrap the overlap endpoints
     * (rtl_env_ranges_overlap also guards the wrap, so this is defense in depth
     * and a clean status for an absurd DestinationLength). RTL_ENV_COUNT_SAT is
     * the all-ones uint64 (this tree defines no UINT64_MAX); size_t is uint64. */
    if (dst_wchars > (RTL_ENV_COUNT_SAT / 2u))
        return STATUS_INVALID_PARAMETER;
    dst_bytes = dst_wchars * 2u;
    block_bytes = (uint64_t)verified_extent * 2u;

    /* Reject any overlap between the output range [dst, dst+dst_bytes) and (a) the
     * Source DATA, (b) the Environment block, (c) the ret_wchars cell; and the
     * ret_wchars cell against (a)/(b). (a)/(b) prevent a two-pass count/write
     * tear; (c) prevents the size store from corrupting output the caller reads.
     * DESCRIPTOR overlaps are the UNICODE_STRING wrapper's concern -- this engine
     * has no descriptors. An empty output range (dst_bytes == 0, the size-query
     * form) overlaps nothing and falls through to BUFFER_TOO_SMALL. */
    if (rtl_env_ranges_overlap(dst, dst_bytes, src, src_bytes) ||
        rtl_env_ranges_overlap(dst, dst_bytes, block, block_bytes) ||
        (ret_wchars &&
         rtl_env_ranges_overlap(dst, dst_bytes, ret_wchars, sizeof(*ret_wchars))))
        return STATUS_INVALID_PARAMETER;
    if (ret_wchars &&
        (rtl_env_ranges_overlap(ret_wchars, sizeof(*ret_wchars), src, src_bytes) ||
         rtl_env_ranges_overlap(ret_wchars, sizeof(*ret_wchars), block, block_bytes)))
        return STATUS_INVALID_PARAMETER;

    /* Pass 1: count the required output length (no write). */
    work.left = budget;
    work.used = 0u;
    work.exhausted = 0;
    required = rtl_env_expand_pass(block, block_wchars, src, src_wchars, NULL, 0u,
                                   &work);
    /* Refuse BEFORE the output-length store. Pass 1 both counts and pays the full
     * lookup cost, so an over-budget expansion is rejected here, with nothing
     * written -- a budget refusal can never land mid-write. The ret_wchars store
     * is safe now: the overlap matrix above proved it aliases no input. */
    if (work.exhausted) {
        if (ret_wchars)
            *ret_wchars = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pass1_work = work.used;

    /* A saturated count is not a length: RTL_ENV_COUNT_SAT is the all-ones uint64
     * (== SIZE_MAX here), so `required` can never exceed it and `required + 1`
     * wraps only AT it. Refuse STATUS_UNSUCCESSFUL (never STATUS_BUFFER_TOO_SMALL,
     * which would loop a grow-and-retry caller forever), BEFORE the ret_wchars
     * store so no truncated size is ever published. The current caps make this
     * unreachable; the guard keeps the +1 below honest if a cap ever moves. */
    if (required == RTL_ENV_COUNT_SAT) {
        if (ret_wchars)
            *ret_wchars = 0;
        return STATUS_UNSUCCESSFUL;
    }
    needed_wchars = required + 1u;                    /* include the WCHAR NUL */

    if (ret_wchars)
        *ret_wchars = needed_wchars;

    if (needed_wchars > dst_wchars)
        /* No partial output on overflow. ret_wchars reports the required size. */
        return STATUS_BUFFER_TOO_SMALL;

    /* The result fits; a NULL output buffer here is a caller error (a size query
     * uses dst_wchars == 0, which took the BUFFER_TOO_SMALL path above). */
    if (!dst)
        return STATUS_INVALID_PARAMETER;

    /* Pass 2: write to the buffer, clamped to `required` WCHARs (room for
     * `required` + the NUL; the clamp defends against any aliasing tear). */
    {
        uint64_t written;

        /* FRESH budget, same ceiling -- never the remainder of pass 1's. Sharing
         * one counter across both passes would let pass 1 consume most of it and
         * strand pass 2 on identical work, making the verdict depend on which
         * pass you are in rather than on the inputs. Starting equal makes the two
         * passes' work identical over immutable inputs, so this cannot trip when
         * pass 1 did not. */
        work.left = budget;
        work.used = 0u;
        work.exhausted = 0;
        written = rtl_env_expand_pass(block, block_wchars, src, src_wchars,
                                      dst, required, &work);
        /* The two passes must agree, in output AND in the work they took. They
         * only differ if the caller-owned Source or Environment was mutated by
         * another CPU between passes (the inputs are kernel-resident but not
         * private/immutable here). A shorter second pass would leave
         * [written, required) UNwritten -- advertising `required` would disclose
         * stale/uninitialized destination bytes. The work comparison catches the
         * same tear when the output length coincidentally matches, and a pass-2
         * exhaustion is by construction that same mutation case (pass 1 already
         * proved the work fits). Refuse rather than return a torn result, and
         * retract the published length: it described a result that was never
         * written. (The Win32 boundary that passes a private per-call snapshot
         * never trips this.) */
        if (work.exhausted || written != required || work.used != pass1_work) {
            if (ret_wchars)
                *ret_wchars = 0;
            return work.exhausted ? STATUS_INSUFFICIENT_RESOURCES
                                  : STATUS_INVALID_PARAMETER;
        }
    }
    /* `required` < needed_wchars <= dst_wchars, so this index is within the
     * caller's buffer. */
    dst[required] = 0;
    return STATUS_SUCCESS;
}

/* The UNICODE_STRING (`_U`-ABI) adapter over the counted engine. Validates the
 * descriptors, performs the descriptor-overlap rejections the raw engine cannot
 * (its Source/Destination are bare ptr+len), delegates the expansion, then
 * re-imposes the USHORT result ceiling and reports ReturnedLength in BYTES
 * including the NUL -- the convention this ABI's callers (and its tests) expect.
 *
 * Splitting the engine out keeps two costs off the paths that do not owe them: a
 * builder-produced block does not pay a re-scan to re-derive a length its builder
 * already reported, and the budget is a policy INPUT rather than a baked-in
 * constant, so a caller with a tighter tolerance (and the tests, which must reach
 * the fit/overshoot boundary without burning the production ceiling) can supply
 * its own. */
static NTSTATUS rtl_env_expand_core(const uint16_t *block, uint32_t block_wchars,
                                    uint32_t verified_extent,
                                    UNICODE_STRING *Source,
                                    UNICODE_STRING *Destination,
                                    uint32_t *ReturnedLength, uint64_t budget)
{
    const uint16_t *src;
    uint16_t *dst_buf;                /* SNAPSHOT of Destination->Buffer */
    uint32_t src_bytes, dst_max;      /* SNAPSHOTs (Source->Length, Dest->MaxLen) */
    uint64_t ret_wchars = 0;          /* engine output: WCHARs incl NUL */
    uint64_t content;                 /* WCHARs excl NUL */
    NTSTATUS status;

    if (!rtl_env_us_valid(Source) || !rtl_env_us_valid(Destination))
        return STATUS_INVALID_PARAMETER;

    /* Snapshot every scalar we later write through or compare against into LOCALS
     * up front. A caller may point Destination->Buffer into the Destination
     * descriptor itself, or aim ReturnedLength at Destination->Buffer's pointer
     * field; if we re-read those fields after any store the write pointer could
     * be corrupted mid-flight. Using locals (and the descriptor-overlap rejection
     * below) makes every write target fixed at entry. */
    src = Source->Buffer;
    src_bytes = Source->Length;
    dst_buf = Destination->Buffer;
    dst_max = Destination->MaximumLength;

    /* Overlaps the raw engine cannot see for THIS ABI: the output range vs the
     * Source/Destination descriptors, and -- because the engine is handed a stack
     * LOCAL ret_wchars, never this caller's ReturnedLength cell -- the caller's
     * ReturnedLength vs the output range, Source data, Environment block, and
     * either descriptor. The engine itself covers the output range vs Source data
     * and block. */
    if (rtl_env_ranges_overlap(dst_buf, dst_max, Destination, sizeof(*Destination)) ||
        rtl_env_ranges_overlap(dst_buf, dst_max, Source, sizeof(*Source)))
        return STATUS_INVALID_PARAMETER;
    if (ReturnedLength &&
        (rtl_env_ranges_overlap(dst_buf, dst_max,
                                ReturnedLength, sizeof(*ReturnedLength)) ||
         rtl_env_ranges_overlap(ReturnedLength, sizeof(*ReturnedLength),
                                src, src_bytes) ||
         rtl_env_ranges_overlap(ReturnedLength, sizeof(*ReturnedLength),
                                block, (uint64_t)verified_extent * 2u) ||
         rtl_env_ranges_overlap(ReturnedLength, sizeof(*ReturnedLength),
                                Source, sizeof(*Source)) ||
         rtl_env_ranges_overlap(ReturnedLength, sizeof(*ReturnedLength),
                                Destination, sizeof(*Destination))))
        return STATUS_INVALID_PARAMETER;

    status = rtl_env_expand_counted(block, block_wchars, verified_extent,
                                    src, (uint64_t)(src_bytes / 2u),
                                    dst_buf, (uint64_t)(dst_max / 2u),
                                    &ret_wchars, budget);

    /* Re-impose the UNICODE_STRING result ceiling on top of the counted result.
     * On SUCCESS or BUFFER_TOO_SMALL the engine set ret_wchars (incl NUL); the
     * content length excl NUL is ret_wchars - 1. A content over
     * RTL_ENV_MAX_RESULT_WCHARS is unrepresentable in a UNICODE_STRING at ANY
     * buffer size, so it is STATUS_UNSUCCESSFUL with *ReturnedLength 0 (never a
     * truncated size) -- matching real ntdll, which zeroes its ResultLength on
     * this branch. On SUCCESS content is bounded by the USHORT dst_max and can
     * never exceed the ceiling, so this only ever fires on the too-small path,
     * where nothing was written. */
    if (status == STATUS_SUCCESS || status == STATUS_BUFFER_TOO_SMALL) {
        content = ret_wchars - 1u;
        if (content > (uint64_t)RTL_ENV_MAX_RESULT_WCHARS) {
            if (ReturnedLength)
                *ReturnedLength = 0;
            return STATUS_UNSUCCESSFUL;
        }
        /* ret_wchars <= 32767 here, so ret_wchars * 2 <= 65534 fits uint32. */
        if (ReturnedLength)
            *ReturnedLength = (uint32_t)(ret_wchars * 2u);
        if (status == STATUS_SUCCESS)
            Destination->Length = (uint16_t)(content * 2u);  /* bytes, excl NUL */
        return status;
    }

    /* Every other status: the engine published no size; mirror that. */
    if (ReturnedLength)
        *ReturnedLength = 0;
    return status;
}

NTSTATUS rtl_env_expand_block_budget(const uint16_t *block, uint32_t block_extent,
                                     UNICODE_STRING *Source,
                                     UNICODE_STRING *Destination,
                                     uint32_t *ReturnedLength, uint64_t budget)
{
    uint32_t bound;                   /* lookup bound (terminator index) */
    uint32_t verified_extent;         /* WCHARs the block VERIFIABLY spans */

    /* Bound the terminator scan by the caller's readable extent, so a malformed
     * block fails here instead of reading past its allocation. */
    if (!rtl_env_block_validate(block, block_extent, &bound, &verified_extent))
        return STATUS_INVALID_PARAMETER;
    return rtl_env_expand_core(block, bound, verified_extent, Source, Destination,
                               ReturnedLength, budget);
}

NTSTATUS rtl_env_expand_block(const uint16_t *block, uint32_t block_extent,
                              UNICODE_STRING *Source, UNICODE_STRING *Destination,
                              uint32_t *ReturnedLength)
{
    return rtl_env_expand_block_budget(block, block_extent, Source, Destination,
                                       ReturnedLength, RTL_ENV_EXPAND_WORK_MAX);
}

NTSTATUS RtlExpandEnvironmentStrings_U(void *Environment, UNICODE_STRING *Source,
                                       UNICODE_STRING *Destination,
                                       uint32_t *ReturnedLength)
{
    struct task *cur;
    uint16_t *synth = NULL;
    uint32_t synth_wchars = 0;
    NTSTATUS status;
    int rc;

    /* A non-NULL Environment is a bare pointer with no allocation EXTENT, and no
     * amount of scanning recovers one: a cap bounds a runaway walk but still
     * reads whatever follows a short block, and probing mapped pages says
     * nothing about the object's length. Refuse the form outright rather than
     * read past a caller's allocation. The ABI-preserving path is a boundary
     * that probes + copies the block into a terminated kernel snapshot and calls
     * rtl_env_expand_block with the extent it then knows; that boundary is owned
     * by TODO-22 s24. Nothing user-reachable calls this today, and every
     * in-kernel caller already uses the extent-taking entry, so this refusal
     * removes no working capability (TODO-22 s20). */
    if (Environment)
        return STATUS_NOT_SUPPORTED;

    /* NULL Environment: the calling process's own block. Synthesize it from the
     * current task's authoritative UTF-8 environ (kernel-resident), not the
     * stale, user-mapped PEB block. env_build_block_utf16 returns the block's
     * TOTAL wchar length including terminators, which is exactly the extent the
     * expansion engine needs. */
    cur = task_current();
    if (!cur)
        return STATUS_INVALID_PARAMETER;
    rc = env_build_block_utf16(cur, &synth, &synth_wchars,
                               RTL_ENV_BLOCK_MAX_WCHARS);
    if (rc == ENV_ERR_NOMEM)
        return STATUS_NO_MEMORY;
    if (rc != ENV_OK)
        return STATUS_INVALID_PARAMETER;

    /* env_build_block_utf16 reports the block's EXACT total length including the
     * terminators, so the bound and extent are known by construction. Going
     * through the validating entry would re-derive them with a full-block scan --
     * up to a million WCHARs of pure cost on every call, even for a Source with no
     * references at all. Trust the builder's own number instead. */
    status = rtl_env_expand_core(synth, synth_wchars, synth_wchars, Source,
                                 Destination, ReturnedLength,
                                 RTL_ENV_EXPAND_WORK_MAX);
    env_free_block_utf16(synth, synth_wchars);
    return status;
}

NTSTATUS RtlExpandEnvironmentStrings(void *Environment,
                                     const uint16_t *Source, uint64_t SourceLength,
                                     uint16_t *Destination, uint64_t DestinationLength,
                                     uint64_t *ReturnLength)
{
    struct task *cur;
    uint16_t *synth = NULL;
    uint32_t synth_wchars = 0;
    NTSTATUS status;
    int rc;

    /* Same foreign-block refusal as RtlExpandEnvironmentStrings_U: a bare pointer
     * carries no allocation extent, so the double-NUL scan cannot be bounded. The
     * probe+copy boundary that would restore the explicit-block form is owned by
     * TODO-22 s24; until then only the NULL (calling-process) form is served. */
    if (Environment)
        return STATUS_NOT_SUPPORTED;

    /* NULL Environment: synthesize the current process's block from the
     * authoritative UTF-8 environ, exactly as the _U form does. env_build_block_utf16
     * reports the block's EXACT total wchar length including terminators, which is
     * the bound and extent the engine needs -- no re-scan. */
    cur = task_current();
    if (!cur)
        return STATUS_INVALID_PARAMETER;
    rc = env_build_block_utf16(cur, &synth, &synth_wchars,
                               RTL_ENV_BLOCK_MAX_WCHARS);
    if (rc == ENV_ERR_NOMEM)
        return STATUS_NO_MEMORY;
    if (rc != ENV_OK)
        return STATUS_INVALID_PARAMETER;

    /* Straight to the counted engine: no UNICODE_STRING ceiling (ReturnLength is
     * SIZE_T here), ReturnLength in WCHARs including the NUL on both success and
     * BUFFER_TOO_SMALL. */
    status = rtl_env_expand_counted(synth, synth_wchars, synth_wchars,
                                    Source, SourceLength,
                                    Destination, DestinationLength,
                                    ReturnLength, RTL_ENV_EXPAND_WORK_MAX);
    env_free_block_utf16(synth, synth_wchars);
    return status;
}

/* ===========================================================================
 * userenv.dll ExpandEnvironmentStringsForUser (TODO-22 s13)
 *
 * A thin per-user front-end over the shared expansion engine: it builds a
 * bounded UTF-16 block from the target user's environment and expands against it.
 * Today only the NULL-token (calling-process environment) path is supported;
 * per-user token expansion is deferred (see the header contract + env_create_block).
 * =========================================================================== */

NTSTATUS ExpandEnvironmentStringsForUser(struct task *caller, const void *htoken,
                                         UNICODE_STRING *Source,
                                         UNICODE_STRING *Destination,
                                         uint32_t *ReturnedLength)
{
    uint16_t *block = NULL;
    uint32_t block_wchars = 0;
    NTSTATUS status;
    int rc;

    /* Per-user token needs a SID->hive map + LoadUserProfile (env_create_block);
     * refuse rather than expand against the wrong identity. */
    if (htoken != NULL)
        return STATUS_NOT_SUPPORTED;
    if (!caller)
        return STATUS_INVALID_PARAMETER;

    /* Build a private, bounded block from the caller's environ, then reuse the
     * fully-guarded explicit-block expansion path. The block is capped at
     * RTL_ENV_BLOCK_MAX_WCHARS so it is always a valid input to that path (an
     * over-large environ fails to build -> STATUS_INVALID_PARAMETER, matching the
     * RtlExpandEnvironmentStrings_U NULL-Environment over-cap mapping; NOT a
     * retryable STATUS_BUFFER_TOO_SMALL). */
    rc = env_build_block_utf16(caller, &block, &block_wchars,
                               RTL_ENV_BLOCK_MAX_WCHARS);
    if (rc == ENV_ERR_NOMEM)
        return STATUS_NO_MEMORY;
    if (rc != ENV_OK)
        /* Includes ENV_ERR_NOSPACE (environ over the RTL_ENV_BLOCK_MAX_WCHARS
         * cap): map it exactly as the RtlExpandEnvironmentStrings_U NULL-Environment
         * path maps its own build failure -- STATUS_INVALID_PARAMETER, NOT a
         * retryable STATUS_BUFFER_TOO_SMALL (a larger Destination cannot resolve an
         * over-cap INTERNAL block; the size limit is not the caller's buffer). */
        return STATUS_INVALID_PARAMETER;

    /* This caller BUILT the block, so its exact length (including terminators) is
     * already known -- it goes straight to the engine rather than paying a scan to
     * re-derive a number it was just handed. The public ntdll entry refuses a
     * non-NULL Environment precisely because its signature cannot carry this. */
    status = rtl_env_expand_core(block, block_wchars, block_wchars, Source,
                                 Destination, ReturnedLength,
                                 RTL_ENV_EXPAND_WORK_MAX);
    env_free_block_utf16(block, block_wchars);
    return status;
}

/* ===========================================================================
 * ntdll Rtl environment exports (TODO-22 s21)
 *
 * Thin routines over the SAME storage API the Nt env syscalls use, so both ABIs
 * observe one authoritative environment with one set of validation rules. See the
 * header for the contract, the uniform foreign-block refusal, and the reachability
 * gate. Nothing here takes environ_lock: env_get_copy / env_set / env_unset /
 * env_create_block each acquire it internally, so calling them while holding it
 * would self-deadlock a non-recursive mutex.
 * =========================================================================== */

/* Map a storage-layer code to the status set these entries document. ENV_ERR_NOSPACE
 * means "block cap reached" for a SET (a quota), which is why it is not mapped here
 * for the create path -- RtlCreateEnvironment maps its own NOSPACE separately. */
static NTSTATUS rtl_env_status_from_env(int rc)
{
    switch (rc) {
    case ENV_OK:              return STATUS_SUCCESS;
    case ENV_ERR_NOTFOUND:    return STATUS_VARIABLE_NOT_FOUND;
    case ENV_ERR_TOOLONG:     return STATUS_NAME_TOO_LONG;
    case ENV_ERR_NOMEM:       return STATUS_NO_MEMORY;
    case ENV_ERR_NOSPACE:     return STATUS_QUOTA_EXCEEDED;
    case ENV_ERR_UNSUPPORTED: return STATUS_NOT_SUPPORTED;
    default:                  return STATUS_INVALID_PARAMETER;
    }
}

/* Validate a counted (raw ptr + WCHAR count) ntdll env NAME and convert it to
 * NUL-terminated UTF-8 in `name8`, which MUST have room for ENV_NAME_MAX + 1
 * bytes. Enforces ntdll's own name rule; the storage layer applies its (stricter)
 * one afterwards. Shared by the counted RtlQueryEnvironmentVariable and, via the
 * UNICODE_STRING wrapper below, by the `_U` form. */
static NTSTATUS rtl_env_name_wchars_to_utf8(const uint16_t *name, uint32_t wchars,
                                            char *name8)
{
    uint32_t i;
    int cvt;

    if (wchars == 0u || !name)
        return STATUS_INVALID_PARAMETER;            /* empty / absent name */
    for (i = 0; i < wchars; i++) {
        if (name[i] == 0u)
            return STATUS_INVALID_PARAMETER;        /* embedded NUL */
        /* ntdll rejects '=' everywhere EXCEPT position 0, which is what admits the
         * hidden "=X:" drive-cwd name (TODO-22 s12). Checking from index 1 is
         * exactly that rule -- not an oversight. */
        if (i > 0u && name[i] == RTL_ENV_WEQ)
            return STATUS_INVALID_PARAMETER;
    }
    cvt = nls_cp_utf16_to_utf8(name, wchars, (uint8_t *)name8,
                               ENV_NAME_MAX, NLS_CP_STRICT);
    if (cvt < 0)
        return (cvt == NLS_CP_ERR_TOO_SMALL) ? STATUS_NAME_TOO_LONG
                                             : STATUS_INVALID_PARAMETER;
    name8[cvt] = '\0';
    return STATUS_SUCCESS;
}

/* UNICODE_STRING adapter: validate the descriptor, then defer to the counted
 * name validator on its Buffer/Length. */
static NTSTATUS rtl_env_name_to_utf8(const UNICODE_STRING *Name, char *name8)
{
    if (!rtl_env_us_valid(Name))
        return STATUS_INVALID_PARAMETER;
    return rtl_env_name_wchars_to_utf8(Name->Buffer, (uint32_t)(Name->Length / 2u),
                                       name8);
}

/* Shared value-fetch core for the counted and `_U` query forms. Copies the value
 * of the already-validated UTF-8 `name8` into `value` (capacity `value_cap_wchars`
 * WCHARs) from the calling process's authoritative environ. Sets `*ret_wchars` to
 * the value length in WCHARs -- EXCLUDING the NUL on STATUS_SUCCESS, INCLUDING it
 * (the required buffer size) on STATUS_BUFFER_TOO_SMALL. It is written ONLY once
 * the value buffer has been validated and proven not to alias the ret cell, so on
 * an early (pre-validation) failure `*ret_wchars` is left UNTOUCHED -- a caller
 * whose ReturnLength aliases its own Name/Value is never corrupted before the
 * alias is detected. EXACT FIT (value_cap_wchars == content) SUCCEEDS (WRK); the
 * NUL is written only
 * when the buffer leaves room for a whole extra WCHAR past the content, so at
 * exact fit SUCCESS does not imply NUL-termination. `value` may be NULL only when
 * value_cap_wchars is 0 (a size query). No UNICODE_STRING ceiling is applied here
 * -- the `_U` wrapper adds its own. */
static NTSTATUS rtl_env_query_value(const char *name8, uint16_t *value,
                                    uint64_t value_cap_wchars, uint64_t *ret_wchars)
{
    char *val8;
    struct task *cur;
    /* ENV_BUF_PAYLOAD_MAX, not ENV_STR_KMALLOC_MAX: the latter bounds the
     * header+payload TOTAL (s22), so asking for it exactly would push this common
     * query onto the unlocked-PMM path (03-memory-concurrency/TODO-03 s1). */
    uint32_t vcap = ENV_BUF_PAYLOAD_MAX;
    uint32_t val_len8, need_wchars;
    int r, cvt;

    if (value_cap_wchars != 0u && !value)
        return STATUS_INVALID_PARAMETER;

    cur = task_current();
    if (!cur)
        return STATUS_INVALID_PARAMETER;

    /* Fetch into a heap-sized buffer first and grow ONCE to the value cap only if
     * the store reports a longer value: the common query is far below 4 KiB, and
     * starting at ENV_VALUE_MAX would force a contiguous-PMM scan on every call. */
    val8 = (char *)env_buf_alloc(vcap);
    if (!val8)
        return STATUS_NO_MEMORY;
    r = env_get_copy(cur, name8, val8, vcap);
    if (r >= 0 && (uint32_t)r >= vcap) {
        env_buf_free(val8, vcap);
        vcap = ENV_VALUE_MAX + 1u;
        val8 = (char *)env_buf_alloc(vcap);
        if (!val8)
            return STATUS_NO_MEMORY;
        r = env_get_copy(cur, name8, val8, vcap);
    }
    if (r >= 0 && (uint32_t)r >= vcap) {
        /* Defense in depth: env_set / env_adopt_block cap a stored value at
         * ENV_VALUE_MAX, so the retry buffer always fits. A length that still meets
         * the buffer means the store is inconsistent -- refuse rather than let the
         * decoder below read past val8[vcap]. */
        env_buf_free(val8, vcap);
        return STATUS_NAME_TOO_LONG;
    }
    if (r < 0) {
        env_buf_free(val8, vcap);
        return rtl_env_status_from_env(r);
    }
    val_len8 = (uint32_t)r;

    cvt = nls_cp_utf8_to_utf16((const uint8_t *)val8, val_len8, NULL, 0,
                               NLS_CP_STRICT);
    if (cvt < 0) {
        env_buf_free(val8, vcap);
        return STATUS_INVALID_PARAMETER;            /* corrupt stored value */
    }
    need_wchars = (uint32_t)cvt;                    /* content, excl NUL */

    /* BEFORE any ret_wchars store, reject a ret cell that aliases the value bytes
     * this call will touch: the size store would otherwise land in the caller's
     * value buffer. The touched span is the content + the NUL (only when written)
     * on the fit path, or the declared capacity on the too-small path (nothing is
     * written there, but the caller's buffer still must not receive the size
     * store). need_wchars <= ENV_VALUE_MAX (32767) and, on too-small,
     * value_cap_wchars < need_wchars, so the byte span never overflows. Harmless
     * for the `_U` wrapper (its ret cell is a stack local aliasing nothing);
     * load-bearing for the counted form, whose ReturnLength is a caller out-param. */
    {
        uint32_t guard_wchars;
        if ((uint64_t)need_wchars > value_cap_wchars)
            guard_wchars = (uint32_t)value_cap_wchars;
        else
            guard_wchars = need_wchars +
                           (value_cap_wchars > (uint64_t)need_wchars ? 1u : 0u);
        if (ret_wchars &&
            rtl_env_ranges_overlap(value, (size_t)guard_wchars * 2u,
                                   ret_wchars, sizeof(*ret_wchars))) {
            env_buf_free(val8, vcap);
            return STATUS_INVALID_PARAMETER;
        }
    }

    /* value_cap_wchars is a SIZE_T; need_wchars <= ENV_VALUE_MAX (32767). */
    if ((uint64_t)need_wchars > value_cap_wchars) {
        /* Too small: no partial output; report the required size INCLUDING NUL. */
        env_buf_free(val8, vcap);
        if (ret_wchars)
            *ret_wchars = (uint64_t)need_wchars + 1u;
        return STATUS_BUFFER_TOO_SMALL;
    }

    cvt = nls_cp_utf8_to_utf16((const uint8_t *)val8, val_len8, value,
                               need_wchars, NLS_CP_STRICT);
    env_buf_free(val8, vcap);
    if (cvt < 0 || (uint32_t)cvt != need_wchars)
        return STATUS_INVALID_PARAMETER;            /* inconsistent converter */
    /* The NUL is a WCHAR: written only when the buffer has room for a whole extra
     * unit past the content. For an odd byte capacity floored to WCHARs this is
     * exactly the `>= content + 2 bytes` guard the _U form needs. */
    if (value_cap_wchars > (uint64_t)need_wchars)
        value[need_wchars] = 0u;
    if (ret_wchars)
        *ret_wchars = (uint64_t)need_wchars;        /* excl NUL on success */
    return STATUS_SUCCESS;
}

NTSTATUS RtlQueryEnvironmentVariable_U(void *Environment, UNICODE_STRING *Name,
                                       UNICODE_STRING *Value)
{
    char name8[ENV_NAME_MAX + 1u];
    uint16_t *vbuf;
    uint32_t vmax;
    uint64_t ret_wchars = 0;
    uint64_t content;                 /* WCHARs excl NUL */
    uint32_t need_bytes;
    NTSTATUS st;

    /* A foreign block is a bare pointer with no extent -- refused for the same
     * reason RtlExpandEnvironmentStrings_U refuses it (see the header). */
    if (Environment)
        return STATUS_NOT_SUPPORTED;
    /* Value is IN/OUT, and only MaximumLength + Buffer are INPUTS: Length is written
     * by this call, so the value a caller happens to arrive with is meaningless and
     * MUST NOT be validated (rtl_env_us_valid would reject an odd or over-long
     * incoming Length -- a descriptor real ntdll accepts, since it overwrites the
     * field anyway). Validate exactly the two fields that bound the write. These
     * DESCRIPTOR-level invariants stay in this wrapper: the shared value core takes
     * a raw ptr + WCHAR count and cannot see them. */
    if (!Value)
        return STATUS_INVALID_PARAMETER;

    /* Snapshot the two INPUT fields into locals before any store: this function
     * writes Value->Length and the core writes THROUGH Value->Buffer, so a caller
     * whose Buffer points into its own descriptor could otherwise have the write
     * target mutate mid-call. With locals, every write target is fixed at entry. */
    vbuf = Value->Buffer;
    vmax = Value->MaximumLength;
    if (vmax != 0u && !vbuf)
        return STATUS_INVALID_PARAMETER;
    /* Reject the self-aliasing descriptor outright: an output range covering the
     * descriptor would have the Length store below corrupt the content the caller
     * is about to read. The sibling NtQueryEnvironmentVariable handler rejects the
     * identical overlap. */
    if (rtl_env_ranges_overlap(vbuf, vmax, Value, sizeof(*Value)))
        return STATUS_INVALID_PARAMETER;
    st = rtl_env_name_to_utf8(Name, name8);
    if (st != STATUS_SUCCESS)
        return st;

    /* Delegate the fetch/convert/copy to the shared core. Flooring the byte
     * capacity to whole WCHARs (vmax / 2u) is exactly the `>= Length + 2` NUL
     * guard for every odd capacity: the core writes the NUL only when the WCHAR
     * capacity strictly exceeds the content. */
    st = rtl_env_query_value(name8, vbuf, (uint64_t)(vmax / 2u), &ret_wchars);
    /* The core reports WCHARs -- EXCLUDING the NUL on success, INCLUDING it on
     * too-small. Subtract the counted NUL on the too-small path BEFORE the USHORT
     * check so a 32767-WCHAR value (65534-byte content, still representable) does
     * not spuriously fail the guard. */
    if (st == STATUS_SUCCESS)
        content = ret_wchars;
    else if (st == STATUS_BUFFER_TOO_SMALL)
        content = ret_wchars - 1u;
    else
        return st;                                  /* Length left untouched */

    need_bytes = (uint32_t)(content * 2u);          /* excl NUL */
    /* Length is a uint16 byte count. A stored value is <= ENV_VALUE_MAX (32767)
     * WCHARs = 65534 bytes, so this cannot trip today; it keeps the cast honest if
     * either cap ever moves. */
    if (need_bytes > 0xFFFEu)
        return STATUS_INVALID_PARAMETER;
    /* Publish the CONTENT size EXCLUDING the NUL on BOTH the success and too-small
     * paths -- ntdll's convention (its kernel32 caller appends the NUL and sizes
     * from this). */
    Value->Length = (uint16_t)need_bytes;
    return st;
}

NTSTATUS RtlQueryEnvironmentVariable(void *Environment,
                                     const uint16_t *Name, uint64_t NameLength,
                                     uint16_t *Value, uint64_t ValueLength,
                                     uint64_t *ReturnLength)
{
    char name8[ENV_NAME_MAX + 1u];
    NTSTATUS st;

    /* ReturnLength is NOT pre-zeroed: it may alias Name (or Value), and a store
     * before Name is consumed / the buffers are validated would corrupt the very
     * input this call still needs. rtl_env_query_value publishes it only after the
     * value buffer is validated; on an early refusal it is left untouched (a
     * caller reads it only after a success/too-small status). */
    /* Same foreign-block refusal as the _U form. */
    if (Environment)
        return STATUS_NOT_SUPPORTED;

    /* A name longer than ENV_NAME_MAX WCHARs cannot name a storable variable;
     * refuse BEFORE narrowing NameLength to the uint32 the validator takes (an
     * unchecked cast could otherwise wrap a huge count to a small one). */
    if (NameLength > (uint64_t)ENV_NAME_MAX)
        return STATUS_NAME_TOO_LONG;
    st = rtl_env_name_wchars_to_utf8(Name, (uint32_t)NameLength, name8);
    if (st != STATUS_SUCCESS)
        return st;

    /* ReturnLength carries the shared core's convention verbatim -- content WCHARs
     * EXCLUDING the NUL on success, the required size INCLUDING the NUL on
     * STATUS_BUFFER_TOO_SMALL. This deliberately differs from the counted
     * RtlExpandEnvironmentStrings (which includes the NUL on both paths); the two
     * are not unified. The counted form has no UNICODE_STRING descriptor, so no
     * descriptor-overlap check applies; Name is fully consumed into name8 before
     * any write to Value, so a Name/Value alias is harmless, and the core rejects
     * a ReturnLength cell that aliases the output range. */
    return rtl_env_query_value(name8, Value, ValueLength, ReturnLength);
}

NTSTATUS RtlSetEnvironmentVariable(void **Environment, UNICODE_STRING *Name,
                                   UNICODE_STRING *Value)
{
    char name8[ENV_NAME_MAX + 1u];
    char *val8;
    struct task *cur;
    uint32_t vlen_w, vcap;
    NTSTATUS st;
    int rc, cvt;

    /* Refuse EVERY non-NULL pointer BEFORE dereferencing it. `Environment != NULL`
     * with `*Environment == NULL` is a distinct ntdll form (an initially empty
     * caller-owned environment to allocate and write back), NOT the current process
     * -- serving it as the current process would mutate the wrong environment. And
     * the pointer is untrusted, so it must not be read to find that out. */
    if (Environment)
        return STATUS_NOT_SUPPORTED;
    st = rtl_env_name_to_utf8(Name, name8);
    if (st != STATUS_SUCCESS)
        return st;
    cur = task_current();
    if (!cur)
        return STATUS_INVALID_PARAMETER;

    if (!Value) {
        /* NULL Value deletes. Deleting an absent variable is a SILENT SUCCESS in
         * ntdll -- the caller asked for a state that already holds. */
        rc = env_unset(cur, name8);
        if (rc == ENV_ERR_NOTFOUND)
            return STATUS_SUCCESS;
        return rtl_env_status_from_env(rc);
    }
    if (!rtl_env_us_valid(Value))
        return STATUS_INVALID_PARAMETER;
    vlen_w = (uint32_t)(Value->Length / 2u);
    if (vlen_w == 0u) {
        /* Empty value: a legal set, and handled without touching Value->Buffer,
         * which a zero-Length descriptor is allowed to leave NULL. */
        rc = env_set(cur, name8, "");
        return rtl_env_status_from_env(rc);
    }

    cvt = nls_cp_utf16_to_utf8(Value->Buffer, vlen_w, NULL, 0, NLS_CP_STRICT);
    if (cvt < 0)
        return STATUS_INVALID_PARAMETER;
    vcap = (uint32_t)cvt + 1u;                      /* + NUL */
    val8 = (char *)env_buf_alloc(vcap);
    if (!val8)
        return STATUS_NO_MEMORY;
    cvt = nls_cp_utf16_to_utf8(Value->Buffer, vlen_w, (uint8_t *)val8,
                               vcap - 1u, NLS_CP_STRICT);
    if (cvt < 0) {
        env_buf_free(val8, vcap);
        return STATUS_INVALID_PARAMETER;
    }
    val8[cvt] = '\0';
    /* env_set applies the name/value caps and the ENV_BLOCK_MAX quota; it copies the
     * string, so the scratch buffer is freed immediately after. */
    rc = env_set(cur, name8, val8);
    env_buf_free(val8, vcap);
    return rtl_env_status_from_env(rc);
}

NTSTATUS RtlCreateEnvironment(uint8_t clone_current, void **out_env)
{
    struct task *cur;
    int rc;

    if (!out_env)
        return STATUS_INVALID_PARAMETER;
    *out_env = NULL;

    if (!clone_current) {
        /* EMPTY, not Registry-derived: ntdll walks no Registry, so this is the
         * complete answer (see env_create_empty_block's contract for why this is a
         * separate entry from env_create_block(inherit == 0)). */
        rc = env_create_empty_block(out_env);
    } else {
        cur = task_current();
        if (!cur)
            return STATUS_INVALID_PARAMETER;
        rc = env_create_block(cur, NULL, 1, out_env);
    }

    switch (rc) {
    case ENV_OK:          return STATUS_SUCCESS;
    case ENV_ERR_NOMEM:   return STATUS_NO_MEMORY;
    /* The environ outgrew ENV_CREATE_BLOCK_MAX_WCHARS. Reported as a size failure
     * rather than the SET path's quota status: nothing was rejected for policy, the
     * result simply does not fit the block format's cap. */
    case ENV_ERR_NOSPACE: return STATUS_BUFFER_TOO_SMALL;
    default:              return rtl_env_status_from_env(rc);
    }
}

NTSTATUS RtlDestroyEnvironment(void *env)
{
    /* NTSTATUS rather than VOID is deliberate (see the header): it is the
     * binary-compatible superset when the sources disagree. env_destroy_block is a
     * no-op on NULL and best-effort on a malformed header, so there is no failure to
     * report today -- the status exists for the ABI, not for a condition we hide. */
    env_destroy_block(env);
    return STATUS_SUCCESS;
}

NTSTATUS RtlSetCurrentEnvironment(void *Environment, void **PreviousEnvironment)
{
    struct task *cur;
    uint32_t wchars;
    int rc;

    if (PreviousEnvironment)
        *PreviousEnvironment = NULL;
    if (!Environment)
        return STATUS_INVALID_PARAMETER;

    /* Provenance FIRST: Environment must be a block from RtlCreateEnvironment /
     * RtlCreateEnvironmentEx (our env_create_block format). env_block_extent reads the
     * hidden header (block - 1) and best-effort-rejects a malformed one; it CANNOT
     * validate a truly foreign pointer (see env.h), so callers are kernel-resident by
     * contract. A block that fails the check is a non-provenanced pointer -> refuse with
     * STATUS_NOT_SUPPORTED and leave the live environment untouched. */
    rc = env_block_extent(Environment, &wchars);
    if (rc != ENV_OK)
        return STATUS_NOT_SUPPORTED;

    cur = task_current();
    if (!cur)
        return STATUS_INVALID_PARAMETER;

    /* Decode the caller block into our store, then CONSUME it on success -- native ntdll
     * TRANSFERS ownership of Environment (it installs the pointer as PEB->Environment), so
     * a compatible caller does NOT free it and instead frees PreviousEnvironment. We have
     * no PEB env pointer, so we copy the contents and free the source, which is
     * ownership-equivalent from the caller's view. On FAILURE the caller retains Environment
     * (we free nothing), and the STRICT adoption leaves the prior environment live. */
    rc = env_replace_from_block_utf16(cur, (const uint16_t *)Environment, wchars,
                                      PreviousEnvironment);
    if (rc == ENV_OK)
        env_destroy_block(Environment);
    return rtl_env_status_from_env(rc);
}

NTSTATUS RtlSetEnvironmentStrings(const uint16_t *NewEnvironment,
                                  uint64_t NewEnvironmentSize)
{
    struct task *cur;
    uint32_t wchars;
    int rc;

    if (!NewEnvironment)
        return STATUS_INVALID_PARAMETER;

    /* Size is BYTES and bounds the SCAN, not a single deref. An ODD byte count cannot
     * describe a WCHAR block -- refuse it rather than floor it (flooring would drop a byte
     * and adopt a different block than the caller passed). Zero is not the empty
     * environment (that is the 4-byte "\0\0" form), so size 0 is malformed input. */
    if (NewEnvironmentSize == 0u || (NewEnvironmentSize & 1u) != 0u)
        return STATUS_INVALID_PARAMETER;

    /* Bound the WCHAR count before narrowing to uint32 so a > 4 GiB size cannot wrap the
     * cast. env_replace_from_block_utf16 applies the authoritative block cap; this only
     * closes the narrowing overflow. */
    if ((NewEnvironmentSize / 2u) > (uint64_t)RTL_ENV_BLOCK_MAX_WCHARS)
        return STATUS_QUOTA_EXCEEDED;
    wchars = (uint32_t)(NewEnvironmentSize / 2u);

    cur = task_current();
    if (!cur)
        return STATUS_INVALID_PARAMETER;

    /* Rides the nt_rtlenv kernel-resident input contract: NewEnvironment is trusted to be
     * a mapped block of at least NewEnvironmentSize bytes (a user pointer needs TODO-22
     * s24's probe+copy first). STRICT, all-or-nothing: a missing terminator, an over-cap
     * or malformed entry, or trailing data past the terminator refuses the WHOLE block
     * with the live environment untouched. NULL out_old: the replaced store is freed. */
    rc = env_replace_from_block_utf16(cur, NewEnvironment, wchars, NULL);
    return rtl_env_status_from_env(rc);
}

NTSTATUS RtlCreateEnvironmentEx(void *SourceEnv, void **Environment, uint32_t Flags)
{
    struct task *cur;
    int rc;

    if (!Environment)
        return STATUS_INVALID_PARAMETER;
    *Environment = NULL;

    /* Translating a caller-supplied source block needs the trusted-snapshot boundary
     * TODO-22 s24 owns; refuse a non-NULL source rather than silently ignore it. */
    if (SourceEnv != NULL)
        return STATUS_NOT_SUPPORTED;

    /* Flag precedence: validate BEFORE acting so EMPTY cannot mask a malformed Flags.
     * (1) An unknown bit is STATUS_INVALID_PARAMETER. (2) TRANSLATE_FROM_OEM MODIFIES
     * TRANSLATE and is meaningless alone, so 0x2 without 0x1 is an incompatible
     * combination (STATUS_INVALID_PARAMETER), distinct from (3) a valid-but-unimplemented
     * TRANSLATE request (STATUS_NOT_SUPPORTED, never accept-and-ignore). */
    if ((Flags & ~(RTL_CREATE_ENVIRONMENT_TRANSLATE |
                   RTL_CREATE_ENVIRONMENT_TRANSLATE_FROM_OEM |
                   RTL_CREATE_ENVIRONMENT_EMPTY)) != 0u)
        return STATUS_INVALID_PARAMETER;
    if ((Flags & RTL_CREATE_ENVIRONMENT_TRANSLATE_FROM_OEM) != 0u &&
        (Flags & RTL_CREATE_ENVIRONMENT_TRANSLATE) == 0u)
        return STATUS_INVALID_PARAMETER;
    if ((Flags & (RTL_CREATE_ENVIRONMENT_TRANSLATE |
                  RTL_CREATE_ENVIRONMENT_TRANSLATE_FROM_OEM)) != 0u)
        return STATUS_NOT_SUPPORTED;

    if ((Flags & RTL_CREATE_ENVIRONMENT_EMPTY) != 0u) {
        rc = env_create_empty_block(Environment);
    } else {
        cur = task_current();
        if (!cur)
            return STATUS_INVALID_PARAMETER;
        rc = env_create_block(cur, NULL, 1, Environment);   /* Flags==0 -> clone current */
    }

    switch (rc) {
    case ENV_OK:          return STATUS_SUCCESS;
    case ENV_ERR_NOMEM:   return STATUS_NO_MEMORY;
    /* The environ outgrew ENV_CREATE_BLOCK_MAX_WCHARS -- a size failure (nothing was
     * rejected for policy), mapped as RtlCreateEnvironment maps the same case. */
    case ENV_ERR_NOSPACE: return STATUS_BUFFER_TOO_SMALL;
    default:              return rtl_env_status_from_env(rc);
    }
}
