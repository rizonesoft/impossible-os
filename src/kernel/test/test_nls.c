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
#include "kernel/nt/nt_unicode.h"
#include "kernel/nt/nt_rtlstr.h"   /* rtl_upcase_char, Rtl*UnicodeString, CompareStringOrdinal */
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
}
