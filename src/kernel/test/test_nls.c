/* ============================================================================
 * test_nls.c -- Atom/NLS/locale subsystem tests (TEST_CAT_NLS)
 *
 * Covers the canonical UNICODE_STRING primitive layer: validation of malformed
 * counted strings, TOCTOU-safe bounded decode into kernel buffers, overflow-safe
 * encode, and the lossless ASCII-range narrowing bridge. All tests run in
 * kernel previous-mode, so the UNICODE_STRINGs point at kernel-stack buffers and
 * no user-memory probing occurs.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "libc/string.h"           /* memset for synthetic blob builders */
#include "kernel/nt/nt_unicode.h"
#include "kernel/nt/nt_rtlstr.h"   /* rtl_upcase_char, Rtl*UnicodeString, CompareStringOrdinal */
#include "kernel/nt/nls.h"         /* S4: nls_table_parse + accessors + fallback */
#include "kernel/nt/nls_cp.h"      /* S5: code page conversion providers */
#include "kernel/nt/nls_locale.h"  /* S6: locale/LCID metadata */
#include "registry.h"              /* S5: HKLM\SYSTEM\Nls policy preservation test */
#include "kernel/kchecksum.h"      /* kcrc32 for synthetic table blobs */
#include "kernel/nt/zw.h"          /* SSDT_KERNEL_MODE */

/* Helper: build a UNICODE_STRING over a kernel WCHAR literal. */
static void make_us(UNICODE_STRING *us, uint16_t *buf, uint16_t len_bytes,
                    uint16_t max_bytes)
{
    us->Length = len_bytes;
    us->MaximumLength = max_bytes;
    us->_pad = 0;
    us->Buffer = buf;
}

static void test_nls_validate_rejects_odd_length(void)
{
    uint16_t w[4] = { 'A', 'B', 0, 0 };
    UNICODE_STRING us;
    NTSTATUS st;
    make_us(&us, w, 3 /* odd */, 8);
    st = nt_unicode_string_validate(&us, SSDT_KERNEL_MODE, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "odd Length rejected");
}

static void test_nls_validate_rejects_len_gt_max(void)
{
    uint16_t w[4] = { 'A', 'B', 0, 0 };
    UNICODE_STRING us;
    NTSTATUS st;
    make_us(&us, w, 8, 4 /* Length > MaximumLength */);
    st = nt_unicode_string_validate(&us, SSDT_KERNEL_MODE, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "Length > MaximumLength rejected");
}

static void test_nls_validate_rejects_null_buffer(void)
{
    UNICODE_STRING us;
    NTSTATUS st;
    make_us(&us, (uint16_t *)0, 4, 8);
    st = nt_unicode_string_validate(&us, SSDT_KERNEL_MODE, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "NULL Buffer rejected");
}

static void test_nls_validate_accepts_valid(void)
{
    uint16_t w[4] = { 'A', 'B', 'C', 0 };
    UNICODE_STRING us;
    uint16_t *obuf = 0;
    uint32_t olen = 999;
    NTSTATUS st;
    make_us(&us, w, 6, 8);
    st = nt_unicode_string_validate(&us, SSDT_KERNEL_MODE, &obuf, &olen);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "valid accepted");
    TEST_ASSERT(obuf == w, "out_buffer is snapshot Buffer");
    TEST_ASSERT_EQ((uint64_t)olen, 6u, "out_length is byte Length");
}

static void test_nls_decode_success(void)
{
    uint16_t w[3] = { 'A', 'B', 'C' };
    uint16_t kbuf[8];
    UNICODE_STRING us;
    uint32_t got = 0;
    NTSTATUS st;
    make_us(&us, w, 6, 6);
    st = nt_decode_unicode_string(&us, kbuf, 8, &got, SSDT_KERNEL_MODE);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "decode ok");
    TEST_ASSERT_EQ((uint64_t)got, 3u, "3 code units");
    TEST_ASSERT(kbuf[0] == 'A' && kbuf[1] == 'B' && kbuf[2] == 'C',
                "content copied");
    TEST_ASSERT_EQ((uint64_t)kbuf[3], 0u, "NUL terminated");
}

static void test_nls_decode_buffer_too_small(void)
{
    uint16_t w[3] = { 'A', 'B', 'C' };
    uint16_t kbuf[3];   /* 3 units: no room for 3 chars + NUL */
    UNICODE_STRING us;
    NTSTATUS st;
    make_us(&us, w, 6, 6);
    st = nt_decode_unicode_string(&us, kbuf, 3, 0, SSDT_KERNEL_MODE);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_BUFFER_TOO_SMALL,
                   "decode into too-small buffer rejected");
}

static void test_nls_decode_empty(void)
{
    uint16_t w[1] = { 0 };
    uint16_t kbuf[4];
    uint32_t got = 999;
    UNICODE_STRING us;
    NTSTATUS st;
    make_us(&us, w, 0, 2);
    st = nt_decode_unicode_string(&us, kbuf, 4, &got, SSDT_KERNEL_MODE);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "empty decode ok");
    TEST_ASSERT_EQ((uint64_t)got, 0u, "0 code units");
    TEST_ASSERT_EQ((uint64_t)kbuf[0], 0u, "empty NUL result");
}

static void test_nls_wchars_to_bytes_overflow(void)
{
    uint32_t out = 0;
    int ok_hi = nt_unicode_wchars_to_bytes(NT_UNICODE_MAX_WCHARS + 1u, &out);
    int ok_lo = nt_unicode_wchars_to_bytes(3u, &out);
    TEST_ASSERT_EQ((uint64_t)ok_hi, 0u, "over-ceiling wchar count rejected");
    TEST_ASSERT_EQ((uint64_t)ok_lo, 1u, "in-range wchar count ok");
    TEST_ASSERT_EQ((uint64_t)out, 6u, "3 wchars = 6 bytes");
}

static void test_nls_encode_success(void)
{
    uint16_t w[3] = { 'X', 'Y', 'Z' };
    UNICODE_STRING us;
    NTSTATUS st;
    st = nt_encode_unicode_string(&us, w, 3, 3);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "encode ok");
    TEST_ASSERT_EQ((uint64_t)us.Length, 6u, "Length = 6");
    TEST_ASSERT_EQ((uint64_t)us.MaximumLength, 6u, "MaximumLength = capacity*2 (not overstated)");
    TEST_ASSERT(us.Buffer == w, "Buffer set");
}

static void test_nls_encode_rejects_over_capacity(void)
{
    uint16_t w[2] = { 'A', 'B' };
    UNICODE_STRING us;
    /* wchars (4) exceeds the declared backing capacity (2). */
    NTSTATUS st = nt_encode_unicode_string(&us, w, 4, 2);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "encode with content larger than capacity rejected");
}

static void test_nls_ascii_narrow_success(void)
{
    uint16_t w[3] = { 'A', 'B', 'C' };
    char a[8];
    uint32_t got = 0;
    NTSTATUS st = nt_unicode_to_ascii(w, 3, a, 8, &got);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "ascii narrow ok");
    TEST_ASSERT_EQ((uint64_t)got, 3u, "3 bytes");
    TEST_ASSERT(a[0] == 'A' && a[1] == 'B' && a[2] == 'C' && a[3] == '\0',
                "narrowed + terminated");
}

static void test_nls_ascii_narrow_rejects_nonascii(void)
{
    uint16_t w[3] = { 'A', 0x00E9 /* e-acute */, 'C' };
    char a[8];
    NTSTATUS st = nt_unicode_to_ascii(w, 3, a, 8, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "non-ASCII code unit rejected (lossless)");
}

static void test_nls_ascii_narrow_rejects_embedded_nul(void)
{
    uint16_t w[3] = { 'A', 0x0000, 'C' };
    char a[8];
    NTSTATUS st = nt_unicode_to_ascii(w, 3, a, 8, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "embedded NUL rejected (lossless)");
}

static void test_nls_ascii_narrow_buffer_too_small(void)
{
    uint16_t w[3] = { 'A', 'B', 'C' };
    char a[3];   /* 3 chars + NUL does not fit */
    NTSTATUS st = nt_unicode_to_ascii(w, 3, a, 3, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_BUFFER_TOO_SMALL,
                   "ascii narrow into too-small buffer rejected");
}

static void test_nls_encode_clears_pad(void)
{
    uint16_t w[2] = { 'H', 'I' };
    UNICODE_STRING us;
    NTSTATUS st;
    us._pad = 0xDEADBEEFu;   /* seed the ABI padding non-zero */
    st = nt_encode_unicode_string(&us, w, 2, 2);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "encode ok");
    TEST_ASSERT_EQ((uint64_t)us._pad, 0u,
                   "encode clears ABI _pad (no kernel-memory leak on copy-out)");
}

