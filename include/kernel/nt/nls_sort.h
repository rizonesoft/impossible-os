/* ============================================================================
 * nls_sort.h -- Invariant sort keys, normalization policy, FoldStringW
 *
 * The collation / normalization layer of the atom/NLS/locale subsystem. Sort
 * keys fold through nls_upcase_char (the section-2 compiled ASCII/Latin-1 fold
 * PLUS the section-4 full-BMP NLS table when loaded), so U+0100+ key bytes are
 * table-version-dependent -- an ephemeral key is compare-then-discard, not
 * persisted across a table reload (as with Windows LCMAP_SORTKEY vs the NLS
 * version). The OB/registry code-unit-exact security compare is a SEPARATE path
 * (rtl_upcase_char), NOT these keys. Kernel-internal like the other NLS layers;
 * the Win32 LCMapStringEx / FoldStringW / CompareStringEx export + SSDT surface
 * is owned by the NLS native-syscall section, not here.
 *
 * Normalization POLICY: the kernel does NOT silently normalize object names --
 * the Object Manager / Registry compare code-unit-exact through the section-2
 * authority. nls_normalize() is the EXPLICIT, opt-in helper, and it is honest
 * about coverage: it performs NFC/NFD over ASCII + the Latin-1 precomposed set
 * only and returns NLS_NORM_ERR_UNSUPPORTED for any code unit outside that range
 * (except the handled combining marks). Full UAX #15 + NFKC/NFKD are user-mode.
 * ============================================================================ */
#ifndef KERNEL_NT_NLS_SORT_H
#define KERNEL_NT_NLS_SORT_H

#include "kernel/types.h"

/* Common negative error codes (a non-negative return is a written/required
 * element count). */
#define NLS_SORT_ERR_PARAM       (-1)
#define NLS_SORT_ERR_TOO_SMALL   (-2)

/* ---- Sort keys (LCMapStringEx / LCMAP_SORTKEY opaque byte format) --------- */
/*
 * nls_sort_key -- build an opaque invariant collation sort key that compares
 * with plain memcmp. src_len is a WCHAR count, or -1 for a NUL-terminated
 * string. When ignore_case != 0 the case band is omitted (primary-only key).
 *
 * Byte layout (this is Impossible's format; it need not match Windows'):
 *   [ primary band: 3 order-preserving bytes per code unit, each >= 0x02,
 *     base-254 encoding of the invariant upcase-fold weight ]
 *   0x01 level separator
 *   [ case band (omitted when ignore_case): 1 byte per unit, 0x02 lowercase-
 *     origin / 0x03 uppercase-or-caseless ]
 *   0x00 terminator
 * Because 0x00 and 0x01 never appear inside a band, memcmp gives a total order
 * in which a prefix sorts before its extensions (e.g. "a" < "aa").
 *
 * dst == NULL runs a sizing pass (returns the required byte count). Returns the
 * byte length written, or a negative NLS_SORT_ERR_*.
 */
int nls_sort_key(const uint16_t *src, int32_t src_len,
                 uint8_t *dst, uint32_t dst_cap, int ignore_case);

/* nls_sort_key_binary -- ordinal binary key: raw big-endian code units. Unlike
 * nls_sort_key (which is pure-memcmp-comparable thanks to its 0x00-free bands +
 * terminator), this raw key has no terminator, so a prefix's bytes ARE a prefix
 * of its extension's bytes -- compare with a LENGTH-AWARE comparator (min-length
 * memcmp, then shorter-sorts-first), not bare memcmp. Same dst==NULL sizing +
 * return contract. */
int nls_sort_key_binary(const uint16_t *src, int32_t src_len,
                        uint8_t *dst, uint32_t dst_cap);

/* nls_sort_key_binary_compare -- the length-aware comparator the terminator-less
 * binary key requires (min-length memcmp, then shorter-sorts-first). Ships with
 * the key so callers do not each re-derive it (a bare memcmp would rank a prefix
 * EQUAL to its extension). Returns -1 / 0 / +1. A NULL side with a non-zero
 * length is treated as empty (sorts first). */
