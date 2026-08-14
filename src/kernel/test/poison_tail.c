/* ============================================================================
 * poison_tail.c -- poisoned-boundary fixture for kernel string helpers
 *
 * See include/kernel/test/poison_tail.h for the contract, the reserved-VA
 * rationale, and the rule about which helpers this fixture may be aimed at.
 *
 * XREF: 00-infrastructure/TODO-03-kernel-test-harness.md section 11
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/poison_tail.h"
#include "kernel/test/test.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/task.h"
#include "kernel/except.h"
#include "kernel/klog.h"
#include "libc/string.h"

/* ---- One-time reservation and single-owner state -------------------------
 *
 * The data page is mapped ONCE and never unmapped, so its translation can
 * never change meaning on a CPU that cached it (poison_tail.h explains why
 * that matters).
 *
 * There is exactly ONE data page, so there can be exactly ONE armed fixture.
 * `s_owner` enforces that: a second arm is refused rather than silently
 * overwriting the first, because an overlapping arm places a SHORTER string
 * and leaves the older fixture's `str` pointing into zero-filled bytes -- a
 * helper reading past that early NUL then stays inside the page and is
 * reported CLEAN. A false green is the one outcome this fixture may never
 * produce, so overlap is refused, not merged.
 *
 * `s_generation` makes stale cleanup harmless: a disarm carrying an older
 * generation than the live arm is ignored instead of clearing a newer one. */
/* Lifecycle: FREE -> OWNED -> CLEANING -> FREE.
 *
 * CLEANING is not bookkeeping, it is the exclusion. Teardown clears 4 KiB and
 * must not do that under an IRQ-disabling lock, so the clear runs unlocked --
 * which means the state must still say "busy" throughout, or a second teardown
 * caller and a fresh arm can both proceed and the late clear wipes the new
 * arm's string. A helper reading past the resulting early NUL then stays inside
 * the mapped page and is reported CLEAN: a false green, which is the one
 * outcome this fixture may never produce. Exactly one caller wins the
 * OWNED -> CLEANING transition under the lock; everyone else backs off. */
#define POISON_STATE_FREE      0
#define POISON_STATE_OWNED     1
#define POISON_STATE_CLEANING  2

static DEFINE_SPINLOCK(s_poison_lock);
static uintptr_t s_poison_frame;    /* 0 until the data page is mapped */
static int       s_poison_failed;   /* permanent latch: fixture unusable */
static int       s_state;           /* POISON_STATE_* */
static const struct test_poison_tail *s_owner;   /* the one armed fixture */
static uint32_t  s_generation;      /* bumped on every successful arm */
static uint32_t  s_probes;          /* probes currently running on the page */
static int       s_teardown_pending;      /* a teardown deferred behind a pin */
static uint32_t  s_pending_generation;    /* which arm that deferred teardown is for */

/* Is the boundary page genuinely absent?
 *
 * vmm_query_flags() rather than vmm_get_physical(): the latter is documented
 * "Returns 0 if not mapped" (include/kernel/mm/vmm.h:122) and so cannot
 * distinguish an absent page from one mapped to physical frame 0. Presence is
 * the property that decides whether this fixture can detect anything at all,
 * so it is read from the PTE's Present bit and from nothing else. */
static int poison_boundary_is_absent(void)
{
    return (vmm_query_flags(TEST_POISON_BOUNDARY_VA) & VMM_FLAG_PRESENT) == 0;
}

/* Latch the fixture permanently unusable. */
static void poison_tail_fail_permanently(void)
{
    uint64_t flags;

    spin_lock_irqsave(&s_poison_lock, &flags);
    s_poison_failed = 1;
    spin_unlock_irqrestore(&s_poison_lock, flags);
}

/* Map the data page on first use and confirm the boundary is absent. Returns 0
 * on success, -1 when the fixture is unusable.
 *
 * Runs WITHOUT the lock, and may only be called by the CPU that has already
 * claimed ownership. Allocating a frame, walking and populating page tables,
 * and clearing 4 KiB are all unbounded work; doing them under an
 * interrupt-disabling spinlock would violate the lock-hold-time rule for a
 * mutual exclusion that ownership already provides. The lock is taken only to
 * publish the resulting flags.
 *
 * The boundary check runs on EVERY arm, not only the first: a mapping
 * installed later would silently turn the detector off, and the readiness
 * flag is never allowed to short-circuit that check. */