static void test_nls_validate_probes_buffer_kernel_ptr(void)
{
    /* Valid header on the (low-range) kernel stack but Buffer points into
     * kernel space: with an EXPLICIT UserMode, validate must probe the Buffer
     * itself and reject it, not just check the header. */
    UNICODE_STRING us;
    uint16_t *obuf = 0;
    NTSTATUS st;
    make_us(&us, (uint16_t *)0xFFFF800000000000ULL, 2, 2);
    st = nt_unicode_string_validate(&us, SSDT_USER_MODE, &obuf, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_ACCESS_VIOLATION,
                   "validate probes the UserMode Buffer pointer and rejects kernel range");
}

static void test_nls_user_mode_probe_rejects_kernel_ptr(void)
{
    /* Pass a pointer ABOVE MM_USER_PROBE_ADDRESS with an EXPLICIT UserMode:
     * the probe must key off the argument (ProbeForRead), not the ambient
     * kernel previous-mode, and reject the out-of-user-range pointer BEFORE
     * dereferencing it. (A kernel-stack address is below the user-probe
     * boundary on this identity-mapped kernel, so a canonical high-half
     * address is used to exercise the range guard.) */
    UNICODE_STRING *bad = (UNICODE_STRING *)0xFFFF800000000000ULL;
    NTSTATUS st = nt_unicode_string_validate(bad, SSDT_USER_MODE, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_ACCESS_VIOLATION,
                   "explicit UserMode probes the non-user pointer and rejects it");
}

/* ---- Section 2: invariant case fold + Rtl*UnicodeString compare ----------- */

static void test_nls_upcase_char_ascii(void)
{
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char('a'), (uint64_t)'A', "a -> A");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char('z'), (uint64_t)'Z', "z -> Z");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char('A'), (uint64_t)'A', "A unchanged");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char('5'), (uint64_t)'5', "digit unchanged");
}

static void test_nls_upcase_char_latin1(void)
{
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char(0x00E0u), (uint64_t)0x00C0u, "0xE0 -> 0xC0");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char(0x00FEu), (uint64_t)0x00DEu, "0xFE -> 0xDE");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char(0x00F7u), (uint64_t)0x00F7u, "0xF7 division unchanged");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char(0x00FFu), (uint64_t)0x0178u, "0xFF -> U+0178");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char(0x00DFu), (uint64_t)0x00DFu, "sharp s unchanged (no SS)");
}

static void test_nls_upcase_char_above_latin1_unchanged(void)
{
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char(0x0100u), (uint64_t)0x0100u,
                   "U+0100 unchanged (pending NLS table)");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char(0x0430u), (uint64_t)0x0430u,
                   "Cyrillic unchanged (pending NLS table)");
}

static void test_nls_upcase_string_success(void)
{
    uint16_t src[4] = { 'f', 'i', 'l', 'e' };
    uint16_t dst[4] = { 0, 0, 0, 0 };
    uint16_t want[4] = { 'F', 'I', 'L', 'E' };
    UNICODE_STRING us, ud;
    NTSTATUS st;
    int i;
    make_us(&us, src, 8, 8);
    make_us(&ud, dst, 0, 8);
    st = RtlUpcaseUnicodeString(&ud, &us, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "upcase success");
    TEST_ASSERT_EQ((uint64_t)ud.Length, (uint64_t)8, "dst Length == src Length");
    for (i = 0; i < 4; i++)
        TEST_ASSERT_EQ((uint64_t)dst[i], (uint64_t)want[i], "upcased code unit");
}

static void test_nls_upcase_string_buffer_too_small(void)
{
    uint16_t src[4] = { 'f', 'i', 'l', 'e' };
    uint16_t dst[2] = { 0, 0 };
    UNICODE_STRING us, ud;
    NTSTATUS st;
    make_us(&us, src, 8, 8);
    make_us(&ud, dst, 0, 4 /* only 2 wchars */);
    st = RtlUpcaseUnicodeString(&ud, &us, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_BUFFER_TOO_SMALL, "too-small dst rejected");
}

static void test_nls_upcase_string_rejects_allocate(void)
{
    uint16_t src[1] = { 'a' };
    uint16_t dst[1] = { 0 };
    UNICODE_STRING us, ud;
    NTSTATUS st;
    make_us(&us, src, 2, 2);
    make_us(&ud, dst, 0, 2);
    st = RtlUpcaseUnicodeString(&ud, &us, 1 /* allocate */);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "allocate_destination != 0 rejected at kernel Rtl layer");
}

static void test_nls_equal_case_insensitive(void)
{
    uint16_t a[4] = { 'F', 'i', 'l', 'e' };
    uint16_t b[4] = { 'f', 'i', 'l', 'e' };
    UNICODE_STRING ua, ub;
    make_us(&ua, a, 8, 8);
    make_us(&ub, b, 8, 8);
    TEST_ASSERT_EQ((uint64_t)RtlEqualUnicodeString(&ua, &ub, 1), (uint64_t)1,
                   "File == file case-insensitive");
    TEST_ASSERT_EQ((uint64_t)RtlEqualUnicodeString(&ua, &ub, 0), (uint64_t)0,
                   "File != file case-sensitive");
}

static void test_nls_equal_different_length(void)
{
    uint16_t a[3] = { 'a', 'b', 'c' };
    uint16_t b[2] = { 'a', 'b' };
    UNICODE_STRING ua, ub;
    make_us(&ua, a, 6, 6);
    make_us(&ub, b, 4, 4);
    TEST_ASSERT_EQ((uint64_t)RtlEqualUnicodeString(&ua, &ub, 1), (uint64_t)0,
                   "different length not equal");
}

static void test_nls_compare_case_sensitive_distinguishes(void)
{
    uint16_t a[1] = { 0x0041 };  /* 'A' */
    uint16_t b[1] = { 0x0061 };  /* 'a' */
    UNICODE_STRING ua, ub;
    int r_cs, r_ci;
    make_us(&ua, a, 2, 2);
    make_us(&ub, b, 2, 2);
    r_cs = RtlCompareUnicodeString(&ua, &ub, 0);
    r_ci = RtlCompareUnicodeString(&ua, &ub, 1);
    TEST_ASSERT(r_cs < 0, "U+0041 sorts before U+0061 case-sensitive");
    TEST_ASSERT_EQ((uint64_t)r_ci, (uint64_t)0, "U+0041 == U+0061 case-insensitive");
}

static void test_nls_compare_prefix_sorts_first(void)
{
    uint16_t a[2] = { 'a', 'b' };
    uint16_t b[3] = { 'a', 'b', 'c' };
    UNICODE_STRING ua, ub;
    make_us(&ua, a, 4, 4);
    make_us(&ub, b, 6, 6);
    TEST_ASSERT(RtlCompareUnicodeString(&ua, &ub, 0) < 0, "prefix sorts before longer");
    TEST_ASSERT(RtlCompareUnicodeString(&ub, &ua, 0) > 0, "longer sorts after prefix");
}

static void test_nls_ordinal_compare(void)
{
    uint16_t a[3] = { 'A', 'B', 'C' };
    uint16_t b[3] = { 'a', 'b', 'c' };
    uint16_t c[3] = { 'A', 'B', 'C' };
    TEST_ASSERT_EQ((uint64_t)CompareStringOrdinal(a, 3, c, 3, 0),
                   (uint64_t)NT_CSTR_EQUAL, "equal ordinal");
    TEST_ASSERT_EQ((uint64_t)CompareStringOrdinal(a, 3, b, 3, 0),
                   (uint64_t)NT_CSTR_LESS_THAN, "A < a ordinal");
    TEST_ASSERT_EQ((uint64_t)CompareStringOrdinal(a, 3, b, 3, 1),
                   (uint64_t)NT_CSTR_EQUAL, "ABC == abc ordinal case-insensitive");
    TEST_ASSERT_EQ((uint64_t)CompareStringOrdinal(0, 3, b, 3, 0),
                   (uint64_t)NT_CSTR_ERROR, "NULL operand -> error");
}

static void test_nls_ordinal_nul_terminated_length(void)
{
    uint16_t a[4] = { 'a', 'b', 'c', 0 };
    uint16_t b[4] = { 'a', 'b', 'c', 0 };
    TEST_ASSERT_EQ((uint64_t)CompareStringOrdinal(a, -1, b, -1, 0),
                   (uint64_t)NT_CSTR_EQUAL, "-1 length scans to NUL, equal");
}

