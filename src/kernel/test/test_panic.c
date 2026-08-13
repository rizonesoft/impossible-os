/* ============================================================================
 * test_panic.c -- Panic-path guarded-read unit tests
 *
 * Covers the two panic-entry primitives that let a crash report itself when the
 * corruption it is reporting reached the pointers it must read: the bounded
 * caller-string snapshot and the fault-safe frame-chain walk.
 *
 * These call ONLY the pure helpers. panic_screen_impl, panic_collect_evidence
 * and write_crash_dump are live crash infrastructure and are never invoked from
 * a test -- the evidence reserve/publish state machine is verified by an
 * operator-gated live double panic instead (see the TODO section).
 *
 * XREF: 01-boot-platform/TODO-10-bare-metal-hardening.md section 19
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/panic.h"
#include "kernel/idt.h"
#include "kernel/drivers/serial.h"   /* PANIC_CTX_* -- snapshot context table */
#include "kernel/mm/vmm.h"           /* vmm_get_physical: find an unmapped VA safely */

/* ---- Panic-entry string snapshot (single guarded copy for all renderers) ----
 *
 * These exercise the copy/placeholder decision table only. They never invoke a
 * panic: panic_snapshot_str and panic_capture_frames are pure over their
 * arguments, which is exactly why they were exposed. */

static int td_streq(const char *a, const char *b)
{
    uint32_t i = 0u;
    while (a[i] && a[i] == b[i])
        i++;
    return a[i] == b[i];
}

static void test_panic_snap_copies_normal(void)
{
    char dst[64];

    panic_snapshot_str(dst, sizeof dst, "kernel heap corruption",
                       PANIC_CTX_NORMAL, PANIC_STR_NONE);
    TEST_ASSERT(td_streq(dst, "kernel heap corruption"),
                "snapshot copies the string verbatim in NORMAL context");
}

static void test_panic_snap_null_uses_fallback(void)
{
    char dst[64];

    panic_snapshot_str(dst, sizeof dst, (const char *)0, PANIC_CTX_NORMAL,
                       PANIC_STR_NONE);
    TEST_ASSERT(td_streq(dst, PANIC_STR_NONE),
                "NULL source renders the caller-supplied fallback");
}

/* Canonical-form check rejects this before any load, so __kread_u8 returns -1
 * WITHOUT faulting (test_serial_emergency.c "rejects #GP operands" pins that).
 * That makes it a DISCRIMINATING probe for the forbidden contexts: a gate that
 * works never touches it and renders NO_GUARD, while a gate that leaks renders
 * UNREADABLE -- two different strings, so the test cannot pass either way. A
 * readable literal here would prove nothing, because an implementation that
 * walked it and then overwrote the result would still look correct. */
#define TD_UNREADABLE_SRC ((const char *)0x0000800000000000ULL)

static void test_panic_snap_nmi_never_walks(void)
{
    char dst[64];

    panic_snapshot_str(dst, sizeof dst, TD_UNREADABLE_SRC, PANIC_CTX_NMI,
                       PANIC_STR_NONE);
    TEST_ASSERT(td_streq(dst, PANIC_STR_NO_GUARD),
                "NMI context renders NO_GUARD, so the source was never read");
}

static void test_panic_snap_unknown_never_walks(void)
{
    char dst[64];

    panic_snapshot_str(dst, sizeof dst, TD_UNREADABLE_SRC, PANIC_CTX_UNKNOWN,
                       PANIC_STR_NONE);
    TEST_ASSERT(td_streq(dst, PANIC_STR_NO_GUARD),
                "UNKNOWN context renders NO_GUARD, so the source was never read");
}

static void test_panic_snap_unreadable_first_byte(void)
{
    char dst[64];

    /* The complement of the two above: in a context that DOES permit the read,
     * the same pointer must produce the fault-recovery placeholder. */
    panic_snapshot_str(dst, sizeof dst, TD_UNREADABLE_SRC, PANIC_CTX_NORMAL,
                       PANIC_STR_NONE);
    TEST_ASSERT(td_streq(dst, PANIC_STR_UNREADABLE),
                "an unreadable first byte renders the fault placeholder");
}

