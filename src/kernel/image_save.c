/* ============================================================================
 * image_save.c -- Image saving (BMP/PNG via stb_image_write)
 *
 * THIS FILE IS COMPILED WITH -msse2 (separate from rest of kernel).
 *
 * Saves images to VFS paths as BMP or PNG using stb_image_write.
 * Input is BGRA (0xAARRGGBB = framebuffer format).
 * stb_image_write expects RGBA channel order, so we swap B↔R before saving.
 *
 * Memory: STBIW_MALLOC/FREE/REALLOC use kmalloc/kfree since
 * stb_image_write's internal work buffers are small (filter lines,
 * hash tables for zlib).
 * ============================================================================ */

#include "kernel/image.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/printk.h"
#include "kernel/fs/vfs.h"
#include "kernel/types.h"

/* ---- Redirect stb_image_write memory to kernel allocators --------------- */

/* stb_image_write's work buffers are small (filter lines, zlib hash tables,
 * output buffer). These fit comfortably in the 2 MiB heap. */
#define STBIW_MALLOC(sz)             kmalloc(sz)
#define STBIW_REALLOC(p, newsz)      krealloc(p, newsz)
#define STBIW_FREE(p)                kfree(p)
#define STBIW_REALLOC_SIZED(p,oldsz,newsz)  krealloc(p, newsz)

/* No stdio in freestanding kernel */
#define STBI_WRITE_NO_STDIO

/* Provide memcpy/memset -- these are already available as builtins */
#define STBIW_MEMCPY   __builtin_memcpy
#define STBIW_MEMMOVE  __builtin_memmove

/* Pull in the implementation */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ---- VFS write callback ------------------------------------------------- */

/* Context for the write callback: accumulates output data, then
 * writes the complete file to VFS in one shot after encoding. */
typedef struct {
    uint8_t  *buf;      /* Accumulated output buffer */
    uint32_t  len;      /* Current length */
    uint32_t  cap;      /* Capacity */
} write_ctx_t;

static void vfs_write_callback(void *context, void *data, int size)
{
    write_ctx_t *ctx = (write_ctx_t *)context;
    uint32_t new_len = ctx->len + (uint32_t)size;

    /* Grow buffer if needed */
    if (new_len > ctx->cap) {
        uint32_t new_cap = ctx->cap * 2;
        uint8_t *new_buf;
        if (new_cap < new_len) new_cap = new_len;
        new_buf = (uint8_t *)kmalloc(new_cap);
        if (!new_buf) {
            printk("image_save: realloc failed (%u bytes)\n", new_cap);
            return;
        }
        if (ctx->buf && ctx->len > 0) {
            __builtin_memcpy(new_buf, ctx->buf, ctx->len);
            kfree(ctx->buf);
        }
        ctx->buf = new_buf;
        ctx->cap = new_cap;
    }

    __builtin_memcpy(ctx->buf + ctx->len, data, (uint32_t)size);
    ctx->len = new_len;
}

/* ---- Channel swap: BGRA -> RGBA ----------------------------------------- */

/* stb_image_write expects RGBA channel order (R at byte offset 0).
 * Our framebuffer/image_t uses BGRA (0xAARRGGBB = B at byte offset 0).
 * Swap B↔R channels in a temporary copy before saving. */
static uint8_t *bgra_to_rgba_copy(const image_t *img)
{
    uint32_t pixel_count = img->width * img->height;
    uint32_t byte_count  = pixel_count * 4;
    uint8_t *rgba;
    uint32_t i;

    /* Use PMM for large buffers to avoid heap exhaustion */
    if (byte_count > 4096) {
        uint64_t frames = (byte_count + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);
        if (phys == 0) return (uint8_t *)0;
        rgba = (uint8_t *)phys;
    } else {
        rgba = (uint8_t *)kmalloc(byte_count);
        if (!rgba) return (uint8_t *)0;
    }

    /* Swap B and R channels */
    {
        const uint8_t *src = (const uint8_t *)img->pixels;
        for (i = 0; i < pixel_count; i++) {
            uint32_t off = i * 4;
            rgba[off + 0] = src[off + 2];  /* R ← old B position */
            rgba[off + 1] = src[off + 1];  /* G stays */
            rgba[off + 2] = src[off + 0];  /* B ← old R position */
            rgba[off + 3] = src[off + 3];  /* A stays */
        }
    }

    return rgba;
}