static void test_nls_ordinal_rejects_bad_negative(void)
{
    uint16_t a[2] = { 'a', 0 };
    uint16_t b[2] = { 'a', 0 };
    /* Only -1 is the NUL-terminated sentinel; any count < -1 is rejected before
     * any scan can run past the operand. */
    TEST_ASSERT_EQ((uint64_t)CompareStringOrdinal(a, -2, b, 1, 0),
                   (uint64_t)NT_CSTR_ERROR, "a_wchars < -1 rejected");
    TEST_ASSERT_EQ((uint64_t)CompareStringOrdinal(a, 1, b, -2, 0),
                   (uint64_t)NT_CSTR_ERROR, "b_wchars < -1 rejected");
    TEST_ASSERT_EQ((uint64_t)CompareStringOrdinal(a, (int32_t)0x80000000, b, 1, 0),
                   (uint64_t)NT_CSTR_ERROR, "INT32_MIN count rejected");
}

static void test_nls_equal_rejects_malformed(void)
{
    uint16_t buf[2] = { 'a', 'b' };
    UNICODE_STRING good, bad;
    make_us(&good, buf, 4, 4);
    /* Odd Length: never reported equal even against an identical odd sibling. */
    make_us(&bad, buf, 3, 4);
    TEST_ASSERT_EQ((uint64_t)RtlEqualUnicodeString(&bad, &bad, 0), (uint64_t)0,
                   "odd-Length operand not equal");
    /* Length > MaximumLength. */
    make_us(&bad, buf, 8, 4);
    TEST_ASSERT_EQ((uint64_t)RtlEqualUnicodeString(&good, &bad, 0), (uint64_t)0,
                   "Length > MaximumLength not equal");
    /* Non-empty Length with NULL Buffer. */
    make_us(&bad, 0, 4, 4);
    TEST_ASSERT_EQ((uint64_t)RtlEqualUnicodeString(&good, &bad, 0), (uint64_t)0,
                   "NULL Buffer with nonzero Length not equal");
}

static void test_nls_compare_malformed_is_total_order(void)
{
    uint16_t bufA[3] = { 'a', 'b', 'c' };
    uint16_t bufB[2] = { 'x', 'y' };
    UNICODE_STRING badA, badB;
    int ab, ba;
    /* Both operands malformed (odd Length). A sort comparator must stay
     * antisymmetric and reflexive even here -- safe-clamp compares content, it
     * does not return a fixed non-equal sentinel. */
    make_us(&badA, bufA, 5, 6);   /* odd Length 5 -> safe 2 wchars "ab" */
    make_us(&badB, bufB, 3, 4);   /* odd Length 3 -> safe 1 wchar "x" */
    ab = RtlCompareUnicodeString(&badA, &badB, 0);
    ba = RtlCompareUnicodeString(&badB, &badA, 0);
    TEST_ASSERT(ab != 0, "distinct malformed operands are ordered");
    TEST_ASSERT((ab < 0 && ba > 0) || (ab > 0 && ba < 0), "malformed compare is antisymmetric");
    TEST_ASSERT_EQ((uint64_t)RtlCompareUnicodeString(&badA, &badA, 0), (uint64_t)0,
                   "malformed self-compare is reflexive (== 0)");
}

static void test_nls_upcase_rejects_malformed(void)
{
    uint16_t src[2] = { 'a', 'b' };
    uint16_t dst[2] = { 0, 0 };
    UNICODE_STRING us, ud;
    make_us(&ud, dst, 0, 4);
    /* src Length > src MaximumLength is malformed and must be rejected. */
    make_us(&us, src, 8, 4);
    TEST_ASSERT_EQ((uint64_t)RtlUpcaseUnicodeString(&ud, &us, 0),
                   (uint64_t)STATUS_INVALID_PARAMETER, "src Length > MaximumLength rejected");
    /* Odd src Length is malformed. */
    make_us(&us, src, 3, 4);
    TEST_ASSERT_EQ((uint64_t)RtlUpcaseUnicodeString(&ud, &us, 0),
                   (uint64_t)STATUS_INVALID_PARAMETER, "odd src Length rejected");
}

/* ==========================================================================
 * Section 4: nls_table_v1 format + loader + fallback (pure helper tests).
 * All tests build synthetic in-memory blobs and drive the pure parser /
 * accessors -- no live boot infrastructure, no VFS.
 * ========================================================================== */

static uint8_t s_nls_blob[2048];

/* Build a table with one UPCASE chunk of `count` u16 entries (entry[i]=0x8000+i).
 * Leaves crc32 unset (0); caller finalizes with nls_recrc after any tampering.
 * Returns the total blob length. */
static uint32_t nls_build_upcase(uint32_t count)
{
    nls_table_header_t *h = (nls_table_header_t *)s_nls_blob;
    nls_chunk_desc_t *d = (nls_chunk_desc_t *)(s_nls_blob + sizeof(*h));
    uint32_t body = (uint32_t)(sizeof(*h) + sizeof(*d));
    uint32_t size = count * 2u;
    uint16_t *up = (uint16_t *)(s_nls_blob + body);
    uint32_t total = body + size;
    uint32_t i;

    memset(s_nls_blob, 0, sizeof(s_nls_blob));
    h->magic = NLS_TABLE_V1_MAGIC;
    h->version = NLS_TABLE_V1_VERSION;
    h->lcid = 0x007Fu;
    h->code_page = 0;
    h->nls_version = 7;
    h->total_size = total;
    h->crc32 = 0;
    h->chunk_count = 1;
    d->type = NLS_CHUNK_UPCASE;
    d->offset = body;
    d->size = size;
    for (i = 0; i < count; i++)
        up[i] = (uint16_t)(0x8000u + i);
    return total;
}

/* Compute + store the CRC over the whole blob with crc32 already zeroed. */
static void nls_recrc(uint32_t total)
{
    nls_table_header_t *h = (nls_table_header_t *)s_nls_blob;
    h->crc32 = 0;
    h->crc32 = kcrc32(s_nls_blob, total);
}

static void test_nls_parse_valid_upcase(void)
{
    nls_published_t d;
    uint32_t total = nls_build_upcase(0x102);   /* covers 0..0x101 */
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_SUCCESS, "valid table parses");
    TEST_ASSERT_EQ((uint64_t)d.upcase_count, 0x102u, "upcase element count resolved");
    TEST_ASSERT_EQ((uint64_t)(d.upcase != 0), 1u, "upcase chunk pointer resolved");
    TEST_ASSERT_EQ((uint64_t)d.nls_version, 7u, "nls_version carried");
}

static void test_nls_parse_rejects_bad_magic(void)
{
    nls_published_t d;
    uint32_t total = nls_build_upcase(0x102);
    ((nls_table_header_t *)s_nls_blob)->magic = 0xDEADBEEFu;
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "bad magic rejected");
}

static void test_nls_parse_rejects_bad_crc(void)
{
    nls_published_t d;
    uint32_t total = nls_build_upcase(0x102);
    nls_recrc(total);
    s_nls_blob[total - 1] ^= 0xFF;   /* flip a body byte AFTER crc finalized */
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "corrupt body fails CRC");
}

static void test_nls_parse_rejects_size_mismatch(void)
{
    nls_published_t d;
    uint32_t total = nls_build_upcase(0x102);
    nls_recrc(total);
    /* total_size in header != bytes handed to parser. */
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total - 2, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "len != total_size rejected");
}

static void test_nls_parse_rejects_oob_chunk(void)
{
    nls_published_t d;
    uint32_t total = nls_build_upcase(0x102);
    nls_chunk_desc_t *cd = (nls_chunk_desc_t *)(s_nls_blob + sizeof(nls_table_header_t));
    cd->size += 4;   /* chunk now runs past total_size */
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "out-of-bounds chunk rejected");
}

static void test_nls_parse_rejects_dup_chunk(void)
{
    nls_published_t d;
    nls_table_header_t *h = (nls_table_header_t *)s_nls_blob;
    nls_chunk_desc_t *d0, *d1;
    uint32_t body, size, total;

    memset(s_nls_blob, 0, sizeof(s_nls_blob));
    h->magic = NLS_TABLE_V1_MAGIC;
    h->version = NLS_TABLE_V1_VERSION;
    h->lcid = 0x007Fu;
    h->nls_version = 1;
    h->chunk_count = 2;
    body = (uint32_t)(sizeof(*h) + 2u * sizeof(nls_chunk_desc_t));
    size = 8;   /* two tiny same-type chunks */
    d0 = (nls_chunk_desc_t *)(s_nls_blob + sizeof(*h));
    d1 = d0 + 1;
    d0->type = NLS_CHUNK_UPCASE; d0->offset = body;        d0->size = size;
    d1->type = NLS_CHUNK_UPCASE; d1->offset = body + size; d1->size = size;
    total = body + 2u * size;
    h->total_size = total;
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "duplicate chunk type rejected");
}