static void test_panic_snap_null_src_null_fallback(void)
{
    char dst[64];

    dst[0] = 0x5A;
    /* Must be decided from src alone: falling through to the reader here would
     * dereference address zero, in every context including the forbidden ones. */
    panic_snapshot_str(dst, sizeof dst, (const char *)0, PANIC_CTX_NMI,
                       (const char *)0);
    TEST_ASSERT_EQ((uint64_t)dst[0], 0u,
                   "NULL source with NULL fallback renders an empty string");
}

static void test_panic_snap_truncates_bounded(void)
{
    char dst[8];

    panic_snapshot_str(dst, sizeof dst, "abcdefghijklmnop", PANIC_CTX_NORMAL,
                       PANIC_STR_NONE);
    TEST_ASSERT(td_streq(dst, "abcdefg"),
                "snapshot truncates to cap-1 bytes");
    TEST_ASSERT_EQ((uint64_t)dst[7], 0u, "snapshot NUL-terminates at cap-1");
}

static void test_panic_snap_exact_fit(void)
{
    char dst[4];

    panic_snapshot_str(dst, sizeof dst, "abc", PANIC_CTX_NORMAL, PANIC_STR_NONE);
    TEST_ASSERT(td_streq(dst, "abc"),
                "a string exactly filling cap-1 is copied whole");
    TEST_ASSERT_EQ((uint64_t)dst[3], 0u, "exact fit still NUL-terminates");
}

static void test_panic_snap_cap_one(void)
{
    char dst[2];

    dst[0] = 0x5A;
    dst[1] = 0x5A;
    panic_snapshot_str(dst, 1u, "abc", PANIC_CTX_NORMAL, PANIC_STR_NONE);
    TEST_ASSERT_EQ((uint64_t)dst[0], 0u, "cap 1 writes only the terminator");
    TEST_ASSERT_EQ((uint64_t)(uint8_t)dst[1], 0x5Au, "cap 1 writes nothing past it");
}

static void test_panic_snap_zero_cap_writes_nothing(void)
{
    char dst[2];

    dst[0] = 0x5A;
    panic_snapshot_str(dst, 0u, "abc", PANIC_CTX_NORMAL, PANIC_STR_NONE);
    TEST_ASSERT_EQ((uint64_t)(uint8_t)dst[0], 0x5Au,
                   "cap 0 leaves the destination untouched");
}

/* ---- Fault-safe panic frame-chain walk ---- */

static void td_fill_frame(struct interrupt_frame *f, uint64_t rsp, uint64_t rbp)
{
    uint8_t *p = (uint8_t *)f;
    for (uint32_t i = 0u; i < sizeof *f; i++)
        p[i] = 0u;
    f->rsp = rsp;
    f->rbp = rbp;
}

static void test_panic_frames_walks_chain(void)
{
    uint64_t chain[4];
    uint64_t out[4];
    struct interrupt_frame f;
    uint32_t n;

    chain[0] = (uint64_t)(uintptr_t)&chain[2];   /* saved rbp -> next frame */
    chain[1] = 0xFFFFFFFF80001111ull;            /* return address */
    chain[2] = 0u;                               /* chain terminator */
    chain[3] = 0xFFFFFFFF80002222ull;

    td_fill_frame(&f, (uint64_t)(uintptr_t)&chain[0],
                  (uint64_t)(uintptr_t)&chain[0]);

    n = panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 4u);
    TEST_ASSERT_EQ((uint64_t)n, 2u, "walker records both synthetic frames");
    TEST_ASSERT_EQ(out[0], 0xFFFFFFFF80001111ull, "frame 0 return address");
    TEST_ASSERT_EQ(out[1], 0xFFFFFFFF80002222ull, "frame 1 return address");
}

