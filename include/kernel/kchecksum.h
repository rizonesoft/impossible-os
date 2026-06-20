/* ============================================================================
 * kchecksum.h -- Kernel checksum dispatch (CRC-32 IEEE + CRC-32C Castagnoli)
 *
 * One kernel-owned CRC layer so subsystems stop hand-rolling incompatible
 * variants. Tables are precomputed const (immutable rodata) -- no lazy init, so
 * concurrent first-use across CPUs cannot race a partially-built table. CRC-32C
 * uses the SSE4.2 `crc32` instruction (a general-register instruction, safe
 * under -mno-sse2) when every online CPU supports it, else a software table;
 * both produce byte-identical results.
 *
 * GPT (gpt_crc32) and the pre-EBS bootloader keep their own local CRC copies on
 * purpose: GPT is its own spec surface and the bootloader cannot link kernel
 * objects.
 *
 * XREF: 02-kernel-core/TODO-03-kernel-libraries.md
 * ============================================================================ */
#ifndef KERNEL_KCHECKSUM_H
#define KERNEL_KCHECKSUM_H

#include "kernel/types.h"

/* IEEE 802.3 CRC-32 (reflected poly 0xEDB88320, init/xorout 0xFFFFFFFF). */
uint32_t kcrc32(const void *data, size_t len);

/* Continue a CRC-32 over more data. `crc` MUST be the FINALIZED value a prior
 * kcrc32 / kcrc32_cont returned (the public CRC, post-xorout); the result is
 * finalized again, so kcrc32_cont(kcrc32(a, na), b, nb) == kcrc32(a||b). Seed a
 * fresh stream with crc == 0 (kcrc32(d, n) == kcrc32_cont(0, d, n)). */
uint32_t kcrc32_cont(uint32_t crc, const void *data, size_t len);

/* CRC-32C (Castagnoli, reflected poly 0x82F63B78). SSE4.2 hardware path when
 * cpu_has(CPU_FEATURE_SSE4_2) (gated on the all-online-CPU feature
 * intersection), else the software table; identical output either way. */
uint32_t kcrc32c(const void *data, size_t len);

/* Continue a CRC-32C; same finalized-in / finalized-out contract as
 * kcrc32_cont (seed a fresh stream with crc == 0). */
uint32_t kcrc32c_cont(uint32_t crc, const void *data, size_t len);

#ifdef KERNEL_TESTS
/* Test-only path forcers so a unit test can prove the software-table and SSE4.2
 * hardware CRC-32C paths are byte-identical. kcrc32c_hw_test MUST only be called
 * when cpu_has(CPU_FEATURE_SSE4_2) is true (else the crc32 instruction #UDs). */
uint32_t kcrc32c_sw_test(const void *data, size_t len);
uint32_t kcrc32c_hw_test(const void *data, size_t len);
#endif

#endif /* KERNEL_KCHECKSUM_H */