static void test_nls_upcase_table_path_and_guard(void)
{
    nls_published_t d;
    uint32_t total = nls_build_upcase(0x102);   /* indices 0..0x101 present */
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_SUCCESS, "table parses for accessor test");
    nls_test_set_active(&d);
    /* < U+0100 stays on the compiled rtl authority, never the table. */
    TEST_ASSERT_EQ((uint64_t)nls_upcase_char('a'), (uint64_t)'A',
                   "ASCII fold uses compiled authority");
    /* U+0100 is in range -> table value 0x8000 + 0x100. */
    TEST_ASSERT_EQ((uint64_t)nls_upcase_char(0x0100), 0x8100u,
                   "in-range code point reads UPCASE chunk");
    /* U+0102 is past upcase_count (0x102) -> guarded, returns unchanged. */
    TEST_ASSERT_EQ((uint64_t)nls_upcase_char(0x0102), 0x0102u,
                   "out-of-range code point falls back to unchanged");
    nls_test_set_active(0);   /* restore compiled fallback */
}

static void test_nls_ctype1_compiled_fallback(void)
{
    nls_test_set_active(0);   /* ensure no table */
    TEST_ASSERT_EQ((uint64_t)(nls_char_type('A', 1) & (NLS_C1_UPPER | NLS_C1_ALPHA)),
                   (uint64_t)(NLS_C1_UPPER | NLS_C1_ALPHA), "A is upper alpha");
    TEST_ASSERT_EQ((uint64_t)(nls_char_type('5', 1) & (NLS_C1_DIGIT | NLS_C1_XDIGIT)),
                   (uint64_t)(NLS_C1_DIGIT | NLS_C1_XDIGIT), "5 is digit + xdigit");
    TEST_ASSERT_EQ((uint64_t)(nls_char_type(' ', 1) & NLS_C1_SPACE),
                   (uint64_t)NLS_C1_SPACE, "space is C1_SPACE");
    TEST_ASSERT_EQ((uint64_t)(nls_char_type('!', 1) & NLS_C1_PUNCT),
                   (uint64_t)NLS_C1_PUNCT, "! is punctuation");
    /* No compiled CTYPE2/CTYPE3 fallback. */
    TEST_ASSERT_EQ((uint64_t)nls_char_type('A', 2), 0u, "no CTYPE2 fallback");
}

static void test_nls_missing_dir_fallback(void)
{
    /* Pure fallback-selection check: with no active table published, upcase of a
     * BMP code point >= U+0100 passes through unchanged and the version reports
     * the compiled default. This is the state nls_init() leaves on a missing
     * C:\Impossible\System\NLS directory. */
    nls_test_set_active(0);
    TEST_ASSERT_EQ((uint64_t)nls_upcase_char(0x0100), 0x0100u,
                   "no table: >= U+0100 upcase unchanged");
    TEST_ASSERT_EQ((uint64_t)nls_get_version(), (uint64_t)NLS_TABLE_V1_VERSION,
                   "no table: compiled version default");
}

/* NOTE: the `total_size > NLS_TABLE_V1_MAX_SIZE` parse branch is not unit-tested
 * here -- it needs a >2 MiB blob (impractical for a static test buffer) and is
 * an OR with the `total_size != len` branch (tested above). nls_init also caps
 * total_size before the PMM allocation; smoke covers the boot path. */

static void test_nls_parse_rejects_null(void)
{
    nls_published_t d;
    uint32_t total = nls_build_upcase(0x102);
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(0, total, &d),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL blob rejected");
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, 0),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL out rejected");
}

static void test_nls_parse_rejects_short_header(void)
{
    nls_published_t d;
    nls_build_upcase(0x102);
    /* len below the 32-byte header size. */
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, 16, &d),
                   (uint64_t)STATUS_INVALID_PARAMETER, "len < header rejected");
}

static void test_nls_parse_rejects_bad_version(void)
{
    nls_published_t d;
    uint32_t total = nls_build_upcase(0x102);
    ((nls_table_header_t *)s_nls_blob)->version = 999;
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "bad version rejected");
}

static void test_nls_parse_rejects_chunk_count_over_max(void)
{
    nls_published_t d;
    uint32_t total = nls_build_upcase(0x102);
    ((nls_table_header_t *)s_nls_blob)->chunk_count = NLS_TABLE_V1_MAX_CHUNKS + 1;
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "chunk_count > max rejected");
}

static void test_nls_parse_rejects_dir_overrun(void)
{
    nls_published_t d;
    nls_table_header_t *h = (nls_table_header_t *)s_nls_blob;
    /* Header claims 5 chunks (dir = 60 bytes -> body_start 92) but total_size 40. */
    memset(s_nls_blob, 0, sizeof(s_nls_blob));
    h->magic = NLS_TABLE_V1_MAGIC;
    h->version = NLS_TABLE_V1_VERSION;
    h->lcid = 0x007Fu;
    h->chunk_count = 5;
    h->total_size = 40;
    nls_recrc(40);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, 40, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "directory past blob rejected");
}

static void test_nls_parse_rejects_bad_chunk_type(void)
{
    nls_published_t d;
    nls_chunk_desc_t *cd = (nls_chunk_desc_t *)(s_nls_blob + sizeof(nls_table_header_t));
    uint32_t total;

    total = nls_build_upcase(0x102);
    cd->type = 0;   /* type 0 invalid */
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "chunk type 0 rejected");

    total = nls_build_upcase(0x102);
    cd->type = NLS_CHUNK_TYPE_MAX + 1;   /* type past max invalid */
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "chunk type > max rejected");
}

static void test_nls_parse_rejects_chunk_in_directory(void)
{
    nls_published_t d;
    nls_chunk_desc_t *cd = (nls_chunk_desc_t *)(s_nls_blob + sizeof(nls_table_header_t));
    uint32_t total = nls_build_upcase(0x102);
    cd->offset = 40;   /* < body_start (44) -- chunk would overlap the directory */
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "chunk inside directory rejected");
}

static void test_nls_parse_rejects_odd_offset_and_size(void)
{
    nls_published_t d;
    nls_chunk_desc_t *cd = (nls_chunk_desc_t *)(s_nls_blob + sizeof(nls_table_header_t));
    uint32_t total;

    total = nls_build_upcase(0x102);
    cd->offset = 45;   /* odd offset on a uint16 chunk */
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "odd uint16 offset rejected");

    total = nls_build_upcase(0x102);
    cd->size = cd->size - 1;   /* odd size on a uint16 chunk */
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "odd uint16 size rejected");
}

static void test_nls_parse_fold_only_success(void)
{
    nls_published_t d;
    nls_table_header_t *h = (nls_table_header_t *)s_nls_blob;
    nls_chunk_desc_t *cd = (nls_chunk_desc_t *)(s_nls_blob + sizeof(*h));
    uint32_t body = (uint32_t)(sizeof(*h) + sizeof(*cd));
    uint32_t total = body + 8;   /* a reserved FOLD_COMPAT chunk, 8 opaque bytes */

    memset(s_nls_blob, 0, sizeof(s_nls_blob));
    h->magic = NLS_TABLE_V1_MAGIC;
    h->version = NLS_TABLE_V1_VERSION;
    h->lcid = 0x007Fu;
    h->nls_version = 3;
    h->chunk_count = 1;
    h->total_size = total;
    cd->type = NLS_CHUNK_FOLD_COMPAT;
    cd->offset = body;
    cd->size = 8;
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_SUCCESS, "reserved FOLD-only table parses");
    TEST_ASSERT_EQ((uint64_t)(d.upcase == 0 && d.ctype1 == 0 && d.ctype2 == 0 &&
                              d.ctype3 == 0), 1u, "no consumed chunk resolved");
    TEST_ASSERT_EQ((uint64_t)(d.upcase_count | d.ctype1_count |
                              d.ctype2_count | d.ctype3_count), 0u, "all counts zero");
    TEST_ASSERT_EQ((uint64_t)d.lcid, 0x007Fu, "lcid carried from header");
}

