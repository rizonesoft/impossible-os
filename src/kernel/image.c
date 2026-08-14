/* ============================================================================
 * image.c -- Runtime image decoding (stb_image wrapper)
 *
 * THIS FILE IS COMPILED WITH -msse2 (separate from rest of kernel).
 *
 * Uses stb_image to decode JPEG, PNG, BMP, GIF, TGA from memory.
 * Output is BGRA (0xAARRGGBB, same as gfx_color_t / framebuffer format).
 *
 * Memory strategy -- tiered allocator:
 *   Allocations <= 64 KB  → kmalloc  (stb_image work buffers)
 *   Allocations >  64 KB  → pmm_alloc_contiguous (decoded pixel data)
 *
 * The kernel heap is only 2 MiB. A 1280×720×4 RGBA image is 3.6 MiB,
 * so the final pixel buffer MUST come from PMM.
 * ============================================================================ */

#include "kernel/image.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/types.h"

/* ---- Tiered allocator --------------------------------------------------- */

/* Allocations ≤ 4 KB go to kmalloc (heap); > 4 KB go to PMM.
 *
 * Why 4 KB? PMM allocates in 4 KB page granularity -- anything smaller wastes
 * a full page. stb_image's small work buffers (Huffman tables, ~100-500 bytes)
 * total only a few KB and are safe on the heap. The big allocations (decoded
 * pixel buffer = w*h*4 = 3.6 MB, component buffers) go to PMM.
 *
 * The heap is 2 MiB. stb_image's small work buffers total ~10-20 KB.
 * The JPEG file read buffer and decoded output use PMM via this threshold. */
#define LARGE_ALLOC_THRESHOLD  (4 * 1024)  /* 4 KB */

/*
 * Track PMM allocations so we know the size for free/realloc.
 * 64 slots is plenty -- stb_image typically has < 10 large allocations at once.
 */
#define PMM_TRACK_MAX  64

typedef struct {
    void     *ptr;
    uint32_t  size;
} pmm_track_entry_t;

static pmm_track_entry_t pmm_track[PMM_TRACK_MAX];

static void pmm_track_add(void *ptr, uint32_t size)
{
    int i;
    for (i = 0; i < PMM_TRACK_MAX; i++) {
        if (pmm_track[i].ptr == (void *)0) {
            pmm_track[i].ptr = ptr;
            pmm_track[i].size = size;
            return;
        }
    }
    /* Table full -- this is a bug, log it */
    klog(LOG_ERROR, "IMG", "PMM track table full! (%u slots)", (uint32_t)PMM_TRACK_MAX);
}

static uint32_t pmm_track_remove(void *ptr)
{
    int i;
    for (i = 0; i < PMM_TRACK_MAX; i++) {
        if (pmm_track[i].ptr == ptr) {
            uint32_t size = pmm_track[i].size;
            pmm_track[i].ptr = (void *)0;
            pmm_track[i].size = 0;
            return size;
        }
    }
    return 0;  /* Not found -- must be a kmalloc allocation */
}

static int pmm_track_is_pmm(void *ptr)
{
    int i;
    for (i = 0; i < PMM_TRACK_MAX; i++) {
        if (pmm_track[i].ptr == ptr)
            return 1;
    }
    return 0;
}

/* Look up a tracked PMM allocation size without removing it */
static uint32_t pmm_track_size(void *ptr)
{
    int i;
    for (i = 0; i < PMM_TRACK_MAX; i++) {
        if (pmm_track[i].ptr == ptr)
            return pmm_track[i].size;
    }
    return 0;
}

static void *stbi_malloc_wrapper(uint32_t size)
{
    if (size > LARGE_ALLOC_THRESHOLD) {
        /* Use PMM for large allocations */
        uint64_t frames = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);
        if (phys == 0) {
            klog(LOG_ERROR, "IMG", "PMM alloc failed for %u bytes (%llu frames)",
                   size, (unsigned long long)frames);
            return (void *)0;
        }
        pmm_track_add((void *)phys, size);
        return (void *)phys;
    }
    return kmalloc(size);
}

static void stbi_free_wrapper(void *ptr)
{
    uint32_t size;

    if (!ptr) return;

    size = pmm_track_remove(ptr);
    if (size > 0) {
        /* Free PMM frames */
        uint64_t frames = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uint64_t i;
        uintptr_t addr = (uintptr_t)ptr;
        for (i = 0; i < frames; i++) {
            pmm_free_frame(addr + i * PMM_FRAME_SIZE);
        }
    } else {
        kfree(ptr);
    }
}

static void *stbi_realloc_wrapper(void *ptr, uint32_t new_size)
{
    void *new_ptr;
    uint32_t old_size;

    if (!ptr) return stbi_malloc_wrapper(new_size);

    /* Look up old size -- check PMM track table first */
    old_size = pmm_track_size(ptr);

    /* Allocate new buffer */
    new_ptr = stbi_malloc_wrapper(new_size);
    if (!new_ptr) return (void *)0;

    /* Copy old data: use min(old_size, new_size) for PMM allocs.
     * For heap allocs (old_size==0), we don't know the exact size,
     * so copy new_size bytes (safe: realloc always grows or stays same). */
    {
        uint32_t copy_size = (old_size > 0 && old_size < new_size)
                             ? old_size : new_size;
        uint8_t *d = (uint8_t *)new_ptr;
        const uint8_t *s = (const uint8_t *)ptr;
        uint32_t j;
        for (j = 0; j < copy_size; j++) d[j] = s[j];
    }

    stbi_free_wrapper(ptr);
    return new_ptr;
}

/* ---- stb_image configuration ------------------------------------------- */

