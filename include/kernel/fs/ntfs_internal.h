/* ============================================================================
 * ntfs_internal.h — Shared inline helpers for NTFS driver sub-modules
 *
 * Little-endian field readers, memory helpers, and simple comparisons.
 * Included by each ntfs_*.c file — all functions are static inline to
 * avoid duplicate-symbol linker errors.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Little-endian field readers ---- */

static inline uint16_t ntfs_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline void ntfs_le16_write(uint8_t *p, uint16_t val)
{
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)(val >> 8);
}

static inline uint32_t ntfs_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void ntfs_le32_write(uint8_t *p, uint32_t val)
{
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
    p[2] = (uint8_t)((val >> 16) & 0xFF);
    p[3] = (uint8_t)((val >> 24) & 0xFF);
}

static inline uint64_t ntfs_le64(const uint8_t *p)
{
    return (uint64_t)ntfs_le32(p) | ((uint64_t)ntfs_le32(p + 4) << 32);
}

static inline void ntfs_le64_write(uint8_t *p, uint64_t val)
{
    ntfs_le32_write(p, (uint32_t)(val & 0xFFFFFFFF));
    ntfs_le32_write(p + 4, (uint32_t)(val >> 32));
}

/* ---- Memory helpers (no libc available) ---- */

static inline int ntfs_memcmp(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

static inline void ntfs_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint64_t i;
    for (i = 0; i < n; i++)
        d[i] = val;
}

static inline void ntfs_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint64_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* Overlapping-safe move: copies backward when dst > src.
 * Marked __attribute__((noinline)) to prevent the compiler from unrolling
 * the backward loop into a pattern that produces 33-bit addresses at -O2. */
static void __attribute__((noinline, unused))
ntfs_memmove(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d <= s || d >= s + n) {
        /* No overlap, forward copy */
        uint64_t i;
        for (i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        /* Overlapping, backward copy */
        uint64_t i = n;
        while (i > 0) {
            i--;
            d[i] = s[i];
        }
    }
}

/* ---- Attribute manipulation helpers (ntfs_attr_write.c) ---- */

struct ntfs_mft_header;
struct ntfs_data_run;

/* Re-parse MFT record header from raw bytes */
void reparse_header(const uint8_t *rec, struct ntfs_mft_header *hdr);

/* Find sorted insertion point for a new attribute by type */
uint32_t find_insert_point(const uint8_t *rec,
                            const struct ntfs_mft_header *hdr,
                            uint32_t new_type);

/* Shift attributes in a record to open/close a gap */
uint32_t shift_attrs(uint8_t *rec, uint32_t frs_size,
                      uint32_t from_off, uint32_t old_used,
                      int32_t delta);

/* Get the next unused attribute instance ID */
uint16_t next_attr_id(const uint8_t *rec,
                       const struct ntfs_mft_header *hdr);

/* Build a non-resident attribute header with encoded data runs */
uint32_t build_nonresident_attr(uint8_t *out, uint32_t type,
                                 const char *name, uint8_t name_len,
                                 uint16_t attr_id,
                                 const struct ntfs_data_run *runs,
                                 int run_count,
                                 uint64_t alloc_size,
                                 uint64_t real_size);
