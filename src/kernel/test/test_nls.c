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
#include "kernel/nt/nls_sort.h"    /* S7: sort keys + normalization + FoldStringW */
#include "registry.h"              /* S5: HKLM\SYSTEM\Nls policy preservation test */
#include "kernel/kchecksum.h"      /* kcrc32 for synthetic table blobs */
#include "kernel/nt/zw.h"          /* SSDT_KERNEL_MODE */
#include "kernel/nt/ssdt.h"        /* S8: ssdt_dispatch for the NLS/MUI syscalls */
#include "kernel/nt/service_numbers.h" /* S8: SSDT_Nt* service numbers */
#include "kernel/nt/nls_syscall_info.h" /* S8: SystemNlsInformation + MUI_REGISTRY_INFO */
#include "kernel/nt/nt_misc.h"     /* S8: nt_locale_get_ui_language */

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

/* ==========================================================================
 * Section 7: sort keys + normalization + FoldStringW (pure helper tests).
 * ========================================================================== */

/* memcmp-style sort-key compare: min-length memcmp, then length tiebreak. */
static int sk_cmp(const uint8_t *a, int la, const uint8_t *b, int lb)
{
    int m = la < lb ? la : lb;
    int c = memcmp(a, b, (uint64_t)m);
    if (c)
        return c;
    return la - lb;
}

static void test_nls_sort_case_insensitive_order(void)
{
    const uint16_t apple[5]  = { 'a', 'p', 'p', 'l', 'e' };
    const uint16_t banana[6] = { 'B', 'a', 'n', 'a', 'n', 'a' };
    const uint16_t cherry[6] = { 'c', 'h', 'e', 'r', 'r', 'y' };
    uint8_t ka[64], kb[64], kc[64];
    int la = nls_sort_key(apple, 5, ka, 64, 1);
    int lb = nls_sort_key(banana, 6, kb, 64, 1);
    int lc = nls_sort_key(cherry, 6, kc, 64, 1);
    TEST_ASSERT_EQ((uint64_t)(la > 0 && lb > 0 && lc > 0), 1u, "keys built");
    TEST_ASSERT_EQ((uint64_t)(sk_cmp(ka, la, kb, lb) < 0), 1u, "apple < Banana (case-insensitive)");
    TEST_ASSERT_EQ((uint64_t)(sk_cmp(kb, lb, kc, lc) < 0), 1u, "Banana < cherry (case-insensitive)");
}

static void test_nls_sort_prefix_order(void)
{
    const uint16_t a[1]  = { 'a' };
    const uint16_t aa[2] = { 'a', 'a' };
    uint8_t ka[32], kaa[32];
    int la = nls_sort_key(a, 1, ka, 32, 1);
    int laa = nls_sort_key(aa, 2, kaa, 32, 1);
    /* The base-254 primary encoding keeps 0x01 below any weight byte, so a
     * prefix sorts before its extension. */
    TEST_ASSERT_EQ((uint64_t)(sk_cmp(ka, la, kaa, laa) < 0), 1u, "\"a\" < \"aa\" (prefix first)");
}

static void test_nls_sort_binary(void)
{
    const uint16_t app[3]   = { 'a', 'p', 'p' };
    const uint16_t apple[5] = { 'a', 'p', 'p', 'l', 'e' };
    const uint16_t upper[1] = { 'A' };
    const uint16_t lower[1] = { 'a' };
    uint8_t k1[16], k2[16], k3[16], k4[16];
    int l1 = nls_sort_key_binary(app, 3, k1, 16);
    int l2 = nls_sort_key_binary(apple, 5, k2, 16);
    int l3 = nls_sort_key_binary(upper, 1, k3, 16);
    int l4 = nls_sort_key_binary(lower, 1, k4, 16);
    TEST_ASSERT_EQ((uint64_t)l1, 6u, "\"app\" binary key is 6 bytes");
    TEST_ASSERT_EQ((uint64_t)(sk_cmp(k1, l1, k2, l2) < 0), 1u, "app < apple (binary prefix)");
    TEST_ASSERT_EQ((uint64_t)(sk_cmp(k3, l3, k4, l4) < 0), 1u, "A < a (binary ordinal)");
}

