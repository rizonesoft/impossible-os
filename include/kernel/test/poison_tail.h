/* ============================================================================
 * poison_tail.h -- poisoned-boundary fixture for kernel string helpers
 *
 * Makes a one-byte buffer OVERREAD fail a single assertion instead of killing
 * the boot. A string is placed so its terminating NUL is the LAST readable
 * byte in the address space region: the very next byte is a page that is never
 * mapped on any CPU, so a helper that indexes one past the NUL takes a kernel
 * #PF, which kernel SEH catches and the fixture reports as a failed assertion.
 *
 * WHY THIS EXISTS. An out-of-bounds READ that still computes the right answer
 * is invisible to every C-level assertion a test can write. The klog renderer
 * read `subsystem[4]` and `subsystem[5]` unconditionally to locate a tag
 * terminator, reading past the end of every ordinary short tag ("ob", "irq"),
 * and the classification result was identical either way -- so the
 * behavior-preservation suite that shipped with the fix passed against the
 * broken code too. Only a fault can tell the two implementations apart.
 *
 * WHAT THIS FIXTURE MAY BE POINTED AT. Ordinary NUL-terminated-string helpers
 * whose contract is that the object ends exactly at the NUL. It must NOT be
 * used on APIs that legitimately require readable tail padding, perform
 * fixed-width or documented SIMD overreads, or otherwise promise the caller a
 * buffer larger than the string -- for those a boundary fault is correct
 * behavior, not a defect, and the fixture would report a false positive.
 *
 * XREF: 00-infrastructure/TODO-03-kernel-test-harness.md section 11
 * ============================================================================ */

#ifndef KERNEL_TEST_POISON_TAIL_H
#define KERNEL_TEST_POISON_TAIL_H

#include "kernel/types.h"
#include "kernel/mm/memmap.h"
#include "kernel/mm/vmm.h"

struct thread;

/* ---- Reserved VA ---------------------------------------------------------
 *
 * A 2 MiB reservation carved from the TOP of the MMIO/fixmap window, so a
 * future MMIO allocator growing from MM_MMIO_BASE never reaches it. Only the
 * FIRST 4 KiB page of the reservation is ever mapped; every other page in it,
 * the boundary page included, stays absent for the life of the boot.
 *
 * The carve is deliberate, and the alternative was measured and rejected:
 * building the boundary out of a pmm_alloc_contiguous() pair in the low
 * identity map would require vmm_split_huge_page() on a live 2 MiB identity
 * PDE. That split publishes its replacement PDE as PRESENT|WRITABLE only
 * (src/kernel/mm/vmm.c:1652), dropping the window's User bit for all 512 pages
 * under it, takes no page-table lock, and is never reversed -- a permanent
 * address-space demotion in exchange for a test fixture.
 *
 * Reserving a whole 2 MiB (rather than two pages) keeps the fixture inside its
 * own page table, so it can never share a PD entry with a future MMIO mapping.
 *
 * Two properties make this shape immune to the stale-TLB false GREEN that a
 * recycled mapping would carry: the boundary page is never mapped on any CPU,
 * so no translation for it can exist to go stale; and the data page is mapped
 * ONCE to ONE frame and never unmapped, so its translation never changes
 * meaning either. vmm_flush_tlb() is a local invlpg with no shootdown, which
 * is exactly why neither VA is allowed to be recycled. */
#define TEST_POISON_WINDOW_SIZE   MM_SIZE_2MIB
#define TEST_POISON_WINDOW_BASE   (MM_MMIO_END - TEST_POISON_WINDOW_SIZE)

/* The mapped data page, and the first byte past it (never mapped). */
#define TEST_POISON_DATA_VA       TEST_POISON_WINDOW_BASE
#define TEST_POISON_BOUNDARY_VA   (TEST_POISON_DATA_VA + VMM_PAGE_SIZE)

_Static_assert(TEST_POISON_WINDOW_BASE >= MM_MMIO_BASE,
               "poison window must start inside the MMIO/fixmap window");
_Static_assert(MM_MMIO_SIZE > TEST_POISON_WINDOW_SIZE,
               "the MMIO/fixmap window must be larger than the carve taken from it");
_Static_assert((TEST_POISON_WINDOW_BASE & MM_MASK_2MIB) == 0ULL,
               "poison window must be 2 MiB aligned so it owns its own PD entry");
_Static_assert(TEST_POISON_BOUNDARY_VA <
               TEST_POISON_WINDOW_BASE + TEST_POISON_WINDOW_SIZE,
               "the boundary page must lie inside the reservation");
_Static_assert(VMM_PAGE_SIZE < TEST_POISON_WINDOW_SIZE,
               "the reservation must be larger than the one page it maps");

/* ---- Fixture state ------------------------------------------------------- */

