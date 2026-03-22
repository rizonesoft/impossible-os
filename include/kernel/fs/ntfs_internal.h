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

static inline uint64_t ntfs_le64(const uint8_t *p)
{
    return (uint64_t)ntfs_le32(p) | ((uint64_t)ntfs_le32(p + 4) << 32);
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
