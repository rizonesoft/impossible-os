/* ============================================================================
 * boot_stack.h -- the stack the kernel executes on from the handoff onward.
 *
 * NAMED "boot" for where it comes from, NOT for how long it lives. task.c
 * (task_init, the PID 0 block) keeps this same run as PID 0's permanent
 * kernel stack -- it is never migrated or freed -- and PID 0 goes on to run
 * the compositor loop, which never returns. Anything sizing this run, or
 * quoting a peak from it, has to mean that lifetime and not early boot.
 *
 * The bootloader allocates a dedicated page run pre-ExitBootServices, fills it
 * with BOOT_KSTACK_POISON, and switches RSP to its top before calling the
 * kernel entry (src/boot/uefi/bootx64.c, jump_to_kernel). This module is the
 * consumer half: it validates the published bounds, hands them to
 * boot_reserved so the PMM cannot reclaim the run, installs the guard page
 * once paging is live, and recovers the high-water mark from the poison.
 *
 * Why it exists (TODO-10 sec32): before this, jump_to_kernel loaded CR3 and
 * CALLED the kernel without ever writing RSP, so early boot ran on the
 * firmware's EfiLoaderData stack -- and pmm_init frees every LoaderCode/Data
 * and BootServicesCode/Data region into the free pool. vmm_init and heap_init
 * then allocated from a pool containing the live stack. Nothing enforced that
 * the allocator avoided it; a boot that survived did so by placement luck,
 * which no emulator can be relied on to disturb.
 * ============================================================================ */

#ifndef KERNEL_MM_BOOT_STACK_H
#define KERNEL_MM_BOOT_STACK_H

#include "kernel/types.h"
#include "kernel/boot_init.h"

struct boot_info;  /* full definition in kernel/boot_info.h */

/* Validated view of the published run. All-zero when no valid stack was
 * published; `valid` is the only field a caller may branch on. */
struct boot_stack_info {
    uint64_t base;        /* page-aligned physical/identity base of the run */
    uint64_t size;        /* total bytes including the guard */
    uint64_t guard_size;  /* bytes at the base the stack never uses */
    int      valid;       /* non-zero once the handoff passed validation */
};

/* Why a published run was refused. Ordered so the FIRST failing check wins,
 * which keeps a diagnostic honest: a zero base is reported as absent rather
 * than as a misalignment of address 0. */
enum boot_stack_error {
    BOOT_STACK_ERR_OK          = 0,
    BOOT_STACK_ERR_ABSENT      = 1,  /* base 0 -- producer published nothing */
    BOOT_STACK_ERR_UNALIGNED   = 2,  /* base not 4 KiB aligned */
    BOOT_STACK_ERR_SIZE_RANGE  = 3,  /* size zero or above BOOT_KSTACK_MAX_SIZE */
    BOOT_STACK_ERR_SIZE_GRAIN  = 4,  /* size not a 4 KiB multiple */
    BOOT_STACK_ERR_GUARD_GRAIN = 5,  /* guard is not exactly one page */
    BOOT_STACK_ERR_USABLE_SHORT = 6, /* usable span below BOOT_KSTACK_MIN_USABLE */
    BOOT_STACK_ERR_WRAP        = 7,  /* base + size wraps past UINT64_MAX */
    BOOT_STACK_ERR_ABOVE_MAP   = 8,  /* run ends above the 4 GiB identity map */
    BOOT_STACK_ERR_GUARD_ABSENT = 9, /* guard_size 0: the run would ship unguarded */
    BOOT_STACK_ERR_USER_WINDOW = 10, /* run intersects the user PT window */
    BOOT_STACK_ERR_LOW_MEM     = 11, /* run starts below 1 MiB (fixed boot tables live there) */
};

/* PURE validator over the three published fields. Takes them explicitly (not
 * a boot_info) so a test can drive every refusal branch without a live
 * handoff, and so the caller cannot accidentally validate a different struct
 * than it consumes. Writes *out only on success; *out_err is always written.
 * Returns non-zero on accept. */
int boot_stack_validate(uint64_t base, uint64_t size, uint64_t guard_size,
                        struct boot_stack_info *out,
                        enum boot_stack_error *out_err);

/* PURE: byte offset of the LOWEST qword in [words, words + bytes) that is not
 * BOOT_KSTACK_POISON, or `bytes` when the whole span is still poison. This is
 * the entire measurement rule, factored out so it can be driven from
 * synthetic buffers -- the live callers read memory a test may not fabricate,
 * so without this seam the peak arithmetic and the all-poison case could only
 * be exercised by booting. Returns `bytes` for a NULL pointer or a
 * non-qword-multiple length, both of which are caller bugs, not measurements. */
