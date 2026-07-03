/* ============================================================================
 * test_nt_misc.c -- Atom table + locale unit tests (NtAddAtom/NtFindAtom/
 * NtDeleteAtom + default-locale surface)
 *
 * Exercises the pure kernel-side helpers in nt_misc.c (nt_atom_* and
 * nt_locale_*) without the syscall marshalling path -- no live boot calls.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_misc.h"

/* Build a wide (UTF-16) string from an ASCII literal into caller storage.
 * Returns the char count. */
static uint32_t wstr(const char *s, uint16_t *out, uint32_t cap)
{
    uint32_t n = 0;
    while (s[n] != 0 && n < cap) {
        out[n] = (uint16_t)(uint8_t)s[n];
        n++;
    }
    return n;
}

/* Atom add/find/delete round-trip + not-found after delete. */
static void test_nt_atom_roundtrip(void)
{
    nt_atom_reset_for_test();
    uint16_t w[16];
    uint32_t n = wstr("TestAtom", w, 16);

    uint16_t a1 = 0, a2 = 0;
    NTSTATUS s = nt_atom_add(w, n, &a1);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "nt_atom_add succeeds");
    TEST_ASSERT(a1 >= NT_STRING_ATOM_BASE, "string atom ID in 0xC000+ range");

    s = nt_atom_find(w, n, &a2);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "nt_atom_find succeeds");
    TEST_ASSERT_EQ(a2, a1, "find returns same atom as add");

    s = nt_atom_delete(a1);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "nt_atom_delete succeeds");

    s = nt_atom_find(w, n, &a2);
    TEST_ASSERT_EQ(s, STATUS_OBJECT_NAME_NOT_FOUND, "find after delete is not-found");
    nt_atom_reset_for_test();
}

/* Refcount: adding the same name twice shares one slot; two deletes free it. */
static void test_nt_atom_refcount(void)
{
    nt_atom_reset_for_test();
    uint16_t w[16];
    uint32_t n = wstr("Shared", w, 16);

    uint16_t a1 = 0, a2 = 0;
    nt_atom_add(w, n, &a1);
    nt_atom_add(w, n, &a2);
    TEST_ASSERT_EQ(a2, a1, "duplicate add returns same atom");

    uint16_t usage = 0, name[16];
    uint32_t nlen = 0;
    NTSTATUS s = nt_atom_query_basic(a1, &usage, name, 16, &nlen);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "query basic succeeds");
    TEST_ASSERT_EQ(usage, 2, "usage count is 2 after two adds");
    TEST_ASSERT_EQ(nlen, n, "query returns correct name length");

    nt_atom_delete(a1);   /* usage 2 -> 1, still present */
    s = nt_atom_find(w, n, &a2);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "atom survives first delete (refcount 1)");

    nt_atom_delete(a1);   /* usage 1 -> 0, freed */
    s = nt_atom_find(w, n, &a2);
    TEST_ASSERT_EQ(s, STATUS_OBJECT_NAME_NOT_FOUND, "atom freed after second delete");
    nt_atom_reset_for_test();
}

/* Case-insensitive lookup (ASCII fold). */
static void test_nt_atom_case_insensitive(void)
{
    nt_atom_reset_for_test();
    uint16_t lo[16], up[16];
    uint32_t n = wstr("MixedCase", lo, 16);
    wstr("MIXEDCASE", up, 16);

    uint16_t a1 = 0, a2 = 0;
    nt_atom_add(lo, n, &a1);
    NTSTATUS s = nt_atom_find(up, n, &a2);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "case-insensitive find matches");
    TEST_ASSERT_EQ(a2, a1, "differently-cased name resolves to same atom");
    nt_atom_reset_for_test();
}

/* Latin-1 fold: the NLS-authority retrofit matches accented case pairs the old
 * ASCII-only fold could not (U+00E9 lower vs U+00C9 upper), and the folded-hash
 * prefilter keeps a genuinely different accented letter from matching. */