static void test_panic_frames_nmi_returns_zero(void)
{
    uint64_t chain[4];
    uint64_t out[4];
    struct interrupt_frame f;

    /* A chain that WOULD walk successfully, plus sentinels in the output: the
     * return value alone cannot tell "withheld" from "walked and found nothing",
     * so the untouched sentinels are what prove no walk happened.
     *
     * LIMIT, stated rather than papered over: sentinels prove nothing was
     * WRITTEN, not that nothing was READ, and only instrumenting
     * __kstack_read_u64 itself could prove the latter -- production cost in the
     * panic path for one early return. What makes that acceptable is that the
     * gate predicate is shared: panic_snapshot_str takes the same
     * serial_emerg_ctx_allows_guarded_read decision, and its tests above ARE
     * read-discriminating (a leaking gate renders UNREADABLE, not NO_GUARD). */
    chain[0] = (uint64_t)(uintptr_t)&chain[2];
    chain[1] = 0xFFFFFFFF80001111ull;
    chain[2] = 0u;
    chain[3] = 0xFFFFFFFF80002222ull;
    out[0] = out[1] = out[2] = out[3] = 0xD1D1D1D1D1D1D1D1ull;
    td_fill_frame(&f, (uint64_t)(uintptr_t)&chain[0],
                  (uint64_t)(uintptr_t)&chain[0]);

    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NMI, out, 4u),
                   0u, "NMI context walks no frames at all");
    TEST_ASSERT_EQ(out[0], 0xD1D1D1D1D1D1D1D1ull,
                   "NMI context leaves the output buffer untouched");

    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_UNKNOWN, out, 4u),
                   0u, "UNKNOWN context walks no frames at all");
    TEST_ASSERT_EQ(out[0], 0xD1D1D1D1D1D1D1D1ull,
                   "UNKNOWN context leaves the output buffer untouched");

    /* Control: the same inputs in a permitted context DO produce frames, so the
     * two assertions above are measuring the context gate and not a chain that
     * was unwalkable to begin with. */
    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 4u),
                   2u, "the same chain walks fine in a permitted context");
}

/* Build `frames` linked stack frames in `chain` (2 slots each: saved rbp, then
 * return address), ascending so the walker sees a strictly climbing chain, and
 * terminate it. Returns the starting frame pointer. */
static uint64_t td_build_chain(uint64_t *chain, uint32_t frames)
{
    for (uint32_t i = 0u; i < frames; i++) {
        chain[i * 2u]      = (i + 1u < frames)
                           ? (uint64_t)(uintptr_t)&chain[(i + 1u) * 2u]
                           : 0u;                        /* terminator */
        chain[i * 2u + 1u] = 0xFFFFFFFF80000000ull + i; /* return address */
    }
    return (uint64_t)(uintptr_t)&chain[0];
}

static void test_panic_frames_clamps_count(void)
{
    uint64_t chain[34];                 /* 17 frames -- one past PANIC_MAX_STACK_DEPTH */
    uint64_t out[64];
    struct interrupt_frame f;
    uint64_t start;
    uint32_t n;

    /* The chain must be LONGER than the clamp, or the terminator stops the walk
     * first and the test passes with the clamp deleted -- which is exactly what
     * a 2-frame chain with count=64 was measuring: nothing. */
    start = td_build_chain(chain, 17u);
    for (uint32_t i = 0u; i < 64u; i++)
        out[i] = 0xD1D1D1D1D1D1D1D1ull;
    td_fill_frame(&f, start, start);

    n = panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 64u);
    TEST_ASSERT_EQ((uint64_t)n, (uint64_t)PANIC_MAX_STACK_DEPTH,
                   "an oversized count is clamped to PANIC_MAX_STACK_DEPTH");
    TEST_ASSERT_EQ(out[PANIC_MAX_STACK_DEPTH - 1u], 0xFFFFFFFF80000000ull + 15u,
                   "the last recorded frame is the 16th of the chain");
    TEST_ASSERT_EQ(out[PANIC_MAX_STACK_DEPTH], 0xD1D1D1D1D1D1D1D1ull,
                   "the 17th chain frame is not written");
}

