/* test_kimage.c -- KIMAGE_ENTRY data model (kernel image registry).
 *
 * Covers the section-1 invariants: the flags bitmask is single-bit
 * non-overlapping, a zero-initialized entry classifies as UNKNOWN format
 * (never ELF), the embedded EX_RUNDOWN_REF lifetime gate acquires and runs
 * down correctly, and every field round-trips through a populated entry.
 */
#include "kernel/test/test.h"
#include "kernel/kimage.h"
#include "kernel/ex.h"

/* The flags bitmask must be single-bit and non-overlapping: the union of all
 * flags has exactly one bit per flag, and each flag is a single bit. */
static void test_kimage_flags_nonoverlap(void)
{
    TEST_ASSERT_EQ(__builtin_popcount(KIMAGE_FLAG_ALL), 7,
        "7 flags -> 7 distinct bits in KIMAGE_FLAG_ALL");
    TEST_ASSERT_EQ(__builtin_popcount(KIMAGE_FLAG_GLOBAL), 1, "GLOBAL is one bit");
    TEST_ASSERT_EQ(__builtin_popcount(KIMAGE_FLAG_STRIPPED), 1, "STRIPPED is one bit");
    TEST_ASSERT_EQ(__builtin_popcount(KIMAGE_FLAG_SIGNED), 1, "SIGNED is one bit");
    TEST_ASSERT_EQ(__builtin_popcount(KIMAGE_FLAG_HAS_UNWIND), 1, "HAS_UNWIND is one bit");
    TEST_ASSERT_EQ(__builtin_popcount(KIMAGE_FLAG_ALIAS), 1, "ALIAS is one bit");
    TEST_ASSERT_EQ(__builtin_popcount(KIMAGE_FLAG_GOING), 1, "GOING is one bit");
    TEST_ASSERT_EQ(__builtin_popcount(KIMAGE_FLAG_TOMBSTONE), 1, "TOMBSTONE is one bit");
    /* A pair of distinct flags must not share bits. */
    TEST_ASSERT_EQ(KIMAGE_FLAG_GOING & KIMAGE_FLAG_TOMBSTONE, 0u,
        "GOING and TOMBSTONE occupy distinct bits");
}

/* A zero-initialized entry must read as UNKNOWN type/format/CI -- not ELF.
 * This is the reason format is kimage_format_t (UNKNOWN=0) and not an
 * EXEC_FMT_* id (where ELF=0 would silently classify a blank entry). */
static void test_kimage_zero_init_unknown(void)
{
    kimage_entry_t e = {0};
    TEST_ASSERT_EQ(e.format, (uint32_t)KIMAGE_FMT_UNKNOWN,
        "zero-init entry classifies as UNKNOWN format, not ELF");
    TEST_ASSERT_EQ(e.type, (uint32_t)KIMAGE_TYPE_UNKNOWN, "zero-init type is UNKNOWN");
    TEST_ASSERT_EQ(e.ci_decision, (uint32_t)KIMAGE_CI_UNKNOWN, "zero-init CI is UNKNOWN");
    TEST_ASSERT_EQ(e.identity_kind, (uint8_t)KIMAGE_ID_NONE, "zero-init identity is NONE");
    TEST_ASSERT_EQ(e.flags, 0u, "zero-init flags are clear");
}

/* The embedded lifetime gate must acquire on a live entry, and refuse new
 * acquires once rundown completes -- the section-9 unload contract. Uses
 * ExRundownCompleted (immediate), never ExWaitForRundownProtectionRelease
 * (which yields cooperatively and is not a pure-data operation). */
static void test_kimage_lifetime_gate(void)
{
    kimage_entry_t e = {0};
    ExInitializeRundownProtection(&e.life);
    TEST_ASSERT(!ExIsRundownActive(&e.life), "fresh entry: rundown not active");
    TEST_ASSERT(ExAcquireRundownProtection(&e.life), "acquire succeeds on live entry");
    ExReleaseRundownProtection(&e.life);
    ExRundownCompleted(&e.life);
    TEST_ASSERT(ExIsRundownActive(&e.life), "rundown active after completion");
    TEST_ASSERT(!ExAcquireRundownProtection(&e.life),
        "acquire refused after rundown -- unload gate holds");
}

/* Every field round-trips through a VALID populated entry (the record
 * invariants hold: strings NUL-terminated, lengths exclude the NUL and are
 * within capacity). Uses string-literal initializers so full_path/name/signer
 * are genuinely terminated, not just a single byte with a lying length. */
