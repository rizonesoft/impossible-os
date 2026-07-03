/* ============================================================================
 * nt_unicode.c -- Canonical UNICODE_STRING validation + bounded-copy primitive
 *
 * See include/kernel/nt/nt_unicode.h for the contract. Stateless and re-entrant:
 * no shared mutable state, so no locking is required on any CPU. User-supplied
 * memory is always snapshot-copied into kernel storage before it is inspected,
 * closing the time-of-check/time-of-use window.
 * ============================================================================ */

#include "kernel/nt/nt_unicode.h"
#include "kernel/nt/zw.h"          /* ssdt_previous_mode, SSDT_USER_MODE, ProbeFor* */
#include "kernel/cpu_security.h"   /* copy_from_user */
#include "kernel/types.h"
#include "libc/string.h"           /* memcpy */

/* Snapshot the load-bearing UNICODE_STRING header fields out of (possibly user)
 * memory into kernel locals. In user mode the whole struct is probed then copied
 * once so a concurrent mutation cannot change what validation already checked. */
static NTSTATUS snapshot_ustring(const UNICODE_STRING *us, uint32_t prev_mode,
                                 uint16_t *o_len, uint16_t *o_max,
                                 uint16_t **o_buf)
{
    UNICODE_STRING snap;

    if (!us)
        return STATUS_INVALID_PARAMETER;

    if (prev_mode == SSDT_USER_MODE) {
        /* Probe keyed to the EXPLICIT prev_mode, not ambient
         * ssdt_previous_mode(): a caller may pass a saved UserMode while the
         * current thread runs in a kernel (nested Zw) context, and this
         * primitive must still validate the untrusted pointer.
         * ProbeForReadIfUser would silently skip that probe. */
        NTSTATUS ps = ProbeForRead(us, sizeof(UNICODE_STRING), 4);
        if (ps != STATUS_SUCCESS)
            return ps;
        if (copy_from_user(&snap, us, (uint32_t)sizeof(UNICODE_STRING)) != 0)
            return STATUS_ACCESS_VIOLATION;
    } else {
        snap = *us;
    }

    *o_len = snap.Length;
    *o_max = snap.MaximumLength;
    *o_buf = snap.Buffer;
    return STATUS_SUCCESS;
}

int nt_unicode_wchars_to_bytes(uint32_t wchars, uint32_t *out_bytes)
{
    /* wchars * 2 must fit the uint16 Length ceiling (and cannot wrap). */
    if (wchars > NT_UNICODE_MAX_WCHARS)
        return 0;
    if (out_bytes)
        *out_bytes = wchars * 2u;
    return 1;
}

NTSTATUS nt_unicode_string_validate(const UNICODE_STRING *us, uint32_t prev_mode,
                                    uint16_t **out_buffer, uint32_t *out_length)
{
    uint16_t len, max;
    uint16_t *buf;
    NTSTATUS st;

    st = snapshot_ustring(us, prev_mode, &len, &max, &buf);
    if (st != STATUS_SUCCESS)
        return st;

    if (!buf)
        return STATUS_INVALID_PARAMETER;
    if (len & 1u)                       /* must be WCHAR-aligned */
        return STATUS_INVALID_PARAMETER;
    if (len > max)                      /* content cannot exceed the buffer */
        return STATUS_INVALID_PARAMETER;
    if (len > NT_UNICODE_MAX_BYTES)     /* absolute ceiling (even => <= 65534) */
        return STATUS_INVALID_PARAMETER;

    /* Probe the Buffer itself for UserMode callers: this helper is the
     * documented syscall-boundary authority, so it must not return a kernel /
     * unmapped pointer as 'validated'. Keyed to the explicit prev_mode. */
    if (prev_mode == SSDT_USER_MODE && len > 0u) {
        NTSTATUS bp = ProbeForRead(buf, (uint64_t)len, 2);
        if (bp != STATUS_SUCCESS)
            return bp;
    }

    if (out_buffer)
        *out_buffer = buf;
    if (out_length)
        *out_length = (uint32_t)len;
    return STATUS_SUCCESS;
}

NTSTATUS nt_decode_unicode_string(const UNICODE_STRING *us, uint16_t *kbuf,
                                  uint32_t kbuf_wchars, uint32_t *out_wchars,
                                  uint32_t prev_mode)
{
    uint16_t *ubuf;
    uint32_t len_bytes, wchars;
    NTSTATUS st;

    if (!kbuf || kbuf_wchars == 0u)
        return STATUS_INVALID_PARAMETER;

    st = nt_unicode_string_validate(us, prev_mode, &ubuf, &len_bytes);
    if (st != STATUS_SUCCESS)
        return st;

    wchars = len_bytes / 2u;
    /* Need room for wchars code units plus one NUL terminator. Comparing with
     * >= (rather than wchars + 1) avoids any add overflow on a hostile count. */
    if (wchars >= kbuf_wchars)
        return STATUS_BUFFER_TOO_SMALL;

    if (wchars > 0u) {
        /* nt_unicode_string_validate already probed ubuf for UserMode. */
        if (prev_mode == SSDT_USER_MODE) {
            if (copy_from_user(kbuf, ubuf, len_bytes) != 0)
                return STATUS_ACCESS_VIOLATION;
        } else {
            memcpy(kbuf, ubuf, len_bytes);
        }
    }
    kbuf[wchars] = 0u;
    if (out_wchars)
        *out_wchars = wchars;
    return STATUS_SUCCESS;
}

NTSTATUS nt_encode_unicode_string(UNICODE_STRING *us, uint16_t *wbuf,
                                  uint32_t wchars, uint32_t wbuf_capacity_wchars)
{
    uint32_t bytes;

    if (!us || !wbuf)
        return STATUS_INVALID_PARAMETER;
    if (wchars > wbuf_capacity_wchars)              /* content must fit the buffer */
        return STATUS_INVALID_PARAMETER;
    if (!nt_unicode_wchars_to_bytes(wchars, &bytes))
        return STATUS_INVALID_PARAMETER;
    /* MaximumLength = the TRUE backing size (capacity*2) so it is never
     * overstated; it must also fit the uint16 field. */
    if (wbuf_capacity_wchars > NT_UNICODE_MAX_WCHARS)
        return STATUS_INVALID_PARAMETER;

    us->Length = (uint16_t)bytes;
    us->MaximumLength = (uint16_t)(wbuf_capacity_wchars * 2u);
    us->_pad = 0u;              /* clear ABI padding: no kernel-memory leak on copy-out */
    us->Buffer = wbuf;
    return STATUS_SUCCESS;
}

NTSTATUS nt_unicode_to_ascii(const uint16_t *wbuf, uint32_t wchars, char *abuf,
                             uint32_t abuf_size, uint32_t *out_len)
{
    uint32_t i;

    if (!wbuf || !abuf || abuf_size == 0u)
        return STATUS_INVALID_PARAMETER;
    /* wchars bytes plus a NUL. >= avoids the wchars + 1 add wrapping. */
    if (wchars >= abuf_size)
        return STATUS_BUFFER_TOO_SMALL;

    for (i = 0; i < wchars; i++) {
        uint16_t c = wbuf[i];
        if (c == 0u || c > 0x7Fu)   /* embedded NUL or non-ASCII: lossless reject */
            return STATUS_INVALID_PARAMETER;
        abuf[i] = (char)c;
    }
    abuf[wchars] = '\0';
    if (out_len)
        *out_len = wchars;
    return STATUS_SUCCESS;
}