static void test_nls_sort_binary_compare(void)
{
    /* The shipped length-aware comparator: min-length memcmp then shorter-first,
     * so a prefix ranks BEFORE its extension (a bare memcmp would tie them). */
    const uint16_t app[3]   = { 'a', 'p', 'p' };
    const uint16_t apple[5] = { 'a', 'p', 'p', 'l', 'e' };
    uint8_t k1[16], k2[16];
    int l1 = nls_sort_key_binary(app, 3, k1, 16);
    int l2 = nls_sort_key_binary(apple, 5, k2, 16);
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_sort_key_binary_compare(k1, l1, k2, l2), (uint64_t)(int64_t)-1,
                   "app < apple (prefix sorts first)");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_sort_key_binary_compare(k2, l2, k1, l1), 1u,
                   "apple > app (antisymmetric)");
    TEST_ASSERT_EQ((uint64_t)nls_sort_key_binary_compare(k1, l1, k1, l1), 0u,
                   "equal keys compare 0");
    /* NULL/empty side sorts first, no deref. */
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_sort_key_binary_compare((const uint8_t *)0, 0, k1, l1),
                   (uint64_t)(int64_t)-1, "empty < non-empty");
    TEST_ASSERT_EQ((uint64_t)nls_sort_key_binary_compare((const uint8_t *)0, 0, (const uint8_t *)0, 0), 0u,
                   "empty == empty");
    /* A NULL side with a STALE non-zero length is still empty (sorts first), not
     * equal to a real same-length key -- else weak ordering collapses. */
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_sort_key_binary_compare((const uint8_t *)0, l1, k1, l1),
                   (uint64_t)(int64_t)-1, "NULL/stale-len < real same-length key");
    TEST_ASSERT_EQ((uint64_t)nls_sort_key_binary_compare(k2, l2, (const uint8_t *)0, l2), 1u,
                   "real key > NULL/stale-len");
}

static void test_nls_sort_sizing(void)
{
    const uint16_t s[3] = { 'a', 'b', 'c' };
    /* primary 3*3 = 9 + separator 1 + case 3 + terminator 1 = 14. */
    TEST_ASSERT_EQ((uint64_t)nls_sort_key(s, 3, 0, 0, 0), 14u, "sort-key sizing pass");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_sort_key(s, 3, 0, 0, 0), 14u, "no over-write on sizing");
}

static void test_nls_normalize_nfd_nfc(void)
{
    const uint16_t e_acute[1] = { 0x00E9 };          /* precomposed e-acute */
    uint16_t nfd[4], nfc[4];
    int ld = nls_normalize(NLS_NORM_NFD, e_acute, 1, nfd, 4);
    TEST_ASSERT_EQ((uint64_t)ld, 2u, "NFD e-acute -> 2 units");
    TEST_ASSERT_EQ((uint64_t)nfd[0], (uint64_t)'e', "NFD base is 'e'");
    TEST_ASSERT_EQ((uint64_t)nfd[1], 0x0301u, "NFD mark is combining acute");
    int lc = nls_normalize(NLS_NORM_NFC, nfd, 2, nfc, 4);
    TEST_ASSERT_EQ((uint64_t)lc, 1u, "NFC recomposes to 1 unit");
    TEST_ASSERT_EQ((uint64_t)nfc[0], 0x00E9u, "NFC -> precomposed e-acute");
}