static int poison_tail_reserve_owned(void)
{
    uintptr_t frame;

    if (!s_poison_frame) {
        frame = pmm_alloc_frame();
        if (!frame) {
            poison_tail_fail_permanently();
            return -1;
        }

        /* Kernel-only, writable, NX: a data page that is never executed and
         * never reachable from ring 3. The page AFTER it is deliberately left
         * absent -- that absence IS the fixture. */
        if (vmm_map_page(TEST_POISON_DATA_VA, frame,
                         VMM_KERNEL_RW | VMM_FLAG_NX) != 0) {
            pmm_free_frame(frame);
            poison_tail_fail_permanently();
            return -1;
        }
        s_poison_frame = frame;
    }

    /* Fail CLOSED and PERMANENTLY. A mapped boundary means an overread would
     * read ordinary memory and be reported clean, so the fixture must never
     * hand out another arm once it has seen this. */
    if (!poison_boundary_is_absent()) {
        poison_tail_fail_permanently();
        return -1;
    }
    return 0;
}

/* Tear down the arm identified by `generation`, exclusively.
 *
 * The single teardown path for BOTH explicit disarm and the drained cleanup
 * action, so the two cannot race each other. Returns 1 if this caller won the
 * transition and did the clearing, 0 if the arm is not ours (or already being
 * torn down), and -1 if a probe is pinned and the caller should retry later.
 *
 * Generation-scoped rather than "release whoever owns": every arm registers its
 * own action, so a suite that armed more than once (or whose arm was refused
 * while another generation was live) would otherwise have a stale action
 * releasing an arm it never made. */
static int poison_tail_teardown(uint32_t generation)
{
    uint64_t flags;

    spin_lock_irqsave(&s_poison_lock, &flags);
    if (s_state != POISON_STATE_OWNED || s_generation != generation) {
        /* Not ours, or someone else is already clearing it. */
        spin_unlock_irqrestore(&s_poison_lock, flags);
        return 0;
    }
    /* DEFER while any probe is pinned; never wait for one, and never drop the
     * request on the floor.
     *
     * A pinned probe is still reading the string, so clearing under it would
     * hand it an early NUL and let an overreading helper stay inside the mapped
     * page -- the false green. Waiting would fix that and introduce something
     * worse: a probe CALLBACK may disarm its own fixture, and it would then
     * wait on a pin it is itself holding, hanging the test boot.
     *
     * Refusing outright is not enough either: the cleanup action fires exactly
     * once at suite drain, so a refusal there would be the only retry and the
     * fixture would stay OWNED for the rest of the boot -- the detector
     * silently gone, which is the disease. So the intent is RECORDED and the
     * last probe to drop its pin completes it. */
    if (s_probes) {
        s_teardown_pending   = 1;
        s_pending_generation = generation;
        s_state              = POISON_STATE_OWNED;
        spin_unlock_irqrestore(&s_poison_lock, flags);
        return -1;
    }

    s_state = POISON_STATE_CLEANING;   /* arms and new probes both blocked */
    spin_unlock_irqrestore(&s_poison_lock, flags);

    /* Unlocked, but exclusive: no other teardown can be in CLEANING, no arm
     * can claim, and no probe is running until this publishes FREE below. */
    if (s_poison_frame)
        memset((void *)TEST_POISON_DATA_VA, 0, VMM_PAGE_SIZE);

    spin_lock_irqsave(&s_poison_lock, &flags);
    s_owner              = (const struct test_poison_tail *)0;
    s_state              = POISON_STATE_FREE;
    s_teardown_pending   = 0;   /* satisfied by this teardown */
    s_pending_generation = 0;
    spin_unlock_irqrestore(&s_poison_lock, flags);
    return 1;
}

/* Test-scoped cleanup. Carries NO context: the caller's fixture lives on the
 * suite's stack and the runner drains actions AFTER that stack frame is gone
 * (include/kernel/test/test.h:345), so dereferencing it here would be a
 * use-after-return that writes through whatever now occupies those bytes.
 * Releasing ownership needs only the global state. */
/* The context is the arm's GENERATION, not a pointer: the caller's fixture
 * lives on the suite's stack and the runner drains actions after that frame is
 * gone (include/kernel/test/test.h:345), so a pointer here would be a
 * use-after-return. A generation is a value, and it is what makes this action
 * release its own arm and no other. */
static void poison_tail_cleanup(void *ctx)
{
    (void)poison_tail_teardown((uint32_t)(uintptr_t)ctx);
}

