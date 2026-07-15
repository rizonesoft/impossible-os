/* ============================================================================
 * nt_env.c -- NtQueryEnvironmentVariable / NtSetEnvironmentVariable (TODO-22 s5)
 *
 * NT-boundary syscalls (SSDT 0x03DD / 0x03DE) over the calling process's
 * kernel-resident UTF-8 environment (kernel/env.h -- the AUTHORITATIVE store).
 * See include/kernel/nt/nt_env.h for the design contract (single SSDT number
 * serves Nt+Zw via ssdt_previous_mode(); PEB block is a startup snapshot).
 *
 * Boundary hardening invariants:
 *  - Counted UTF-16 inputs may carry embedded U+0000; env_* are C-string APIs,
 *    so an embedded NUL would silently truncate the effective name/value. Both
 *    Name and Value reject any embedded NUL (and empty Name) before conversion.
 *  - env_get_copy snapshots value+length under ONE environ_lock acquisition, so
 *    the value is read into a single MAX-sized buffer with no size-then-copy
 *    race against a sibling thread's env_set.
 *  - The output regions (Value.Buffer span, the Value descriptor, the optional
 *    ValueLength cell) must not overlap; an aliased store would corrupt an
 *    output still owed to the caller. Overlap is rejected before any store.
 *  - Name/value size limits are UTF-8 BYTE limits (ENV_NAME_MAX/ENV_VALUE_MAX),
 *    matching the storage layer; over-limit conversion is rejected, never
 *    silently narrowed.
 *  - Value.Length / *ValueLength report BYTES EXCLUDING the trailing NUL
 *    (RtlQueryEnvironmentVariable_U semantics); the NUL is written only when the
 *    caller buffer has an extra WCHAR of room.
 * ============================================================================ */

#include "kernel/types.h"
#include "libc/string.h"             /* memcpy */
#include "kernel/klog.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/zw.h"            /* Probe*, ssdt_previous_mode, SSDT_USER_MODE */
#include "kernel/nt/ssdt.h"          /* ssdt_register / SSDT_HANDLER */
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/nls_cp.h"        /* nls_cp_utf16_to_utf8 / nls_cp_utf8_to_utf16 */
#include "kernel/nt/nt_unicode.h"    /* nt_decode_unicode_string */
#include "kernel/nt/nt_env.h"
#include "kernel/ob/peb.h"           /* UNICODE_STRING */
#include "kernel/env.h"              /* env_get_copy / env_set / env_unset / ENV_* */
#include "kernel/sched/task.h"       /* task_current */
#include "kernel/cpu_security.h"     /* copy_from_user / copy_to_user */

/* Widest UTF-16 payload a user Value descriptor can carry: UNICODE_STRING.Length
 * is a uint16 byte count, so at most 65534 even bytes = 32767 WCHARs; +1 holds
 * the terminator the decode helper appends. */
#define NT_ENV_VAL_WCHARS_MAX  32768u

/* Transient buffers here come straight from the shared env allocator
 * (env_buf_alloc / env_buf_free, env.h) -- the heap-vs-PMM size-class rule lives
 * there, in one place. */

/* Half-open [a,a+alen) intersects [b,b+blen); both empty extents intersect
 * nothing. Mirrors nt_rtlenv.c rtl_env_ranges_overlap. */