static void test_nls_normalize_unsupported(void)
{
    const uint16_t ext[1] = { 0x0100 };              /* Latin Extended-A A-macron */
    uint16_t out[4];
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_normalize(NLS_NORM_NFC, ext, 1, out, 4),
                   (uint64_t)(int64_t)NLS_NORM_ERR_UNSUPPORTED, "code unit >= U+0100 unsupported");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_normalize(99, ext, 1, out, 4),
                   (uint64_t)(int64_t)NLS_NORM_ERR_FORM, "unknown form rejected");
    /* NFC fails closed on a composable base+mark pair not in the table
     * (C + U+0301 = U+0106, Latin Extended-A, unsupported here). */
    const uint16_t c_acute[2] = { 'C', 0x0301 };
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_normalize(NLS_NORM_NFC, c_acute, 2, out, 4),
                   (uint64_t)(int64_t)NLS_NORM_ERR_UNSUPPORTED, "NFC C+acute unsupported");
    /* Multi-mark: A + ring + acute = U+01FA (outside the table) must fail closed,
     * not compose A+ring and emit a trailing standalone acute. */
    const uint16_t a_ring_acute[3] = { 'A', 0x030A, 0x0301 };
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_normalize(NLS_NORM_NFC, a_ring_acute, 3, out, 4),
                   (uint64_t)(int64_t)NLS_NORM_ERR_UNSUPPORTED, "NFC multi-mark fails closed");
    /* A leading combining mark (no starter) also fails closed. */
    const uint16_t lead_mark[2] = { 0x0301, 'x' };
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_normalize(NLS_NORM_NFC, lead_mark, 2, out, 4),
                   (uint64_t)(int64_t)NLS_NORM_ERR_UNSUPPORTED, "NFC leading mark fails closed");
    /* NFD: a precomposed (1 mark) followed by a 2nd mark needs combining-class
     * ordering we do not do -> fail closed (A-acute + cedilla). */
    const uint16_t aacute_ced[2] = { 0x00C1, 0x0327 };
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_normalize(NLS_NORM_NFD, aacute_ced, 2, out, 4),
                   (uint64_t)(int64_t)NLS_NORM_ERR_UNSUPPORTED, "NFD 2nd mark fails closed");
}

static void test_nls_bad_length_rejected(void)
{
    const uint16_t s[2] = { 'a', 'b' };
    uint8_t k[16];
    uint16_t o[4];
    /* src_len < -1 must be rejected, not treated as a NUL scan. */
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_sort_key(s, -2, k, 16, 0),
                   (uint64_t)(int64_t)NLS_SORT_ERR_PARAM, "sort_key rejects src_len -2");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_sort_key_binary(s, -2, k, 16),
                   (uint64_t)(int64_t)NLS_SORT_ERR_PARAM, "binary rejects src_len -2");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_normalize(NLS_NORM_NFC, s, -2, o, 4),
                   (uint64_t)(int64_t)NLS_NORM_ERR_PARAM, "normalize rejects src_len -2");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_fold_string(NLS_MAP_FOLDDIGITS, s, -2, o, 4),
                   (uint64_t)(int64_t)NLS_FOLD_ERR_PARAM, "fold rejects src_len -2");
}

static void test_nls_no_silent_normalization(void)
{
    /* Policy: the kernel does NOT fold a precomposed vs decomposed name to equal;
     * the explicit helper does when asked. */
    uint16_t pre[2]  = { 0x00E9, 0 };                /* e-acute */
    uint16_t dec[3]  = { 'e', 0x0301, 0 };           /* e + combining acute */
    UNICODE_STRING a, b;
    uint16_t na[4], nb[4];
    make_us(&a, pre, 2, 4);
    make_us(&b, dec, 4, 6);
    TEST_ASSERT_EQ((uint64_t)RtlEqualUnicodeString(&a, &b, 0), 0u,
                   "precomposed != decomposed (no silent normalize)");
    /* After explicit NFC both collapse to the same precomposed form. */
    TEST_ASSERT_EQ((uint64_t)nls_normalize(NLS_NORM_NFC, pre, 1, na, 4), 1u, "NFC pre");
    TEST_ASSERT_EQ((uint64_t)nls_normalize(NLS_NORM_NFC, dec, 2, nb, 4), 1u, "NFC dec");
    TEST_ASSERT_EQ((uint64_t)(na[0] == nb[0]), 1u, "NFC makes them equal");
}

