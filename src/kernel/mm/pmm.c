/* ============================================================================
 * pmm.c — Physical Memory Manager (Bitmap Allocator)
 *
 * Uses a bitmap where each bit represents a 4 KiB physical frame.
 *   0 = free, 1 = used/reserved
 *
 * The bitmap is placed immediately after the kernel image (__kernel_end).
 *
 * Initialization:
 *   1. Find the highest usable address from the Multiboot2 memory map
 *   2. Allocate bitmap (total_frames / 8 bytes)
 *   3. Mark ALL frames as used (safe default)
 *   4. Walk the memory map and mark "available" regions as free
 *   5. Re-reserve: first 1 MiB, kernel image, bitmap itself
 * ============================================================================ */

#include "pmm.h"
#include "boot_info.h"
#include "printk.h"

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
static void pmm_mark_region_used(uintptr_t base, uint64_t length)
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

void pmm_init(void)
{
    uint64_t highest_addr = 0;
    uint32_t i;
    uintptr_t kernel_end_phys;
    uintptr_t bitmap_end;

    /* Step 1: Find the highest usable physical address */
    for (i = 0; i < g_boot_info.mmap_count; i++) {
        uint64_t region_end;
        region_end = g_boot_info.mmap[i].base_addr + g_boot_info.mmap[i].length;
        if (region_end > highest_addr)
            highest_addr = region_end;
    }

    /* Cap at 4 GiB for now (our identity map covers this range) */
    if (highest_addr > 0x100000000ULL)
        highest_addr = 0x100000000ULL;

    /* Step 2: Calculate bitmap dimensions */
    total_frames = highest_addr / PMM_FRAME_SIZE;
    bitmap_size = (total_frames + 7) / 8;    /* round up to whole bytes */

    /* Place bitmap right after the kernel */
    kernel_end_phys = (uintptr_t)__kernel_end;
    bitmap = (uint8_t *)kernel_end_phys;
    bitmap_end = kernel_end_phys + bitmap_size;

    /* Page-align bitmap_end for cleanliness */
    bitmap_end = (bitmap_end + PMM_FRAME_SIZE - 1) & ~((uintptr_t)PMM_FRAME_SIZE - 1);

    /* Step 3: Mark ALL frames as used (safe default) */
    for (i = 0; i < bitmap_size; i++)
        bitmap[i] = 0xFF;
    used_frames = total_frames;

    /* Step 4: Walk memory map and free "available" regions (type 1) */
    for (i = 0; i < g_boot_info.mmap_count; i++) {
        if (g_boot_info.mmap[i].type == 1) {
            pmm_mark_region_free(
                (uintptr_t)g_boot_info.mmap[i].base_addr,
                g_boot_info.mmap[i].length
            );
        }
    }

    /* Step 5: Re-reserve critical regions */

    /* First 1 MiB: BIOS, IVT, BDA, VGA, legacy area */
    pmm_mark_region_used(0, 0x100000);

    /* Kernel image: 1 MiB → __kernel_end */
    pmm_mark_region_used(0x100000, kernel_end_phys - 0x100000);

    /* Bitmap itself */
    pmm_mark_region_used(kernel_end_phys, bitmap_end - kernel_end_phys);

    printk("[OK] PMM: %u MiB total, %u MiB free (%u/%u frames)\n",
           (uint64_t)(total_frames * PMM_FRAME_SIZE / (1024 * 1024)),
           (uint64_t)((total_frames - used_frames) * PMM_FRAME_SIZE / (1024 * 1024)),
           total_frames - used_frames,
           total_frames);
}

uintptr_t pmm_alloc_frame(void)
{
    uint64_t i, bit;

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