int nls_sort_key_binary_compare(const uint8_t *a, uint32_t alen,
                                const uint8_t *b, uint32_t blen);

/* ---- Normalization (explicit, opt-in) ------------------------------------ */
#define NLS_NORM_NFC   1   /* canonical composition */
#define NLS_NORM_NFD   2   /* canonical decomposition */

#define NLS_NORM_ERR_PARAM       (-1)
#define NLS_NORM_ERR_TOO_SMALL   (-2)
/* out-of-range code unit, OR an in-range sequence outside the 1-starter/1-mark model (see nls_normalize doc) */
#define NLS_NORM_ERR_UNSUPPORTED (-3)
#define NLS_NORM_ERR_FORM        (-4)  /* unknown form */

/*
 * nls_normalize -- NFC/NFD over ASCII + the Latin-1 precomposed set only. NFD
 * decomposes the precomposed Latin-1 letters to base + combining mark; NFC
 * composes base + a handled combining mark back to the precomposed letter.
 * src_len is a WCHAR count, or -1 for NUL-terminated. dst == NULL sizes.
 *
 * Honest coverage: returns NLS_NORM_ERR_UNSUPPORTED if the input contains any
 * code unit outside {U+0000..U+00FF} plus the handled combining marks, OR an
 * in-range sequence the single-starter/single-mark model cannot represent (a
 * leading mark with no starter, a second mark on one starter, or a base+mark
 * pair absent from the Latin-1 composition table) -- the kernel does NOT
 * guarantee full UAX #15, so a caller must fall back to the user-mode normalizer
 * rather than trust a partial fold. Returns the WCHAR count written, or a
 * negative NLS_NORM_ERR_*.
 */
int nls_normalize(int form, const uint16_t *src, int32_t src_len,
                  uint16_t *dst, uint32_t dst_cap);

/* ---- FoldStringW (flag values match winnls.h) ---------------------------- */
#define NLS_MAP_FOLDCZONE   0x00000010u  /* MAP_FOLDCZONE: compatibility fold (incl. fullwidth -> halfwidth) */
#define NLS_MAP_FOLDDIGITS  0x00000080u  /* MAP_FOLDDIGITS: fold digits -> ASCII 0-9 */

#define NLS_FOLD_ERR_PARAM       (-1)
#define NLS_FOLD_ERR_TOO_SMALL   (-2)
#define NLS_FOLD_ERR_FLAGS       (-3)  /* no supported fold flag, or an unsupported flag bit set */

/*
 * nls_fold_string -- FoldStringW subset. Applies the requested compiled folds
 * per code unit: MAP_FOLDDIGITS (fullwidth FF10-FF19 + Arabic-Indic 0660-0669 +
 * Extended Arabic-Indic 06F0-06F9 -> '0'-'9'); MAP_FOLDCZONE (compatibility:
 * fullwidth FF01-FF5E -> ASCII 0x21-0x7E and the ideographic space 3000 -> 0x20).
 * NARROW COVERAGE (by design): this is the compiled fallback. Only the listed
 * ranges fold; every other code unit passes through UNCHANGED -- a Unicode
 * decimal digit outside the three digit ranges (FOLDDIGITS) or a compatibility
 * character outside FF01-FF5E/3000 (FOLDCZONE) is NOT folded. Full FoldStringW
 * coverage (all Nd digits / all compatibility decompositions) needs the disk
 * fold-table data (the reserved nls_table_v1 FOLD_* chunks) and is owned by the
 * NLS syscall section that exposes the public FoldStringW; a caller needing
 * complete coverage must not treat this helper's success as a full fold.
 *
 * An unsupported flag bit is rejected with NLS_FOLD_ERR_FLAGS. src_len is a
 * WCHAR count, or -1 for NUL-terminated. dst == NULL sizes. Returns the WCHAR
 * count written, or a negative NLS_FOLD_ERR_*.
 */
int nls_fold_string(uint32_t flags, const uint16_t *src, int32_t src_len,
                    uint16_t *dst, uint32_t dst_cap);

#endif /* KERNEL_NT_NLS_SORT_H */
