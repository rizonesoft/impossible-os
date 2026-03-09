/* ============================================================================
 * image.c — Runtime image decoding (stb_image wrapper)
 *
 * THIS FILE IS COMPILED WITH -msse2 (separate from rest of kernel).
 *
 * Uses stb_image to decode JPEG, PNG, BMP, GIF, TGA from memory.
 * Output is BGRA (0xAARRGGBB, same as gfx_color_t / framebuffer format).
 *
 * Memory strategy — tiered allocator:
 *   Allocations <= 64 KB  → kmalloc  (stb_image work buffers)
 *   Allocations >  64 KB  → pmm_alloc_contiguous (decoded pixel data)
 *
 * The kernel heap is only 2 MiB. A 1280×720×4 RGBA image is 3.6 MiB,
 * so the final pixel buffer MUST come from PMM.
 * ============================================================================ */

#include "kernel/image.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/printk.h"
#include "kernel/fs/vfs.h"
#include "kernel/types.h"

/* ---- Tiered allocator --------------------------------------------------- */

#define LARGE_ALLOC_THRESHOLD  (64 * 1024)  /* 64 KB */

/*
 * Track PMM allocations: we prefix each PMM block with a small header
 * storing the allocation size so we know how many frames to free.
 *
 * For simplicity, we use a static table of recent PMM allocations.
 * This is sufficient since stb_image only has a few live allocations at once.
 */
#define PMM_TRACK_MAX  16

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
    /* Table full — shouldn't happen with stb_image's allocation pattern */
    printk("image: PMM track table full!\n");
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
    return 0;  /* Not found — must be a kmalloc allocation */
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

static void *stbi_malloc_wrapper(uint32_t size)
{
    if (size > LARGE_ALLOC_THRESHOLD) {
        /* Use PMM for large allocations */
        uint64_t frames = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);
        if (phys == 0) {
            printk("image: PMM alloc failed for %u bytes (%llu frames)\n",
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

    /* Check if old pointer was PMM */
    old_size = 0;
    {
        int i;
        for (i = 0; i < PMM_TRACK_MAX; i++) {
            if (pmm_track[i].ptr == ptr) {
                old_size = pmm_track[i].size;
                break;
            }
        }
    }

    new_ptr = stbi_malloc_wrapper(new_size);
    if (!new_ptr) return (void *)0;

    /* Copy old data */
    {
        uint32_t copy_size = old_size > 0 ? old_size : new_size;
        if (copy_size > new_size) copy_size = new_size;
        /* Use byte-wise copy since we can't include string.h easily */
        {
            uint8_t *d = (uint8_t *)new_ptr;
            const uint8_t *s = (const uint8_t *)ptr;
            uint32_t j;
            for (j = 0; j < copy_size; j++) d[j] = s[j];
        }
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

/* ---- Weak string/memory functions for linking -------------------------- *
 * stb_image calls memcpy/memset/memmove/memcmp/strlen.  These weak symbols
 * provide implementations for the freestanding kernel environment.
 * They may already be provided by stb_truetype_impl.o — weak linkage
 * ensures no duplicate symbol errors. */

typedef unsigned long stbi_sz;

__attribute__((weak))
void *memcpy(void *dst, const void *src, stbi_sz n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    stbi_sz i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

__attribute__((weak))
void *memset(void *s, int c, stbi_sz n)
{
    unsigned char *p = (unsigned char *)s;
    stbi_sz i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

__attribute__((weak))
stbi_sz strlen(const char *s)
{
    stbi_sz len = 0;
    while (s[len]) len++;
    return len;
}

__attribute__((weak))
void *memmove(void *dst, const void *src, stbi_sz n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s2 = (const unsigned char *)src;
    stbi_sz i;
    if (d < s2) {
        for (i = 0; i < n; i++) d[i] = s2[i];
    } else {
        for (i = n; i > 0; i--) d[i-1] = s2[i-1];
    }
    return dst;
}

__attribute__((weak))
int memcmp(const void *a, const void *b, stbi_sz n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    stbi_sz i;
    for (i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}

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
        printk("image: decode failed\n");
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

    if (!img || !path)
        return -1;

    /* Open file via VFS */
    f = vfs_open(path, VFS_O_READ);
    if (!f) {
        printk("image: cannot open '%s'\n", path);
        return -1;
    }

    file_size = f->size;
    if (file_size == 0 || file_size > 32 * 1024 * 1024) {  /* Max 32 MB */
        printk("image: invalid size %u for '%s'\n", file_size, path);
        vfs_close(f);
        return -1;
    }

    /* Read entire file into buffer */
    file_buf = (uint8_t *)kmalloc(file_size);
    if (!file_buf) {
        printk("image: cannot alloc %u bytes for '%s'\n", file_size, path);
        vfs_close(f);
        return -1;
    }

    bytes_read = vfs_read(f, 0, file_size, file_buf);
    vfs_close(f);

    if (bytes_read <= 0) {
        printk("image: read error on '%s' (got %d)\n", path, bytes_read);
        kfree(file_buf);
        return -1;
    }

    /* Decode */
    result = image_load_mem(img, file_buf, file_size);

    /* Free the compressed file data (always from kmalloc) */
    kfree(file_buf);

    if (result == 0) {
        printk("[IMG] Decoded '%s': %ux%u %s\n",
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