/* Freestanding: no stdio, no linear-light, no HDR, no thread-locals */
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_SIMD         /* We handle SIMD ourselves via gfx_simd.c */

/* Redirect memory allocation to tiered allocator */
#define STBI_MALLOC(sz)         stbi_malloc_wrapper((uint32_t)(sz))
#define STBI_REALLOC(p, newsz)  stbi_realloc_wrapper(p, (uint32_t)(newsz))
#define STBI_FREE(p)            stbi_free_wrapper(p)

/* Disable assert in production kernel */
#define STBI_ASSERT(x)  ((void)0)

/* Disable failure strings to save space */
#define STBI_NO_FAILURE_STRINGS

/* String/memory functions provided by libc/string.c */

/* Include the implementation */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ---- RGBA → BGRA channel swap ------------------------------------------ */

static void rgba_to_bgra(uint8_t *pixels, uint32_t width, uint32_t height)
{
    uint32_t count = width * height;
    uint32_t i;

    for (i = 0; i < count; i++) {
        uint8_t r = pixels[i * 4 + 0];
        uint8_t b = pixels[i * 4 + 2];
        pixels[i * 4 + 0] = b;  /* B */
        pixels[i * 4 + 2] = r;  /* R */
        /* G (index 1) and A (index 3) stay the same */
    }
}

/* ---- Public API -------------------------------------------------------- */

int image_load_mem(image_t *img, const void *data, uint32_t size)
{
    int w, h, channels;
    uint8_t *decoded;

    if (!img || !data || size == 0)
        return -1;

    /* Decode to RGBA (4 channels forced) */
    decoded = stbi_load_from_memory(
        (const unsigned char *)data, (int)size,
        &w, &h, &channels, 4
    );

    if (!decoded) {
        klog(LOG_ERROR, "IMG", "decode failed");
        return -1;
    }

    /* Convert RGBA → BGRA for framebuffer compatibility */
    rgba_to_bgra(decoded, (uint32_t)w, (uint32_t)h);

    img->pixels    = (uint32_t *)decoded;
    img->width     = (uint32_t)w;
    img->height    = (uint32_t)h;
    img->alloc_size = (uint32_t)(w * h * 4);
    img->from_pmm  = pmm_track_is_pmm(decoded);

    return 0;
}

int image_load(image_t *img, const char *path)
{
    struct vfs_node *f;
    uint8_t *file_buf;
    uint32_t file_size;
    int32_t bytes_read;
    int result;
    int buf_is_pmm = 0;

    if (!img || !path)
        return -1;

    /* Open file via VFS */
    f = vfs_open(path, VFS_O_READ);
    if (!f) {
        klog(LOG_ERROR, "IMG", "cannot open '%s'", path);
        return -1;
    }

    file_size = f->size;
    if (file_size == 0 || file_size > 32 * 1024 * 1024) {  /* Max 32 MB */
        klog(LOG_ERROR, "IMG", "invalid size %u for '%s'", file_size, path);
        vfs_close(f);
        return -1;
    }

    /* Allocate file read buffer -- use PMM for large files (>64KB)
     * to avoid exhausting the 2 MiB kernel heap. */
    if (file_size > LARGE_ALLOC_THRESHOLD) {
        uint64_t frames = (file_size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);
        if (phys == 0) {
            klog(LOG_ERROR, "IMG", "PMM alloc failed for file buf %u bytes", file_size);
            vfs_close(f);
            return -1;
        }
        file_buf = (uint8_t *)phys;
        buf_is_pmm = 1;
    } else {
        file_buf = (uint8_t *)kmalloc(file_size);
        if (!file_buf) {
            klog(LOG_ERROR, "IMG", "cannot alloc %u bytes for '%s'", file_size, path);
            vfs_close(f);
            return -1;
        }
    }

    bytes_read = vfs_read(f, 0, file_size, file_buf);
    vfs_close(f);

    if (bytes_read <= 0) {
        klog(LOG_ERROR, "IMG", "read error on '%s' (got %d)", path,
             (int64_t)bytes_read);
        if (buf_is_pmm) {
            uint64_t frames = (file_size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
            uint64_t fi;
            for (fi = 0; fi < frames; fi++)
                pmm_free_frame((uintptr_t)file_buf + fi * PMM_FRAME_SIZE);
        } else {
            kfree(file_buf);
        }
        return -1;
    }

    /* Decode */
    result = image_load_mem(img, file_buf, file_size);

    /* Free the compressed file data */
    if (buf_is_pmm) {
        uint64_t frames = (file_size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uint64_t fi;
        for (fi = 0; fi < frames; fi++)
            pmm_free_frame((uintptr_t)file_buf + fi * PMM_FRAME_SIZE);
    } else {
        kfree(file_buf);
    }

    if (result == 0) {
        klog(LOG_INFO, "IMG", "Decoded '%s': %ux%u %s",
               path, img->width, img->height,
               img->from_pmm ? "(PMM)" : "(heap)");
    }

    return result;
}

void image_free(image_t *img)
{
    if (!img || !img->pixels)
        return;

    if (img->from_pmm) {
        /* Free PMM frames */
        uint64_t frames = (img->alloc_size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uint64_t i;
        uintptr_t addr = (uintptr_t)img->pixels;

        /* Remove from tracking table if still there */
        pmm_track_remove(img->pixels);

        for (i = 0; i < frames; i++) {
            pmm_free_frame(addr + i * PMM_FRAME_SIZE);
        }
    } else {
        kfree(img->pixels);
    }

    img->pixels = (void *)0;
    img->width = 0;
    img->height = 0;
    img->alloc_size = 0;
    img->from_pmm = 0;
}
