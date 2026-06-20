/* ============================================================================
 * kcodec.c -- Kernel base64 / hex codec dispatch
 *
 * Caller-buffer, bounds-checked, no allocation. base64 is RFC 4648 (standard
 * alphabet, '=' padding) with a strict mode (no stray bytes, padding placement
 * validated) and a MIME mode (ASCII whitespace skipped). hex decode accepts
 * mixed case and rejects odd length / non-hex bytes. Every function returns the
 * byte count written or -1 on malformed input / insufficient destination.
 *
 * XREF: 02-kernel-core/TODO-03-kernel-libraries.md
 * ============================================================================ */

#include "kernel/kcodec.h"

#define KCODEC_INT_MAX 0x7FFFFFFF

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- base64 ------------------------------------------------------------- */

int base64_encode(char *dst, size_t dst_cap, const void *src, size_t src_len)
{
    if (!dst || (!src && src_len))
        return -1;
    /* Bound src_len BEFORE the +2 so the addition cannot wrap on a pathological
     * length; 3*(KCODEC_INT_MAX/4) is the largest input that still encodes to
     * <= KCODEC_INT_MAX output bytes (4*ceil(n/3)). */
    if (src_len > 3 * (size_t)(KCODEC_INT_MAX / 4))
        return -1;
    size_t groups = (src_len + 2) / 3;
    size_t out_len = groups * 4;
    if (out_len > dst_cap)
        return -1;

    const uint8_t *p = (const uint8_t *)src;
    size_t o = 0, i = 0;
    while (i + 3 <= src_len) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        dst[o++] = B64_ALPHABET[(v >> 18) & 63];
        dst[o++] = B64_ALPHABET[(v >> 12) & 63];
        dst[o++] = B64_ALPHABET[(v >> 6) & 63];
        dst[o++] = B64_ALPHABET[v & 63];
        i += 3;
    }
    size_t rem = src_len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        dst[o++] = B64_ALPHABET[(v >> 18) & 63];
        dst[o++] = B64_ALPHABET[(v >> 12) & 63];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
        dst[o++] = B64_ALPHABET[(v >> 18) & 63];
        dst[o++] = B64_ALPHABET[(v >> 12) & 63];
        dst[o++] = B64_ALPHABET[(v >> 6) & 63];
        dst[o++] = '=';
    }
    return (int)o;
}

int base64_decode(void *dst, size_t dst_cap, const char *src, size_t src_len, int mime)
{
    if (!dst || (!src && src_len))
        return -1;
    uint8_t *out = (uint8_t *)dst;
    /* Clamp the effective capacity so the per-quad bounds check stops the loop
     * before o can exceed INT_MAX -- guarantees no >INT_MAX write and a valid
     * int return rather than mutating the buffer then reporting -1. */
    if (dst_cap > (size_t)KCODEC_INT_MAX)
        dst_cap = (size_t)KCODEC_INT_MAX;
    size_t o = 0;
    uint32_t acc = 0;
    int quad = 0, pad = 0, done = 0;

    for (size_t i = 0; i < src_len; i++) {
        char ch = src[i];
        if (mime && (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'))
            continue;
        if (done)                       /* non-whitespace after a padded quad */
            return -1;
        if (ch == '=') {
            if (quad < 2)               /* '=' cannot be the 1st/2nd char of a quad */
                return -1;
            acc <<= 6;
            pad++;
            quad++;
            if (pad > 2)
                return -1;
        } else {
            if (pad)                    /* data char after '=' within the quad */
                return -1;
            int v = b64_val(ch);
            if (v < 0)
                return -1;
            acc = (acc << 6) | (uint32_t)v;
            quad++;
        }
        if (quad == 4) {
            int nbytes = 3 - pad;       /* pad 0->3, 1->2, 2->1 bytes */
            if ((size_t)nbytes > dst_cap - o)
                return -1;
            if (nbytes >= 1) out[o++] = (uint8_t)(acc >> 16);
            if (nbytes >= 2) out[o++] = (uint8_t)(acc >> 8);
            if (nbytes >= 3) out[o++] = (uint8_t)acc;
            if (pad)
                done = 1;               /* a padded quad must be the last one */
            acc = 0;
            quad = 0;
            pad = 0;
        }
    }
    if (quad != 0)                       /* trailing partial quad (length not a multiple of 4) */
        return -1;
    if (o > (size_t)KCODEC_INT_MAX)
        return -1;
    return (int)o;
}

/* ---- hex ---------------------------------------------------------------- */

int hex_encode(char *dst, size_t dst_cap, const void *src, size_t src_len)
{
    if (!dst || (!src && src_len))
        return -1;
    if (src_len > (size_t)(KCODEC_INT_MAX / 2))
        return -1;
    if (src_len * 2 > dst_cap)
        return -1;
    static const char H[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)src;
    for (size_t i = 0; i < src_len; i++) {
        dst[i * 2]     = H[p[i] >> 4];
        dst[i * 2 + 1] = H[p[i] & 0x0F];
    }
    return (int)(src_len * 2);
}

int hex_decode(void *dst, size_t dst_cap, const char *src, size_t src_len)
{
    if (!dst || (!src && src_len))
        return -1;
    if (src_len & 1u)                    /* odd length */
        return -1;
    size_t out_len = src_len / 2;
    if (out_len > (size_t)KCODEC_INT_MAX)   /* int-return overflow guard */
        return -1;
    if (out_len > dst_cap)
        return -1;
    uint8_t *out = (uint8_t *)dst;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_val(src[i * 2]);
        int lo = hex_val(src[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)out_len;
}