static void test_nls_parse_rejects_odd_fold_chunk(void)
{
    /* Even-alignment is enforced for EVERY defined chunk type, including the
     * reserved FOLD_* chunks that fall through the parser's resolve switch. */
    nls_published_t d;
    nls_table_header_t *h = (nls_table_header_t *)s_nls_blob;
    nls_chunk_desc_t *cd = (nls_chunk_desc_t *)(s_nls_blob + sizeof(*h));
    uint32_t body = (uint32_t)(sizeof(*h) + sizeof(*cd));
    uint32_t total = body + 7;   /* odd-size FOLD chunk */

    memset(s_nls_blob, 0, sizeof(s_nls_blob));
    h->magic = NLS_TABLE_V1_MAGIC;
    h->version = NLS_TABLE_V1_VERSION;
    h->lcid = 0x007Fu;
    h->chunk_count = 1;
    h->total_size = total;
    cd->type = NLS_CHUNK_FOLD_WIDTH;
    cd->offset = body;
    cd->size = 7;   /* odd */
    nls_recrc(total);
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_INVALID_IMAGE_FORMAT, "odd-size FOLD chunk rejected");
}

/* Build a table with CTYPE1/2/3 chunks each of `count` u16 entries; entry[i]=0.
 * entry[0x100] is set to a distinct sentinel per namespace. nls_version = 5. */
static uint32_t nls_build_ctype123(void)
{
    nls_table_header_t *h = (nls_table_header_t *)s_nls_blob;
    nls_chunk_desc_t *dd = (nls_chunk_desc_t *)(s_nls_blob + sizeof(*h));
    uint32_t count = 0x102;
    uint32_t size = count * 2u;
    uint32_t body = (uint32_t)(sizeof(*h) + 3u * sizeof(*dd));
    uint16_t *c1 = (uint16_t *)(s_nls_blob + body);
    uint16_t *c2 = (uint16_t *)(s_nls_blob + body + size);
    uint16_t *c3 = (uint16_t *)(s_nls_blob + body + 2u * size);
    uint32_t total = body + 3u * size;

    memset(s_nls_blob, 0, sizeof(s_nls_blob));
    h->magic = NLS_TABLE_V1_MAGIC;
    h->version = NLS_TABLE_V1_VERSION;
    h->lcid = 0x007Fu;
    h->nls_version = 5;
    h->chunk_count = 3;
    h->total_size = total;
    dd[0].type = NLS_CHUNK_CTYPE1; dd[0].offset = body;              dd[0].size = size;
    dd[1].type = NLS_CHUNK_CTYPE2; dd[1].offset = body + size;       dd[1].size = size;
    dd[2].type = NLS_CHUNK_CTYPE3; dd[2].offset = body + 2u * size;  dd[2].size = size;
    c1[0x100] = 0x1111;
    c2[0x100] = 0x2222;
    c3[0x100] = 0x3333;
    nls_recrc(total);
    return total;
}

static void test_nls_ctype_table_paths(void)
{
    nls_published_t d;
    uint32_t total = nls_build_ctype123();
    TEST_ASSERT_EQ((uint64_t)nls_table_parse(s_nls_blob, total, &d),
                   (uint64_t)STATUS_SUCCESS, "ctype table parses");
    nls_test_set_active(&d);
    TEST_ASSERT_EQ((uint64_t)nls_char_type(0x0100, 1), 0x1111u, "CTYPE1 table value");
    TEST_ASSERT_EQ((uint64_t)nls_char_type(0x0100, 2), 0x2222u, "CTYPE2 table value");
    TEST_ASSERT_EQ((uint64_t)nls_char_type(0x0100, 3), 0x3333u, "CTYPE3 table value");
    /* Out-of-range code point: CTYPE2/3 have no fallback -> 0. */
    TEST_ASSERT_EQ((uint64_t)nls_char_type(0x0102, 2), 0u, "CTYPE2 out-of-range 0");
    TEST_ASSERT_EQ((uint64_t)nls_char_type(0x0102, 3), 0u, "CTYPE3 out-of-range 0");
    /* Invalid namespace selectors. */
    TEST_ASSERT_EQ((uint64_t)nls_char_type('A', 0), 0u, "which=0 -> 0");
    TEST_ASSERT_EQ((uint64_t)nls_char_type('A', 4), 0u, "which=4 -> 0");
    /* Active descriptor reports its own nls_version. */
    TEST_ASSERT_EQ((uint64_t)nls_get_version(), 5u, "active version reported");
    nls_test_set_active(0);
}

/* ==========================================================================
 * Section 5: code page conversion providers (pure helper tests).
 * ========================================================================== */

static void test_nls_cp_utf8_3byte_roundtrip(void)
{
    /* U+20AC EURO = UTF-8 E2 82 AC = UTF-16 0x20AC. */
    const uint8_t u8[3] = { 0xE2, 0x82, 0xAC };
    uint16_t u16[4];
    uint8_t back[8];
    int n = nls_cp_utf8_to_utf16(u8, 3, u16, 4, NLS_CP_STRICT);
    TEST_ASSERT_EQ((uint64_t)n, 1u, "3-byte UTF-8 decodes to 1 code unit");
    TEST_ASSERT_EQ((uint64_t)u16[0], 0x20ACu, "decodes to U+20AC");
    n = nls_cp_utf16_to_utf8(u16, 1, back, 8, NLS_CP_STRICT);
    TEST_ASSERT_EQ((uint64_t)n, 3u, "re-encodes to 3 bytes");
    TEST_ASSERT_EQ((uint64_t)(back[0] == 0xE2 && back[1] == 0x82 && back[2] == 0xAC), 1u,
                   "round-trips to original bytes");
}

static void test_nls_cp_utf8_surrogate_roundtrip(void)
{
    /* U+1F600 = UTF-8 F0 9F 98 80 = UTF-16 D83D DE00 (surrogate pair). */
    const uint8_t u8[4] = { 0xF0, 0x9F, 0x98, 0x80 };
    uint16_t u16[4];
    uint8_t back[8];
    int n = nls_cp_utf8_to_utf16(u8, 4, u16, 4, NLS_CP_STRICT);
    TEST_ASSERT_EQ((uint64_t)n, 2u, "4-byte UTF-8 decodes to a surrogate pair");
    TEST_ASSERT_EQ((uint64_t)u16[0], 0xD83Du, "high surrogate");
    TEST_ASSERT_EQ((uint64_t)u16[1], 0xDE00u, "low surrogate");
    n = nls_cp_utf16_to_utf8(u16, 2, back, 8, NLS_CP_STRICT);
    TEST_ASSERT_EQ((uint64_t)n, 4u, "surrogate pair re-encodes to 4 bytes");
    TEST_ASSERT_EQ((uint64_t)(back[0] == 0xF0 && back[3] == 0x80), 1u, "astral round-trips");
}

static void test_nls_cp_utf8_invalid_lead(void)
{
    const uint8_t bad[1] = { 0xFF };
    uint16_t u16[4];
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_utf8_to_utf16(bad, 1, u16, 4, NLS_CP_STRICT),
                   (uint64_t)(int64_t)NLS_CP_ERR_INVALID, "invalid lead: strict fails");
    int n = nls_cp_utf8_to_utf16(bad, 1, u16, 4, NLS_CP_REPLACE);
    TEST_ASSERT_EQ((uint64_t)n, 1u, "invalid lead: replace emits one unit");
    TEST_ASSERT_EQ((uint64_t)u16[0], 0xFFFDu, "invalid lead: replace emits U+FFFD");
}

static void test_nls_cp_utf8_overlong_and_surrogate(void)
{
    /* Overlong '/' (C0 AF) and a UTF-8-encoded surrogate (ED A0 80 = U+D800). */
    const uint8_t overlong[2] = { 0xC0, 0xAF };
    const uint8_t surro[3] = { 0xED, 0xA0, 0x80 };
    uint16_t u16[4];
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_utf8_to_utf16(overlong, 2, u16, 4, NLS_CP_STRICT),
                   (uint64_t)(int64_t)NLS_CP_ERR_INVALID, "overlong rejected");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_utf8_to_utf16(surro, 3, u16, 4, NLS_CP_STRICT),
                   (uint64_t)(int64_t)NLS_CP_ERR_INVALID, "UTF-8 surrogate rejected");
}

static void test_nls_cp_utf8_truncated(void)
{
    /* A 3-byte lead (E2 82 ..) with only 2 bytes available: the subtraction-form
     * length guard rejects it (strict) / replaces it (replace) without overread. */
    const uint8_t trunc[2] = { 0xE2, 0x82 };
    uint16_t u16[4];
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_utf8_to_utf16(trunc, 2, u16, 4, NLS_CP_STRICT),
                   (uint64_t)(int64_t)NLS_CP_ERR_INVALID, "truncated 3-byte: strict fails");
    int n = nls_cp_utf8_to_utf16(trunc, 2, u16, 4, NLS_CP_REPLACE);
    TEST_ASSERT_EQ((uint64_t)u16[0], 0xFFFDu, "truncated 3-byte: replace -> U+FFFD");
    (void)n;
}