static void test_nls_fold_digits(void)
{
    const uint16_t fw[1]  = { 0xFF11 };              /* fullwidth digit one */
    const uint16_t ar[1]  = { 0x0661 };              /* Arabic-Indic one */
    uint16_t out[4];
    TEST_ASSERT_EQ((uint64_t)nls_fold_string(NLS_MAP_FOLDDIGITS, fw, 1, out, 4), 1u, "fold digit len");
    TEST_ASSERT_EQ((uint64_t)out[0], (uint64_t)'1', "fullwidth 1 -> '1'");
    nls_fold_string(NLS_MAP_FOLDDIGITS, ar, 1, out, 4);
    TEST_ASSERT_EQ((uint64_t)out[0], (uint64_t)'1', "Arabic-Indic 1 -> '1'");
}

static void test_nls_fold_width_and_flags(void)
{
    const uint16_t fwA[1]  = { 0xFF21 };             /* fullwidth A */
    const uint16_t space[1] = { 0x3000 };            /* ideographic space */
    const uint16_t plain[1] = { 'x' };
    uint16_t out[4];
    nls_fold_string(NLS_MAP_FOLDCZONE, fwA, 1, out, 4);
    TEST_ASSERT_EQ((uint64_t)out[0], (uint64_t)'A', "fullwidth A -> 'A' (FOLDCZONE)");
    nls_fold_string(NLS_MAP_FOLDCZONE, space, 1, out, 4);
    TEST_ASSERT_EQ((uint64_t)out[0], 0x0020u, "ideographic space -> space");
    /* No fold flag set -> error; an unsupported flag bit (0x40 MAP_COMPOSITE) also
     * fails closed rather than silently doing nothing. */
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_fold_string(0, plain, 1, out, 4),
                   (uint64_t)(int64_t)NLS_FOLD_ERR_FLAGS, "no fold flag rejected");
    TEST_ASSERT_EQ((uint64_t)(int64_t)nls_fold_string(0x40u, plain, 1, out, 4),
                   (uint64_t)(int64_t)NLS_FOLD_ERR_FLAGS, "unsupported fold flag rejected");
}

/* Documents the DELIBERATE narrow coverage: this compiled fallback folds only
 * the listed ranges; a Unicode decimal digit outside the three digit ranges, or
 * a compatibility character outside FF01-FF5E/3000, passes through UNCHANGED with
 * success. Full FoldStringW coverage (all Nd digits / all compatibility
 * decompositions) needs the disk fold-table data -- a caller must not read this
 * helper's success as a complete fold. */
static void test_nls_fold_narrow_passthrough(void)
{
    const uint16_t deva[1] = { 0x0966 };            /* Devanagari digit zero (Nd, not in the 3 ranges) */
    const uint16_t cjk[1]  = { 0x3231 };            /* PARENTHESIZED IDEOGRAPH (compat, outside FF01-FF5E/3000) */
    uint16_t out[4];
    TEST_ASSERT_EQ((uint64_t)nls_fold_string(NLS_MAP_FOLDDIGITS, deva, 1, out, 4), 1u,
                   "non-covered digit: len 1");
    TEST_ASSERT_EQ((uint64_t)out[0], 0x0966u,
                   "non-covered Nd digit passes through unchanged (narrow coverage)");
    TEST_ASSERT_EQ((uint64_t)nls_fold_string(NLS_MAP_FOLDCZONE, cjk, 1, out, 4), 1u,
                   "non-covered compat char: len 1");
    TEST_ASSERT_EQ((uint64_t)out[0], 0x3231u,
                   "non-covered compatibility char passes through unchanged (narrow coverage)");
}

/* ---- Section 8: native NLS/locale/MUI syscalls (via ssdt_dispatch) -------- */

/* NtQuerySystemInformation(SystemNlsInformation): two-pass length contract +
 * field snapshot. Runs in kernel previous-mode so the probes are no-ops and
 * copy_to_user targets the kernel-stack buffer directly. */
