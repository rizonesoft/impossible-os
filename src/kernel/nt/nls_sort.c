/* ============================================================================
 * nls_sort.c -- invariant sort keys + normalization + FoldStringW
 *
 * See nls_sort.h for the contract. Design points adopted pre-code:
 *  - Sort-key primary weights use an order-preserving base-254 encoding with
 *    every payload byte >= 0x02, so the 0x01 level separator and 0x00 terminator
 *    never collide with a weight byte and memcmp yields a correct total order
 *    (a prefix sorts before its extensions).
 *  - nls_normalize is honest about coverage: ASCII + Latin-1 precomposed only,
 *    returning NLS_NORM_ERR_UNSUPPORTED for code units outside that range so a
 *    caller cannot mistake a partial fold for full UAX #15.
 * ============================================================================ */

#include "kernel/types.h"
#include "libc/string.h"           /* memcmp -- binary-key comparator */
#include "kernel/nt/nls_sort.h"
#include "kernel/nt/nls.h"         /* nls_upcase_char -- the full-BMP fold */

/* Sort-key primary-band radix: 256 minus the two reserved control bytes (0x00
 * terminator + 0x01 level separator), and the +bias that lifts every payload
 * byte clear of them. The whole memcmp total-order guarantee rests on the bias
 * keeping payload bytes >= 0x02, so name these load-bearing values explicitly. */
#define NLS_SORTKEY_BIAS  2u
#define NLS_SORTKEY_BASE  (256u - NLS_SORTKEY_BIAS)    /* 254 */

/* ---- length helper (src_len -1 == NUL-terminated) ------------------------ */
static uint32_t nls_wlen(const uint16_t *src, int32_t src_len)
{
    uint32_t n = 0;
    if (src_len >= 0)
        return (uint32_t)src_len;
    while (src[n] != 0)
        n++;
    return n;
}

/* Append one byte / u16; returns 0 ok, 1 = out of cap (count still advances so
 * a dst==NULL sizing pass works). */
static int b_put(uint8_t *dst, uint32_t cap, uint64_t *n, uint8_t v)
{
    if (dst) { if (*n >= cap) return 1; dst[*n] = v; }
    (*n)++;
    return 0;
}
static int w_put(uint16_t *dst, uint32_t cap, uint64_t *n, uint16_t v)
{
    if (dst) { if (*n >= cap) return 1; dst[*n] = v; }
    (*n)++;
    return 0;
}

/* ---- Sort keys ----------------------------------------------------------- */

int nls_sort_key(const uint16_t *src, int32_t src_len,
                 uint8_t *dst, uint32_t dst_cap, int ignore_case)
{
    uint32_t len, i;
    uint64_t n = 0;
    if (!src || src_len < -1)
        return NLS_SORT_ERR_PARAM;
    len = nls_wlen(src, src_len);

    /* Primary band: base-254 encode the invariant upcase weight into 3 bytes,
     * each biased +2 so no byte is 0x00 or 0x01. */
    for (i = 0; i < len; i++) {
        uint16_t w = nls_upcase_char(src[i]);
        uint8_t b0 = (uint8_t)(w / (NLS_SORTKEY_BASE * NLS_SORTKEY_BASE));
        uint8_t b1 = (uint8_t)((w / NLS_SORTKEY_BASE) % NLS_SORTKEY_BASE);
        uint8_t b2 = (uint8_t)(w % NLS_SORTKEY_BASE);
        if (b_put(dst, dst_cap, &n, (uint8_t)(b0 + NLS_SORTKEY_BIAS))) return NLS_SORT_ERR_TOO_SMALL;
        if (b_put(dst, dst_cap, &n, (uint8_t)(b1 + NLS_SORTKEY_BIAS))) return NLS_SORT_ERR_TOO_SMALL;
        if (b_put(dst, dst_cap, &n, (uint8_t)(b2 + NLS_SORTKEY_BIAS))) return NLS_SORT_ERR_TOO_SMALL;
    }
    if (b_put(dst, dst_cap, &n, 0x01)) return NLS_SORT_ERR_TOO_SMALL;   /* level separator */

    if (!ignore_case) {
        /* Case band: 0x02 for a lowercase-origin unit (folds to a different
         * upper form), 0x03 for uppercase-or-caseless. Lowercase sorts first at
         * this tertiary level when primaries are equal. */
        for (i = 0; i < len; i++) {
            uint8_t cw = (nls_upcase_char(src[i]) != src[i]) ? 0x02 : 0x03;
            if (b_put(dst, dst_cap, &n, cw)) return NLS_SORT_ERR_TOO_SMALL;
        }
    }
    if (b_put(dst, dst_cap, &n, 0x00)) return NLS_SORT_ERR_TOO_SMALL;   /* terminator */
    if (n > 0x7FFFFFFFu) return NLS_SORT_ERR_TOO_SMALL;
    return (int)n;
}

