/* ============================================================================
 * pmm.c -- Physical Memory Manager (Bitmap Allocator)
 *
 * Uses a bitmap where each bit represents a 4 KiB physical frame.
 *   0 = free, 1 = used/reserved
 *
 * The bitmap is placed immediately after the kernel image (__kernel_end).
 *
 * Initialization:
 *   1. Find the highest usable address from the UEFI memory map
 *   2. Allocate bitmap (total_frames / 8 bytes)
 *   3. Mark ALL frames as used (safe default)
 *   4. Walk the UEFI memory map and mark usable regions as free
 *   5. Re-reserve: first 1 MiB, kernel image, bitmap itself
 * ============================================================================ */

#include "kernel/mm/pmm.h"
#include "kernel/mm/memmap.h"           /* HHDM extent check + phys->virt alias */
#include "kernel/mm/user_range.h"
#include "kernel/mm/boot_reserved.h"
#include "kernel/mm/boot_stack.h"        /* boot_stack_get/overlaps: stack vs image + bitmap */
#include "kernel/boot_info.h"
#include "kernel/boot_halt.h"
#include "kernel/klog.h"
#ifdef KERNEL_TESTS
#include "kernel/smp.h"                 /* smp_this_cpu() for per-CPU countdown */
#include "kernel/sched/irql.h"          /* KeGetCurrentIrql for thread-context gate */
#include "kernel/sched/task.h" /* task_current() for task-filter gate */
#endif
/* Linker symbols */
extern char __kernel_end[];

/* Bitmap storage */
static uint8_t *bitmap;
static uint64_t bitmap_size;     /* bytes in the bitmap */
static uint64_t total_frames;    /* total physical frames tracked */
static uint64_t used_frames;     /* number of frames marked as used */

/* --- Bitmap helpers --- */

static inline void bitmap_set(uint64_t frame)
{
    bitmap[frame / 8] |= (uint8_t)(1 << (frame % 8));
}

static inline void bitmap_clear(uint64_t frame)
{
    bitmap[frame / 8] &= (uint8_t)~(1 << (frame % 8));
}

static inline uint8_t bitmap_test(uint64_t frame)
{
    return bitmap[frame / 8] & (uint8_t)(1 << (frame % 8));
}

/* Mark a range of frames as used */
void pmm_mark_region_used(uintptr_t base, uint64_t length)
{
    uint64_t frame_start = base / PMM_FRAME_SIZE;
    uint64_t frame_end = (base + length + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    uint64_t f;

    if (frame_end > total_frames)
        frame_end = total_frames;

    for (f = frame_start; f < frame_end; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
        }
    }
}

/* Mark a range of frames as free */
static void pmm_mark_region_free(uintptr_t base, uint64_t length)
{
    uint64_t frame_start = base / PMM_FRAME_SIZE;
    uint64_t frame_end = (base + length) / PMM_FRAME_SIZE;
    uint64_t f;

    if (frame_end > total_frames)
        frame_end = total_frames;

    for (f = frame_start; f < frame_end; f++) {
        if (bitmap_test(f)) {
            bitmap_clear(f);
            used_frames--;
        }
    }
}