static void test_nls_syscall_system_nls_info(void)
{
    SYSTEM_NLS_INFORMATION info;
    uint32_t retlen = 0xDEADBEEF;
    /* Zero-size reports the required length, no copy. */
    NTSTATUS st = ssdt_dispatch(SSDT_NtQuerySystemInformation, SystemNlsInformation,
                                (uint64_t)(uintptr_t)&info, 0,
                                (uint64_t)(uintptr_t)&retlen, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INFO_LENGTH_MISMATCH, "0-size -> mismatch");
    TEST_ASSERT_EQ((uint64_t)retlen, 24u, "required length reported = 24");
    /* Exactly one byte short (23) still fails at the boundary. */
    retlen = 0;
    st = ssdt_dispatch(SSDT_NtQuerySystemInformation, SystemNlsInformation,
                       (uint64_t)(uintptr_t)&info, sizeof(info) - 1,
                       (uint64_t)(uintptr_t)&retlen, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INFO_LENGTH_MISMATCH, "23 -> mismatch");
    TEST_ASSERT_EQ((uint64_t)retlen, 24u, "required length still 24 at boundary");
    /* NULL buffer with adequate size still mismatches (no copy target). */
    st = ssdt_dispatch(SSDT_NtQuerySystemInformation, SystemNlsInformation,
                       0, sizeof(info), (uint64_t)(uintptr_t)&retlen, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INFO_LENGTH_MISMATCH, "NULL buffer -> mismatch");
    /* return_length == NULL is allowed on the success path. */
    memset(&info, 0, sizeof(info));
    st = ssdt_dispatch(SSDT_NtQuerySystemInformation, SystemNlsInformation,
                       (uint64_t)(uintptr_t)&info, sizeof(info), 0, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "NULL return_length ok on success");
    /* Full success from a sentinel retlen: every field matches the live accessors. */
    retlen = 0;
    st = ssdt_dispatch(SSDT_NtQuerySystemInformation, SystemNlsInformation,
                       (uint64_t)(uintptr_t)&info, sizeof(info),
                       (uint64_t)(uintptr_t)&retlen, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "SystemNlsInformation ok");
    TEST_ASSERT_EQ((uint64_t)retlen, 24u, "return_length written from sentinel");
    TEST_ASSERT_EQ((uint64_t)info.AnsiCodePage, (uint64_t)nls_cp_get_acp(), "ACP");
    TEST_ASSERT_EQ((uint64_t)info.OemCodePage, (uint64_t)nls_cp_get_oemcp(), "OEMCP");
    TEST_ASSERT_EQ((uint64_t)info.SystemLcid, (uint64_t)nls_locale_get_system(), "system LCID");
    TEST_ASSERT_EQ((uint64_t)info.UserLcid, (uint64_t)nls_locale_get_user(), "user LCID");
    TEST_ASSERT_EQ((uint64_t)info.NlsVersion, (uint64_t)nls_get_version(), "NLS version");
    TEST_ASSERT_EQ((uint64_t)info.UiLangId, (uint64_t)nls_locale_get_ui_language(), "UI langid");
    TEST_ASSERT_EQ((uint64_t)info.InstallUiLangId,
                   (uint64_t)nt_locale_get_install_ui_language(), "install langid");
}

/* NtIsUILanguageComitted: active UI language is committed; a different one is not. */
static void test_nls_syscall_is_ui_committed(void)
{
    uint16_t active = nls_locale_get_ui_language();
    uint8_t committed = 0xFF;
    NTSTATUS st = ssdt_dispatch(SSDT_NtIsUILanguageComitted, active,
                                (uint64_t)(uintptr_t)&committed, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "committed query ok");
    TEST_ASSERT_EQ((uint64_t)committed, 1u, "active UI language is committed");
    committed = 0xFF;
    st = ssdt_dispatch(SSDT_NtIsUILanguageComitted, (uint16_t)(active ^ 0x0F0F),
                       (uint64_t)(uintptr_t)&committed, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "non-active query ok");
    TEST_ASSERT_EQ((uint64_t)committed, 0u, "other UI language not committed");
    /* NULL out pointer rejected. */
    st = ssdt_dispatch(SSDT_NtIsUILanguageComitted, active, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INVALID_PARAMETER, "NULL out rejected");
}