static void test_panic_frames_honours_small_count(void)
{
    uint64_t chain[34];
    uint64_t out[8];
    struct interrupt_frame f;
    uint64_t start;

    /* A caller asking for fewer than the chain holds must get exactly that
     * many -- the count bound and the PANIC_MAX_STACK_DEPTH clamp are separate. */
    start = td_build_chain(chain, 17u);
    for (uint32_t i = 0u; i < 8u; i++)
        out[i] = 0xD1D1D1D1D1D1D1D1ull;
    td_fill_frame(&f, start, start);

    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 1u),
                   1u, "a count of 1 records exactly one frame");
    TEST_ASSERT_EQ(out[0], 0xFFFFFFFF80000000ull, "and it is the innermost one");
    TEST_ASSERT_EQ(out[1], 0xD1D1D1D1D1D1D1D1ull, "nothing past the requested count");
}

static void test_panic_frames_zero_count(void)
{
    uint64_t chain[2];
    uint64_t out[1];
    struct interrupt_frame f;

    chain[0] = 0u;
    chain[1] = 0xFFFFFFFF80001111ull;
    out[0] = 0xD1D1D1D1D1D1D1D1ull;
    td_fill_frame(&f, (uint64_t)(uintptr_t)&chain[0],
                  (uint64_t)(uintptr_t)&chain[0]);

    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 0u),
                   0u, "a zero count records no frames");
    TEST_ASSERT_EQ(out[0], 0xD1D1D1D1D1D1D1D1ull,
                   "a zero count leaves the output buffer untouched");
}

static void test_panic_frames_rejects_span_wrap(void)
{
    uint64_t out[4];
    struct interrupt_frame f;

    /* An RSP within one span of the top of the address space would overflow the
     * upper bound; the walk must decline rather than compute a wrapped `hi`. */
    td_fill_frame(&f, 0xFFFFFFFFFFFFF000ull, 0xFFFFFFFFFFFFF000ull);
    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 4u),
                   0u, "an RSP that would wrap the span records no frames");
}

static void test_panic_frames_rejects_above_span(void)
{
    uint64_t chain[2];
    uint64_t out[4];
    struct interrupt_frame f;

    chain[0] = 0u;
    chain[1] = 0xFFFFFFFF80001111ull;
    /* RBP more than one span above RSP is not a frame on this stack. */
    td_fill_frame(&f, (uint64_t)(uintptr_t)&chain[0] - 0x20000ull,
                  (uint64_t)(uintptr_t)&chain[0]);

    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 4u),
                   0u, "a frame pointer beyond the searched span records no frames");
}

static void test_panic_frames_rejects_misaligned(void)
{
    uint64_t chain[4];
    uint64_t out[4];
    struct interrupt_frame f;

    chain[0] = 0u;
    chain[1] = 0xFFFFFFFF80001111ull;
    td_fill_frame(&f, (uint64_t)(uintptr_t)&chain[0],
                  (uint64_t)(uintptr_t)&chain[0] + 1u);

    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 4u),
                   0u, "a misaligned frame pointer records no frames");
}

static void test_panic_frames_rejects_below_sp(void)
{
    uint64_t chain[8];
    uint64_t out[4];
    struct interrupt_frame f;

    chain[0] = 0u;
    chain[1] = 0xFFFFFFFF80001111ull;
    /* RBP below the interrupted RSP is not a live frame. */
    td_fill_frame(&f, (uint64_t)(uintptr_t)&chain[4],
                  (uint64_t)(uintptr_t)&chain[0]);

    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 4u),
                   0u, "a frame pointer below RSP records no frames");
}