boot_result_t pmm_init(void)
{
    uint64_t highest_addr = 0;
    uint32_t i;
    uintptr_t kernel_end_phys;
    uintptr_t bitmap_end;

    /* Warn if bootloader truncated the memory map (S6) */
    if (g_boot_info.mmap_truncated)
        klog(LOG_WARN, "mm",
             "PMM: memory map truncated by bootloader (%u of %u+ entries)",
             (uint64_t)g_boot_info.mmap_count,
             (uint64_t)BOOT_MMAP_MAX_ENTRIES);

    /* Warn if bootloader found quirky descriptors (S12) */
    if (g_boot_info.mmap_quirks)
        klog(LOG_WARN, "mm",
             "PMM: memory map had quirky descriptors (stripped by bootloader)");

    /* Fail-closed consumer-side validation. The bootloader
     * normalizes and bounds the map, but PMM must not trust the
     * post-handoff contents: a corrupt same-version BOOTX64.EFI, a
     * memory clobber after Phase 0, or a future producer bug could
     * deliver mmap_count > BOOT_MMAP_MAX_ENTRIES (out-of-bounds
     * read) or a base+length pair that wraps past UINT64_MAX (bogus
     * highest_addr / free-region selection). Validate count cap
     * and each entry's base+length non-wrap before consuming. */
    if (g_boot_info.mmap_count > BOOT_MMAP_MAX_ENTRIES) {
        klog(LOG_ERROR, "mm",
             "PMM: mmap_count %u exceeds BOOT_MMAP_MAX_ENTRIES %u "
             "(corrupt boot_info)",
             (uint64_t)g_boot_info.mmap_count,
             (uint64_t)BOOT_MMAP_MAX_ENTRIES);
        boot_halt("pmm_init: mmap_count exceeds cap");
    }
    for (i = 0; i < g_boot_info.mmap_count; i++) {
        uint64_t base = g_boot_info.mmap[i].base_addr;
        uint64_t length = g_boot_info.mmap[i].length;
        if (length == 0)
            continue;  /* zero-length entry is benign; just skipped */
        if (base + length < base) {
            klog(LOG_ERROR, "mm",
                 "PMM: mmap[%u] base=0x%lx length=0x%lx wraps past "
                 "UINT64_MAX (corrupt boot_info)",
                 (uint64_t)i,
                 base,
                 length);
            boot_halt("pmm_init: mmap entry wraps");
        }
    }

    /* Step 1: Find the highest usable physical address.
     *
     * Only consider memory types that represent actual RAM (conventional,
     * loader, boot-services).  MMIO and runtime-services regions can have
     * addresses well above physical RAM (e.g. 0xFED40000) -- including
     * them would make the bitmap cover the MMIO hole and pmm_alloc would
     * hand out non-existent pages.
     */
    for (i = 0; i < g_boot_info.mmap_count; i++) {
        uint32_t utype = g_boot_info.mmap[i].uefi_memory_type;
        if (utype == UEFI_MMAP_CONVENTIONAL ||
            utype == UEFI_MMAP_LOADER_CODE ||
            utype == UEFI_MMAP_LOADER_DATA ||
            utype == UEFI_MMAP_BOOT_SERVICES_CODE ||
            utype == UEFI_MMAP_BOOT_SERVICES_DATA) {
            uint64_t region_end;
            region_end = g_boot_info.mmap[i].base_addr +
                         g_boot_info.mmap[i].length;
            if (region_end > highest_addr)
                highest_addr = region_end;
        }
    }

    /* Cap at 4 GiB for now (our identity map covers this range) */
    if (highest_addr > PMM_PHYS_ADDR_CAP)
        highest_addr = PMM_PHYS_ADDR_CAP;

    /* Step 2: Calculate bitmap dimensions */
    total_frames = highest_addr / PMM_FRAME_SIZE;
    bitmap_size = (total_frames + 7) / 8;    /* round up to whole bytes */

    /* Place bitmap right after the kernel */
    kernel_end_phys = (uintptr_t)__kernel_end;
    bitmap = (uint8_t *)kernel_end_phys;
    bitmap_end = kernel_end_phys + bitmap_size;

    /* Page-align bitmap_end for cleanliness */
    bitmap_end = (bitmap_end + PMM_FRAME_SIZE - 1) & ~((uintptr_t)PMM_FRAME_SIZE - 1);

    /* BEFORE THE FIRST BITMAP WRITE: the loader-owned kernel boot stack
     * (TODO-10 sec32) must be disjoint from the kernel image and from the
     * bitmap extent computed just above. This CPU is standing on that run;
     * the Step 3 fill below writes 0xFF over [kernel_end_phys, bitmap_end), so
     * a run placed there would have its saved return state overwritten before
     * any later check could report it (an earlier form of this check sat
     * after the fill, which is exactly that failure). The reserved-table
     * payload predicate skips every kind but PAYLOAD, so nothing else compares
     * the run against these two ranges; boot_stack_validate already refuses
     * the low 1 MiB and the user PT window. boot_stack_init ran at
     * boot_hw.c:156, so the run is validated by now. */
    {
        const struct boot_stack_info *ks = boot_stack_get();
        if (ks != (const struct boot_stack_info *)0 && ks->valid) {
            if (boot_stack_overlaps(ks, 0x100000ull,
                                    (uint64_t)(kernel_end_phys - 0x100000ull))) {
                klog(LOG_FATAL, "mm",
                     "PMM: kernel boot stack 0x%lx+0x%lx intersects the "
                     "kernel image [0x100000, 0x%lx)",
                     ks->base, ks->size, (uint64_t)kernel_end_phys);
                return BOOT_FATAL;
            }
            if (boot_stack_overlaps(ks, (uint64_t)kernel_end_phys,
                                    (uint64_t)(bitmap_end - kernel_end_phys))) {
                klog(LOG_FATAL, "mm",
                     "PMM: kernel boot stack 0x%lx+0x%lx intersects the "
                     "PMM bitmap [0x%lx, 0x%lx)",
                     ks->base, ks->size, (uint64_t)kernel_end_phys,
                     (uint64_t)bitmap_end);
                return BOOT_FATAL;
            }
        }
    }

    /* Step 3: Mark ALL frames as used (safe default) */
    for (i = 0; i < bitmap_size; i++)
        bitmap[i] = 0xFF;
    used_frames = total_frames;

    /* Step 4: Walk memory map and free usable regions using UEFI memory types.
     *
     * Free (usable by OS):
     *   - EfiConventionalMemory  (7) -- free RAM
     *   - EfiLoaderCode/Data     (1,2) -- bootloader memory, reclaimable after boot
     *   - EfiBootServicesCode/Data (3,4) -- reclaimable after ExitBootServices()
     *
     * Reserved (stays used):
     *   - EfiRuntimeServicesCode/Data (5,6) -- must preserve for runtime services
     *   - EfiACPIReclaimMemory   (9) -- ACPI tables (keep for now)
     *   - EfiACPIMemoryNVS      (10) -- ACPI firmware working memory
     *   - EfiMemoryMappedIO     (11) -- device MMIO
     *   - EfiMemoryMappedIOPort (12) -- device MMIO port space
     *   - EfiReservedMemoryType  (0) -- firmware reserved
     *   - EfiUnusableMemory      (8) -- bad RAM
     */
    for (i = 0; i < g_boot_info.mmap_count; i++) {
        uint32_t utype = g_boot_info.mmap[i].uefi_memory_type;
        if (utype == UEFI_MMAP_CONVENTIONAL ||
            utype == UEFI_MMAP_LOADER_CODE ||
            utype == UEFI_MMAP_LOADER_DATA ||
            utype == UEFI_MMAP_BOOT_SERVICES_CODE ||
            utype == UEFI_MMAP_BOOT_SERVICES_DATA) {
            pmm_mark_region_free(
                (uintptr_t)g_boot_info.mmap[i].base_addr,
                g_boot_info.mmap[i].length
            );
        }
        /* All other types stay marked as used (the safe default from Step 3) */
    }

    /* Step 5: Re-reserve critical regions */

    /* First 1 MiB: BIOS, IVT, BDA, VGA, legacy area */
    pmm_mark_region_used(0, 0x100000);

    /* Kernel image: 1 MiB → __kernel_end */
    pmm_mark_region_used(0x100000, kernel_end_phys - 0x100000);

    /* Bitmap itself */
    pmm_mark_region_used(kernel_end_phys, bitmap_end - kernel_end_phys);

    /* User PT window (USER_PT_WINDOW_BASE .. USER_PT_WINDOW_END): the FULL
     * 2 MiB huge page containing the user ELF range, not just USER_ELF_SIZE.
     * vmm_create_user_pml4() replaces this PD entry per process (User bits
     * on ELF pages, Present CLEARED on the 0x900000 guard page), so kernel
     * data placed anywhere in the window would vanish under a user CR3.
     * Historical near-miss: heap_init's contiguous run landed at 0x900000
     * once the kernel image grew, and the per-process guard page made the
     * heap's block-list head not-present in process context (boot hang at
     * first user exec). Since the kernel uses identity mapping, reserve the
     * whole physical window so pmm_alloc_* never hands any of it out. */
    pmm_mark_region_used(USER_PT_WINDOW_BASE, USER_PT_WINDOW_SIZE);

    /* Runtime verify: confirm first and last frames of the window are
     * reserved. If these aren't set, pmm_alloc_contiguous could hand out
     * user-window memory. */
    {
        uint64_t first_frame = USER_PT_WINDOW_BASE / PMM_FRAME_SIZE;
        uint64_t last_frame  = (USER_PT_WINDOW_END - PMM_FRAME_SIZE) / PMM_FRAME_SIZE;
        if (!bitmap_test(first_frame) || !bitmap_test(last_frame)) {
            klog(LOG_FATAL, "PMM",
                 "User PT window 0x%x-0x%x not reserved in bitmap!",
                 (uint64_t)USER_PT_WINDOW_BASE, (uint64_t)USER_PT_WINDOW_END);
        }
    }

    /* bone authoritative pass that
     * captures every boot_info-derived retained region (struct boot_info,
     * USB DMA + scratchpad, TPM event log, framebuffer, UEFI runtime
     * memory, typed payloads with FLAG_RESERVED), detects overlap,
     * and applies to the PMM bitmap. Replaces the previous inline USB
     * DMA reservation block. */
    {
        enum boot_reserved_error br_err = BOOT_RESERVED_ERR_OK;
        if (boot_reserved_populate_from_info((const struct boot_info *)&g_boot_info, &br_err) != BOOT_OK) {
            klog(LOG_FATAL, "mm",
                 "PMM: boot_reserved populate failed (err=%u); "
                 "bootloader/kernel disagree on a retained region",
                 (uint64_t)br_err);
            return BOOT_FATAL;
        }
        /* boot_payload_validate ran before PMM init and cross-checked
         * payloads against boot_info/USB/TPM/FB/rt_mmap. It could not
         * see the PMM-internal reservations that depend on RAM size
         * (bitmap extent) or the user PT window. Check now before apply
         * so a bootloader-loaded payload that happens to land on the
         * bitmap or the user PT window fails boot with a clear
         * diagnostic, rather than silently getting clobbered by the
         * bitmap initializer or vanishing under a user CR3. The window
         * is the FULL 2 MiB PD entry (matches pmm_mark_region_used
         * above), not just USER_ELF_SIZE -- a payload at 0x900000 is
         * equally unsafe. */
        if (boot_reserved_check_payloads_disjoint(0ull, 0x100000ull,
                                                  "low_mem_1mb") != 0u ||
            boot_reserved_check_payloads_disjoint((uint64_t)0x100000ull,
                                                  (uint64_t)(kernel_end_phys - 0x100000ull),
                                                  "kernel_image") != 0u ||
            boot_reserved_check_payloads_disjoint((uint64_t)kernel_end_phys,
                                                  (uint64_t)(bitmap_end - kernel_end_phys),
                                                  "pmm_bitmap") != 0u ||
            boot_reserved_check_payloads_disjoint((uint64_t)USER_PT_WINDOW_BASE,
                                                  (uint64_t)USER_PT_WINDOW_SIZE,
                                                  "user_pt_window") != 0u) {
            klog(LOG_FATAL, "mm",
                 "PMM: payload range collides with PMM-internal region");
            return BOOT_FATAL;
        }
        boot_reserved_apply();
        boot_reserved_log();
    }

    /* ACCEPTANCE CHECK for the loader-owned kernel boot stack (TODO-10
     * sec32). Everything above is the mechanism; this is the PROOF, and it is
     * the point of the section: a boot that happens to survive is not
     * evidence that the allocator avoided the running stack, because the
     * placement that makes it survive is luck no emulator disturbs.
     *
     * Ask the bitmap directly whether the frames of the run are marked used.
     * It cannot pass by accident: step 4 above freed every LoaderCode/Data
     * region a moment ago, so the only thing that can have re-marked them is
     * the boot_reserved entry naming the run.
     *
     * EVERY frame of the published run is swept, not just the one holding the
     * current RSP. A single-frame probe proves the allocator is off the byte
     * this CPU happens to be standing on and says nothing about the other 63
     * frames of a 256 KiB run -- and the frames that matter most are the ones
     * further DOWN, which the stack has not reached yet and which vmm_init and
     * heap_init are about to allocate from. The RSP frame is checked
     * separately afterwards because it is the one claim that stays true even
     * if the published bounds are wrong: it is measured, not published. */
    {
        uint64_t rsp_now;
        const struct boot_stack_info *ks = boot_stack_get();
        __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp_now));

        if (ks != (const struct boot_stack_info *)0 && ks->valid) {
            uint64_t f = ks->base / PMM_FRAME_SIZE;
            uint64_t f_end = (ks->base + ks->size) / PMM_FRAME_SIZE;
            for (; f < f_end; f++) {
                if (f >= total_frames || !bitmap_test(f)) {
                    klog(LOG_FATAL, "mm",
                         "PMM: kernel boot stack frame %lu (0x%lx) of run "
                         "0x%lx+0x%lx is ALLOCATABLE -- vmm_init and heap_init "
                         "would allocate the run this CPU is standing on",
                         f, f * (uint64_t)PMM_FRAME_SIZE, ks->base, ks->size);
                    return BOOT_FATAL;
                }
            }
            klog(LOG_INFO, "mm",
                 "PMM: kernel boot stack run 0x%lx+0x%lx is reserved "
                 "(%lu frames)",
                 ks->base, ks->size, f_end - (ks->base / PMM_FRAME_SIZE));
        }

        {
            uint64_t f_lo = (rsp_now - 8u) / PMM_FRAME_SIZE;
            uint64_t f_hi = rsp_now / PMM_FRAME_SIZE;
            if (f_hi >= total_frames || !bitmap_test(f_lo) || !bitmap_test(f_hi)) {
                klog(LOG_FATAL, "mm",
                     "PMM: live kernel stack at 0x%lx is ALLOCATABLE (frames "
                     "%lu/%lu of %lu) -- vmm_init and heap_init would allocate "
                     "the memory this CPU is standing on",
                     rsp_now, f_lo, f_hi, (uint64_t)total_frames);
                return BOOT_FATAL;
            }
            klog(LOG_INFO, "mm",
                 "PMM: live kernel stack at 0x%lx is reserved (frame %lu)",
                 rsp_now, f_hi);
        }
    }

    /* Log UEFI memory map summary */
    klog(LOG_INFO, "UEFI", "Memory map: %u MB RAM, %u descriptors",
           (uint64_t)(total_frames * PMM_FRAME_SIZE / (1024 * 1024)),
           (uint64_t)g_boot_info.mmap_count);

    klog(LOG_INFO, "mm", "PMM: %u MiB total, %u MiB free (%u/%u frames)",
           (uint64_t)(total_frames * PMM_FRAME_SIZE / (1024 * 1024)),
           (uint64_t)((total_frames - used_frames) * PMM_FRAME_SIZE / (1024 * 1024)),
           total_frames - used_frames,
           total_frames);

    return (total_frames > 0 && total_frames > used_frames) ? BOOT_OK : BOOT_FATAL;
}