/* There is exactly ONE data page, so exactly ONE fixture may be armed at a
 * time. An overlapping arm is REFUSED rather than merged: a second, shorter
 * string would leave the first fixture's `str` pointing into zero-filled bytes,
 * and a helper reading past that early NUL would stay inside the page and be
 * reported CLEAN. Refusing is the only direction that cannot produce a false
 * green. `generation` lets a superseded disarm identify itself and decline to
 * clear a newer arm. */
struct test_poison_tail {
    char     *str;        /* the placed string; its NUL is the last readable byte */
    uint32_t  len;        /* strlen(str) */
    uint32_t  generation; /* identifies this arm; 0 when unarmed */
    int       armed;      /* 0 when unarmed; arms do not nest */
};

/* Probe outcomes. */
#define TEST_POISON_NO_FAULT     0   /* the helper stayed inside the string */
#define TEST_POISON_OVERREAD     1   /* it faulted ON the boundary page */
#define TEST_POISON_OTHER_FAULT  2   /* it faulted somewhere else entirely */
#define TEST_POISON_UNAVAILABLE (-1) /* fixture unusable; the test should SKIP */

/* Place `s` so its NUL is the last readable byte before the boundary page.
 * Returns 0 on success, or TEST_POISON_UNAVAILABLE when the string does not
 * fit in one page, no current thread exists, another fixture is already armed,
 * or the fixture cannot guarantee detection (no frame, the map refused, or the
 * boundary page turned out to be PRESENT -- which is latched permanently,
 * because a mapped boundary means an overread would read ordinary memory and
 * be reported clean). On success a context-free test-scoped cleanup action is
 * registered, so a suite that returns early still releases ownership.
 *
 * A refused arm leaves `pt` untouched, so an owner that re-arms by mistake
 * does not lose the arm it is still holding. Callers must therefore check the
 * return value rather than inspecting `pt->armed` after a failed call. */
int test_poison_tail_arm(struct test_poison_tail *pt, const char *s);

/* Clear the fixture and release ownership. Idempotent, and safe to call from a
 * superseded fixture: only the live owner clears the shared page. The reserved
 * frame stays mapped by design -- see the stale-translation note above. */
void test_poison_tail_disarm(struct test_poison_tail *pt);

/* Run `fn(ctx)` with kernel SEH armed and report whether it read past the NUL.
 *
 * `fn` MUST NOT arm this fixture. It MAY disarm it, but the disarm is DEFERRED:
 * the probe pins the arm for the whole callback so a concurrent teardown cannot
 * clear the page underneath it, and a teardown requested while that pin is held
 * is recorded and completed by the last probe to drop its pin. Teardown never
 * waits on a pin, so a callback that disarms cannot deadlock.
 * Returns one of the TEST_POISON_* outcomes above. A fault anywhere other than
 * the boundary page is reported as TEST_POISON_OTHER_FAULT rather than being
 * misattributed to an overread. */
int test_poison_tail_probe(struct test_poison_tail *pt,
                           void (*fn)(void *), void *ctx);

/* Assert that `fn(ctx)` did not read past the NUL. A non-overread fault is
 * reported under its own message so the two failures are never confused. */
#define TEST_POISON_TAIL_ASSERT_NO_OVERREAD(pt, fn, ctx, msg)                 \
    do {                                                                      \
        int _tp_rc = test_poison_tail_probe((pt), (fn), (ctx));               \
        TEST_ASSERT(_tp_rc != TEST_POISON_OVERREAD, (msg));                   \
        TEST_ASSERT(_tp_rc != TEST_POISON_OTHER_FAULT,                        \
                    "the probe faulted away from the poisoned boundary");     \
        TEST_ASSERT(_tp_rc != TEST_POISON_UNAVAILABLE,                        \
                    "the poisoned-boundary probe could not run");             \
    } while (0)

/* ---- Kernel-SEH stack window --------------------------------------------
 *
 * ki_seh_register() only publishes a registration node that lies inside the
 * current thread's tracked stack window (src/kernel/except.c:555), and the
 * boot thread the test runner uses does not track one -- so an unbracketed
 * KI_TRY on it silently provides ZERO protection and the fault stays terminal.
 * These bracket the caller's frame and restore the previous window. Shared so
 * the fixture and the SEH suite cannot drift apart.
 *
 * open returns the stack pointer the window is centred on, which the SEH suite
 * uses to synthesize plausible trap-frame RSP values. The window reaches below
 * that SP for the frames the probe will push, and above it for the caller's own
 * frame, which sits at a higher address. */
#define TEST_SEH_WINDOW_BELOW  0x4000u   /* 16 KiB below the captured SP */
#define TEST_SEH_WINDOW_SIZE   0x8000u   /* 32 KiB window centred on it */

uintptr_t test_seh_open_window(struct thread *t, uint8_t **saved_base,
                               uint32_t *saved_size);
void test_seh_close_window(struct thread *t, uint8_t *saved_base,
                           uint32_t saved_size);

#endif /* KERNEL_TEST_POISON_TAIL_H */