static int nt_env_ranges_overlap(const void *a, uint32_t alen,
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

/* Decode an untrusted Name UNICODE_STRING into a bounded UTF-8 C string.
 * Rejects an empty name and any embedded U+0000 (which would truncate the
 * effective name). The UTF-8 byte length is capped at ENV_NAME_MAX (the storage
 * limit); an over-cap or malformed name is a hard error, never a silent narrow.
 * `name8` must hold ENV_NAME_MAX + 1 bytes; `name_w` ENV_NAME_MAX + 1 WCHARs. */
static NTSTATUS nt_env_decode_name(const UNICODE_STRING *uName, uint16_t *name_w,
                                   char *name8, uint32_t prev)
{
    uint32_t wchars = 0, i;
    int cvt;
    NTSTATUS st = nt_decode_unicode_string(uName, name_w, ENV_NAME_MAX + 1u,
                                           &wchars, prev);
    if (st != STATUS_SUCCESS)
        return (st == STATUS_BUFFER_TOO_SMALL) ? STATUS_NAME_TOO_LONG : st;
    if (wchars == 0u)
        return STATUS_INVALID_PARAMETER;            /* empty name */
    for (i = 0; i < wchars; i++)
        if (name_w[i] == 0u)
            return STATUS_INVALID_PARAMETER;        /* embedded NUL */
    cvt = nls_cp_utf16_to_utf8(name_w, wchars, (uint8_t *)name8, ENV_NAME_MAX,
                               NLS_CP_STRICT);
    if (cvt < 0)
        return (cvt == NLS_CP_ERR_TOO_SMALL) ? STATUS_NAME_TOO_LONG
                                             : STATUS_INVALID_PARAMETER;
    name8[cvt] = '\0';
    return STATUS_SUCCESS;
}

/* ---- NtQueryEnvironmentVariable (0x03DD) --------------------------------
 * a1 = const UNICODE_STRING *Name   (UTF-16, case-insensitive)
 * a2 = UNICODE_STRING       *Value  (out: Buffer/MaximumLength in, Length out)
 * a3 = PULONG                ValueLength (optional; required byte size, excl NUL)
 * STATUS_VARIABLE_NOT_FOUND if absent; STATUS_BUFFER_TOO_SMALL if it will not fit. */
static NTSTATUS NtQueryEnvironmentVariable_handler(uint64_t a1, uint64_t a2,
                                                   uint64_t a3, uint64_t a4,
                                                   uint64_t a5, uint64_t a6)
{
    const UNICODE_STRING *uName  = (const UNICODE_STRING *)a1;
    UNICODE_STRING       *uValue = (UNICODE_STRING *)a2;
    uint32_t             *uVlen  = (uint32_t *)a3;            /* optional */
    uint32_t prev = ssdt_previous_mode();
    uint16_t name_w[ENV_NAME_MAX + 1];
    char     name8[ENV_NAME_MAX + 1];
    UNICODE_STRING vsnap;
    uint16_t *vbuf;
    uint32_t  vmax;
    char     *val8 = NULL;
    uint16_t *val_w = NULL;
    uint32_t  val_len8, need_wchars, need_bytes, write_bytes, vcap;
    int cvt;
    NTSTATUS st;

    (void)a4; (void)a5; (void)a6;

    if (!uName || !uValue)
        return STATUS_INVALID_PARAMETER;

    st = nt_env_decode_name(uName, name_w, name8, prev);
    if (st != STATUS_SUCCESS)
        return st;

    /* Snapshot the Value descriptor ONCE (we read Buffer/MaximumLength and later
     * write Length; ProbeForWrite is range-only so it validates both). */
    if (prev == SSDT_USER_MODE) {
        st = ProbeForWrite(uValue, sizeof(*uValue), 8);
        if (st != STATUS_SUCCESS)
            return st;
        if (copy_from_user(&vsnap, uValue, (uint32_t)sizeof(vsnap)) != 0)
            return STATUS_ACCESS_VIOLATION;
    } else {
        vsnap = *uValue;
    }
    vbuf = vsnap.Buffer;
    vmax = vsnap.MaximumLength;

    /* Reject overlaps among the output regions: Value.Buffer span, the Value
     * descriptor, and the ValueLength cell. An aliased store would corrupt an
     * output the caller still needs. */
    if (nt_env_ranges_overlap(vbuf, vmax, uValue, (uint32_t)sizeof(*uValue)))
        return STATUS_INVALID_PARAMETER;
    if (uVlen) {
        if (nt_env_ranges_overlap(vbuf, vmax, uVlen, (uint32_t)sizeof(*uVlen)) ||
            nt_env_ranges_overlap(uVlen, (uint32_t)sizeof(*uVlen),
                                  uValue, (uint32_t)sizeof(*uValue)))
            return STATUS_INVALID_PARAMETER;
        if (prev == SSDT_USER_MODE) {
            st = ProbeForWrite(uVlen, (uint32_t)sizeof(*uVlen), 4);
            if (st != STATUS_SUCCESS)
                return st;
        }
    }

    /* Snapshot the value under one environ_lock. Env values are usually short,
     * so start with a small heap buffer (kmalloc, no contiguous-PMM scan); only
     * if env_get_copy reports a length that reached the buffer size (truncation)
     * is the value genuinely large, so retry ONCE with the MAX buffer -- the
     * value is bounded by ENV_VALUE_MAX, so the retry always fits. Each
     * env_get_copy is internally atomic (returned length matches the bytes
     * copied), so there is no size-then-copy race, and a tiny or missing query
     * no longer forces a multi-page PMM allocation. */
    vcap = ENV_STR_KMALLOC_MAX;                      /* fast path: kmalloc */
    val8 = (char *)env_buf_alloc(vcap);
    if (!val8)
        return STATUS_NO_MEMORY;
    {
        int r = env_get_copy(task_current(), name8, val8, vcap);
        if (r >= 0 && (uint32_t)r >= vcap) {
            env_buf_free(val8, vcap);                 /* truncated: value is large */
            vcap = ENV_VALUE_MAX + 1u;
            val8 = (char *)env_buf_alloc(vcap);
            if (!val8)
                return STATUS_NO_MEMORY;
            r = env_get_copy(task_current(), name8, val8, vcap);
        }
        if (r >= 0 && (uint32_t)r >= vcap) {
            /* Defense in depth: a stored value is always <= ENV_VALUE_MAX
             * (env_set + env_adopt_block enforce the value cap), so the
             * ENV_VALUE_MAX+1 retry buffer fits. If a returned length still
             * meets or exceeds the buffer, refuse rather than let the UTF-8
             * decoder below read past val8[vcap]. */
            env_buf_free(val8, vcap);
            return STATUS_NAME_TOO_LONG;
        }
        if (r < 0) {
            env_buf_free(val8, vcap);
            switch (r) {
            case ENV_ERR_NOTFOUND: return STATUS_VARIABLE_NOT_FOUND;
            case ENV_ERR_TOOLONG:  return STATUS_NAME_TOO_LONG;
            default:               return STATUS_INVALID_PARAMETER;
            }
        }
        val_len8 = (uint32_t)r;
    }

    /* Size the UTF-16 form (WCHARs, excl NUL). */
    cvt = nls_cp_utf8_to_utf16((const uint8_t *)val8, val_len8, NULL, 0,
                               NLS_CP_STRICT);
    if (cvt < 0) {
        env_buf_free(val8, vcap);
        return STATUS_INVALID_PARAMETER;            /* corrupt stored value */
    }
    need_wchars = (uint32_t)cvt;
    need_bytes  = need_wchars * 2u;                 /* excl the WCHAR NUL */

    /* Report the required size regardless of fit. */
    if (uVlen) {
        if (prev == SSDT_USER_MODE) {
            if (copy_to_user(uVlen, &need_bytes, (uint32_t)sizeof(need_bytes)) != 0) {
                env_buf_free(val8, vcap);
                return STATUS_ACCESS_VIOLATION;
            }
        } else {
            *uVlen = need_bytes;
        }
    }

    if (need_bytes > vmax) {
        env_buf_free(val8, vcap);
        return STATUS_BUFFER_TOO_SMALL;             /* no partial write */
    }
    if (need_bytes > 0u && !vbuf) {
        env_buf_free(val8, vcap);
        return STATUS_INVALID_PARAMETER;
    }

    /* Convert into a private buffer, then copy out. NUL-terminate only when the
     * caller buffer has an extra WCHAR of room. */
    write_bytes = need_bytes;
    if (need_wchars > 0u) {
        val_w = (uint16_t *)env_buf_alloc((need_wchars + 1u) * 2u);
        if (!val_w) {
            env_buf_free(val8, vcap);
            return STATUS_NO_MEMORY;
        }
        cvt = nls_cp_utf8_to_utf16((const uint8_t *)val8, val_len8, val_w,
                                   need_wchars, NLS_CP_STRICT);
        env_buf_free(val8, vcap);
        val8 = NULL;
        if (cvt < 0 || (uint32_t)cvt != need_wchars) {
            env_buf_free(val_w, (need_wchars + 1u) * 2u);
            return STATUS_INVALID_PARAMETER;        /* two passes disagreed */
        }
        if (vmax >= need_bytes + 2u) {
            val_w[need_wchars] = 0u;
            write_bytes = need_bytes + 2u;
        }
    } else {
        /* Empty value: append a NUL only if there is room. */
        env_buf_free(val8, vcap);
        val8 = NULL;
        if (vmax >= 2u) {
            /* Sized with the SAME expression every env_buf_free(val_w, ...) below
             * uses. need_wchars is 0 on this branch, so this is literally 2 -- but
             * writing it as the formula keeps alloc and free textually paired: the
             * size selects the deallocator, so a divergence here would be a
             * wrong-allocator free the day this branch's invariant changes. */
            val_w = (uint16_t *)env_buf_alloc((need_wchars + 1u) * 2u);
            if (!val_w)
                return STATUS_NO_MEMORY;
            val_w[0] = 0u;
            write_bytes = 2u;
        }
    }

    /* A stored empty value still owes a terminator when there is room; the
     * need_bytes NULL guard above is bypassed for it (need_bytes == 0), so
     * reject a NULL Buffer here whenever the FINAL write is nonzero. Without
     * this, a KernelMode/Zw caller with Buffer == NULL and MaximumLength >= 2
     * would memcpy a terminator through NULL. */
    if (write_bytes > 0u && !vbuf) {
        if (val_w)
            env_buf_free(val_w, (need_wchars + 1u) * 2u);
        return STATUS_INVALID_PARAMETER;
    }

    if (write_bytes > 0u) {
        if (prev == SSDT_USER_MODE) {
            st = ProbeForWrite(vbuf, write_bytes, 2);
            if (st != STATUS_SUCCESS) {
                env_buf_free(val_w, (need_wchars + 1u) * 2u);
                return st;
            }
            if (copy_to_user(vbuf, val_w, write_bytes) != 0) {
                env_buf_free(val_w, (need_wchars + 1u) * 2u);
                return STATUS_ACCESS_VIOLATION;
            }
        } else {
            memcpy(vbuf, val_w, write_bytes);
        }
    }
    if (val_w)
        env_buf_free(val_w, (need_wchars + 1u) * 2u);

    /* Write Value.Length back (bytes, excl NUL). */
    {
        uint16_t len16 = (uint16_t)need_bytes;
        if (prev == SSDT_USER_MODE) {
            if (copy_to_user(&uValue->Length, &len16, (uint32_t)sizeof(len16)) != 0)
                return STATUS_ACCESS_VIOLATION;
        } else {
            uValue->Length = len16;
        }
    }
    return STATUS_SUCCESS;
}

/* ---- NtSetEnvironmentVariable (0x03DE) ----------------------------------
 * a1 = const UNICODE_STRING *Name
 * a2 = const UNICODE_STRING *Value  (NULL -> delete the variable)
 * Updates ONLY the kernel-authoritative task->environ. */
static NTSTATUS NtSetEnvironmentVariable_handler(uint64_t a1, uint64_t a2,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    const UNICODE_STRING *uName  = (const UNICODE_STRING *)a1;
    const UNICODE_STRING *uValue = (const UNICODE_STRING *)a2;   /* NULL -> unset */
    uint32_t prev = ssdt_previous_mode();
    uint16_t name_w[ENV_NAME_MAX + 1];
    char     name8[ENV_NAME_MAX + 1];
    uint16_t *val_w = NULL;
    char     *val8 = NULL;
    uint32_t  val_wchars = 0, val_wcap, i;
    int cvt, r, val8_n;
    NTSTATUS st;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!uName)
        return STATUS_INVALID_PARAMETER;

    st = nt_env_decode_name(uName, name_w, name8, prev);
    if (st != STATUS_SUCCESS)
        return st;

    if (!uValue) {
        r = env_unset(task_current(), name8);
        return (r == ENV_OK)            ? STATUS_SUCCESS
             : (r == ENV_ERR_NOTFOUND)  ? STATUS_VARIABLE_NOT_FOUND
             : (r == ENV_ERR_TOOLONG)   ? STATUS_NAME_TOO_LONG
                                        : STATUS_INVALID_PARAMETER;
    }

    /* Size the private UTF-16 buffer to the descriptor's own Length so a short
     * value does not force a fixed 64 KiB contiguous-PMM allocation. Snapshot the
     * header first (probed for UserMode); nt_decode_unicode_string re-validates
     * the descriptor and enforces its own capacity bound, so a concurrent grow of
     * Length can only make it return BUFFER_TOO_SMALL, never overflow this
     * buffer. +1 WCHAR holds the terminator the decode helper appends. */
    {
        uint16_t vlen_bytes;
        if (prev == SSDT_USER_MODE) {
            UNICODE_STRING vhdr;
            st = ProbeForRead(uValue, (uint32_t)sizeof(*uValue), 8);
            if (st != STATUS_SUCCESS)
                return st;
            if (copy_from_user(&vhdr, uValue, (uint32_t)sizeof(vhdr)) != 0)
                return STATUS_ACCESS_VIOLATION;
            vlen_bytes = vhdr.Length;
        } else {
            vlen_bytes = uValue->Length;
        }
        if (vlen_bytes & 1u)
            return STATUS_INVALID_PARAMETER;            /* must be WCHAR-aligned */
        val_wcap = (uint32_t)(vlen_bytes / 2u) + 1u;
    }
    val_w = (uint16_t *)env_buf_alloc(val_wcap * 2u);
    if (!val_w)
        return STATUS_NO_MEMORY;
    st = nt_decode_unicode_string(uValue, val_w, val_wcap, &val_wchars, prev);
    if (st != STATUS_SUCCESS) {
        env_buf_free(val_w, val_wcap * 2u);
        return st;
    }
    for (i = 0; i < val_wchars; i++) {
        if (val_w[i] == 0u) {
            env_buf_free(val_w, val_wcap * 2u);
            return STATUS_INVALID_PARAMETER;        /* embedded NUL */
        }
    }

    /* Size the UTF-8 form and reject over the storage cap BEFORE the big alloc. */
    cvt = nls_cp_utf16_to_utf8(val_w, val_wchars, NULL, 0, NLS_CP_STRICT);
    if (cvt < 0) {
        env_buf_free(val_w, val_wcap * 2u);
        return STATUS_INVALID_PARAMETER;
    }
    if ((uint32_t)cvt > ENV_VALUE_MAX) {
        env_buf_free(val_w, val_wcap * 2u);
        return STATUS_NAME_TOO_LONG;                /* value exceeds ENV_VALUE_MAX */
    }
    val8_n = cvt + 1;
    val8 = (char *)env_buf_alloc((uint32_t)val8_n);
    if (!val8) {
        env_buf_free(val_w, val_wcap * 2u);
        return STATUS_NO_MEMORY;
    }
    cvt = nls_cp_utf16_to_utf8(val_w, val_wchars, (uint8_t *)val8,
                               (uint32_t)(val8_n - 1), NLS_CP_STRICT);
    env_buf_free(val_w, val_wcap * 2u);
    if (cvt < 0) {
        env_buf_free(val8, (uint32_t)val8_n);
        return STATUS_INVALID_PARAMETER;
    }
    val8[cvt] = '\0';

    r = env_set(task_current(), name8, val8);
    env_buf_free(val8, (uint32_t)val8_n);
    /* ENV_ERR_NOSPACE is the per-process quota (block > 1 MiB or entry-count cap),
     * a deterministic limit -- STATUS_QUOTA_EXCEEDED, not the transient
     * STATUS_INSUFFICIENT_RESOURCES (which is ENV_ERR_NOMEM's actual-alloc-failure
     * signal). TODO-22 s10 requires QUOTA_EXCEEDED for block overflow. */
    return (r == ENV_OK)          ? STATUS_SUCCESS
         : (r == ENV_ERR_NOMEM)   ? STATUS_NO_MEMORY
         : (r == ENV_ERR_NOSPACE) ? STATUS_QUOTA_EXCEEDED
         : (r == ENV_ERR_TOOLONG) ? STATUS_NAME_TOO_LONG
                                  : STATUS_INVALID_PARAMETER;
}

/* ---- Registration ------------------------------------------------------- */

int nt_env_register_ssdt(void)
{
    int fail = 0;
    if (ssdt_register(SSDT_NtQueryEnvironmentVariable,
                      (SSDT_HANDLER)NtQueryEnvironmentVariable_handler) != 0)
        fail++;
    if (ssdt_register(SSDT_NtSetEnvironmentVariable,
                      (SSDT_HANDLER)NtSetEnvironmentVariable_handler) != 0)
        fail++;
    klog(LOG_INFO, "nt", "env vars: 2 NtXxx handlers registered");
    return fail;
}
