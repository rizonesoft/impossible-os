/* ============================================================================
 * boot_stack.c -- consumer half of the loader-owned kernel boot stack.
 *
 * See include/kernel/mm/boot_stack.h for why this exists. In one line: early
 * boot used to execute on firmware memory that pmm_init hands back to the
 * allocator, so the kernel could be given its own live stack as free RAM.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/boot_halt.h"
#include "kernel/klog.h"
#include "kernel/mm/boot_stack.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/user_range.h"
#include "kernel/mm/vmm.h"

static struct boot_stack_info s_stack;
/* Set once vmm_install_guard_page() has CLEARED the guard PTE. After that
 * the guard region is unreadable, so nothing may scan it. */
static int s_guard_installed;

/* ---------------------------------------------------------------------------
 * Pure validation
 * ------------------------------------------------------------------------ */

int boot_stack_validate(uint64_t base, uint64_t size, uint64_t guard_size,
                        struct boot_stack_info *out,
                        enum boot_stack_error *out_err)
{
    enum boot_stack_error err = BOOT_STACK_ERR_OK;

    /* Order matters. Each check assumes the ones above it passed, so a zero
     * base is reported as ABSENT rather than falling through to a misleading
     * alignment or range verdict about address 0. */
    if (base == 0)
        err = BOOT_STACK_ERR_ABSENT;
    else if ((base & (uint64_t)(PMM_FRAME_SIZE - 1)) != 0)
        err = BOOT_STACK_ERR_UNALIGNED;
    else if (size == 0 || size > (uint64_t)BOOT_KSTACK_MAX_SIZE)
        /* Sanity band only. The real floor is on USABLE bytes below -- see
         * why this is not `size < BOOT_KSTACK_MIN_SIZE` there. */
        err = BOOT_STACK_ERR_SIZE_RANGE;
    else if ((size & (uint64_t)(PMM_FRAME_SIZE - 1)) != 0)
        err = BOOT_STACK_ERR_SIZE_GRAIN;
    else if (guard_size == 0)
        /* A zero guard passes grain and range and then ships a run with no
         * overflow detection at all, silently. The contract promises a
         * guarded stack; a producer that publishes no guard is not offering
         * one, and accepting it would make the guarantee unfalsifiable. */
        err = BOOT_STACK_ERR_GUARD_ABSENT;
    else if (guard_size != (uint64_t)BOOT_KSTACK_GUARD_BYTES)
        /* Exactly one page, not merely a page multiple. A wider guard would
         * move the boundary the stack actually grows through to the page
         * below base + guard_size, while the installer unmaps base -- so the
         * real boundary would stay mapped and an overflow would not fault. */
        err = BOOT_STACK_ERR_GUARD_GRAIN;
    else if (size <= guard_size ||
             size - guard_size < (uint64_t)BOOT_KSTACK_MIN_USABLE)
        /* THE floor, and it is on USABLE bytes rather than on the total run.
         * Guard bytes are not stack, so a floor applied to the total accepts
         * a geometry that overflows on the already-measured path.
         *
         * This is deliberately the ONLY lower bound. An earlier draft kept a
         * `size < BOOT_KSTACK_MIN_SIZE` check above as well, which made this
         * branch structurally unreachable -- the total floor always fired
         * first -- so the usable rule was dead code that read as enforcement.
         * The test that was supposed to prove the rule is what caught it.
         * BOOT_KSTACK_MIN_SIZE survives as the derived total a conforming
         * producer needs, not as a second gate.
         *
         * The `size <= guard_size` arm is not redundant: without it the
         * subtraction underflows for a run smaller than its own guard and
         * wraps to a huge value that passes the floor. */
        err = BOOT_STACK_ERR_USABLE_SHORT;
    else if (base + size < base)
        err = BOOT_STACK_ERR_WRAP;
    else if (base + size > BOOT_INFO_EARLY_MAP_END)
        /* The kernel dereferences the run through the boot identity map,
         * which covers the first 4 GiB only. A run above it is unreadable
         * here whatever the producer intended. */
        err = BOOT_STACK_ERR_ABOVE_MAP;
    else if (base < 0x100000ull)
        /* The low 1 MiB holds structures the loader writes at FIXED addresses
         * without claiming them from firmware: the boot page tables at
         * 0x70000-0x75fff (setup_page_tables), boot_info at 0x10000, the AP
         * trampoline. A run starting below 1 MiB can therefore contain the
         * live PML4 the kernel is walking, and the stack would overwrite it
         * as it grows. The loader excludes this too; this is the fail-closed
         * net for a producer that does not. */
        err = BOOT_STACK_ERR_LOW_MEM;
    else if (base < (uint64_t)USER_PT_WINDOW_END &&
             base + size > (uint64_t)USER_PT_WINDOW_BASE)
        /* The user page-table window is not kernel-private memory:
         * vmm_create_user_pml4() REPLACES that PD entry per process, so
         * anything the kernel keeps here stops being mapped the moment a user
         * CR3 loads (include/kernel/mm/user_range.h). A stack placed here
         * would vanish underneath the running kernel at the first user exec,
         * and this run is PID 0's permanent stack.
         *
         * The producer avoids the window, but avoidance is not a guarantee:
         * AllocateMaxAddress only bounds the TOP of the allocation and the
         * UEFI spec does not require firmware to allocate top-down, so a
         * conforming firmware may legitimately place it here. This is the
         * fail-closed net for that case, and it is the reason the check is
         * consumer-side rather than only in the loader. */
        err = BOOT_STACK_ERR_USER_WINDOW;