static void test_nls_cp_invalid_mode_fails_closed(void)
{
    /* An out-of-range mode must fail closed (ERR_PARAM), never silently downgrade
     * invalid/unmapped input to replacement. */
    const uint8_t bad[1] = { 0xFF };
    const uint16_t pair[1] = { 0xD800 };
    uint16_t u16[4];
    uint8_t u8[4];
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_utf8_to_utf16(bad, 1, u16, 4, 99),
                   (uint64_t)(int64_t)NLS_CP_ERR_PARAM, "utf8 decode invalid mode -> ERR_PARAM");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_from_utf16(NLS_CP_1252, pair, 1, u8, 4, -1),
                   (uint64_t)(int64_t)NLS_CP_ERR_PARAM, "sbcs encode invalid mode -> ERR_PARAM");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_to_utf16(NLS_CP_1252, bad, 1, u16, 4, 7),
                   (uint64_t)(int64_t)NLS_CP_ERR_PARAM, "sbcs decode invalid mode -> ERR_PARAM");
}

static void test_nls_cp1252_euro(void)
{
    const uint8_t b[1] = { 0x80 };
    uint16_t u16[4];
    int n = nls_cp_to_utf16(NLS_CP_1252, b, 1, u16, 4, NLS_CP_STRICT);
    TEST_ASSERT_EQ((uint64_t)n, 1u, "CP1252 byte decodes");
    TEST_ASSERT_EQ((uint64_t)u16[0], 0x20ACu, "CP1252 0x80 -> U+20AC");
}

static void test_nls_cp1252_undefined_byte(void)
{
    const uint8_t b[1] = { 0x81 };   /* undefined in CP1252 */
    uint16_t u16[4];
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_to_utf16(NLS_CP_1252, b, 1, u16, 4, NLS_CP_STRICT),
                   (uint64_t)(int64_t)NLS_CP_ERR_INVALID, "undefined byte: strict fails");
    int n = nls_cp_to_utf16(NLS_CP_1252, b, 1, u16, 4, NLS_CP_REPLACE);
    TEST_ASSERT_EQ((uint64_t)n, 1u, "undefined byte: replace emits a unit");
    TEST_ASSERT_EQ((uint64_t)u16[0], 0xFFFDu, "undefined byte: replace -> U+FFFD");
}

static void test_nls_cp_unknown_returns_null(void)
{
    TEST_ASSERT_EQ((uint64_t)(nls_cp_get_provider(99999u) == 0), 1u,
                   "unknown code page -> NULL provider");
    TEST_ASSERT_EQ((uint64_t)nls_cp_is_valid(99999u), 0u, "unknown code page invalid");
    TEST_ASSERT_EQ((uint64_t)nls_cp_is_valid(NLS_CP_1252), 1u, "CP1252 valid");
}

static void test_nls_cp_encode_lone_surrogate(void)
{
    const uint16_t lone_high[1] = { 0xD800 };
    uint8_t u8[8];
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_utf16_to_utf8(lone_high, 1, u8, 8, NLS_CP_STRICT),
                   (uint64_t)(int64_t)NLS_CP_ERR_INVALID, "lone surrogate to UTF-8: strict fails");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_from_utf16(NLS_CP_1252, lone_high, 1, u8, 8, NLS_CP_STRICT),
                   (uint64_t)(int64_t)NLS_CP_ERR_INVALID, "lone surrogate to SBCS: strict fails");
    int n = nls_cp_from_utf16(NLS_CP_1252, lone_high, 1, u8, 8, NLS_CP_REPLACE);
    TEST_ASSERT_EQ((uint64_t)n, 1u, "lone surrogate to SBCS: replace emits one default byte");
    TEST_ASSERT_EQ((uint64_t)u8[0], (uint64_t)NLS_CP_DEFAULT_BYTE, "replace uses default char");
}

static void test_nls_cp_encode_astral_to_sbcs(void)
{
    /* A valid astral pair is one scalar; SBCS cannot map it. */
    const uint16_t pair[2] = { 0xD83D, 0xDE00 };
    uint8_t u8[8];
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_from_utf16(NLS_CP_1252, pair, 2, u8, 8, NLS_CP_STRICT),
                   (uint64_t)(int64_t)NLS_CP_ERR_INVALID, "astral to SBCS: strict fails");
    int n = nls_cp_from_utf16(NLS_CP_1252, pair, 2, u8, 8, NLS_CP_REPLACE);
    TEST_ASSERT_EQ((uint64_t)n, 1u, "astral to SBCS replace: one default byte (pair = one scalar)");
    TEST_ASSERT_EQ((uint64_t)u8[0], (uint64_t)NLS_CP_DEFAULT_BYTE, "default char emitted once");
}

static void test_nls_cp_bestfit(void)
{
    /* U+2018 LEFT SINGLE QUOTE is not in CP437; best-fit -> 0x27 ('), replace -> '?'. */
    const uint16_t q[1] = { 0x2018 };
    uint8_t u8[4];
    int n = nls_cp_from_utf16(NLS_CP_437, q, 1, u8, 4, NLS_CP_BESTFIT);
    TEST_ASSERT_EQ((uint64_t)n, 1u, "best-fit emits one byte");
    TEST_ASSERT_EQ((uint64_t)u8[0], 0x27u, "best-fit U+2018 -> apostrophe");
    n = nls_cp_from_utf16(NLS_CP_437, q, 1, u8, 4, NLS_CP_REPLACE);
    TEST_ASSERT_EQ((uint64_t)u8[0], (uint64_t)NLS_CP_DEFAULT_BYTE, "replace (no best-fit) -> default");
}

static void test_nls_cp_sizing_and_too_small(void)
{
    const uint8_t u8[3] = { 0xE2, 0x82, 0xAC };
    uint16_t u16[1];
    /* Sizing pass: dst == NULL returns required count without writing. */
    TEST_ASSERT_EQ((uint64_t)nls_cp_utf8_to_utf16(u8, 3, 0, 0, NLS_CP_STRICT), 1u,
                   "sizing pass returns required unit count");
    /* Two euros need 2 units but cap is 1 -> TOO_SMALL. */
    const uint8_t two[6] = { 0xE2, 0x82, 0xAC, 0xE2, 0x82, 0xAC };
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_cp_utf8_to_utf16(two, 6, u16, 1, NLS_CP_STRICT),
                   (uint64_t)(int64_t)NLS_CP_ERR_TOO_SMALL, "over-cap output -> TOO_SMALL");
}

static void test_nls_cp_policy_and_info(void)
{
    nls_cpinfo_t info;
    uint32_t ids[8];
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_acp(), (uint64_t)NLS_CP_DEFAULT_ACP,
                   "GetACP default is 1252");
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_oemcp(), (uint64_t)NLS_CP_DEFAULT_OEMCP,
                   "GetOEMCP default is 437");
    TEST_ASSERT_EQ((uint64_t)(nls_cp_get_provider(NLS_CP_ACP) != 0), 1u,
                   "CP_ACP resolves to a provider");
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_info(NLS_CP_1252, &info), 0u, "GetCPInfoEx ok");
    TEST_ASSERT_EQ((uint64_t)info.max_char_size, 1u, "CP1252 max_char_size 1");
    TEST_ASSERT_EQ((uint64_t)info.default_char, (uint64_t)NLS_CP_DEFAULT_BYTE, "default char '?'");
    TEST_ASSERT_EQ((uint64_t)nls_cp_is_dbcs_lead_byte(NLS_CP_1252, 0x81), 0u, "SBCS has no lead byte");
    TEST_ASSERT_EQ((uint64_t)nls_cp_enum(ids, 8), 4u, "four code pages enumerated");
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_info(NLS_CP_UTF8, &info), 0u, "UTF-8 info ok");
    TEST_ASSERT_EQ((uint64_t)info.max_char_size, (uint64_t)NLS_CP_UTF8_MAX_CHAR, "UTF-8 max 4");
}

static void test_nls_cp_register_preserves_policy(void)
{
    /* nls_cp_register_defaults must NOT clobber an existing supported ACP. Set a
     * non-default supported value (437), re-seed, verify preservation, then
     * restore so later tests see the boot default again. */
    HKEY k;
    uint32_t saved = nls_cp_get_acp();
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Nls", 0, KEY_ALL_ACCESS, &k) == 0) {
        RegSetDword(k, "ACP", 437);
        RegCloseKey(k);
    }
    nls_cp_register_defaults();
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_acp(), 437u,
                   "register_defaults preserves an existing supported ACP");
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Nls", 0, KEY_ALL_ACCESS, &k) == 0) {
        RegSetDword(k, "ACP", saved);
        RegCloseKey(k);
    }
    nls_cp_register_defaults();
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_acp(), (uint64_t)saved, "ACP restored for later tests");
}