#ifdef KERNEL_TESTS
/* Aggregate count across all CPUs of pmm_alloc_* calls that were forced
 * to return 0 by the fault-injection hook. Updated with __atomic ops so
 * tests on either CPU read a coherent value. */
static uint64_t s_pmm_fault_injections;

/* Check the per-CPU pmm-fault gate. Returns 1 if the current call should
 * be forced to return 0 (and updates per-CPU bookkeeping), 0 otherwise.
 * Same gate set as the kmalloc hook:
 *   - IRQL must be PASSIVE_LEVEL.
 *   - Task filter: non-zero kmalloc_fail_task_pid restricts firings.
 *   - Max-injections cap: fired_counter < max (or max == 0).
 *   - Countdown: --countdown == 0 triggers fire.
 * On fire, increments s_pmm_fault_injections + fired_counter. */
static int pmm_fault_should_fire(void)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return 0;

    /* Site mode first, without touching the per-CPU countdown -- see the
     * matching comment in heap.c's gate for why the two modes are disjoint
     * rather than layered. */
    if (fault_site_claim(FI_ALLOC_PMM)) {
        __atomic_fetch_add(&s_pmm_fault_injections, 1ull, __ATOMIC_RELAXED);
        return 1;
    }

    struct per_cpu_data *pc = smp_this_cpu();
    if (!pc || !pc->pmm_alloc_fail_countdown)
        return 0;

    if (pc->pmm_alloc_fail_task_pid != 0) {
        struct task *t = task_current();
        if (!t || t->pid != pc->pmm_alloc_fail_task_pid)
            return 0;
    }
    if (pc->pmm_alloc_fail_max_injections != 0 &&
        pc->pmm_alloc_fail_fired_counter >=
            pc->pmm_alloc_fail_max_injections) {
        return 0;
    }
    if (--pc->pmm_alloc_fail_countdown != 0)
        return 0;

    __atomic_fetch_add(&s_pmm_fault_injections, 1ull, __ATOMIC_RELAXED);
    pc->pmm_alloc_fail_fired_counter++;
    /* auto-reload for multi-fire: see the heap-side comment. */
    if (pc->pmm_alloc_fail_max_injections != 0 &&
        pc->pmm_alloc_fail_fired_counter <
            pc->pmm_alloc_fail_max_injections) {
        pc->pmm_alloc_fail_countdown = 1;
    }
    return 1;
}

