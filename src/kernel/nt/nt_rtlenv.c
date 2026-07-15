/* ============================================================================
 * nt_rtlenv.c -- ntdll Rtl environment layer (UTF-16 %VAR% expansion)
 *
 * See include/kernel/nt/nt_rtlenv.h for the contract. Arch-neutral: no gdt/idt/
 * msr/cpuid includes. Stateless apart from a single per-call allocation on the
 * NULL-Environment path; no shared mutable state, so no locking is required
 * here (env_build_block_utf16 does its own snapshot under the env mutex).
 *
 * Three invariants keep this safe and consistent with the UTF-8 env layer:
 *  - the raw double-NUL scan is HARD-BOUNDED (RTL_ENV_BLOCK_MAX_WCHARS) and the
 *    transformer takes explicit kernel-resident lengths, never scanning user
 *    memory to an unbounded terminator;
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

/* Content length in WCHARs of a double-NUL-terminated block, scanning at most
 * `cap` wchars. Returns the index of the terminating empty entry (block[idx]==0
 * at an entry start), which is exactly the bound the lookup below walks. Returns
 * `cap` when no empty entry is found within `cap` (caller rejects as malformed).
 * `block` MUST be kernel-resident (contract in the header). */
static uint32_t rtl_env_block_len(const uint16_t *block, uint32_t cap)
{
    uint32_t p = 0;
    while (p < cap) {
        if (block[p] == 0)
            return p;                     /* empty entry -> end of block */
        while (p < cap && block[p] != 0)  /* skip this entry's content */
            p++;
        if (p < cap)
            p++;                          /* skip this entry's NUL */
    }
    return cap;                           /* no terminator within cap */
}

/* Look up `name` (namelen WCHARs) in `block` (bounded by block_wchars). On a
 * case-insensitive ASCII match sets out_val + out_vlen to the value run and
 * returns 1; returns 0 when absent. */