static void test_nls_cp_register_rejects_malformed_acp(void)
{
    /* A short (2-byte) REG_DWORD ACP is malformed; register_defaults must reseed
     * the compiled default rather than snapshot a partially-initialized value. */
    HKEY k;
    uint32_t saved = nls_cp_get_acp();
    uint16_t shortval = 0x1234;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Nls", 0, KEY_ALL_ACCESS, &k) == 0) {
        RegSetValueEx(k, "ACP", 0, REG_DWORD, (const uint8_t *)&shortval, 2);
        RegCloseKey(k);
    }
    nls_cp_register_defaults();
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_acp(), (uint64_t)NLS_CP_DEFAULT_ACP,
                   "malformed short ACP -> compiled default");
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Nls", 0, KEY_ALL_ACCESS, &k) == 0) {
        RegSetDword(k, "ACP", saved);
        RegCloseKey(k);
    }
    nls_cp_register_defaults();
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_acp(), (uint64_t)saved, "ACP restored for later tests");
}

static void test_nls_cp_register_rejects_unsupported_acp(void)
{
    /* A well-formed but unsupported ACP (99999) must fall back to the compiled
     * default in the cache, never poisoning CP_ACP conversions into BADCP. */
    HKEY k;
    uint32_t saved = nls_cp_get_acp();
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Nls", 0, KEY_ALL_ACCESS, &k) == 0) {
        RegSetDword(k, "ACP", 99999);
        RegCloseKey(k);
    }
    nls_cp_register_defaults();
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_acp(), (uint64_t)NLS_CP_DEFAULT_ACP,
                   "unsupported ACP -> compiled default in cache");
    TEST_ASSERT_EQ((uint64_t)(nls_cp_get_provider(NLS_CP_ACP) != 0), 1u,
                   "CP_ACP still resolves to a provider");
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Nls", 0, KEY_ALL_ACCESS, &k) == 0) {
        RegSetDword(k, "ACP", saved);
        RegCloseKey(k);
    }
    nls_cp_register_defaults();
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_acp(), (uint64_t)saved, "ACP restored for later tests");
}

static void test_nls_cp_get_info_zeroes_output(void)
{
    /* nls_cp_get_info must fully initialize the ABI struct -- the name tail after
     * the NUL and padding must be zero even if the caller passed dirty memory. */
    nls_cpinfo_t info;
    memset(&info, 0xFF, sizeof(info));
    TEST_ASSERT_EQ((uint64_t)nls_cp_get_info(NLS_CP_1252, &info), 0u, "get_info ok");
    /* "windows-1252" is 12 chars; name[12] is the NUL, name[13..] must be zeroed. */
    TEST_ASSERT_EQ((uint64_t)info.name[12], 0u, "name NUL-terminated");
    TEST_ASSERT_EQ((uint64_t)info.name[sizeof(info.name) - 1], 0u, "name tail zeroed");
    TEST_ASSERT_EQ((uint64_t)info.lead_bytes[0], 0u, "lead_bytes zeroed");
}

/* ==========================================================================
 * Section 6: locale / LCID metadata (pure helper tests).
 * ========================================================================== */

static void test_nls_locale_lookup(void)
{
    const nls_locale_t *l = nls_locale_by_lcid(0x0409);
    TEST_ASSERT_EQ((uint64_t)(l != 0), 1u, "en-US LCID resolves");
    TEST_ASSERT_EQ((uint64_t)(strcmp(l->bcp47, "en-US") == 0), 1u, "en-US BCP-47 name");
    /* Round-trip name -> record -> LCID. */
    TEST_ASSERT_EQ((uint64_t)nls_locale_by_bcp47("en-US")->lcid, 0x0409u, "BCP-47 round-trips");
    TEST_ASSERT_EQ((uint64_t)(nls_locale_by_lcid(NLS_LCID_INVARIANT) != 0), 1u,
                   "invariant always resolvable");
    /* The invariant locale's Windows name is "" -- it must round-trip by name. */
    TEST_ASSERT_EQ((uint64_t)(nls_locale_by_bcp47("") != 0), 1u, "invariant name resolves");
    TEST_ASSERT_EQ((uint64_t)nls_locale_by_bcp47("")->lcid, (uint64_t)NLS_LCID_INVARIANT,
                   "invariant \"\" -> 0x007F");
    TEST_ASSERT_EQ((uint64_t)(nls_locale_by_lcid(0xDEADu) == 0), 1u, "unknown LCID -> NULL");
    TEST_ASSERT_EQ((uint64_t)(nls_locale_by_bcp47("xx-XX") == 0), 1u, "unknown name -> NULL");
    TEST_ASSERT_EQ((uint64_t)(nls_locale_by_bcp47(0) == 0), 1u, "NULL name -> NULL");
}

static void test_nls_locale_ui_fallback(void)
{
    uint16_t chain[8];
    /* es-MX (0x080A) -> [0x080A, 0x000A neutral, 0x0409 en-US]. */
    uint32_t n = nls_locale_ui_fallback(0x080A, chain, 8);
    TEST_ASSERT_EQ((uint64_t)n, 3u, "es-MX fallback has 3 entries");
    TEST_ASSERT_EQ((uint64_t)chain[0], 0x080Au, "specific LANGID first");
    TEST_ASSERT_EQ((uint64_t)chain[1], 0x000Au, "neutral (primary) second");
    TEST_ASSERT_EQ((uint64_t)chain[2], 0x0409u, "en-US backstop last");
    /* en-US (0x0409, primary 0x0009): specific -> neutral English; the en-US
     * backstop de-dups with entry[0], so the chain is [0x0409, 0x0009]. */
    n = nls_locale_ui_fallback(0x0409, chain, 8);
    TEST_ASSERT_EQ((uint64_t)n, 2u, "en-US fallback: specific + neutral");
    TEST_ASSERT_EQ((uint64_t)chain[0], 0x0409u, "en-US specific first");
    TEST_ASSERT_EQ((uint64_t)chain[1], 0x0009u, "neutral English second (backstop deduped)");
    /* Small caps: full count returned, only cap entries written, no over-read. */
    chain[1] = 0xEEEE;
    n = nls_locale_ui_fallback(0x080A, chain, 1);
    TEST_ASSERT_EQ((uint64_t)n, 3u, "cap=1 still returns full count");
    TEST_ASSERT_EQ((uint64_t)chain[0], 0x080Au, "cap=1 writes only entry 0");
    TEST_ASSERT_EQ((uint64_t)chain[1], 0xEEEEu, "cap=1 leaves entry 1 untouched");
    n = nls_locale_ui_fallback(0x080A, chain, 0);
    TEST_ASSERT_EQ((uint64_t)n, 3u, "cap=0 returns full count, writes nothing");
}

static void test_nls_locale_policy(void)
{
    /* register_defaults ran at boot; the compiled default is en-US for all three. */
    TEST_ASSERT_EQ((uint64_t)nls_locale_get_system(), 0x0409u, "system locale en-US");
    TEST_ASSERT_EQ((uint64_t)nls_locale_get_user(), 0x0409u, "user locale en-US");
    TEST_ASSERT_EQ((uint64_t)nls_locale_get_ui_language(), 0x0409u, "UI language en-US");
}

static void test_nls_locale_codepages_valid(void)
{
    /* Every locale record's ANSI/OEM code page must name a real provider so a
     * future CP_THREAD_ACP can derive a thread ACP from a locale. */
    const nls_locale_t *l = nls_locale_by_lcid(0x0407);   /* de-DE */
    TEST_ASSERT_EQ((uint64_t)(l != 0), 1u, "de-DE resolves");
    TEST_ASSERT_EQ((uint64_t)(nls_cp_get_provider(l->ansi_code_page) != 0), 1u,
                   "de-DE ANSI code page has a provider");
    TEST_ASSERT_EQ((uint64_t)(nls_cp_get_provider(l->oem_code_page) != 0), 1u,
                   "de-DE OEM code page has a provider");
    l = nls_locale_by_lcid(0x0409);                        /* en-US */
    TEST_ASSERT_EQ((uint64_t)l->ansi_code_page, 1252u, "en-US ANSI is 1252");
    TEST_ASSERT_EQ((uint64_t)l->oem_code_page, 437u, "en-US OEM is 437");
}

