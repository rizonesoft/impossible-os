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
#include "kernel/smp.h"           /* smp_cpu_by_apic_id -- GS-independent slot lookup */
#include "kernel/drivers/serial.h"   /* PANIC_CTX_* -- snapshot context table */
#include "kernel/mm/vmm.h"           /* vmm_get_physical: find an unmapped VA safely */
#include "kernel/mm/pmm.h"           /* two-frame fixture for the mid-copy fault */
#include "kernel/cpu_security.h"     /* __kstr_read_guarded + KSTR_STOP_* */

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

/* Return addresses must lie inside the kernel text window: the walker applies
 * the same code-PC test the RTL walker does (rtlp_is_code_pc), so a made-up
 * 0xFFFFFFFF8000xxxx constant is correctly REJECTED and a fixture built from
 * one measures nothing. Offsets into a real function in this translation unit
 * give distinct, valid PCs -- td_streq is a loop, comfortably larger than the
 * offsets used here, and anything past it is still kernel text. */
static uint64_t td_pc(uint32_t i)
{
    return (uint64_t)(uintptr_t)&td_streq + (uint64_t)(i * 2u);
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
    chain[1] = td_pc(0u);                        /* return address */
    chain[2] = 0u;                               /* chain terminator */
    chain[3] = td_pc(1u);

    td_fill_frame(&f, (uint64_t)(uintptr_t)&chain[0],
                  (uint64_t)(uintptr_t)&chain[0]);

    n = panic_capture_frames(&f, PANIC_CTX_NORMAL, out, 4u);
    TEST_ASSERT_EQ((uint64_t)n, 2u, "walker records both synthetic frames");
    TEST_ASSERT_EQ(out[0], td_pc(0u), "frame 0 return address");
    TEST_ASSERT_EQ(out[1], td_pc(1u), "frame 1 return address");
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
    chain[1] = td_pc(0u);
    chain[2] = 0u;
    chain[3] = td_pc(1u);
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
        chain[i * 2u + 1u] = td_pc(i);               /* return address */
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
    TEST_ASSERT_EQ(out[PANIC_MAX_STACK_DEPTH - 1u], td_pc(15u),
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
    TEST_ASSERT_EQ(out[0], td_pc(0u), "and it is the innermost one");
    TEST_ASSERT_EQ(out[1], 0xD1D1D1D1D1D1D1D1ull, "nothing past the requested count");
}

static void test_panic_frames_zero_count(void)
{
    uint64_t chain[2];
    uint64_t out[1];
    struct interrupt_frame f;

    chain[0] = 0u;
    chain[1] = td_pc(0u);
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
    chain[1] = td_pc(0u);
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
    chain[1] = td_pc(0u);
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
    chain[1] = td_pc(0u);
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
    chain[1] = td_pc(0u);
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

/* THE MID-COPY FAULT -- the invariant the bounded guarded string loop exists
 * for, and the one no other case can reach.
 *
 * test_panic_snap_recovers_real_fault above faults on the FIRST byte, so it
 * proves the fixup is wired but says nothing about the index: a clobbered
 * counter, a misplaced increment, or an increment that ran BEFORE the store
 * would all still produce an empty result there. What has to hold is that a
 * fault after N successful bytes leaves exactly those N bytes, terminates at
 * dst[N], and reports N -- because that prefix is the only crash evidence the
 * machine gets when a panic description is itself corrupt.
 *
 * The fixture is a two-frame contiguous allocation with a guard page installed
 * over the SECOND frame, so the walk starts on mapped memory the test owns and
 * runs off the end of it. The guarded-load fixup is consulted ahead of
 * guard_page_lookup in page_fault_handler, so this recovers rather than
 * reporting a guard hit. Same technique as the guard cases in test_vmm.c. */
static void test_kstr_read_guarded_mid_copy_fault(void)
{
    uintptr_t base = pmm_alloc_contiguous(2u);
    char      dst[32];
    uint32_t  stop = 0xFFu;
    uint32_t  n;
    char     *page1_end;
    uint32_t  i;
    int       rc;

    TEST_ASSERT(base != 0, "two-frame fixture allocated");
    if (!base)
        return;

    /* Five readable bytes ending exactly at the frame boundary, so byte six is
     * the first address on the guarded page. No NUL: the walk must be stopped
     * by the fault, not by a terminator. */
    page1_end = (char *)(base + 4096u) - 5;
    for (i = 0u; i < 5u; i++)
        page1_end[i] = (char)('A' + i);

    rc = vmm_install_guard_page(base + 4096u, "TEST: kstr mid-copy fault");
    if (rc != VMM_GUARD_OK) {
        /* Only UNAVAILABLE leaves the run PMM-safe. VMM_GUARD_VA_UNSAFE means
         * the VA is not confirmed reusable (vmm.h:227), so the run is
         * QUARANTINED rather than freed -- handing such a frame back would give
         * the next owner an identity address that writes into nothing, into
         * another frame, or into a read-only page. */
        if (rc == VMM_GUARD_UNAVAILABLE)
            pmm_free_contiguous(base, 2u);
        TEST_SKIP("no guard slot available for the mid-copy fault fixture");
        return;
    }

    for (i = 0u; i < sizeof dst; i++)
        dst[i] = 'Z';

    /* TEST-SIDE-EFFECT-ALLOWED: deliberately walks a readable prefix into a
     * guarded page so the bounded guarded load takes a real #PF mid-copy and
     * must be recovered by the fixup with its index intact -- the exact fault
     * the panic collector's string copy exists to survive. */
    n = __kstr_read_guarded(dst, page1_end, sizeof dst, &stop);

    TEST_ASSERT_EQ(stop, KSTR_STOP_FAULT,
                   "a mid-copy fault is reported as a fault, not a terminator");
    TEST_ASSERT_EQ(n, 5u, "and reports exactly the bytes it stored");
    TEST_ASSERT_EQ((uint32_t)dst[0], (uint32_t)'A', "prefix byte 0 preserved");
    TEST_ASSERT_EQ((uint32_t)dst[4], (uint32_t)'E', "prefix byte 4 preserved");
    TEST_ASSERT_EQ((uint32_t)dst[5], 0u, "the prefix is terminated at dst[n]");
    TEST_ASSERT_EQ((uint32_t)dst[6], (uint32_t)'Z',
                   "and nothing past the terminator was written");

    /* TEST_ASSERT_EQ does not abort, so the free must be gated on the RESULT,
     * not merely follow the assertion: vmm_uninstall_guard_page returns 0 only
     * when the VA is confirmed to map itself Present+Writable, and the frame
     * must not be freed on any other outcome (vmm.h:249). A refused uninstall
     * fails this test AND quarantines the run, which is the safe pair. */
    rc = vmm_uninstall_guard_page(base + 4096u);
    TEST_ASSERT_EQ(rc, 0, "guard page removed after the fixture");
    if (rc == 0)
        pmm_free_contiguous(base, 2u);
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

/* ---- Async-worker park: disposition table and staged cleanup (S30) ----
 *
 * Both helpers are pure over their arguments, which is why they exist as
 * helpers: the behaviour they encode is what a second fault on a parking CPU
 * does, and there is no way to schedule a real nested abort into the middle of
 * the panic tail from a unit test. The live branch in panic.c is verified by
 * the multi-platform boot matrix; the DECISIONS it makes are verified here. */

static void test_park_async_worker_always_isolates(void)
{
    /* A CPU still identifying as an async worker has not published, so it owes
     * the publication no matter how far a stage word claims to have got. This
     * is the row that stops a stale stage talking the branch out of telling the
     * BSP anything -- the BSP would otherwise sit out its whole barrier. */
    TEST_ASSERT_EQ(panic_async_disposition(1, PANIC_PARK_NONE),
                   PANIC_ASYNC_ISOLATE,
                   "first entry on an async worker isolates and publishes");
    TEST_ASSERT_EQ(panic_async_disposition(1, PANIC_PARK_DIAGNOSED),
                   PANIC_ASYNC_ISOLATE,
                   "in_async_work outranks even a fully advanced stage");
}

static void test_park_not_parking_is_terminal(void)
{
    TEST_ASSERT_EQ(panic_async_disposition(0, PANIC_PARK_NONE),
                   PANIC_ASYNC_TERMINAL,
                   "an ordinary CPU with no park stage takes terminal arbitration");
}

static void test_park_pre_publication_still_isolates(void)
{
    /* THE INTERVAL THE SECTION EXISTS FOR. in_async_work has been cleared but
     * the completion is not out yet, so a nested abort here must still run the
     * publishing tail rather than reboot a machine that is recovering. */
    TEST_ASSERT_EQ(panic_async_disposition(0, PANIC_PARK_ENTERED),
                   PANIC_ASYNC_ISOLATE,
                   "nested abort before the publication settles still owes it");
}

static void test_park_post_publication_parks_quietly(void)
{
    /* The boundary is the PUBLISHED stage exactly, not the park: once the BSP
     * has been told, re-running the publication would write a completion into a
     * slot this CPU has given up. */
    TEST_ASSERT_EQ(panic_async_disposition(0, PANIC_PARK_PUBLISHED),
                   PANIC_ASYNC_PARK,
                   "the publication stage is the boundary between isolate and park");
    TEST_ASSERT_EQ(panic_async_disposition(0, PANIC_PARK_DIAGNOSED),
                   PANIC_ASYNC_PARK,
                   "and every later stage parks without republishing");
}

static void test_park_claim_is_at_most_once(void)
{
    volatile uint8_t slot = PANIC_PARK_NONE;

    TEST_ASSERT_EQ(panic_park_claim_step(&slot, PANIC_PARK_ENTERED), 1,
                   "a fresh slot grants the first step");
    TEST_ASSERT_EQ((uint32_t)slot, PANIC_PARK_ENTERED,
                   "and records it before the caller performs it");
    TEST_ASSERT_EQ(panic_park_claim_step(&slot, PANIC_PARK_ENTERED), 0,
                   "the same step is never granted twice");
    TEST_ASSERT_EQ((uint32_t)slot, PANIC_PARK_ENTERED,
                   "and a refused claim leaves the slot untouched");
}

static void test_park_claim_never_moves_backwards(void)
{
    volatile uint8_t slot = PANIC_PARK_NONE;

    TEST_ASSERT_EQ(panic_park_claim_step(&slot, PANIC_PARK_PUBLISHED), 1,
                   "a nested entry may resume at a later step");
    TEST_ASSERT_EQ(panic_park_claim_step(&slot, PANIC_PARK_ENTERED), 0,
                   "an already-passed step is refused, never re-run");
    TEST_ASSERT_EQ((uint32_t)slot, PANIC_PARK_PUBLISHED,
                   "and the stage does not regress");
}

static void test_park_second_pass_claims_nothing(void)
{
    /* THE PROPERTY THAT BOUNDS THE RECURSION. Walk the whole ladder as the
     * first entry does, then walk it again as a nested abort would: the second
     * pass must perform no step at all, so a fault inside any step cannot make
     * the tail restart it, and the park is reached in a bounded number of
     * entries however many aborts land on the CPU. */
    static const uint32_t ladder[] = {
        PANIC_PARK_ENTERED, PANIC_PARK_PUBLISHED, PANIC_PARK_DIAGNOSED,
    };
    volatile uint8_t slot = PANIC_PARK_NONE;
    uint32_t granted = 0u;
    uint32_t i;

    for (i = 0u; i < (uint32_t)(sizeof ladder / sizeof ladder[0]); i++)
        granted += (uint32_t)panic_park_claim_step(&slot, ladder[i]);
    TEST_ASSERT_EQ(granted, (uint32_t)(sizeof ladder / sizeof ladder[0]),
                   "the first entry performs every step of the tail exactly once");

    granted = 0u;
    for (i = 0u; i < (uint32_t)(sizeof ladder / sizeof ladder[0]); i++)
        granted += (uint32_t)panic_park_claim_step(&slot, ladder[i]);
    TEST_ASSERT_EQ(granted, 0u,
                   "a nested entry re-walking the tail performs no step again");
    TEST_ASSERT_EQ((uint32_t)slot, PANIC_PARK_DIAGNOSED,
                   "and the stage rests at the end of the ladder");
}

static void test_park_claim_interrupted_at_every_stage(void)
{
    /* THE ORDERING A REAL NESTED ABORT PRODUCES, which the full-ladder replay
     * above does NOT cover: the fault lands after some INTERMEDIATE step was
     * claimed and before its action finished, and the nested entry then re-walks
     * the tail from the top. For every such interruption point the already-
     * claimed prefix must stay refused (so no step is ever performed twice, and
     * a step that faulted is never retried) and the whole unclaimed suffix must
     * still be granted exactly once (so the cleanup is not abandoned and the
     * park is still reached). Both halves are needed: refusing everything would
     * strand the UART lock and the crash record, and granting the prefix again
     * would restore the unbounded fault loop the stage exists to prevent. */
    static const uint32_t ladder[] = {
        PANIC_PARK_ENTERED, PANIC_PARK_PUBLISHED, PANIC_PARK_DIAGNOSED,
    };
    const uint32_t n = (uint32_t)(sizeof ladder / sizeof ladder[0]);
    uint32_t cut, i;

    for (cut = 0u; cut < n; cut++) {
        volatile uint8_t slot = PANIC_PARK_NONE;
        uint32_t granted = 0u;

        /* Interrupt exactly here: the first entry got as far as claiming
         * ladder[cut] and never returned from performing it. */
        for (i = 0u; i <= cut; i++)
            (void)panic_park_claim_step(&slot, ladder[i]);
        TEST_ASSERT_EQ((uint32_t)slot, ladder[cut],
                       "the interrupted entry leaves the stage at the step it claimed");

        /* The nested abort re-walks the whole tail from the top. */
        for (i = 0u; i < n; i++) {
            int got = panic_park_claim_step(&slot, ladder[i]);
            if (i <= cut)
                TEST_ASSERT_EQ(got, 0,
                               "a step the interrupted entry already claimed is never re-run");
            else
                TEST_ASSERT_EQ(got, 1,
                               "every step it had not reached is still performed once");
            granted += (uint32_t)got;
        }

        TEST_ASSERT_EQ(granted, n - cut - 1u,
                       "the nested entry performs exactly the unfinished suffix");
        TEST_ASSERT_EQ((uint32_t)slot, PANIC_PARK_DIAGNOSED,
                       "and the tail still completes, so the CPU reaches the park");
    }
}

static void test_park_publication_settles_after_its_stores(void)
{
    /* THE PUBLICATION IS RECORDED AFTER ITS STORES, NOT BEFORE, and this pins
     * the direction the error leans. An abort landing between the stores and
     * the claim leaves the stage at ENTERED, so the nested entry recomputes
     * ISOLATE and writes the completion again -- idempotent, and the BSP is told
     * either way. The opposite order would let the same abort leave a completion
     * RECORDED and never WRITTEN, and the BSP would wait out its whole barrier
     * deadline on a signal that was never sent. */
    volatile uint8_t slot = PANIC_PARK_NONE;

    (void)panic_park_claim_step(&slot, PANIC_PARK_ENTERED);
    TEST_ASSERT_EQ(panic_async_disposition(0, (uint32_t)slot),
                   PANIC_ASYNC_ISOLATE,
                   "an abort before the stores settle still owes the publication");

    TEST_ASSERT_EQ(panic_park_claim_step(&slot, PANIC_PARK_PUBLISHED), 1,
                   "the stores having run, the publication is recorded settled");
    TEST_ASSERT_EQ(panic_async_disposition(0, (uint32_t)slot),
                   PANIC_ASYNC_PARK,
                   "and every later entry parks instead of republishing");
}

static void test_guarded_append_never_walks_in_a_forbidden_context(void)
{
    /* THE FALLBACK IS A PLACEHOLDER, NOT THE POINTER. The park path's early
     * guards enter with PANIC_CTX_UNKNOWN and still render the async step name,
     * which is BSP-supplied; if the forbidden-context branch handed that pointer
     * to the raw appender, a corrupt one would fault while handling an NMI or a
     * double fault -- a triple fault and a machine reset before the UART is
     * handed back. The probe is the same non-canonical address the snapshot
     * tests use, so a regression FAULTS rather than quietly rendering. */
    char     buf[96];
    uint32_t pos = 0u;

    panic_append_guarded(buf, sizeof buf, &pos, TD_UNREADABLE_SRC,
                         PANIC_CTX_UNKNOWN);
    buf[pos] = '\0';
    TEST_ASSERT(td_streq(buf, PANIC_STR_NO_GUARD),
                "UNKNOWN context renders the placeholder, never walking the pointer");

    pos = 0u;
    panic_append_guarded(buf, sizeof buf, &pos, TD_UNREADABLE_SRC,
                         PANIC_CTX_NMI);
    buf[pos] = '\0';
    TEST_ASSERT(td_streq(buf, PANIC_STR_NO_GUARD),
                "and so does NMI context, which the park path also enters with");
}

static void test_guarded_append_placeholder_for_a_readable_pointer(void)
{
    /* THE GENERAL PROPERTY, not just the old regression. The non-canonical
     * probe above proves the direct raw-appender fallback would fault, but an
     * implementation that special-cased non-canonical addresses and raw-walked
     * everything else would pass it -- and a corrupt panic pointer is very often
     * canonical and merely unmapped. A perfectly READABLE literal in a forbidden
     * context must still render the placeholder, which no pointer-classifying
     * implementation can satisfy. */
    char     buf[96];
    uint32_t pos = 0u;

    panic_append_guarded(buf, sizeof buf, &pos, "definitely-readable",
                         PANIC_CTX_UNKNOWN);
    buf[pos] = '\0';
    TEST_ASSERT(td_streq(buf, PANIC_STR_NO_GUARD),
                "a readable string still renders the placeholder in UNKNOWN context");

    pos = 0u;
    panic_append_guarded(buf, sizeof buf, &pos, "definitely-readable",
                         PANIC_CTX_NORMAL);
    buf[pos] = '\0';
    TEST_ASSERT(td_streq(buf, "definitely-readable"),
                "and the same string IS copied where the guarded read is allowed");
}

static void test_guarded_append_holds_its_buffer_contract(void)
{
    char     buf[8];
    uint32_t pos;

    /* cap 0 must write NOTHING: the underlying appender's terminator store is
     * unconditional, so a zero cap used to put a byte past the buffer. */
    buf[0] = 0x5A;
    pos    = 0u;
    panic_append_guarded(buf, 0u, &pos, "x", PANIC_CTX_NORMAL);
    TEST_ASSERT_EQ((uint64_t)(uint8_t)buf[0], 0x5Au, "cap 0 leaves the buffer untouched");
    TEST_ASSERT_EQ((uint64_t)pos, 0u, "and does not advance the position");

    /* A position already at the end is the same hazard wearing a different
     * hat -- the terminator would land on buf[cap]. */
    buf[7] = 0x5A;
    pos    = 8u;
    panic_append_guarded(buf, 8u, &pos, "x", PANIC_CTX_NORMAL);
    TEST_ASSERT_EQ((uint64_t)pos, 8u, "a full buffer refuses to advance");
    TEST_ASSERT_EQ((uint64_t)(uint8_t)buf[7], 0x5Au, "and writes nothing");

    /* A cap smaller than the placeholder truncates inside the buffer. */
    pos = 0u;
    panic_append_guarded(buf, sizeof buf, &pos, "x", PANIC_CTX_UNKNOWN);
    TEST_ASSERT(pos < sizeof buf, "a short buffer truncates the placeholder");
    TEST_ASSERT_EQ((uint64_t)buf[pos], 0u, "and still terminates inside the buffer");
}

static void test_park_claim_rejects_a_step_outside_the_ladder(void)
{
    /* THE NARROWING THE 32-BIT COMPARE HIDES. The slot is 8 bits, so a step of
     * 256 passes `cur >= step` and then stores its low byte -- moving a fully
     * advanced stage back to NONE, which reads as "not parking" and would
     * reopen terminal arbitration in the middle of a park. */
    volatile uint8_t slot = PANIC_PARK_DIAGNOSED;

    TEST_ASSERT_EQ(panic_park_claim_step(&slot, 256u), 0,
                   "a step above the 8-bit slot is refused, not truncated");
    TEST_ASSERT_EQ((uint32_t)slot, PANIC_PARK_DIAGNOSED,
                   "and the stage does not fall back to NONE");
    TEST_ASSERT_EQ(panic_park_claim_step(&slot, PANIC_PARK_NONE), 0,
                   "claiming NONE is refused: it is the absence of a stage");
    TEST_ASSERT_EQ(panic_park_claim_step(&slot, PANIC_PARK_DIAGNOSED + 1u), 0,
                   "and so is one past the end of the ladder");
    TEST_ASSERT_EQ((uint32_t)slot, PANIC_PARK_DIAGNOSED,
                   "the slot is byte-identical after every refusal");
}

static void test_retract_mask_gate_is_panic_safe_domain(void)
{
    /* THE DECISIVE NEGATIVE, which live state cannot show: the two identity
     * derivations agree on every machine that runs this suite, so only a
     * fixture can prove the gate reads the panic-safe field rather than
     * lapic_id. A regression to the old comparison would pass everything else
     * and then, on firmware that remaps the LAPIC id, refuse the mask clear and
     * leave a parked CPU online for later async groups to select. */
    TEST_ASSERT_EQ(smp_retract_may_clear_mask(0x0Au + 1u, 0x0Au), 1,
                   "matching panic-safe identities permit the mask clear");
    TEST_ASSERT_EQ(smp_retract_may_clear_mask(0x0Bu + 1u, 0x0Au), 0,
                   "a slot publishing a different id is refused");
    TEST_ASSERT_EQ(smp_retract_may_clear_mask(0u, 0x0Au), 0,
                   "an unpublished slot is refused");
    TEST_ASSERT_EQ(smp_retract_may_clear_mask(0u, 0u), 0,
                   "including against APIC id 0, which the +1 encoding separates");
    TEST_ASSERT_EQ(smp_retract_may_clear_mask(1u, 0u), 1,
                   "while APIC id 0 published as 1 does permit it");
    TEST_ASSERT_EQ(smp_retract_may_clear_mask(0x0Au + 1u, 0x10Au), 1,
                   "the observed id is masked to 8 bits before comparison");
}

static void test_park_slot_resolves_without_gs(void)
{
    /* THE ROUND TRIP THE PARK PATH DEPENDS ON. Read-only over live per-CPU
     * state: the executing CPU derives its own panic-safe id from CPUID and must
     * find the slot that published that same id, with no GS involved. If this
     * fails, a nested abort cannot retract its CPU or publish its completion.
     *
     * Matched on panic_safe_id_plus1 rather than lapic_id deliberately: those
     * are different identity domains (CPUID leaf-1 initial apic id against the
     * MADT/LAPIC register id) and cpu_security.h is explicit that a panic-safe
     * id is only ever compared with another panic-safe id. */
    uint32_t             me   = cpu_panic_safe_apic_id();
    struct per_cpu_data *slot = smp_cpu_by_apic_id(me);

    TEST_ASSERT(slot != (struct per_cpu_data *)0,
                "the executing CPU resolves its own slot from its CPUID id");
    TEST_ASSERT_EQ(slot->panic_safe_id_plus1,
                   (me & CPU_PANIC_SAFE_ID_MASK) + 1u,
                   "and the slot carries exactly the id that was looked up");
    TEST_ASSERT(slot->panic_safe_id_plus1 != 0u,
                "a published slot is never 0, so an unpublished one cannot alias it");
}

static void test_park_claim_rejects_null_slot(void)
{
    /* The panic path is the worst place to fault out of a bookkeeping helper,
     * so a NULL slot is refused rather than dereferenced. */
    TEST_ASSERT_EQ(panic_park_claim_step((volatile uint8_t *)0,
                                         PANIC_PARK_ENTERED), 0,
                   "a NULL stage slot is refused, not dereferenced");
}

/* ---- Registration ---- */

void test_register_panic(void)
{
    test_suite_register_cat("Crash: async worker always isolates",
                            test_park_async_worker_always_isolates, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: no park stage means terminal arbitration",
                            test_park_not_parking_is_terminal, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: nested abort before publication isolates",
                            test_park_pre_publication_still_isolates, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: nested abort after publication parks",
                            test_park_post_publication_parks_quietly, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: park step is claimed at most once",
                            test_park_claim_is_at_most_once, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: park stage never moves backwards",
                            test_park_claim_never_moves_backwards, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: nested park pass performs no step again",
                            test_park_second_pass_claims_nothing, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: park resumes correctly from every interruption point",
                            test_park_claim_interrupted_at_every_stage, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: publication settles after its stores",
                            test_park_publication_settles_after_its_stores, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: guarded append never walks in a forbidden context",
                            test_guarded_append_never_walks_in_a_forbidden_context,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Crash: guarded append renders a placeholder for a readable pointer",
                            test_guarded_append_placeholder_for_a_readable_pointer,
                            TEST_CAT_BOOT);
    test_suite_register_cat("Crash: guarded append holds its buffer contract",
                            test_guarded_append_holds_its_buffer_contract, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: park claim rejects a step outside the ladder",
                            test_park_claim_rejects_a_step_outside_the_ladder, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: retract mask gate uses the panic-safe domain",
                            test_retract_mask_gate_is_panic_safe_domain, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: park slot resolves without GS",
                            test_park_slot_resolves_without_gs, TEST_CAT_BOOT);
    test_suite_register_cat("Crash: park claim rejects a NULL slot",
                            test_park_claim_rejects_null_slot, TEST_CAT_BOOT);

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
    test_suite_register_cat("Crash: guarded string keeps the prefix on a mid-copy fault",
                            test_kstr_read_guarded_mid_copy_fault, TEST_CAT_BOOT);
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
