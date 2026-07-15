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
 * the pass clamp already guarantees memory safety regardless). */
static int rtl_env_ranges_overlap(const void *a, uint32_t alen,
                                  const void *b, uint32_t blen)
{
    uintptr_t a0, a1, b0, b1;
    if (!a || !b || alen == 0u || blen == 0u)
        return 0;
    a0 = (uintptr_t)a;
    a1 = a0 + alen;
    b0 = (uintptr_t)b;
    b1 = b0 + blen;
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

/* The expansion engine proper. Takes the block's bound + extent ALREADY
 * established (either verified by rtl_env_block_validate for a caller-supplied
 * block, or known by construction for a builder-produced one) and the work
 * `budget` to spend per pass.
 *
 * Splitting this out keeps two costs off the paths that do not owe them: a
 * builder-produced block does not pay a re-scan to re-derive a length its
 * builder already reported, and the budget is a policy INPUT rather than a
 * baked-in constant, so a caller with a tighter tolerance (and the tests, which
 * must reach the fit/overshoot boundary without burning the production ceiling)
 * can supply its own. */
static NTSTATUS rtl_env_expand_core(const uint16_t *block, uint32_t block_wchars,
                                    uint32_t verified_extent,
                                    UNICODE_STRING *Source,
                                    UNICODE_STRING *Destination,
                                    uint32_t *ReturnedLength, uint64_t budget)
{
    const uint16_t *src;
    uint32_t src_wchars, src_bytes;
    uint16_t *dst_buf;                /* SNAPSHOT of Destination->Buffer */
    uint32_t dst_max;                 /* SNAPSHOT of Destination->MaximumLength */
    uint64_t required;                /* output WCHARs excluding NUL (saturating) */
    uint64_t needed_bytes;            /* wide accumulator (incl NUL), avoids u16 wrap */
    uint64_t pass1_work;              /* lookup work pass 1 consumed */
    struct rtl_env_work work;
    NTSTATUS status = STATUS_SUCCESS;

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
    src_wchars = (uint32_t)(src_bytes / 2u);        /* Length is an even byte count */
    dst_buf = Destination->Buffer;
    dst_max = Destination->MaximumLength;

    /* Reject any overlap between the output range [dst_buf, dst_buf+dst_max) and
     * (a) the Source DATA, (b) the Environment block, (c)/(d) the Source and
     * Destination DESCRIPTORS, (e) the ReturnedLength cell. (a)/(b) prevent a
     * two-pass count/write tear; (c)-(e) prevent a store from corrupting a
     * pointer/length we still need. An empty output range (dst_max == 0, the
     * size-query form) overlaps nothing and falls through to BUFFER_TOO_SMALL. */
    if (rtl_env_ranges_overlap(dst_buf, dst_max, src, src_bytes) ||
        rtl_env_ranges_overlap(dst_buf, dst_max, block, verified_extent * 2u) ||
        rtl_env_ranges_overlap(dst_buf, dst_max, Destination,
                               (uint32_t)sizeof(*Destination)) ||
        rtl_env_ranges_overlap(dst_buf, dst_max, Source,
                               (uint32_t)sizeof(*Source)) ||
        (ReturnedLength &&
         rtl_env_ranges_overlap(dst_buf, dst_max, ReturnedLength,
                                (uint32_t)sizeof(*ReturnedLength)))) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    /* ReturnedLength is stored BETWEEN the two passes; if it aliases any input
     * (Source data or the Environment block) or either descriptor, that store
     * would mutate state pass 2 still reads (yielding stale output that Length
     * would misreport) or corrupt metadata. Reject those aliases too -- the
     * output-range case is already covered by the matrix above. */
    if (ReturnedLength &&
        (rtl_env_ranges_overlap(ReturnedLength, (uint32_t)sizeof(*ReturnedLength),
                                src, src_bytes) ||
         rtl_env_ranges_overlap(ReturnedLength, (uint32_t)sizeof(*ReturnedLength),
                                block, verified_extent * 2u) ||
         rtl_env_ranges_overlap(ReturnedLength, (uint32_t)sizeof(*ReturnedLength),
                                Source, (uint32_t)sizeof(*Source)) ||
         rtl_env_ranges_overlap(ReturnedLength, (uint32_t)sizeof(*ReturnedLength),
                                Destination, (uint32_t)sizeof(*Destination)))) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    /* Pass 1: count the required output length (no write). */
    work.left = budget;
    work.used = 0u;
    work.exhausted = 0;
    required = rtl_env_expand_pass(block, block_wchars, src, src_wchars, NULL, 0u,
                                   &work);
    /* Refuse BEFORE any store. Pass 1 both counts and pays the full lookup cost,
     * so an over-budget expansion is rejected here, with nothing written and no
     * length published -- a budget refusal can never land mid-write. */
    if (work.exhausted) {
        if (ReturnedLength)
            *ReturnedLength = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pass1_work = work.used;

    /* CHECKED-CONVERSION ORDER (TODO-22 s19). Every narrowing below is guarded
     * BEFORE it happens, so no caller can ever receive a truncated required size
     * and under-allocate on the retry:
     *   1. a saturated count is not a length -- refuse it outright;
     *   2. a result over RTL_ENV_MAX_RESULT_WCHARS cannot be expressed in a
     *      UNICODE_STRING at ANY buffer size, so it is STATUS_UNSUCCESSFUL, not
     *      STATUS_BUFFER_TOO_SMALL (which would loop a grow-and-retry caller
     *      forever). Both refusals precede the *ReturnedLength store, so a
     *      truncated value is never published;
     *   3. only then is needed_bytes (<= 65535 by step 2) narrowed to uint32 for
     *      ReturnedLength and compared against the uint16 MaximumLength snapshot.
     */
    if (required == RTL_ENV_COUNT_SAT ||
        required > (uint64_t)RTL_ENV_MAX_RESULT_WCHARS) {
        /* Publish 0, never the real (unrepresentable) size: real ntdll zeroes its
         * ResultLength on this branch and then stores it unconditionally, so a
         * caller with an uninitialized ReturnedLength local reads 0 rather than
         * stack garbage. 0 is not a truncated length -- it is "no length" -- so
         * the no-truncated-size-published rule is kept, not weakened. The overlap
         * matrix above already proved ReturnedLength aliases no input. */
        if (ReturnedLength)
            *ReturnedLength = 0;
        status = STATUS_UNSUCCESSFUL;
        goto done;
    }
    needed_bytes = (required + 1u) * 2u;             /* include the WCHAR NUL */

    /* needed_bytes <= (32766 + 1) * 2 = 65534 here, so this narrowing is exact. */
    if (ReturnedLength)
        *ReturnedLength = (uint32_t)needed_bytes;

    if (needed_bytes > (uint64_t)dst_max) {
        /* No partial output on overflow; Destination->Length left unchanged. */
        status = STATUS_BUFFER_TOO_SMALL;
        goto done;
    }

    /* The result fits; a NULL output buffer here is a caller error (a size query
     * uses MaximumLength == 0, which takes the BUFFER_TOO_SMALL path above). */
    if (!dst_buf) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    /* Pass 2: write to the SNAPSHOT buffer, clamped to `required` WCHARs (room
     * for `required` + the NUL; the clamp defends against any aliasing tear). */
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
                                      dst_buf, required, &work);
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
            if (ReturnedLength)
                *ReturnedLength = 0;
            status = work.exhausted ? STATUS_INSUFFICIENT_RESOURCES
                                    : STATUS_INVALID_PARAMETER;
            goto done;
        }
    }
    /* Both narrowings are exact: `required` <= RTL_ENV_MAX_RESULT_WCHARS (32766)
     * by the step-2 check above, so required * 2 <= 65532 fits the USHORT Length
     * and the index cannot overflow. */
    dst_buf[(uint32_t)required] = 0;
    Destination->Length = (uint16_t)(required * 2u);  /* bytes, excluding NUL */

done:
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
     * by TODO-22 s21. Nothing user-reachable calls this today, and every
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