void pmm_alloc_fail_countdown_set(uint32_t n)
{
    smp_this_cpu()->pmm_alloc_fail_countdown = n;
}
void pmm_alloc_fail_countdown_clear(void)
{
    smp_this_cpu()->pmm_alloc_fail_countdown = 0;
}
void pmm_alloc_fail_next(void)
{
    smp_this_cpu()->pmm_alloc_fail_countdown = 1;
}
uint64_t pmm_alloc_fail_injections_triggered(void)
{
    return __atomic_load_n(&s_pmm_fault_injections, __ATOMIC_RELAXED);
}
void pmm_alloc_fail_task_filter_set(uint32_t task_pid)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->pmm_alloc_fail_task_pid     = task_pid;
    pc->pmm_alloc_fail_fired_counter = 0;
}
void pmm_alloc_fail_task_filter_clear(void)
{
    smp_this_cpu()->pmm_alloc_fail_task_pid = 0;
}
void pmm_alloc_fail_max_injections_set(uint32_t max)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->pmm_alloc_fail_max_injections = max;
    pc->pmm_alloc_fail_fired_counter  = 0;
}
void pmm_alloc_fail_max_injections_clear(void)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->pmm_alloc_fail_max_injections = 0;
    pc->pmm_alloc_fail_fired_counter  = 0;
}
uint32_t pmm_alloc_fail_fired_counter(void)
{
    return smp_this_cpu()->pmm_alloc_fail_fired_counter;
}
#endif /* KERNEL_TESTS */