static void test_nt_atom_latin1_fold(void)
{
    nt_atom_reset_for_test();
    uint16_t lo[8] = { 'c', 'a', 'f', 0x00E9, 0 };   /* "caf" + small e-acute */
    uint16_t up[8] = { 'C', 'A', 'F', 0x00C9, 0 };   /* "CAF" + capital E-acute */
    uint16_t a1 = 0, a2 = 0;
    nt_atom_add(lo, 4, &a1);
    NTSTATUS s = nt_atom_find(up, 4, &a2);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "Latin-1 case-insensitive find matches");
    TEST_ASSERT_EQ(a2, a1, "accented differently-cased name resolves to same atom");

    uint16_t other[8] = { 'c', 'a', 'f', 0x00E8, 0 };  /* small e-grave: a different letter */
    uint16_t a3 = 0;
    s = nt_atom_find(other, 4, &a3);
    TEST_ASSERT_EQ(s, STATUS_OBJECT_NAME_NOT_FOUND, "distinct accented letter does not match");
    nt_atom_reset_for_test();
}

/* Integer atoms: no table entry, delete is a no-op success. */
static void test_nt_atom_integer(void)
{
    nt_atom_reset_for_test();
    /* Integer atom 0x1234 (< NT_MAXINTATOM) -- delete must succeed without
     * touching the table. */
    NTSTATUS s = nt_atom_delete(0x1234);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "integer-atom delete is no-op success");

    uint16_t usage = 0;
    uint32_t nlen = 99;
    s = nt_atom_query_basic(0x1234, &usage, (uint16_t *)0, 0, &nlen);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "integer-atom query succeeds");
    TEST_ASSERT_EQ(usage, 1, "integer-atom usage is 1");
    TEST_ASSERT_EQ(nlen, 0, "integer-atom has no name");
    nt_atom_reset_for_test();
}

/* Bad inputs are rejected. */
static void test_nt_atom_bad_input(void)
{
    nt_atom_reset_for_test();
    uint16_t w[NT_MAX_ATOM_LEN + 8];
    uint16_t a = 0;

    NTSTATUS s = nt_atom_add((uint16_t *)0, 4, &a);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER, "NULL name rejected");

    for (uint32_t i = 0; i < NT_MAX_ATOM_LEN + 4; i++)
        w[i] = (uint16_t)'x';
    s = nt_atom_add(w, NT_MAX_ATOM_LEN + 1, &a);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER, "over-length name rejected");

    s = nt_atom_delete(0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE, "atom 0 delete is invalid handle");

    s = nt_atom_delete((uint16_t)(NT_STRING_ATOM_BASE + 5));
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE, "unallocated string atom delete is invalid");
    nt_atom_reset_for_test();
}

/* Locale get/set round-trip; install language is immutable and defaults hold. */
static void test_nt_locale(void)
{
    uint32_t saved_lcid = nt_locale_get_default();
    uint16_t saved_lang = nt_locale_get_ui_language();

    TEST_ASSERT_EQ(nt_locale_get_install_ui_language(), NT_DEFAULT_LANGID,
                   "install UI language is en-US");

    nt_locale_set_default(0x0407);      /* de-DE */
    TEST_ASSERT_EQ(nt_locale_get_default(), 0x0407u, "default locale updates");

    nt_locale_set_ui_language(0x0411);  /* ja-JP */
    TEST_ASSERT_EQ(nt_locale_get_ui_language(), 0x0411u, "UI language updates");

    /* install language must not have moved. */
    TEST_ASSERT_EQ(nt_locale_get_install_ui_language(), NT_DEFAULT_LANGID,
                   "install UI language stays immutable after sets");

    nt_locale_set_default(saved_lcid);
    nt_locale_set_ui_language(saved_lang);
}

/* Build a unique wide name "a<idx>" into caller storage; returns char count. */
static uint32_t wname_idx(uint32_t idx, uint16_t *out)
{
    out[0] = (uint16_t)'a';
    char tmp[12];
    int t = 0;
    if (idx == 0)
        tmp[t++] = '0';
    while (idx > 0) {
        tmp[t++] = (char)('0' + (idx % 10));
        idx /= 10;
    }
    uint32_t n = 1;
    for (int i = t - 1; i >= 0; i--)
        out[n++] = (uint16_t)tmp[i];
    return n;
}