static int rtl_env_block_lookup(const uint16_t *block, uint32_t block_wchars,
                                const uint16_t *name, uint32_t namelen,
                                const uint16_t **out_val, uint32_t *out_vlen)
{
    uint32_t p = 0;
    while (p < block_wchars && block[p] != 0) {
        uint32_t start = p;
        /* A hidden "=X:" drive variable's name BEGINS with '=' (entry
         * "=C:=value"), so that leading '=' is part of the NAME and the
         * separator is the NEXT one. Start the scan one WCHAR in for such an
         * entry; for an ordinary "KEY=VALUE" entry this is the plain first-'='
         * offset. Mirrors the UTF-8 storage comparator (env.c env_entry_keylen)
         * so `%=C:%` resolves identically on both expansion paths -- before this,
         * an "=C:=..." entry split at index 0 and yielded an empty key that no
         * reference could ever name (TODO-22 s19). */
        uint32_t eq = (block[start] == RTL_ENV_WEQ) ? start + 1u : start;
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
                                    uint16_t *dst, uint64_t dst_cap)
{
    uint32_t i = 0;
    uint64_t out = 0;
    while (i < src_wchars) {
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
                                         namelen, &val, &vlen)) {
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

NTSTATUS RtlExpandEnvironmentStrings_U(void *Environment, UNICODE_STRING *Source,
                                       UNICODE_STRING *Destination,
                                       uint32_t *ReturnedLength)
{
    const uint16_t *block;
    uint32_t block_wchars, block_extent;
    const uint16_t *src;
    uint32_t src_wchars, src_bytes;
    uint16_t *dst_buf;                /* SNAPSHOT of Destination->Buffer */
    uint32_t dst_max;                 /* SNAPSHOT of Destination->MaximumLength */
    uint64_t required;                /* output WCHARs excluding NUL (saturating) */
    uint64_t needed_bytes;            /* wide accumulator (incl NUL), avoids u16 wrap */
    uint16_t *synth = NULL;
    uint32_t synth_wchars = 0;
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

    if (Environment) {
        block = (const uint16_t *)Environment;
        /* PRECONDITION: a non-NULL Environment MUST be a well-formed double-NUL-
         * terminated block (as ntdll requires); the cap only backstops a runaway
         * loop and does NOT substitute for a valid terminator. A user-supplied
         * block is probed+copied into a terminated kernel snapshot by the Win32
         * boundary before it reaches here. */
        block_wchars = rtl_env_block_len(block, RTL_ENV_BLOCK_MAX_WCHARS);
        if (block_wchars >= RTL_ENV_BLOCK_MAX_WCHARS)
            return STATUS_INVALID_PARAMETER;         /* unterminated within the cap */
        /* `block_wchars` is the terminator INDEX, so a non-empty supplied block
         * spans block_wchars + 1 WCHARs (through the terminating NUL); an empty
         * block (index 0) is the two-WCHAR "\0\0" form. */
        block_extent = (block_wchars == 0u) ? 2u : (block_wchars + 1u);
    } else {
        /* NULL Environment: the calling process's own block. Synthesize it from
         * the current task's authoritative UTF-8 environ (kernel-resident), not
         * the stale, user-mapped PEB block. */
        struct task *cur = task_current();
        int rc;
        if (!cur)
            return STATUS_INVALID_PARAMETER;
        rc = env_build_block_utf16(cur, &synth, &synth_wchars,
                                   RTL_ENV_BLOCK_MAX_WCHARS);
        if (rc == ENV_ERR_NOMEM)
            return STATUS_NO_MEMORY;
        if (rc != ENV_OK)
            return STATUS_INVALID_PARAMETER;
        block = synth;
        block_wchars = synth_wchars;   /* lookup bound (stops at the NULs anyway) */
        block_extent = synth_wchars;   /* EXACT allocation length (incl terminators) */
    }

    /* Reject any overlap between the output range [dst_buf, dst_buf+dst_max) and
     * (a) the Source DATA, (b) the Environment block, (c)/(d) the Source and
     * Destination DESCRIPTORS, (e) the ReturnedLength cell. (a)/(b) prevent a
     * two-pass count/write tear; (c)-(e) prevent a store from corrupting a
     * pointer/length we still need. An empty output range (dst_max == 0, the
     * size-query form) overlaps nothing and falls through to BUFFER_TOO_SMALL. */
    if (rtl_env_ranges_overlap(dst_buf, dst_max, src, src_bytes) ||
        rtl_env_ranges_overlap(dst_buf, dst_max, block, block_extent * 2u) ||
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
                                block, block_extent * 2u) ||
         rtl_env_ranges_overlap(ReturnedLength, (uint32_t)sizeof(*ReturnedLength),
                                Source, (uint32_t)sizeof(*Source)) ||
         rtl_env_ranges_overlap(ReturnedLength, (uint32_t)sizeof(*ReturnedLength),
                                Destination, (uint32_t)sizeof(*Destination)))) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    /* Pass 1: count the required output length (no write). */
    required = rtl_env_expand_pass(block, block_wchars, src, src_wchars, NULL, 0u);

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
        uint64_t written = rtl_env_expand_pass(block, block_wchars, src,
                                               src_wchars, dst_buf, required);
        /* The two passes must agree. They only differ if the caller-owned Source
         * or explicit Environment was mutated by another CPU between passes (the
         * inputs are kernel-resident but not private/immutable here). A shorter
         * second pass would leave [written, required) UNwritten -- advertising
         * `required` would disclose stale/uninitialized destination bytes. Refuse
         * rather than return a torn result. (The Win32 boundary that passes a
         * private per-call snapshot never trips this.) */
        if (written != required) {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
    }
    /* Both narrowings are exact: `required` <= RTL_ENV_MAX_RESULT_WCHARS (32766)
     * by the step-2 check above, so required * 2 <= 65532 fits the USHORT Length
     * and the index cannot overflow. */
    dst_buf[(uint32_t)required] = 0;
    Destination->Length = (uint16_t)(required * 2u);  /* bytes, excluding NUL */

done:
    if (synth)
        env_free_block_utf16(synth, synth_wchars);
    return status;
}

/* ===========================================================================
 * userenv.dll ExpandEnvironmentStringsForUser (TODO-22 s13)
 *
 * A thin per-user front-end over RtlExpandEnvironmentStrings_U: it builds a
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

    status = RtlExpandEnvironmentStrings_U(block, Source, Destination,
                                           ReturnedLength);
    env_free_block_utf16(block, block_wchars);
    return status;
}