uintptr_t pmm_alloc_frame(void)
{
    uint64_t i, bit;

#ifdef KERNEL_TESTS
    /* Test-only hook: forced-failure path leaves the bitmap unaltered,
     * so tests observe a real OOM shape on return. See
     * pmm_fault_should_fire() for the gate set. */
    if (pmm_fault_should_fire())
        return 0;
#endif

    for (i = 0; i < bitmap_size; i++) {
        if (bitmap[i] == 0xFF)
            continue;   /* all 8 frames in this byte are used */

        /* Find the first free bit */
        for (bit = 0; bit < 8; bit++) {
            uint64_t frame = i * 8 + bit;
            if (frame >= total_frames)
                return 0;   /* out of range */

            if (!bitmap_test(frame)) {
                bitmap_set(frame);
                used_frames++;
                return frame * PMM_FRAME_SIZE;
            }
        }
    }

    return 0;   /* out of memory */
}

/* Allocate N contiguous physical frames by scanning the bitmap for a run */
uintptr_t pmm_alloc_contiguous(uint64_t count)
{
    uint64_t run_start = 0;
    uint64_t run_len = 0;
    uint64_t f;

    if (count == 0) return 0;
    /* count==1 delegates to pmm_alloc_frame, which runs its own
     * fault-inject check -- avoids double-decrementing the countdown
     * for a single-frame call routed through _contiguous. */
    if (count == 1) return pmm_alloc_frame();

#ifdef KERNEL_TESTS
    if (pmm_fault_should_fire())
        return 0;
#endif


    for (f = 0; f < total_frames; f++) {
        if (bitmap_test(f)) {
            /* Used frame -- reset run */
            run_len = 0;
            run_start = f + 1;
        } else {
            run_len++;
            if (run_len >= count) {
                /* Found enough contiguous free frames -- mark them used */
                uint64_t i;
                for (i = 0; i < count; i++) {
                    bitmap_set(run_start + i);
                    used_frames++;
                }
                return run_start * PMM_FRAME_SIZE;
            }
        }
    }

    return 0;   /* no contiguous block large enough */
}

