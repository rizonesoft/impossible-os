/* le_bytes.h -- little-endian loads from an untrusted byte buffer.
 *
 * A parser of on-disk or on-wire bytes cannot dereference a multibyte field at
 * an offset the input controls: the address may be odd, which is undefined
 * behaviour in C and a fault on a strict-alignment target, and the trap-mode
 * UBSan build stops on it. These assemble the value byte by byte, which clang
 * folds to a single unaligned load on x86-64.
 */
#ifndef KERNEL_LE_BYTES_H
#define KERNEL_LE_BYTES_H

#include "kernel/types.h"

static inline uint16_t le16_at(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t le32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t le64_at(const uint8_t *p)
{
    return (uint64_t)le32_at(p) | ((uint64_t)le32_at(p + 4) << 32);
}

#endif /* KERNEL_LE_BYTES_H */