static void free_rgba_copy(uint8_t *rgba, uint32_t byte_count)
{
    if (!rgba) return;
    if (byte_count > 4096) {
        uint64_t frames = (byte_count + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uint64_t fi;
        for (fi = 0; fi < frames; fi++)
            pmm_free_frame((uintptr_t)rgba + fi * PMM_FRAME_SIZE);
    } else {
        kfree(rgba);
    }
}

/* ---- Flush write context to VFS ----------------------------------------- */

static int flush_to_vfs(write_ctx_t *ctx, const char *path)
{
    struct vfs_node *file;
    int rc;

    if (!ctx->buf || ctx->len == 0) return -1;

    /* Create file if it doesn't exist */
    vfs_create(path, VFS_FILE);
    file = vfs_open(path, VFS_O_WRITE | VFS_O_TRUNC);
    if (!file) {
        printk("image_save: cannot create '%s'\n", path);
        return -1;
    }

    rc = vfs_write(file, 0, ctx->len, ctx->buf);
    vfs_close(file);

    return rc >= 0 ? 0 : -1;
}

/* ---- Public API --------------------------------------------------------- */

int image_save_bmp(const image_t *img, const char *path)
{
    write_ctx_t ctx = { (uint8_t *)0, 0, 0 };
    uint8_t *rgba;
    uint32_t byte_count;
    int ok;
    int result;

    if (!img || !img->pixels || !path) return -1;

    byte_count = img->width * img->height * 4;

    /* Convert BGRA -> RGBA for stb_image_write */
    rgba = bgra_to_rgba_copy(img);
    if (!rgba) {
        printk("image_save: BGRA->RGBA alloc failed\n");
        return -1;
    }

    /* Initial output buffer (BMP is uncompressed, ~= pixel data + header) */
    ctx.cap = byte_count + 256;
    ctx.buf = (uint8_t *)kmalloc(ctx.cap);
    if (!ctx.buf) {
        /* Try PMM for large images */
        uint64_t frames = (ctx.cap + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);
        if (phys == 0) {
            free_rgba_copy(rgba, byte_count);
            return -1;
        }
        ctx.buf = (uint8_t *)phys;
    }

    ok = stbi_write_bmp_to_func(vfs_write_callback, &ctx,
                                 (int)img->width, (int)img->height,
                                 4, rgba);

    free_rgba_copy(rgba, byte_count);

    if (ok) {
        result = flush_to_vfs(&ctx, path);
        printk("[IMG] Saved BMP '%s' (%u bytes)\n", path, ctx.len);
    } else {
        printk("image_save: BMP encode failed\n");
        result = -1;
    }

    if (ctx.buf) kfree(ctx.buf);
    return result;
}

int image_save_png(const image_t *img, const char *path)
{
    write_ctx_t ctx = { (uint8_t *)0, 0, 0 };
    uint8_t *rgba;
    uint32_t byte_count;
    int ok;
    int result;

    if (!img || !img->pixels || !path) return -1;

    byte_count = img->width * img->height * 4;

    /* Convert BGRA -> RGBA for stb_image_write */
    rgba = bgra_to_rgba_copy(img);
    if (!rgba) {
        printk("image_save: BGRA->RGBA alloc failed\n");
        return -1;
    }

    /* Initial output buffer (PNG is compressed, much smaller than raw) */
    ctx.cap = byte_count / 2;
    if (ctx.cap < 4096) ctx.cap = 4096;
    ctx.buf = (uint8_t *)kmalloc(ctx.cap);
    if (!ctx.buf) {
        ctx.cap = 4096;
        ctx.buf = (uint8_t *)kmalloc(ctx.cap);
        if (!ctx.buf) {
            free_rgba_copy(rgba, byte_count);
            return -1;
        }
    }

    ok = stbi_write_png_to_func(vfs_write_callback, &ctx,
                                 (int)img->width, (int)img->height,
                                 4, rgba, (int)(img->width * 4));

    free_rgba_copy(rgba, byte_count);

    if (ok) {
        result = flush_to_vfs(&ctx, path);
        printk("[IMG] Saved PNG '%s' (%u bytes)\n", path, ctx.len);
    } else {
        printk("image_save: PNG encode failed\n");
        result = -1;
    }

    if (ctx.buf) kfree(ctx.buf);
    return result;
}