void test_register_nls(void)
{
    test_suite_register_cat("nls: validate rejects odd length",
                            test_nls_validate_rejects_odd_length, TEST_CAT_NLS);
    test_suite_register_cat("nls: validate rejects len > max",
                            test_nls_validate_rejects_len_gt_max, TEST_CAT_NLS);
    test_suite_register_cat("nls: validate rejects null buffer",
                            test_nls_validate_rejects_null_buffer, TEST_CAT_NLS);
    test_suite_register_cat("nls: validate accepts valid",
                            test_nls_validate_accepts_valid, TEST_CAT_NLS);
    test_suite_register_cat("nls: decode success + NUL term",
                            test_nls_decode_success, TEST_CAT_NLS);
    test_suite_register_cat("nls: decode buffer too small",
                            test_nls_decode_buffer_too_small, TEST_CAT_NLS);
    test_suite_register_cat("nls: decode empty string",
                            test_nls_decode_empty, TEST_CAT_NLS);
    test_suite_register_cat("nls: wchars_to_bytes overflow",
                            test_nls_wchars_to_bytes_overflow, TEST_CAT_NLS);
    test_suite_register_cat("nls: encode success",
                            test_nls_encode_success, TEST_CAT_NLS);
    test_suite_register_cat("nls: encode rejects over-capacity",
                            test_nls_encode_rejects_over_capacity, TEST_CAT_NLS);
    test_suite_register_cat("nls: ascii narrow success",
                            test_nls_ascii_narrow_success, TEST_CAT_NLS);
    test_suite_register_cat("nls: ascii narrow rejects non-ASCII",
                            test_nls_ascii_narrow_rejects_nonascii, TEST_CAT_NLS);
    test_suite_register_cat("nls: ascii narrow rejects embedded NUL",
                            test_nls_ascii_narrow_rejects_embedded_nul, TEST_CAT_NLS);
    test_suite_register_cat("nls: ascii narrow buffer too small",
                            test_nls_ascii_narrow_buffer_too_small, TEST_CAT_NLS);
    test_suite_register_cat("nls: encode clears ABI pad",
                            test_nls_encode_clears_pad, TEST_CAT_NLS);
    test_suite_register_cat("nls: user-mode probe rejects kernel ptr",
                            test_nls_user_mode_probe_rejects_kernel_ptr, TEST_CAT_NLS);
    test_suite_register_cat("nls: validate probes buffer ptr",
                            test_nls_validate_probes_buffer_kernel_ptr, TEST_CAT_NLS);
    test_suite_register_cat("nls: upcase char ASCII",
                            test_nls_upcase_char_ascii, TEST_CAT_NLS);
    test_suite_register_cat("nls: upcase char Latin-1",
                            test_nls_upcase_char_latin1, TEST_CAT_NLS);
    test_suite_register_cat("nls: upcase char above Latin-1 unchanged",
                            test_nls_upcase_char_above_latin1_unchanged, TEST_CAT_NLS);
    test_suite_register_cat("nls: upcase string success",
                            test_nls_upcase_string_success, TEST_CAT_NLS);
    test_suite_register_cat("nls: upcase string buffer too small",
                            test_nls_upcase_string_buffer_too_small, TEST_CAT_NLS);
    test_suite_register_cat("nls: upcase string rejects allocate",
                            test_nls_upcase_string_rejects_allocate, TEST_CAT_NLS);
    test_suite_register_cat("nls: equal case-insensitive",
                            test_nls_equal_case_insensitive, TEST_CAT_NLS);
    test_suite_register_cat("nls: equal different length",
                            test_nls_equal_different_length, TEST_CAT_NLS);
    test_suite_register_cat("nls: compare case-sensitive distinguishes",
                            test_nls_compare_case_sensitive_distinguishes, TEST_CAT_NLS);
    test_suite_register_cat("nls: compare prefix sorts first",
                            test_nls_compare_prefix_sorts_first, TEST_CAT_NLS);
    test_suite_register_cat("nls: ordinal compare",
                            test_nls_ordinal_compare, TEST_CAT_NLS);
    test_suite_register_cat("nls: ordinal NUL-terminated length",
                            test_nls_ordinal_nul_terminated_length, TEST_CAT_NLS);
    test_suite_register_cat("nls: ordinal rejects bad negative count",
                            test_nls_ordinal_rejects_bad_negative, TEST_CAT_NLS);
    test_suite_register_cat("nls: equal rejects malformed",
                            test_nls_equal_rejects_malformed, TEST_CAT_NLS);
    test_suite_register_cat("nls: compare malformed is total order",
                            test_nls_compare_malformed_is_total_order, TEST_CAT_NLS);
    test_suite_register_cat("nls: upcase rejects malformed",
                            test_nls_upcase_rejects_malformed, TEST_CAT_NLS);
    /* Section 4: nls_table_v1 format + loader + fallback. */
    test_suite_register_cat("nls: table parse valid upcase",
                            test_nls_parse_valid_upcase, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects bad magic",
                            test_nls_parse_rejects_bad_magic, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects bad crc",
                            test_nls_parse_rejects_bad_crc, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects size mismatch",
                            test_nls_parse_rejects_size_mismatch, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects oob chunk",
                            test_nls_parse_rejects_oob_chunk, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects dup chunk",
                            test_nls_parse_rejects_dup_chunk, TEST_CAT_NLS);
    test_suite_register_cat("nls: upcase table path + oob guard",
                            test_nls_upcase_table_path_and_guard, TEST_CAT_NLS);
    test_suite_register_cat("nls: ctype1 compiled fallback",
                            test_nls_ctype1_compiled_fallback, TEST_CAT_NLS);
    test_suite_register_cat("nls: missing dir fallback selection",
                            test_nls_missing_dir_fallback, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects null",
                            test_nls_parse_rejects_null, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects short header",
                            test_nls_parse_rejects_short_header, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects bad version",
                            test_nls_parse_rejects_bad_version, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects chunk_count over max",
                            test_nls_parse_rejects_chunk_count_over_max, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects dir overrun",
                            test_nls_parse_rejects_dir_overrun, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects bad chunk type",
                            test_nls_parse_rejects_bad_chunk_type, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects chunk in directory",
                            test_nls_parse_rejects_chunk_in_directory, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects odd offset/size",
                            test_nls_parse_rejects_odd_offset_and_size, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse fold-only success",
                            test_nls_parse_fold_only_success, TEST_CAT_NLS);
    test_suite_register_cat("nls: table parse rejects odd fold chunk",
                            test_nls_parse_rejects_odd_fold_chunk, TEST_CAT_NLS);
    test_suite_register_cat("nls: ctype table paths + invalid which",
                            test_nls_ctype_table_paths, TEST_CAT_NLS);
    /* Section 5: code page conversion providers. */
    test_suite_register_cat("nls: cp utf8 3-byte roundtrip",
                            test_nls_cp_utf8_3byte_roundtrip, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp utf8 surrogate roundtrip",
                            test_nls_cp_utf8_surrogate_roundtrip, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp utf8 invalid lead",
                            test_nls_cp_utf8_invalid_lead, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp utf8 overlong + surrogate rejected",
                            test_nls_cp_utf8_overlong_and_surrogate, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp utf8 truncated multibyte",
                            test_nls_cp_utf8_truncated, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp invalid mode fails closed",
                            test_nls_cp_invalid_mode_fails_closed, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp1252 euro",
                            test_nls_cp1252_euro, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp1252 undefined byte",
                            test_nls_cp1252_undefined_byte, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp unknown returns null",
                            test_nls_cp_unknown_returns_null, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp encode lone surrogate",
                            test_nls_cp_encode_lone_surrogate, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp encode astral to sbcs",
                            test_nls_cp_encode_astral_to_sbcs, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp best-fit substitution",
                            test_nls_cp_bestfit, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp sizing + too small",
                            test_nls_cp_sizing_and_too_small, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp policy + cpinfo",
                            test_nls_cp_policy_and_info, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp get_info zeroes output",
                            test_nls_cp_get_info_zeroes_output, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp register preserves policy",
                            test_nls_cp_register_preserves_policy, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp register rejects malformed acp",
                            test_nls_cp_register_rejects_malformed_acp, TEST_CAT_NLS);
    test_suite_register_cat("nls: cp register rejects unsupported acp",
                            test_nls_cp_register_rejects_unsupported_acp, TEST_CAT_NLS);
    /* Section 6: locale / LCID metadata. */
    test_suite_register_cat("nls: locale lookup + bcp47 round-trip",
                            test_nls_locale_lookup, TEST_CAT_NLS);
    test_suite_register_cat("nls: locale MUI fallback chain",
                            test_nls_locale_ui_fallback, TEST_CAT_NLS);
    test_suite_register_cat("nls: locale registry policy",
                            test_nls_locale_policy, TEST_CAT_NLS);
    test_suite_register_cat("nls: locale code pages valid",
                            test_nls_locale_codepages_valid, TEST_CAT_NLS);
}
