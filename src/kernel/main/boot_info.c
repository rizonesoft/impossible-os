/* ============================================================================
 * boot_info.c -- boot_info handoff validators (S16)
 *
 * Pure functions with no side effects.  Tests call these directly with
 * synthetic buffers.  Production code in boot_phase0() calls them to
 * reject malformed handoff data BEFORE copying struct boot_info from
 * the bootloader-supplied pointer.
 *
 * Split into two phases so the boot_hw.c failure-log path can safely
 * dereference the header AFTER the address phase has confirmed the
 * pointer is aligned and within mapped memory:
 *
 *   1. boot_info_validate_addr()   -- NULL, alignment, bounds, wrap
 *   2. boot_info_validate_header() -- magic, version, size
 *
 * boot_info_validate() chains both with max_addr = (uintptr_t)-1, so
 * unit tests using kernel-VA static buffers are not rejected by the
 * early-boot 4 GiB envelope.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"

/* BOOT_INFO_MIN_ADDR (the NULL-page / IVT / BDA floor) is single-sourced
 * from kernel/boot_version_constants.h via kernel/boot_info.h, beside the
 * BOOT_INFO_PHYS_ADDR it bounds -- a _Static_assert there pins the compiled
 * handoff base above this floor, so the value cannot drift out from under
 * the check below. */

boot_result_t boot_info_validate_addr(const void *p,
                                      size_t kernel_struct_size,
                                      uintptr_t max_addr)
{
    uintptr_t addr;
    uintptr_t end;

    if (p == (const void *)0)
        return BOOT_FATAL;

    addr = (uintptr_t)p;

    if (addr < BOOT_INFO_MIN_ADDR)
        return BOOT_FATAL;

    /* 8-byte alignment: struct boot_info contains uint64_t fields and
     * the header itself is uint32 + uint16 + uint16 = 8 bytes.  Natural
     * alignment also avoids cache-line straddling on the read path. */
    if ((addr & 7u) != 0u)
        return BOOT_FATAL;

    /* Must be able to hold the header we are about to dereference. */
    if (kernel_struct_size < sizeof(struct boot_info_header))
        return BOOT_FATAL;

    /* header.size is uint16_t -- anything bigger cannot round-trip. */
    if (kernel_struct_size > 65535u)
        return BOOT_FATAL;

    /* Wraparound check: addr + size must not overflow uintptr_t.  Must
     * run before the range check below so we do not wrap into a valid
     * max_addr comparison. */
    end = addr + (uintptr_t)kernel_struct_size;
    if (end < addr)
        return BOOT_FATAL;

    /* Range check: [addr, end) must fit within max_addr.  Callers pass
     * BOOT_INFO_EARLY_MAP_END during boot_phase0() to restrict the
     * handoff to the bootloader's 4 GiB identity map; tests pass
     * (uintptr_t)-1 so kernel-VA buffers are accepted. */
    if (end > max_addr)
        return BOOT_FATAL;

    return BOOT_OK;
}

boot_result_t boot_info_validate_header(const struct boot_info_header *hdr,
                                        size_t kernel_struct_size)
{
    if (hdr == (const struct boot_info_header *)0)
        return BOOT_FATAL;

    if (hdr->magic != BOOT_INFO_MAGIC)
        return BOOT_FATAL;

    if (hdr->version != BOOT_INFO_VERSION)
        return BOOT_FATAL;

    if ((size_t)hdr->size != kernel_struct_size)
        return BOOT_FATAL;

    return BOOT_OK;
}

boot_result_t boot_info_validate(const void *p, size_t kernel_struct_size)
{
    boot_result_t r;

    r = boot_info_validate_addr(p, kernel_struct_size, (uintptr_t)-1);
    if (r != BOOT_OK)
        return r;

    return boot_info_validate_header((const struct boot_info_header *)p,
                                     kernel_struct_size);
}
