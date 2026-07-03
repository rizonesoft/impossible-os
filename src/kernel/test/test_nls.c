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
    st = nt_encode_unicode_string(&us, w, 3);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "encode ok");
    TEST_ASSERT_EQ((uint64_t)us.Length, 6u, "Length = 6");
    TEST_ASSERT_EQ((uint64_t)us.MaximumLength, 8u, "MaximumLength = Length + 2");
    TEST_ASSERT(us.Buffer == w, "Buffer set");
}

static void test_nls_encode_overflow(void)
{
    uint16_t w[1] = { 0 };
    UNICODE_STRING us;
    NTSTATUS st = nt_encode_unicode_string(&us, w, NT_UNICODE_MAX_WCHARS);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "encode past ceiling (no NUL room) rejected");
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
    st = nt_encode_unicode_string(&us, w, 2);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS, "encode ok");
    TEST_ASSERT_EQ((uint64_t)us._pad, 0u,
                   "encode clears ABI _pad (no kernel-memory leak on copy-out)");
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
    test_suite_register_cat("nls: encode overflow",
                            test_nls_encode_overflow, TEST_CAT_NLS);
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
}