/* NtFlushInstallUILanguage: fail-closed for any language/flags, state unchanged. */
static void test_nls_syscall_flush_fail_closed(void)
{
    uint16_t active = nls_locale_get_ui_language();
    TEST_ASSERT_EQ((uint64_t)ssdt_dispatch(SSDT_NtFlushInstallUILanguage, active,
                   0, 0, 0, 0, 0), (uint64_t)STATUS_PRIVILEGE_NOT_HELD,
                   "flush active fail-closed");
    /* A different language + nonzero commit flag is also denied. */
    TEST_ASSERT_EQ((uint64_t)ssdt_dispatch(SSDT_NtFlushInstallUILanguage,
                   (uint16_t)(active ^ 0x0F0F), 1, 0, 0, 0, 0),
                   (uint64_t)STATUS_PRIVILEGE_NOT_HELD, "flush other lang + flag fail-closed");
    /* The denied calls left the UI/install language policy untouched. */
    TEST_ASSERT_EQ((uint64_t)nls_locale_get_ui_language(), (uint64_t)active,
                   "UI language unchanged after denied flush");
}

/* NtGetMUIRegistryInfo: in/out size contract (capacity in, required out) proven
 * against the caller buffer BEFORE any copy. */
static void test_nls_syscall_mui_registry_info(void)
{
    MUI_REGISTRY_INFO info;
    uint32_t size, i;
    /* Every capacity below the required 16 reports the required size, rewrites
     * *size to 16, and leaves the output blob byte-for-byte untouched. */
    for (i = 0; i < sizeof(MUI_REGISTRY_INFO); i++) {
        memset(&info, 0xAB, sizeof(info));
        size = i;
        NTSTATUS st = ssdt_dispatch(SSDT_NtGetMUIRegistryInfo, 0,
                                    (uint64_t)(uintptr_t)&size,
                                    (uint64_t)(uintptr_t)&info, 0, 0, 0);
        TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INFO_LENGTH_MISMATCH, "cap<16 -> mismatch");
        TEST_ASSERT_EQ((uint64_t)size, 16u, "required size 16 written on mismatch");
        TEST_ASSERT_EQ((uint64_t)((const uint8_t *)&info)[0], 0xABu, "blob untouched on mismatch");
    }
    /* NULL buffer with adequate capacity still mismatches, size still reported. */
    size = sizeof(info);
    TEST_ASSERT_EQ((uint64_t)ssdt_dispatch(SSDT_NtGetMUIRegistryInfo, 0,
                   (uint64_t)(uintptr_t)&size, 0, 0, 0, 0),
                   (uint64_t)STATUS_INFO_LENGTH_MISMATCH, "NULL buffer -> mismatch");
    TEST_ASSERT_EQ((uint64_t)size, 16u, "required size written even with NULL buffer");
    /* Oversized capacity from a sentinel: success rewrites *size to 16 and fills. */
    memset(&info, 0, sizeof(info));
    size = 0xDEAD;
    NTSTATUS st = ssdt_dispatch(SSDT_NtGetMUIRegistryInfo, 0, (uint64_t)(uintptr_t)&size,
                                (uint64_t)(uintptr_t)&info, 0, 0, 0);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "oversized cap ok");
    TEST_ASSERT_EQ((uint64_t)size, 16u, "size rewritten to 16 on success");
    TEST_ASSERT_EQ((uint64_t)info.SystemLocale, (uint64_t)nls_locale_get_system(), "system locale");
    TEST_ASSERT_EQ((uint64_t)info.UserLocale, (uint64_t)nls_locale_get_user(), "user locale");
    TEST_ASSERT_EQ((uint64_t)info.UILanguage, (uint64_t)nls_locale_get_ui_language(), "UI language");
    TEST_ASSERT_EQ((uint64_t)info.NlsVersion, (uint64_t)nls_get_version(), "NLS version");
    /* size_ptr aliasing the output buffer is rejected before the blob copy can
     * clobber the required-size word (in/out ABI integrity). */
    memset(&info, 0, sizeof(info));
    *(uint32_t *)&info = sizeof(info);   /* cap = 16, aliased into the buffer */
    TEST_ASSERT_EQ((uint64_t)ssdt_dispatch(SSDT_NtGetMUIRegistryInfo, 0,
                   (uint64_t)(uintptr_t)&info, (uint64_t)(uintptr_t)&info, 0, 0, 0),
                   (uint64_t)STATUS_INVALID_PARAMETER, "size/buffer alias rejected");
    /* Partial overlap (size word straddling the output buffer) is also rejected. */
    uint8_t scratch[24];
    uint32_t *sz = (uint32_t *)&scratch[8];   /* size word inside [scratch, scratch+16) */
    *sz = sizeof(info);
    TEST_ASSERT_EQ((uint64_t)ssdt_dispatch(SSDT_NtGetMUIRegistryInfo, 0,
                   (uint64_t)(uintptr_t)sz, (uint64_t)(uintptr_t)scratch, 0, 0, 0),
                   (uint64_t)STATUS_INVALID_PARAMETER, "partial size/buffer overlap rejected");
    /* NULL size pointer rejected. */
    TEST_ASSERT_EQ((uint64_t)ssdt_dispatch(SSDT_NtGetMUIRegistryInfo, 0, 0,
                   (uint64_t)(uintptr_t)&info, 0, 0, 0),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL size rejected");
}