static void test_kimage_field_roundtrip(void)
{
    /* full_path = "C:\Impossible\System32\kernel.exe" (33 chars, NUL-terminated
     * by the literal); name = "kernel.exe" (10); signer = "Impossible OS" (13). */
    kimage_entry_t e = {
        .base = 0x1000,
        .size = 0x2000,
        .entry_point = 0x1400,
        .timestamp = 0xDEADBEEF,
        .type = KIMAGE_TYPE_DRIVER,
        .format = KIMAGE_FMT_PE,
        .flags = KIMAGE_FLAG_GLOBAL | KIMAGE_FLAG_SIGNED,
        .ci_decision = KIMAGE_CI_VALID,
        .owner_pid = 42,
        .section_count = 5,
        .load_order = 7,
        .checksum = 0xABCD,
        .symbols = 0x9000,
        .unwind_ranges = 0x9100,
        .exports = 0x9200,
        .imports = 0x9300,
        .relocations = 0x9400,
        .debug_info = 0x9500,
        .hash = { [0] = 0xAA, [31] = 0xBB },
        .identity = { [0] = 0x11, [19] = 0x99 },
        .identity_kind = KIMAGE_ID_PE_CODEVIEW,
        .identity_len = 20,
        .full_path = "C:\\Impossible\\System32\\kernel.exe",
        .full_path_len = 33,
        .name = "kernel.exe",
        .name_len = 10,
        .signer = "Impossible OS",
    };

    /* Runtime confirmation of the pinned ABI (the _Static_assert is compile
     * time; this catches a mismatched build of the two translation units). */
    TEST_ASSERT_EQ(sizeof(kimage_entry_t), 832u, "kimage_entry_t is 832 bytes");

    TEST_ASSERT_EQ(e.base, 0x1000u, "base round-trips");
    TEST_ASSERT_EQ(e.size, 0x2000u, "size round-trips");
    TEST_ASSERT_EQ(e.entry_point, 0x1400u, "entry_point round-trips");
    TEST_ASSERT_EQ(e.timestamp, 0xDEADBEEFu, "timestamp round-trips");
    /* type (role) and format (binary format) are independent fields. */
    TEST_ASSERT_EQ(e.type, (uint32_t)KIMAGE_TYPE_DRIVER, "type round-trips");
    TEST_ASSERT_EQ(e.format, (uint32_t)KIMAGE_FMT_PE, "format round-trips independently");
    TEST_ASSERT_EQ(e.flags, (uint32_t)(KIMAGE_FLAG_GLOBAL | KIMAGE_FLAG_SIGNED),
        "flags round-trip");
    TEST_ASSERT_EQ(e.ci_decision, (uint32_t)KIMAGE_CI_VALID, "ci_decision round-trips");
    TEST_ASSERT_EQ(e.owner_pid, 42u, "owner_pid round-trips");
    TEST_ASSERT_EQ(e.section_count, 5u, "section_count round-trips");
    TEST_ASSERT_EQ(e.load_order, 7u, "load_order round-trips");
    TEST_ASSERT_EQ(e.checksum, 0xABCDu, "checksum round-trips");
    TEST_ASSERT_EQ(e.symbols, 0x9000u, "symbols handle round-trips");
    TEST_ASSERT_EQ(e.unwind_ranges, 0x9100u, "unwind_ranges handle round-trips");
    TEST_ASSERT_EQ(e.exports, 0x9200u, "exports handle round-trips");
    TEST_ASSERT_EQ(e.imports, 0x9300u, "imports handle round-trips");
    TEST_ASSERT_EQ(e.relocations, 0x9400u, "relocations handle round-trips");
    TEST_ASSERT_EQ(e.debug_info, 0x9500u, "debug_info handle round-trips");
    TEST_ASSERT_EQ(e.hash[0], 0xAAu, "hash first byte round-trips");
    TEST_ASSERT_EQ(e.hash[31], 0xBBu, "hash last byte round-trips");
    TEST_ASSERT_EQ(e.identity_kind, (uint8_t)KIMAGE_ID_PE_CODEVIEW, "identity_kind round-trips");
    TEST_ASSERT_EQ(e.identity_len, 20u, "identity_len round-trips (<= KIMAGE_IDENTITY_MAX)");
    TEST_ASSERT(e.identity_len <= KIMAGE_IDENTITY_MAX, "identity_len within capacity");
    TEST_ASSERT_EQ(e.identity[19], 0x99u, "durable identity bytes round-trip");
    /* full_path: length excludes NUL, is within capacity, and the byte at
     * [len] is the terminator -- the record invariant. */
    TEST_ASSERT_EQ(e.full_path_len, 33u, "full_path_len round-trips");
    TEST_ASSERT(e.full_path_len < KIMAGE_PATH_MAX, "full_path_len within capacity");
    TEST_ASSERT_EQ(e.full_path[0], 'C', "full_path starts at 'C'");
    TEST_ASSERT_EQ(e.full_path[32], 'e', "full_path ends at 'e'");
    TEST_ASSERT_EQ(e.full_path[e.full_path_len], '\0', "full_path is NUL-terminated at len");
    TEST_ASSERT_EQ(e.name_len, 10u, "name_len round-trips");
    TEST_ASSERT(e.name_len < KIMAGE_NAME_MAX, "name_len within capacity");
    TEST_ASSERT_EQ(e.name[0], 'k', "name starts at 'k'");
    TEST_ASSERT_EQ(e.name[e.name_len], '\0', "name is NUL-terminated at len");
    TEST_ASSERT_EQ(e.signer[0], 'I', "signer content round-trips");
    TEST_ASSERT_EQ(e.signer[KIMAGE_SIGNER_MAX - 1], '\0', "signer stays bounded/terminated");
}