/* Fill the table to capacity; verify first/last IDs and table-full rejection. */
static void test_nt_atom_table_full(void)
{
    nt_atom_reset_for_test();
    uint16_t w[16], first = 0, last = 0, a = 0;
    for (uint32_t i = 0; i < NT_ATOM_TABLE_CAP; i++) {
        uint32_t n = wname_idx(i, w);
        NTSTATUS s = nt_atom_add(w, n, &a);
        TEST_ASSERT_EQ(s, STATUS_SUCCESS, "fill add succeeds");
        if (i == 0)
            first = a;
        if (i == NT_ATOM_TABLE_CAP - 1)
            last = a;
    }
    TEST_ASSERT_EQ(first, NT_STRING_ATOM_BASE, "first atom is 0xC000");
    TEST_ASSERT_EQ(last, (uint16_t)(NT_STRING_ATOM_BASE + NT_ATOM_TABLE_CAP - 1),
                   "last atom is base + cap - 1");

    uint32_t n = wname_idx(NT_ATOM_TABLE_CAP, w);
    NTSTATUS s = nt_atom_add(w, n, &a);
    TEST_ASSERT_EQ(s, STATUS_INSUFFICIENT_RESOURCES, "full table rejects new atom");
    nt_atom_reset_for_test();
}

/* Query boundary + error cases. */
static void test_nt_atom_query_boundary(void)
{
    nt_atom_reset_for_test();
    uint16_t usage = 0, name[8];
    uint32_t nlen = 0;

    /* Out-of-range string atom. */
    NTSTATUS s = nt_atom_query_basic(
        (uint16_t)(NT_STRING_ATOM_BASE + NT_ATOM_TABLE_CAP), &usage, name, 8, &nlen);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE, "out-of-range atom query is invalid");

    /* Stale (deleted) atom. */
    uint16_t w[8], a = 0;
    uint32_t n = wstr("Q", w, 8);
    nt_atom_add(w, n, &a);
    nt_atom_delete(a);
    s = nt_atom_query_basic(a, &usage, name, 8, &nlen);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE, "deleted atom query is invalid");

    /* NULL out_usage / out_name_len are rejected. */
    nt_atom_add(w, n, &a);
    s = nt_atom_query_basic(a, (uint16_t *)0, name, 8, &nlen);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER, "NULL usage pointer rejected");

    /* Too-small name buffer: full length is still reported. */
    uint16_t h[8];
    uint32_t hn = wstr("Hello", h, 8);
    nt_atom_add(h, hn, &a);
    uint16_t small[2];
    nlen = 0;
    s = nt_atom_query_basic(a, &usage, small, 2, &nlen);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "query with small buffer succeeds");
    TEST_ASSERT_EQ(nlen, hn, "full name length reported despite small buffer");
    nt_atom_reset_for_test();
}

/* UsageCount is clamped to the 16-bit ABI field when the internal count
 * exceeds 0xFFFF. */
static void test_nt_atom_usage_clamp(void)
{
    nt_atom_reset_for_test();
    uint16_t w[8], a = 0;
    uint32_t n = wstr("Clamp", w, 8);
    for (uint32_t i = 0; i < 0x10000u; i++)
        nt_atom_add(w, n, &a);   /* refcount reaches 0x10000 > 0xFFFF */

    uint16_t usage = 0;
    uint32_t nlen = 0;
    NTSTATUS s = nt_atom_query_basic(a, &usage, (uint16_t *)0, 0, &nlen);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "clamp query succeeds");
    TEST_ASSERT_EQ(usage, 0xFFFFu, "UsageCount clamped to 0xFFFF");
    nt_atom_reset_for_test();
}

void test_register_nt_misc(void)
{
    test_suite_register_cat("NT: atom add/find/delete round-trip", test_nt_atom_roundtrip, TEST_CAT_ABI);
    test_suite_register_cat("NT: atom refcount", test_nt_atom_refcount, TEST_CAT_ABI);
    test_suite_register_cat("NT: atom case-insensitive", test_nt_atom_case_insensitive, TEST_CAT_ABI);
    test_suite_register_cat("NT: atom Latin-1 fold + hash prefilter", test_nt_atom_latin1_fold, TEST_CAT_ABI);
    test_suite_register_cat("NT: integer atom passthrough", test_nt_atom_integer, TEST_CAT_ABI);
    test_suite_register_cat("NT: atom bad input", test_nt_atom_bad_input, TEST_CAT_ABI);
    test_suite_register_cat("NT: atom table full boundary", test_nt_atom_table_full, TEST_CAT_ABI);
    test_suite_register_cat("NT: atom query boundary", test_nt_atom_query_boundary, TEST_CAT_ABI);
    test_suite_register_cat("NT: atom usage clamp", test_nt_atom_usage_clamp, TEST_CAT_ABI);
    test_suite_register_cat("NT: default locale get/set", test_nt_locale, TEST_CAT_ABI);
}
