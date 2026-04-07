/* ============================================================================
 * random.h -- Shared kernel random byte source (RDRAND wrapper)
 *
 * Provides a single rdrand_bytes() helper used by GUID generation (gpt.c),
 * KUSER_SHARED_DATA Cookie (kusd_time.c), and ELF auxv AT_RANDOM stack
 * canary seed (task.c). Replaces three duplicated inline-asm copies.
 *
 * This is NOT a CSPRNG. Callers that need cryptographic-grade entropy
 * (key generation, session tokens) must use the kernel CSPRNG once it
 * lands -- see TODO-20 §5/§6.
 *
 * The implementation gates RDRAND on cpu_has(CPU_FEATURE_RDRAND), retries
 * the instruction up to 10 times per 8-byte chunk on hardware backoff,
 * and returns failure if RDRAND is unavailable or all retries fail.
 *
 * ARCH: x86-64 specific. The HAL random source for ARM64 will live in
 *       arch/arm64/random.c when domain 16 lands.
 * -> XREF: 16-architecture-ports/TODO-01 §1 -- HAL random source
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Fill 'n' bytes of 'buf' with hardware-random data via RDRAND.
 *
 * Returns:
 *   1 on success (all 'n' bytes filled with RDRAND output)
 *   0 if RDRAND is unavailable on this CPU, or if any 8-byte chunk failed
 *     all 10 retries (in which case the buffer contents are undefined --
 *     callers MUST NOT use the buffer when this returns 0).
 *
 * Thread-safety: stateless and reentrant. Each call issues fresh RDRAND
 * instructions; there is no shared state to lock.
 *
 * Cost: ~50-200 cycles per 8-byte chunk on bare metal; can be slower
 * under hypervisor emulation. Do not call from ISR or per-tick paths.
 */
int rdrand_bytes(uint8_t *buf, uint32_t n);