/* Symmetric counterpart to pmm_alloc_contiguous(). Frees each frame in the run
 * individually: pmm_free_frame() releases exactly one frame, so a caller
 * rolling back an N-frame pool with a single call leaks N-1 frames. */
void pmm_free_contiguous(uintptr_t base, uint64_t count)
{
    uint64_t i;

    if (!base || count == 0)
        return;

    for (i = 0; i < count; i++)
        pmm_free_frame(base + i * PMM_FRAME_SIZE);
}

/* Back a large kernel pool with contiguous frames reached through the HHDM.
 * Validates the WHOLE extent (mm_phys_extent_in_hhdm), not just the base: a
 * multi-frame run can start inside the direct-map window and end past its top,
 * which would leave the pool's tail aliasing nothing. On any failure after the
 * frames are taken, the full run is released -- never a single pmm_free_frame. */
void *pmm_alloc_pages_hhdm(uint64_t bytes, uintptr_t *out_phys,
                           uint64_t *out_pages)
{
    uint64_t pages;
    uintptr_t phys;
    void *virt;

    if (bytes == 0)
        return (void *)0;

    /* Round up to whole frames; reject a size whose rounding would wrap. */
    if (bytes > (uint64_t)-1 - (PMM_FRAME_SIZE - 1))
        return (void *)0;
    pages = (bytes + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    phys = pmm_alloc_contiguous(pages);
    if (!phys)
        return (void *)0;

    if (!mm_phys_extent_in_hhdm((uint64_t)phys, pages * PMM_FRAME_SIZE)) {
        pmm_free_contiguous(phys, pages);
        return (void *)0;
    }

    virt = mm_phys_to_hhdm((uint64_t)phys);
    if (!virt) {
        pmm_free_contiguous(phys, pages);
        return (void *)0;
    }

    if (out_phys)  *out_phys  = phys;
    if (out_pages) *out_pages = pages;
    return virt;
}

void pmm_free_frame(uintptr_t addr)
{
    uint64_t frame = addr / PMM_FRAME_SIZE;

    if (frame >= total_frames)
        return;

    if (bitmap_test(frame)) {
        bitmap_clear(frame);
        used_frames--;
    }
}


int pmm_frame_is_free(uintptr_t addr)
{
    uint64_t frame = (uint64_t)addr / PMM_FRAME_SIZE;
    if (frame >= total_frames)
        return 0;
    return bitmap_test(frame) ? 0 : 1;
}

uint64_t pmm_get_total_frames(void)
{
    return total_frames;
}

uint64_t pmm_get_used_frames(void)
{
    return used_frames;
}

uint64_t pmm_get_free_frames(void)
{
    return total_frames - used_frames;
}