int test_poison_tail_arm(struct test_poison_tail *pt, const char *s)
{
    uint64_t flags;
    size_t   len;
    char    *dst;
    int      reserved;
    uint32_t generation;

    /* A REFUSED arm must leave `pt` exactly as it found it. Clearing the
     * fields up front looks tidy and is a live defect: the owner re-arming
     * without disarming is a refusal, and wiping its `str`/`armed` there
     * silently destroys the arm that is still holding the page. Nothing below
     * writes through `pt` until the arm has actually succeeded. */
    if (!pt || !s)
        return TEST_POISON_UNAVAILABLE;

    if (!thread_current())
        return TEST_POISON_UNAVAILABLE;   /* kernel SEH needs a current thread */

    len = strlen(s);
    if (len + 1u > (size_t)VMM_PAGE_SIZE)
        return TEST_POISON_UNAVAILABLE;   /* cannot end at the boundary */

    /* Claim ownership under a SHORT critical section: flag reads and a few
     * stores, no allocation and no page work. Everything expensive happens
     * afterwards, protected by the state machine this just entered rather than
     * by an interrupt-disabling lock. */
    spin_lock_irqsave(&s_poison_lock, &flags);
    if (s_poison_failed || s_state != POISON_STATE_FREE) {
        int busy = (s_state != POISON_STATE_FREE);
        spin_unlock_irqrestore(&s_poison_lock, flags);
        klog(LOG_WARN, "test", "poison-tail: arm refused (%s)",
             busy ? "another fixture is armed" : "fixture latched unusable");
        return TEST_POISON_UNAVAILABLE;
    }
    s_state    = POISON_STATE_OWNED;
    s_owner    = pt;
    generation = ++s_generation;
    spin_unlock_irqrestore(&s_poison_lock, flags);

    /* Register the cleanup net IMMEDIATELY after the claim and BEFORE anything
     * that can fault. Everything below -- allocation, page-table population,
     * the copies -- can take a fault, and a fault caught by an OUTER KI_TRY
     * resumes at that handler's landing pad WITHOUT unwinding this function,
     * so a rollback placed after that work would never run. Ownership would be
     * stranded for the rest of the boot, every later arm refused, and every
     * caller's refusal turned into TEST_SKIP: the overread coverage would
     * disappear without one failing assertion. Nothing between the claim and
     * this call can fault, so the window it leaves open is empty.
     *
     * The ticket is this arm's generation, so the action releases THIS arm and
     * never a later one. */
    if (test_add_action(poison_tail_cleanup,
                        (void *)(uintptr_t)generation) != 0) {
        (void)poison_tail_teardown(generation);
        klog(LOG_WARN, "test",
             "poison-tail: no cleanup slot -- refusing rather than stranding the fixture");
        return TEST_POISON_UNAVAILABLE;
    }

    /* Exclusive from here: no other fixture can be armed, so the page and the
     * one-time reservation belong to this caller alone. */
    reserved = poison_tail_reserve_owned();
    if (reserved != 0) {
        (void)poison_tail_teardown(generation);
        klog(LOG_ERROR, "test",
             "poison-tail: fixture unavailable -- it cannot detect an overread");
        return TEST_POISON_UNAVAILABLE;
    }

    /* Clear the whole page first: a leftover byte from a previous arm sitting
     * past the new NUL would absorb an overread and hide the very fault this
     * fixture exists to surface. */
    memset((void *)TEST_POISON_DATA_VA, 0, VMM_PAGE_SIZE);

    /* Place the string so its NUL lands on the LAST byte of the page. */
    dst = (char *)(TEST_POISON_DATA_VA + VMM_PAGE_SIZE - (uintptr_t)(len + 1u));
    memcpy(dst, s, len);
    dst[len] = '\0';

    pt->str        = dst;
    pt->len        = (uint32_t)len;
    pt->generation = generation;
    pt->armed      = 1;
    return 0;
}

void test_poison_tail_disarm(struct test_poison_tail *pt)
{
    if (!pt || !pt->armed)
        return;

    /* Same exclusive path the drained cleanup action takes, keyed on this
     * fixture's own generation -- so a superseded disarm cannot clear a newer
     * arm, and a disarm racing the action cannot double-clear. */
    /* A -1 means a probe is pinned (typically this call came from inside a
     * probe callback), so the teardown was RECORDED and the last pin to drop
     * will complete it. The disarm still takes effect, just not synchronously,
     * so the fixture fields are cleared either way. */
    (void)poison_tail_teardown(pt->generation);

    pt->str        = (char *)0;
    pt->len        = 0;
    pt->generation = 0;
    pt->armed      = 0;
}