static void test_panic_frames_breaks_on_cycle(void)
{
    uint64_t chain[4];
    uint64_t out[4];
    struct interrupt_frame f;

    chain[0] = (uint64_t)(uintptr_t)&chain[0];   /* points at itself */
    chain[1] = 0xFFFFFFFF80001111ull;
    td_fill_frame(&f, (uint64_t)(uintptr_t)&chain[0],
                  (uint64_t)(uintptr_t)&chain[0]);

    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 4u),
                   1u, "a self-referencing frame stops the walk after one entry");
}

/* ---- The real fault path ----
 *
 * Everything above uses a NON-canonical pointer, which the guarded readers
 * reject BEFORE issuing a load -- excellent for proving the context gate never
 * touches the source, useless for proving the #PF fixup works, since no fault
 * is ever taken. These use a canonical-but-UNMAPPED kernel VA instead, so the
 * load really executes, really faults, and really has to be recovered by the
 * RIP-keyed fixup. Same fixture technique as test_unwind.c:1226. */
static uint64_t td_find_unmapped_va(void)
{
    uint64_t a;

    /* TWO non-faulting page-table queries, because neither answers the question
     * alone. vmm.c:525-533 says outright that vmm_get_physical returns 0 for
     * "not mapped" AND for "mapped to physical frame 0" -- so on its own it can
     * hand back a PRESENT alias, and a test that then took no fault would pass
     * while proving nothing about the fixup. vmm_query_flags reports the PTE
     * flags (0 when absent, and also 0 for an unsplit huge page, which is why it
     * is not sufficient alone either). Absent means both: no translation AND no
     * present bit. */
    for (a = 0xffffff0000000000ull; a < 0xffffff0000200000ull; a += 0x1000ull) {
        if (vmm_get_physical((uintptr_t)a) == 0 &&
            (vmm_query_flags((uintptr_t)a) & VMM_FLAG_PRESENT) == 0)
            return a;
    }
    return 0u;
}

static void test_panic_snap_recovers_real_fault(void)
{
    uint64_t bad = td_find_unmapped_va();
    char dst[64];

    if (!bad) {
        TEST_SKIP("no unmapped kernel VA found in the probe window");
        return;
    }

    /* TEST-SIDE-EFFECT-ALLOWED: deliberately points the snapshot at an unmapped
     * kernel VA so the guarded byte read takes a real #PF and must be recovered
     * by the fixup -- the exact fault this whole path exists to survive. */
    panic_snapshot_str(dst, sizeof dst, (const char *)(uintptr_t)bad,
                       PANIC_CTX_NORMAL, PANIC_STR_NONE);
    TEST_ASSERT(td_streq(dst, PANIC_STR_UNREADABLE),
                "a faulting first byte is recovered and renders the placeholder");

    /* The same address in a forbidden context must not reach the load at all --
     * proven by the DIFFERENT placeholder, not by the absence of a crash. */
    panic_snapshot_str(dst, sizeof dst, (const char *)(uintptr_t)bad,
                       PANIC_CTX_NMI, PANIC_STR_NONE);
    TEST_ASSERT(td_streq(dst, PANIC_STR_NO_GUARD),
                "the same faulting address is never loaded in NMI context");
}

static void test_panic_frames_recovers_real_fault(void)
{
    uint64_t bad = td_find_unmapped_va();
    uint64_t out[4];
    struct interrupt_frame f;

    if (!bad) {
        TEST_SKIP("no unmapped kernel VA found in the probe window");
        return;
    }

    out[0] = 0xD1D1D1D1D1D1D1D1ull;
    /* Page-aligned, at its own RSP and inside the span, so every validity check
     * passes and the walker reaches the guarded read -- which then faults. */
    td_fill_frame(&f, bad, bad);

    /* TEST-SIDE-EFFECT-ALLOWED: deliberately walks an unmapped frame pointer to
     * prove __kstack_read_u64 recovers instead of bugchecking mid-panic. */
    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 4u),
                   0u, "a faulting frame slot ends the walk with no frames");
    TEST_ASSERT_EQ(out[0], 0xD1D1D1D1D1D1D1D1ull,
                   "a faulting frame slot writes nothing to the output");
}