/* ---- Section 9: consumer retrofit (inline fold fast path) ----------------- */

/* The hot-loop inline fast path MUST fold identically to the out-of-line
 * authority for every code unit -- OB/registry/atom all fold through the inline
 * now, so any divergence (wrong ASCII boundary, missed Latin-1, dropped
 * delegation) would silently split the namespace. Exhaustive over the BMP. */
static void test_nls_upcase_inline_matches_authority(void)
{
    uint32_t c;
    uint32_t mismatches = 0;
    for (c = 0; c <= 0xFFFFu; c++) {
        if (rtl_upcase_char_inline((uint16_t)c) != rtl_upcase_char((uint16_t)c))
            mismatches++;
    }
    TEST_ASSERT_EQ((uint64_t)mismatches, 0u,
                   "inline fold matches the out-of-line authority for all BMP units");
    /* Spot-check the load-bearing cases the retrofit relies on. */
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char_inline('a'), (uint64_t)'A', "ASCII a->A inline");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char_inline(0x007Fu), 0x007Fu, "0x7F passthrough");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char_inline(0x00E9u), 0x00C9u, "Latin-1 e-acute folds");
    TEST_ASSERT_EQ((uint64_t)rtl_upcase_char_inline(0x00FFu), 0x0178u, "y-diaeresis -> U+0178");
}

/* File-name APIs fail closed on a non-ASCII code unit instead of masking it to
 * an aliased ASCII path (which would target the WRONG file). */
