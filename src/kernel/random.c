/* ============================================================================
 * random.c -- Shared kernel random byte source (RDRAND wrapper)
 *
 * Implements rdrand_bytes() for use by gpt.c (GUID v4), kusd_time.c
 * (KUSER_SHARED_DATA Cookie), and task.c (ELF auxv AT_RANDOM canary seed).
 *
 * Replaces three duplicated inline-asm copies that previously lived in
 * gpt.c (rdrand_fill), kusd_time.c (kusd_rdrand32), and would have been
 * a third copy in task.c. Per the kernel-code-quality Gate 10 third-
 * occurrence rule, the pattern is now extracted to one place.
 *
 * ARCH: x86-64 specific -- the RDRAND instruction is x86-only. When the
 * ARM64 port lands (domain 16), the HAL will provide an arch-neutral
 * random_bytes() that dispatches to RDRAND on x86 and DRBG on ARM64.
 * -> XREF: 16-architecture-ports/TODO-01 §1
 * ============================================================================ */

#include "kernel/random.h"
#include "kernel/cpuid.h"

int rdrand_bytes(uint8_t *buf, uint32_t n)
{
    uint32_t i;

    if (!buf || n == 0)
        return 0;

    if (!cpu_has(CPU_FEATURE_RDRAND))
        return 0;

    for (i = 0; i < n; i += 8) {
        uint64_t val;
        int ok, retries = 10;

        do {
            __asm__ volatile (
                "rdrand %0\n\t"
                "setc   %1\n\t"
                : "=r"(val), "=qm"(ok)
            );
        } while (!ok && --retries > 0);

        if (!ok)
            return 0;

        /* Copy as many bytes as remain in this chunk (1..8). */
        {
            uint32_t remain = n - i;
            uint32_t copy = remain > 8 ? 8 : remain;
            uint32_t j;
            for (j = 0; j < copy; j++)
                buf[i + j] = (uint8_t)(val >> (j * 8));
        }
    }

    return 1;
}