    if (out_err)
        *out_err = err;
    if (err != BOOT_STACK_ERR_OK)
        return 0;

    if (out) {
        out->base       = base;
        out->size       = size;
        out->guard_size = guard_size;
        out->valid      = 1;
    }
    return 1;
}

uint64_t boot_stack_scan_first_touched(const void *words, uint64_t bytes)
{
    const volatile uint64_t *p;
    const volatile uint64_t *end;

    if (words == (const void *)0)
        return bytes;
    if ((bytes & 7u) != 0u)
        return bytes;

    p   = (const volatile uint64_t *)words;
    end = (const volatile uint64_t *)((uintptr_t)words + (uintptr_t)bytes);
    while (p < end && *p == BOOT_KSTACK_POISON)
        p++;

    return (uint64_t)((uintptr_t)p - (uintptr_t)words);
}

int boot_stack_overlaps(const struct boot_stack_info *si,
                        uint64_t start, uint64_t len)
{
    uint64_t end;

    if (si == (const struct boot_stack_info *)0 || !si->valid)
        return 0;
    if (len == 0)
        return 0;
    end = start + len;
    if (end < start)
        return 0;
    return (si->base < end && si->base + si->size > start) ? 1 : 0;
}

int boot_stack_contains(const struct boot_stack_info *si,
                        uint64_t addr, uint64_t len)
{
    uint64_t lo;
    uint64_t hi;

    if (si == (const struct boot_stack_info *)0 || !si->valid)
        return 0;
    if (len == 0)
        return 0;
    if (addr + len < addr)
        return 0;

    lo = si->base + si->guard_size;
    hi = si->base + si->size;
    return (addr >= lo && addr + len <= hi) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Handoff consumption
 * ------------------------------------------------------------------------ */

static const char *boot_stack_err_text(enum boot_stack_error err)
{
    switch (err) {
    case BOOT_STACK_ERR_OK:          return "ok";
    case BOOT_STACK_ERR_ABSENT:      return "no stack published (kstack_base 0)";
    case BOOT_STACK_ERR_UNALIGNED:   return "base not page-aligned";
    case BOOT_STACK_ERR_SIZE_RANGE:  return "size zero or above the accepted maximum";
    case BOOT_STACK_ERR_SIZE_GRAIN:  return "size not a page multiple";
    case BOOT_STACK_ERR_GUARD_GRAIN: return "guard is not exactly one page";
    case BOOT_STACK_ERR_USABLE_SHORT: return "usable span below the measured floor";
    case BOOT_STACK_ERR_WRAP:        return "base + size wraps";
    case BOOT_STACK_ERR_ABOVE_MAP:   return "run ends above the identity map";
    case BOOT_STACK_ERR_GUARD_ABSENT: return "no guard published";
    case BOOT_STACK_ERR_USER_WINDOW: return "run intersects the user page-table window";
    case BOOT_STACK_ERR_LOW_MEM:     return "run starts below 1 MiB (fixed boot tables)";
    }
    return "unknown";
}

void boot_stack_init(const struct boot_info *info)
{
    enum boot_stack_error err = BOOT_STACK_ERR_OK;
    struct boot_stack_info si;
    uint64_t rsp;

    s_stack.base = 0;
    s_stack.size = 0;
    s_stack.guard_size = 0;
    s_stack.valid = 0;

    if (info == (const struct boot_info *)0) {
        klog(LOG_FATAL, "mm", "boot stack: NULL boot_info");
        boot_halt("boot_stack_init: NULL boot_info");
    }

    if (!boot_stack_validate(info->kstack_base,
                             (uint64_t)info->kstack_size,
                             (uint64_t)info->kstack_guard_size,
                             &si, &err)) {
        klog(LOG_FATAL, "mm",
             "boot stack: refused handoff (%s) base=0x%lx size=%u guard=%u",
             boot_stack_err_text(err),
             (uint64_t)info->kstack_base,
             (uint64_t)info->kstack_size,
             (uint64_t)info->kstack_guard_size);
        /* Not recoverable by continuing. Whatever the kernel is standing on
         * is memory pmm_init is about to hand to the allocator, and the
         * corruption that follows would surface far from here as a heap or
         * page-table fault with no trace back to the handoff. */
        boot_halt("boot_stack_init: invalid kernel stack handoff");
    }

    /* Prove the producer's claim rather than trusting it: we are executing on
     * this run right now, so our own RSP must lie inside its usable span. A
     * bootloader that published bounds it did not actually switch to would
     * otherwise pass every structural check above. */
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    if (!boot_stack_contains(&si, rsp, 8)) {
        klog(LOG_FATAL, "mm",
             "boot stack: RSP 0x%lx outside published run 0x%lx..0x%lx "
             "-- producer did not switch",
             rsp, si.base + si.guard_size, si.base + si.size);
        boot_halt("boot_stack_init: RSP outside the published kernel stack");
    }

    s_stack = si;
    klog(LOG_INFO, "mm",
         "boot stack: 0x%lx..0x%lx (%u KiB usable, %u KiB guard), rsp=0x%lx",
         si.base, si.base + si.size,
         (uint64_t)((si.size - si.guard_size) / 1024),
         (uint64_t)(si.guard_size / 1024),
         rsp);
}

const struct boot_stack_info *boot_stack_get(void)
{
    return &s_stack;
}

/* ---------------------------------------------------------------------------
 * Guard page
 * ------------------------------------------------------------------------ */

int boot_stack_install_guard(void)
{
    int rc;

    if (!s_stack.valid || s_stack.guard_size == 0)
        return VMM_GUARD_UNAVAILABLE;

    /* Idempotent, and not merely for tidiness: the poison scan below reads the
     * guard region, and a second call would read it AFTER its PTE was cleared
     * -- a page fault raised by the routine whose job is to prevent one. */
    if (s_guard_installed)
        return VMM_GUARD_OK;

    /* Check the guard region for poison damage BEFORE clearing its PTE. This
     * is the only moment the check is possible: an overflow that happened
     * between the RSP switch and here left its evidence in these bytes, and
     * one instruction later they are unmapped. The guard page itself can
     * never report that window -- it did not exist during it. */
    {
        uint64_t hit = boot_stack_scan_first_touched(
            (const void *)(uintptr_t)s_stack.base, s_stack.guard_size);
        if (hit < s_stack.guard_size) {
            klog(LOG_FATAL, "mm",
                 "boot stack: guard region written at 0x%lx -- early boot "
                 "already overflowed the usable stack",
                 s_stack.base + hit);
            /* FAIL CLOSED. This is not a lost diagnostic: the run demonstrably
             * went deeper than its usable span, so the size contract is
             * already violated and the next-deeper path runs off the end of
             * the run into memory nothing reserved. Continuing would convert a
             * detectable overflow into an untraceable corruption later. */
            boot_halt("boot_stack_install_guard: stack overflowed before the "
                      "guard could be armed");
        }
    }

    rc = vmm_install_guard_page((uintptr_t)s_stack.base,
                                "GUARD: kernel boot stack overflow");
    if (rc == VMM_GUARD_VA_UNSAFE) {
        /* FATAL, matching the BSP entry stack (src/kernel/gdt.c:173).
         * VA_UNSAFE means the identity address is absent, read-only, or
         * ALIASED TO A DIFFERENT FRAME. The last case is not a missing
         * diagnostic: this run is already the live stack, so an alias means
         * pushes are landing on somebody else's memory. There is no degraded
         * mode for that. */
        klog(LOG_FATAL, "mm",
             "boot stack: identity mapping at 0x%lx is not usable for a "
             "stack (absent, read-only or aliased)", s_stack.base);
        boot_halt("boot_stack_install_guard: identity mapping is not usable "
                  "for a stack");
    }
    if (rc != VMM_GUARD_OK) {
        /* VMM_GUARD_UNAVAILABLE only: the guard TABLE is full or the huge-page
         * split failed. The mapping is intact and the run is reserved, so the
         * stack itself is correct -- and refusing a boot that worked yesterday
         * over a missing diagnostic is the wrong trade (the same call section
         * 31 made at gdt.c:159).
         *
         * State the loss accurately. The page stays PRESENT, so an overflow
         * does NOT fault at all: what is missing is CONTAINMENT, not merely
         * the label. */
        klog(LOG_ERROR, "mm",
             "boot stack: guard page at 0x%lx NOT installed (rc=%d) -- the "
             "page stays mapped, so an overflow will NOT fault and will not "
             "be contained",
             s_stack.base, (uint64_t)rc);
    } else {
        s_guard_installed = 1;
        klog(LOG_INFO, "mm", "boot stack: guard page installed at 0x%lx",
             s_stack.base);
    }
    return rc;
}

int boot_stack_guarded(void)
{
    return s_guard_installed;
}

/* ---------------------------------------------------------------------------
 * Depth measurement
 * ------------------------------------------------------------------------ */

uint64_t boot_stack_peak(void)
{
    uint64_t usable_lo;
    uint64_t top;
    uint64_t touched;

    if (!s_stack.valid)
        return 0;

    usable_lo = s_stack.base + s_stack.guard_size;
    top       = s_stack.base + s_stack.size;
    touched   = boot_stack_scan_first_touched((const void *)(uintptr_t)usable_lo,
                                              top - usable_lo);
    if (touched >= top - usable_lo)
        return 0;                      /* whole run still poison */
    return (top - usable_lo) - touched;
}

uint64_t boot_stack_measure(const char *span)
{
    const volatile uint64_t *p;
    uint64_t usable_lo;
    uint64_t top;
    uint64_t touched;
    uint64_t peak;

    if (!s_stack.valid)
        return 0;

    usable_lo = s_stack.base + s_stack.guard_size;
    top       = s_stack.base + s_stack.size;

    /* Starts at usable_lo, never at base: by the time this runs the guard
     * page's PTE is cleared, so reading it would fault inside the very
     * measurement meant to report on it. boot_stack_install_guard() checked
     * that region while it was still mapped.
     *
     * The scan itself is the shared pure rule: UPWARD for the LOWEST qword
     * the bootloader's poison no longer covers. Scanning down from the top
     * would stop at the first untouched slot, and a stack is full of
     * untouched slots between live frames -- the high-water mark is by
     * definition the lowest write, not the first. */
    touched = boot_stack_scan_first_touched((const void *)(uintptr_t)usable_lo,
                                            top - usable_lo);
    p = (const volatile uint64_t *)(uintptr_t)(usable_lo + touched);

    if (span == (const char *)0)
        span = "boot";

    if ((uint64_t)(uintptr_t)p >= top) {
        /* Entire run still poison. Only reachable if the producer poisoned a
         * run nothing then executed on, which boot_stack_init's RSP check
         * already refuses -- report it rather than returning a plausible 0. */
        klog(LOG_WARN, "mm",
             "boot stack: %s -- whole run still poison, measurement "
             "unavailable", (uint64_t)(uintptr_t)span);
        return 0;
    }

    peak = top - (uint64_t)(uintptr_t)p;

    klog(LOG_INFO, "mm",
         "boot stack: %s peak %lu bytes of %lu usable (%lu bytes headroom, "
         "deepest 0x%lx)",
         (uint64_t)(uintptr_t)span,
         peak,
         s_stack.size - s_stack.guard_size,
         (s_stack.size - s_stack.guard_size) - peak,
         (uint64_t)(uintptr_t)p);

    return peak;
}

#ifdef KERNEL_TESTS
void boot_stack_reset_for_test(void)
{
    s_stack.base = 0;
    s_stack.size = 0;
    s_stack.guard_size = 0;
    s_stack.valid = 0;
    s_guard_installed = 0;
}
#endif