static void test_nls_file_nonascii_fail_closed(void)
{
    uint16_t name[2] = { 'x', 0x00E9 };     /* "x" + e-acute (non-ASCII) */
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    uint8_t fbi[64];   /* non-NULL out buffer so NtQueryAttributesFile reaches the name check */
    us.Length = 4; us.MaximumLength = 4; us.Buffer = name;
    memset(&oa, 0, sizeof(oa));
    oa.ObjectName = &us;
    TEST_ASSERT_EQ((uint64_t)(int64_t)ssdt_dispatch(SSDT_NtDeleteFile,
                   (uint64_t)(uintptr_t)&oa, 0, 0, 0, 0, 0),
                   (uint64_t)(int64_t)STATUS_OBJECT_NAME_INVALID,
                   "NtDeleteFile non-ASCII name fail-closed");
    TEST_ASSERT_EQ((uint64_t)(int64_t)ssdt_dispatch(SSDT_NtQueryAttributesFile,
                   (uint64_t)(uintptr_t)&oa, (uint64_t)(uintptr_t)fbi, 0, 0, 0, 0),
                   (uint64_t)(int64_t)STATUS_OBJECT_NAME_INVALID,
                   "NtQueryAttributesFile non-ASCII name fail-closed");
    /* Embedded NUL must be rejected, not truncate "vic\0tim" into "vic" (which
     * would unlink a DIFFERENT object than the counted name). */
    uint16_t embnul[3] = { 'a', 0x0000, 'b' };
    us.Length = 6; us.MaximumLength = 6; us.Buffer = embnul;
    TEST_ASSERT_EQ((uint64_t)(int64_t)ssdt_dispatch(SSDT_NtDeleteFile,
                   (uint64_t)(uintptr_t)&oa, 0, 0, 0, 0, 0),
                   (uint64_t)(int64_t)STATUS_OBJECT_NAME_INVALID,
                   "NtDeleteFile embedded-NUL name fail-closed");
    /* Odd byte Length is not whole WCHARs -> rejected before any path use. */
    uint16_t ascii[2] = { 'a', 'b' };
    us.Length = 3; us.MaximumLength = 4; us.Buffer = ascii;
    TEST_ASSERT_EQ((uint64_t)(int64_t)ssdt_dispatch(SSDT_NtDeleteFile,
                   (uint64_t)(uintptr_t)&oa, 0, 0, 0, 0, 0),
                   (uint64_t)(int64_t)STATUS_OBJECT_NAME_INVALID,
                   "NtDeleteFile odd-Length name fail-closed");
    /* Length past MaximumLength would read WCHARs outside the declared buffer. */
    us.Length = 4; us.MaximumLength = 2; us.Buffer = ascii;
    TEST_ASSERT_EQ((uint64_t)(int64_t)ssdt_dispatch(SSDT_NtDeleteFile,
                   (uint64_t)(uintptr_t)&oa, 0, 0, 0, 0, 0),
                   (uint64_t)(int64_t)STATUS_OBJECT_NAME_INVALID,
                   "NtDeleteFile Length>MaximumLength fail-closed");
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
    /* Section 7: sort keys + normalization + FoldStringW. */
    test_suite_register_cat("nls: sort key case-insensitive order",
                            test_nls_sort_case_insensitive_order, TEST_CAT_NLS);
    test_suite_register_cat("nls: sort key prefix order",
                            test_nls_sort_prefix_order, TEST_CAT_NLS);
    test_suite_register_cat("nls: sort key binary ordinal",
                            test_nls_sort_binary, TEST_CAT_NLS);
    test_suite_register_cat("nls: sort key binary compare",
                            test_nls_sort_binary_compare, TEST_CAT_NLS);
    test_suite_register_cat("nls: sort key sizing",
                            test_nls_sort_sizing, TEST_CAT_NLS);
    test_suite_register_cat("nls: normalize NFD/NFC round-trip",
                            test_nls_normalize_nfd_nfc, TEST_CAT_NLS);
    test_suite_register_cat("nls: normalize unsupported range",
                            test_nls_normalize_unsupported, TEST_CAT_NLS);
    test_suite_register_cat("nls: no silent normalization",
                            test_nls_no_silent_normalization, TEST_CAT_NLS);
    test_suite_register_cat("nls: fold digits",
                            test_nls_fold_digits, TEST_CAT_NLS);
    test_suite_register_cat("nls: fold width + flags",
                            test_nls_fold_width_and_flags, TEST_CAT_NLS);
    test_suite_register_cat("nls: fold narrow passthrough",
                            test_nls_fold_narrow_passthrough, TEST_CAT_NLS);
    test_suite_register_cat("nls: bad length rejected",
                            test_nls_bad_length_rejected, TEST_CAT_NLS);
    /* Section 8: native NLS/locale/MUI syscalls. */
    test_suite_register_cat("nls: syscall SystemNlsInformation",
                            test_nls_syscall_system_nls_info, TEST_CAT_NLS);
    test_suite_register_cat("nls: syscall IsUILanguageComitted",
                            test_nls_syscall_is_ui_committed, TEST_CAT_NLS);
    test_suite_register_cat("nls: syscall FlushInstallUILanguage fail-closed",
                            test_nls_syscall_flush_fail_closed, TEST_CAT_NLS);
    test_suite_register_cat("nls: syscall GetMUIRegistryInfo",
                            test_nls_syscall_mui_registry_info, TEST_CAT_NLS);
    /* Section 9: consumer retrofit. */
    test_suite_register_cat("nls: inline fold matches authority",
                            test_nls_upcase_inline_matches_authority, TEST_CAT_NLS);
    test_suite_register_cat("nls: file non-ASCII name fail-closed",
                            test_nls_file_nonascii_fail_closed, TEST_CAT_NLS);
}