int nls_sort_key_binary(const uint16_t *src, int32_t src_len,
                        uint8_t *dst, uint32_t dst_cap)
{
    uint32_t len, i;
    uint64_t n = 0;
    if (!src || src_len < -1)
        return NLS_SORT_ERR_PARAM;
    len = nls_wlen(src, src_len);
    for (i = 0; i < len; i++) {
        if (b_put(dst, dst_cap, &n, (uint8_t)(src[i] >> 8))) return NLS_SORT_ERR_TOO_SMALL;
        if (b_put(dst, dst_cap, &n, (uint8_t)(src[i] & 0xFF))) return NLS_SORT_ERR_TOO_SMALL;
    }
    if (n > 0x7FFFFFFFu) return NLS_SORT_ERR_TOO_SMALL;
    return (int)n;
}

int nls_sort_key_binary_compare(const uint8_t *a, uint32_t alen,
                                const uint8_t *b, uint32_t blen)
{
    uint32_t m;
    /* A NULL side is empty: normalize the length to 0 FIRST so the tiebreak
     * below cannot rank a NULL/non-zero-length input equal to a real key of the
     * same length (which would collapse distinct keys / break weak ordering). */
    if (!a) alen = 0;
    if (!b) blen = 0;
    m = alen < blen ? alen : blen;
    if (m) {   /* m > 0 implies both sides non-NULL, so memcmp cannot overread */
        int c = memcmp(a, b, m);
        if (c) return c < 0 ? -1 : 1;
    }
    /* Common prefix equal: shorter sorts first. */
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

/* ---- Normalization (ASCII + Latin-1) ------------------------------------- */

/* Canonical decomposition of the Latin-1 precomposed letters: precomposed ->
 * base + one combining mark. The atomic Latin-1 letters (0xC6 AE, 0xD0 ETH,
 * 0xD8 O-slash, 0xDE THORN, 0xDF sharp-s, and their lowercase) have no canonical
 * decomposition and are absent here (they pass through). */
struct nls_decomp { uint16_t pre; uint16_t base; uint16_t mark; };
static const struct nls_decomp s_decomp[] = {
    { 0x00C0, 'A', 0x0300 }, { 0x00C1, 'A', 0x0301 }, { 0x00C2, 'A', 0x0302 },
    { 0x00C3, 'A', 0x0303 }, { 0x00C4, 'A', 0x0308 }, { 0x00C5, 'A', 0x030A },
    { 0x00C7, 'C', 0x0327 },
    { 0x00C8, 'E', 0x0300 }, { 0x00C9, 'E', 0x0301 }, { 0x00CA, 'E', 0x0302 },
    { 0x00CB, 'E', 0x0308 },
    { 0x00CC, 'I', 0x0300 }, { 0x00CD, 'I', 0x0301 }, { 0x00CE, 'I', 0x0302 },
    { 0x00CF, 'I', 0x0308 },
    { 0x00D1, 'N', 0x0303 },
    { 0x00D2, 'O', 0x0300 }, { 0x00D3, 'O', 0x0301 }, { 0x00D4, 'O', 0x0302 },
    { 0x00D5, 'O', 0x0303 }, { 0x00D6, 'O', 0x0308 },
    { 0x00D9, 'U', 0x0300 }, { 0x00DA, 'U', 0x0301 }, { 0x00DB, 'U', 0x0302 },
    { 0x00DC, 'U', 0x0308 },
    { 0x00DD, 'Y', 0x0301 },
    { 0x00E0, 'a', 0x0300 }, { 0x00E1, 'a', 0x0301 }, { 0x00E2, 'a', 0x0302 },
    { 0x00E3, 'a', 0x0303 }, { 0x00E4, 'a', 0x0308 }, { 0x00E5, 'a', 0x030A },
    { 0x00E7, 'c', 0x0327 },
    { 0x00E8, 'e', 0x0300 }, { 0x00E9, 'e', 0x0301 }, { 0x00EA, 'e', 0x0302 },
    { 0x00EB, 'e', 0x0308 },
    { 0x00EC, 'i', 0x0300 }, { 0x00ED, 'i', 0x0301 }, { 0x00EE, 'i', 0x0302 },
    { 0x00EF, 'i', 0x0308 },
    { 0x00F1, 'n', 0x0303 },
    { 0x00F2, 'o', 0x0300 }, { 0x00F3, 'o', 0x0301 }, { 0x00F4, 'o', 0x0302 },
    { 0x00F5, 'o', 0x0303 }, { 0x00F6, 'o', 0x0308 },
    { 0x00F9, 'u', 0x0300 }, { 0x00FA, 'u', 0x0301 }, { 0x00FB, 'u', 0x0302 },
    { 0x00FC, 'u', 0x0308 },
    { 0x00FD, 'y', 0x0301 }, { 0x00FF, 'y', 0x0308 },
};
#define NLS_DECOMP_COUNT (sizeof(s_decomp) / sizeof(s_decomp[0]))

static int nls_is_handled_mark(uint16_t c)
{
    return c == 0x0300 || c == 0x0301 || c == 0x0302 || c == 0x0303 ||
           c == 0x0308 || c == 0x030A || c == 0x0327;
}

/* Reject any code unit the kernel range excludes (so a partial fold is never
 * mistaken for full normalization): allow ASCII + Latin-1 + the handled marks. */
static int nls_norm_supported(const uint16_t *src, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        if (src[i] > 0x00FF && !nls_is_handled_mark(src[i]))
            return 0;
    return 1;
}

int nls_normalize(int form, const uint16_t *src, int32_t src_len,
                  uint16_t *dst, uint32_t dst_cap)
{
    uint32_t len, i, j;
    uint64_t n = 0;
    if (!src || src_len < -1)
        return NLS_NORM_ERR_PARAM;
    if (form != NLS_NORM_NFC && form != NLS_NORM_NFD)
        return NLS_NORM_ERR_FORM;
    len = nls_wlen(src, src_len);
    if (!nls_norm_supported(src, len))
        return NLS_NORM_ERR_UNSUPPORTED;

    if (form == NLS_NORM_NFD) {
        /* Only one handled mark per starter is canonically ordered without a
         * combining-class sort. A 2nd mark on a starter (e.g. A-acute + cedilla)
         * or a leading mark with no starter fails closed rather than emitting an
         * un-reordered (non-NFD) sequence as success. */
        int have_starter = 0, starter_marks = 0;
        for (i = 0; i < len; i++) {
            uint16_t c = src[i];
            if (nls_is_handled_mark(c)) {
                if (!have_starter || starter_marks >= 1)
                    return NLS_NORM_ERR_UNSUPPORTED;
                starter_marks = 1;
                if (w_put(dst, dst_cap, &n, c)) return NLS_NORM_ERR_TOO_SMALL;
            } else {
                const struct nls_decomp *d = 0;
                for (j = 0; j < NLS_DECOMP_COUNT; j++)
                    if (s_decomp[j].pre == c) { d = &s_decomp[j]; break; }
                if (d) {
                    if (w_put(dst, dst_cap, &n, d->base)) return NLS_NORM_ERR_TOO_SMALL;
                    if (w_put(dst, dst_cap, &n, d->mark)) return NLS_NORM_ERR_TOO_SMALL;
                    starter_marks = 1;
                } else {
                    if (w_put(dst, dst_cap, &n, c)) return NLS_NORM_ERR_TOO_SMALL;
                    starter_marks = 0;
                }
                have_starter = 1;
            }
        }
    } else { /* NFC */
        /* Pending-starter model: a handled combining mark MUST compose with the
         * current starter via the table. A mark with no starter (leading mark), a
         * base+mark pair absent from the table (e.g. C + U+0301 = U+0106), OR a
         * SECOND mark on an already-composed precomposed starter (e.g.
         * A + U+030A + U+0301 = U+01FA -- the precomposed is never a table base)
         * all fail closed, so a partially-normalized multi-mark sequence is never
         * returned as success. */
        uint16_t starter = 0;
        int have_starter = 0;
        for (i = 0; i < len; i++) {
            uint16_t c = src[i];
            if (nls_is_handled_mark(c)) {
                int composed = 0;
                if (have_starter)
                    for (j = 0; j < NLS_DECOMP_COUNT; j++)
                        if (s_decomp[j].base == starter && s_decomp[j].mark == c) {
                            starter = s_decomp[j].pre;   /* absorb into the starter */
                            composed = 1;
                            break;
                        }
                if (!composed)
                    return NLS_NORM_ERR_UNSUPPORTED;
            } else {
                if (have_starter && w_put(dst, dst_cap, &n, starter))
                    return NLS_NORM_ERR_TOO_SMALL;
                starter = c;
                have_starter = 1;
            }
        }
        if (have_starter && w_put(dst, dst_cap, &n, starter))
            return NLS_NORM_ERR_TOO_SMALL;
    }
    if (n > 0x7FFFFFFFu) return NLS_NORM_ERR_TOO_SMALL;
    return (int)n;
}

/* ---- FoldStringW --------------------------------------------------------- */

static uint16_t nls_fold_unit(uint32_t flags, uint16_t c)
{
    uint16_t o = c;
    if (flags & NLS_MAP_FOLDDIGITS) {
        if (c >= 0xFF10 && c <= 0xFF19)                 /* fullwidth digits */
            o = (uint16_t)(0x0030 + (c - 0xFF10));
        else if (c >= 0x0660 && c <= 0x0669)            /* Arabic-Indic */
            o = (uint16_t)(0x0030 + (c - 0x0660));
        else if (c >= 0x06F0 && c <= 0x06F9)            /* Extended Arabic-Indic */
            o = (uint16_t)(0x0030 + (c - 0x06F0));
    }
    if ((flags & NLS_MAP_FOLDCZONE) && o == c) {
        if (c >= 0xFF01 && c <= 0xFF5E)                 /* fullwidth ASCII */
            o = (uint16_t)(c - 0xFEE0);
        else if (c == 0x3000)                           /* ideographic space */
            o = 0x0020;
    }
    return o;
}

#define NLS_FOLD_SUPPORTED (NLS_MAP_FOLDDIGITS | NLS_MAP_FOLDCZONE)

int nls_fold_string(uint32_t flags, const uint16_t *src, int32_t src_len,
                    uint16_t *dst, uint32_t dst_cap)
{
    uint32_t len, i;
    uint64_t n = 0;
    if (!src || src_len < -1)
        return NLS_FOLD_ERR_PARAM;
    /* Require at least one supported flag and reject any unsupported bit, so a
     * caller passing e.g. MAP_COMPOSITE/MAP_EXPAND_LIGATURES fails closed instead
     * of getting a silently partial transform reported as success. */
    if (!(flags & NLS_FOLD_SUPPORTED) || (flags & ~NLS_FOLD_SUPPORTED))
        return NLS_FOLD_ERR_FLAGS;
    len = nls_wlen(src, src_len);
    for (i = 0; i < len; i++)
        if (w_put(dst, dst_cap, &n, nls_fold_unit(flags, src[i]))) return NLS_FOLD_ERR_TOO_SMALL;
    if (n > 0x7FFFFFFFu) return NLS_FOLD_ERR_TOO_SMALL;
    return (int)n;
}