uint64_t boot_stack_scan_first_touched(const void *words, uint64_t bytes);

/* PURE: does [addr, addr + len) lie inside the usable span of `si`?
 * Used by the pmm_init acceptance check and by the tests. Returns 0 when
 * `si` is invalid, when len is 0, or when the range wraps. */
/* PURE: 1 when the validated run intersects [start, start + len), else 0.
 * 0 for an invalid/NULL info, a zero length, or a wrapping range. This is the
 * consumer-side disjointness rule pmm_init applies against the kernel image
 * and the PMM bitmap extent BEFORE the bitmap is trusted: the reserved-table
 * payload predicate skips every kind but PAYLOAD, so without this the run was
 * never checked against either. */
int boot_stack_overlaps(const struct boot_stack_info *si,
                        uint64_t start, uint64_t len);

int boot_stack_contains(const struct boot_stack_info *si,
                        uint64_t addr, uint64_t len);

/* Consume the handoff. Runs in boot_phase0 immediately after the boot_info
 * copy, BEFORE pmm_init. On refusal this calls boot_halt: the kernel is by
 * definition still executing on memory pmm_init is about to free, and there
 * is no recovery from that which is safer than stopping. */
void boot_stack_init(const struct boot_info *info);

/* The validated run, or an all-zero struct with valid == 0. Never NULL. */
const struct boot_stack_info *boot_stack_get(void);

/* Guard the lowest page of the run. Called once vmm_init has published page
 * tables, and idempotent. Returns the vmm_install_guard_page() result, but the
 * two failure modes are NOT equivalent and are not treated alike (same split
 * as the BSP entry stack, src/kernel/gdt.c:156-174):
 *
 *   VMM_GUARD_VA_UNSAFE   -> FATAL, boot_halt. The identity address is absent,
 *                            read-only, or ALIASED to a different frame. This
 *                            run is already the live stack, so an alias means
 *                            pushes are landing on somebody else's memory.
 *                            There is no degraded mode for that.
 *   VMM_GUARD_UNAVAILABLE -> degraded, LOG_ERROR, boot continues. The guard
 *                            table is full or the huge-page split failed; the
 *                            mapping is intact and the run is reserved, so the
 *                            stack is still correct. What is lost is
 *                            CONTAINMENT, not merely the label: the page stays
 *                            present, so an overflow does not fault at all.
 *                            boot_stack_guarded() reports this state.
 *
 * Before touching any PTE it scans the guard region for poison damage and
 * boot_halts on a hit -- an overflow that already happened during the
 * necessarily-unguarded window between the loader's RSP switch and this call.
 * That scan is only possible here, while the region is still mapped. */
int boot_stack_install_guard(void);

/* Non-zero once the guard page is actually armed. Exists because the
 * production caller has nothing useful to do with the return value at boot
 * time, so without an accessor a silently-unguarded stack would be
 * indistinguishable from a guarded one for the rest of the run -- and a test
 * asking the live boot is the only thing that can prove the SUCCESS path.
 * Same shape as section 31's bsp_entry_stack_guarded(). */
int boot_stack_guarded(void);

/* Recover how deep boot went by scanning upward from the first usable byte
 * for the lowest qword that is not BOOT_KSTACK_POISON. Reports peak bytes
 * used and remaining headroom via klog under `span`, and returns the peak
 * (0 when no valid stack, or when the whole run is still poison).
 *
 * Called from three points, and each earns its place. Phase 1 is the number
 * this section set out to produce. After boot_tests_run() catches the
 * in-kernel test runner, which is the deepest path on a `test=1` boot at
 * roughly ten times the Phase 0/1 peak. The compositor call is the one that
 * makes the figure honest: PID 0 keeps this run forever, so any earlier
 * reading describes a prefix of the stack's life rather than its workload.
 * The poison is never rewritten, so the readings are cumulative and each is
 * always >= the ones before it. */
uint64_t boot_stack_measure(const char *span);

/* The same peak WITHOUT the klog line. For callers that sample repeatedly and
 * only want to report when the number actually grows -- logging every sample
 * would bury the one reading that matters in identical lines. */
uint64_t boot_stack_peak(void);

#ifdef KERNEL_TESTS
void boot_stack_reset_for_test(void);
#endif

#endif /* KERNEL_MM_BOOT_STACK_H */