int test_poison_tail_probe(struct test_poison_tail *pt,
                           void (*fn)(void *), void *ctx)
{
    struct thread *t;
    uint8_t       *saved_base;
    uint32_t       saved_size;
    volatile int   faulted = 0;
    uintptr_t      fault_addr = 0;
    uint64_t       flags;
    uint32_t       deferred = 0;
    int            run_deferred = 0;

    if (!pt || !pt->armed || !fn)
        return TEST_POISON_UNAVAILABLE;

    t = thread_current();
    if (!t)
        return TEST_POISON_UNAVAILABLE;

    /* PIN the arm for the whole callback. `pt->armed` alone is not enough: it
     * is cleared only after teardown returns, so a probe could start (or keep
     * running) while a concurrent disarm zeroes the page. Pinning validates the
     * LIVE state and generation, and teardown waits for the pin to drop before
     * it clears -- so the string a probe reads cannot change underneath it. */
    spin_lock_irqsave(&s_poison_lock, &flags);
    if (s_state != POISON_STATE_OWNED || s_owner != pt ||
        s_generation != pt->generation) {
        spin_unlock_irqrestore(&s_poison_lock, flags);
        return TEST_POISON_UNAVAILABLE;
    }
    s_probes++;
    spin_unlock_irqrestore(&s_poison_lock, flags);

    test_seh_open_window(t, &saved_base, &saved_size);
    {
        KI_EXCEPTION_FRAME(reg);
        KI_TRY(reg)
            fn(ctx);
        KI_EXCEPT(reg)
            faulted    = 1;
            fault_addr = (uintptr_t)KI_EXCEPTION_ADDR(reg);
        KI_END_TRY;
    }
    test_seh_close_window(t, saved_base, saved_size);

    /* Always reached, including on the caught-fault path: KI_EXCEPT resumes
     * inside THIS function, so the pin cannot be leaked by a fault.
     *
     * The LAST pin to drop owns any teardown that was deferred behind it. The
     * cleanup action fires only once, so without this the deferred request
     * would be lost and the fixture would stay OWNED for the rest of the boot
     * -- the detector gone with nothing left to notice. */
    spin_lock_irqsave(&s_poison_lock, &flags);
    if (s_probes)
        s_probes--;
    if (!s_probes && s_teardown_pending) {
        deferred            = s_pending_generation;
        run_deferred        = 1;
        s_teardown_pending  = 0;
    }
    spin_unlock_irqrestore(&s_poison_lock, flags);

    if (run_deferred)
        (void)poison_tail_teardown(deferred);

    if (!faulted)
        return TEST_POISON_NO_FAULT;

    /* Only a fault ON the boundary page is an overread. Anything else is a
     * different bug wearing the same costume, and saying so is the difference
     * between a diagnosis and a guess. */
    if (fault_addr >= TEST_POISON_BOUNDARY_VA &&
        fault_addr <  TEST_POISON_BOUNDARY_VA + (uintptr_t)VMM_PAGE_SIZE)
        return TEST_POISON_OVERREAD;

    klog(LOG_WARN, "test", "poison-tail: probe faulted at %p, not the boundary",
         (void *)fault_addr);
    return TEST_POISON_OTHER_FAULT;
}

/* ---- Kernel-SEH stack window --------------------------------------------
 *
 * The window has to bracket the CALLER's frame, which sits at a HIGHER address
 * than this function's own. Reaching 16 KiB below the current SP and 16 KiB
 * above it covers both with room for the probe's callee frames. */
uintptr_t test_seh_open_window(struct thread *t, uint8_t **saved_base,
                               uint32_t *saved_size)
{
    volatile uint8_t probe = 0;
    uintptr_t sp = (uintptr_t)&probe;

    if (!t || !saved_base || !saved_size)
        return sp;

    *saved_base = t->stack_base;
    *saved_size = t->stack_size;
    t->stack_base = (uint8_t *)(sp - TEST_SEH_WINDOW_BELOW);
    t->stack_size = TEST_SEH_WINDOW_SIZE;
    return sp;
}

void test_seh_close_window(struct thread *t, uint8_t *saved_base,
                           uint32_t saved_size)
{
    if (!t)
        return;

    t->stack_base = saved_base;
    t->stack_size = saved_size;
}

#endif /* KERNEL_TESTS */