static void test_panic_frames_null_out(void)
{
    struct interrupt_frame f;

    td_fill_frame(&f, 0x1000u, 0x1000u);
    TEST_ASSERT_EQ((uint64_t)panic_capture_frames(&f, PANIC_CTX_NORMAL,
                                                  (uint64_t *)0, 4u),
                   0u, "NULL output buffer records no frames");
}

/* The frame == NULL branch: a SOFTWARE bugcheck (panic_screen(NULL, ...))
 * carries no trap frame, so the walker snapshots the live RBP/RSP instead.
 * Nothing else here reaches it -- every other case hands in a synthetic frame
 * or exits through a NULL-output/zero-count guard -- so a regression there
 * would silently drop the stack trace from every software panic while all the
 * other cases stayed green. Reachable with no live panic infrastructure. */
static __attribute__((noinline)) uint32_t td_capture_live(uint64_t *out,
                                                          uint32_t count)
{
    return panic_capture_frames((struct interrupt_frame *)0, PANIC_CTX_NORMAL,
                                out, count);
}

static void test_panic_frames_null_frame_walks_live(void)
{
    uint64_t out[8];
    uint32_t n;

    for (uint32_t i = 0u; i < 8u; i++)
        out[i] = 0xD1D1D1D1D1D1D1D1ull;

    n = td_capture_live(out, 8u);
    TEST_ASSERT(n >= 1u && n <= 8u,
                "a NULL frame walks the live stack within the requested bound");
    TEST_ASSERT(out[0] != 0u && out[0] != 0xD1D1D1D1D1D1D1D1ull,
                "and records a real return address in the first slot");
}

/* ---- Registration ---- */

void test_register_panic(void)
{
    test_suite_register_cat("Crash: snapshot copies in NORMAL ctx",
                            test_panic_snap_copies_normal, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot NULL uses fallback",
                            test_panic_snap_null_uses_fallback, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot NMI never walks",
                            test_panic_snap_nmi_never_walks, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot UNKNOWN never walks",
                            test_panic_snap_unknown_never_walks, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot unreadable first byte",
                            test_panic_snap_unreadable_first_byte, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot NULL src and NULL fallback",
                            test_panic_snap_null_src_null_fallback, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot truncates bounded",
                            test_panic_snap_truncates_bounded, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot exact fit",
                            test_panic_snap_exact_fit, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot cap 1",
                            test_panic_snap_cap_one, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot zero cap writes nothing",
                            test_panic_snap_zero_cap_writes_nothing, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk records chain",
                            test_panic_frames_walks_chain, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk withheld in NMI",
                            test_panic_frames_nmi_returns_zero, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: snapshot recovers a real fault",
                            test_panic_snap_recovers_real_fault, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk recovers a real fault",
                            test_panic_frames_recovers_real_fault, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk clamps oversized count",
                            test_panic_frames_clamps_count, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk honours a small count",
                            test_panic_frames_honours_small_count, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk zero count",
                            test_panic_frames_zero_count, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk rejects span wrap",
                            test_panic_frames_rejects_span_wrap, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk rejects RBP above span",
                            test_panic_frames_rejects_above_span, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk rejects misaligned RBP",
                            test_panic_frames_rejects_misaligned, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk rejects RBP below RSP",
                            test_panic_frames_rejects_below_sp, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk breaks on cycle",
                            test_panic_frames_breaks_on_cycle, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk NULL frame walks live stack",
                            test_panic_frames_null_frame_walks_live, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: frame walk NULL output",
                            test_panic_frames_null_out, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