/* The EXEC_FMT_* -> kimage_format_t bridge must map every known value and
 * reject unknowns -- a raw field copy would silently corrupt (the id spaces
 * diverge: EXEC ELF/PE/EIF = 0/1/2, KIMAGE ELF/PE/EIF = 1/2/3). */
static void test_kimage_format_conversion(void)
{
    TEST_ASSERT_EQ(kimage_format_from_exec_fmt(EXEC_FMT_ELF), (uint32_t)KIMAGE_FMT_ELF,
        "EXEC_FMT_ELF -> KIMAGE_FMT_ELF");
    TEST_ASSERT_EQ(kimage_format_from_exec_fmt(EXEC_FMT_PE), (uint32_t)KIMAGE_FMT_PE,
        "EXEC_FMT_PE -> KIMAGE_FMT_PE");
    TEST_ASSERT_EQ(kimage_format_from_exec_fmt(EXEC_FMT_EIF), (uint32_t)KIMAGE_FMT_EIF,
        "EXEC_FMT_EIF -> KIMAGE_FMT_EIF");
    TEST_ASSERT_EQ(kimage_format_from_exec_fmt(999u), (uint32_t)KIMAGE_FMT_UNKNOWN,
        "unrecognized EXEC_FMT -> UNKNOWN (no silent corruption)");
    /* The raw-copy hazard the converter prevents: EXEC_FMT_ELF(0) copied
     * directly would read as KIMAGE_FMT_UNKNOWN, not KIMAGE_FMT_ELF. */
    TEST_ASSERT_NEQ((uint32_t)EXEC_FMT_ELF, (uint32_t)KIMAGE_FMT_ELF,
        "id spaces diverge -- direct copy is unsafe, converter is required");
}

/* Records must be cache-line aligned so an array never straddles lines.
 * The addresses are routed through a `volatile` pointer so -O2 cannot
 * constant-fold `&arr[i] & 63` into a `0 == 0` tautology: the mask must be a
 * real runtime check of the linked array's actual placement. */
static void test_kimage_array_alignment(void)
{
    static kimage_entry_t arr[3];
    kimage_entry_t *volatile p = arr;   /* opaque load; forces arr to be emitted */
    TEST_ASSERT_EQ(((uintptr_t)&p[0]) & 63u, 0u, "arr[0] is cache-line aligned");
    TEST_ASSERT_EQ(((uintptr_t)&p[1]) & 63u, 0u, "arr[1] is cache-line aligned");
    TEST_ASSERT_EQ(((uintptr_t)&p[2]) & 63u, 0u, "arr[2] is cache-line aligned");
}

void test_register_kimage(void)
{
    test_suite_register_cat("KImage: flags non-overlap",
        test_kimage_flags_nonoverlap, TEST_CAT_EXEC);
    test_suite_register_cat("KImage: zero-init is UNKNOWN",
        test_kimage_zero_init_unknown, TEST_CAT_EXEC);
    test_suite_register_cat("KImage: lifetime gate rundown",
        test_kimage_lifetime_gate, TEST_CAT_EXEC);
    test_suite_register_cat("KImage: field round-trip",
        test_kimage_field_roundtrip, TEST_CAT_EXEC);
    test_suite_register_cat("KImage: EXEC_FMT conversion",
        test_kimage_format_conversion, TEST_CAT_EXEC);
    test_suite_register_cat("KImage: array cache-line alignment",
        test_kimage_array_alignment, TEST_CAT_EXEC);
}
