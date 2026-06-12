/* ============================================================================
 * entropy.h -- Early entropy source model (inventory + quality + policy)
 *
 * THE MODEL ONLY. Collectors live with their subsystems (bootloader EFI RNG,
 * RDSEED/RDRAND, TPM2_GetRandom, seed file, jitter); the boot_info seed
 * descriptor handoff and CSPRNG seeding are owned by later TODO-12 sections.
 * Runtime reseeding (csprng_add_entropy) is owned by the kernel CSPRNG
 * (02-kernel-core kernel-libraries, Monocypher section).
 *
 * Source mask + quality pack into two u32 values so the pair can later be
 * mirrored verbatim into the boot_info seed payload descriptor:
 *   mask     -- bit n set means source class n contributed bytes
 *   quality  -- 2 bits per source class (ENTROPY_Q_*), source n at bits 2n
 *
 * CONDITIONER CONTRACT (implemented when Monocypher lands): seed material
 * is combined as a source-tagged, length-framed transcript hashed with
 * Blake2b -- never raw XOR, so a weak or attacker-influenced source cannot
 * cancel a strong one. Each contribution is framed as:
 *   u8 src_id | u32 len (little-endian) | len bytes payload
 * entropy_frame_source() below builds that framing today; the hash arrives
 * with the kernel CSPRNG. Quality bits carried in a boot descriptor are
 * ADVISORY (Linux RANDOM_TRUST_BOOTLOADER analog): the kernel re-derives
 * the overall class with entropy_classify() and decides credit itself.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Source classes (mask bit index) ---- */
typedef enum {
    ENTROPY_SRC_FW_RNG      = 0,   /* EFI_RNG_PROTOCOL output */
    ENTROPY_SRC_CPU_RNG     = 1,   /* RDSEED preferred, RDRAND fallback */
    ENTROPY_SRC_TPM_RNG     = 2,   /* TPM2_GetRandom */
    ENTROPY_SRC_ACPI_OEM0   = 3,   /* ACPI OEM0 entropy table (Win11 parity) */
    ENTROPY_SRC_SEED_FILE   = 4,   /* X:\Boot\random-seed.bin carryover */
    ENTROPY_SRC_GUEST_HWRNG = 5,   /* virtio-rng (post-PCI; runtime reseed only) */
    ENTROPY_SRC_JITTER      = 6,   /* boot timing / interrupt jitter */
    ENTROPY_SRC_TIME        = 7,   /* wall clock / TSC sample */
    ENTROPY_SRC_COUNT       = 8
} entropy_src_t;

#define ENTROPY_SRC_BIT(src)  (1u << (src))

/* ---- Per-source quality class (2 bits each in the packed u32) ----
 * Conservative by design: jitter and time NEVER classify above LOW, and
 * a VM-deterministic timing source classifies NONE (handled by the
 * collector passing ENTROPY_Q_NONE; the model does not inspect platform). */
typedef enum {
    ENTROPY_Q_NONE = 0,   /* source absent or output rejected */
    ENTROPY_Q_LOW  = 1,   /* personalization only -- never credited alone */
    ENTROPY_Q_HIGH = 2    /* hardware-backed, full credit */
    /* value 3 reserved */
} entropy_quality_t;

/* ---- Overall classification ---- */
typedef enum {
    ENTROPY_CLASS_DEGRADED = 0,  /* no HIGH source; LOW-only or empty */
    ENTROPY_CLASS_MINIMUM  = 1,  /* exactly one HIGH source */
    ENTROPY_CLASS_GOOD     = 2   /* two or more independent HIGH sources */
} entropy_class_t;

/* Pack/unpack a source's quality in the 2-bit-per-source u32. */
static inline uint32_t entropy_quality_set(uint32_t packed, entropy_src_t src,
                                           entropy_quality_t q)
{
    uint32_t shift = (uint32_t)src * 2u;
    return (packed & ~(3u << shift)) | (((uint32_t)q & 3u) << shift);
}

static inline entropy_quality_t entropy_quality_get(uint32_t packed,
                                                    entropy_src_t src)
{
    return (entropy_quality_t)((packed >> ((uint32_t)src * 2u)) & 3u);
}

/* Derive the overall class from mask + packed quality. Pure. */
entropy_class_t entropy_classify(uint32_t mask, uint32_t quality);

/* Policy gate: release mode requires >= ENTROPY_CLASS_MINIMUM (at least
 * one hardware-backed HIGH source); debug mode accepts anything but the
 * caller is expected to log DEGRADED loudly. Returns 1 = OK to proceed
 * with cryptographic operations, 0 = refuse (release) / degraded (debug). */
int entropy_policy_ok(entropy_class_t cls, int release_mode);

/* Append one source-tagged, length-framed contribution to the transcript
 * buffer: u8 src_id | u32 len LE | payload. Returns the new position, or
 * 0 when the framed record would not fit in cap (caller treats as hard
 * failure -- silent truncation of seed material is forbidden). */
uint32_t entropy_frame_source(uint8_t *buf, uint32_t cap, uint32_t pos,
                              entropy_src_t src, const uint8_t *data,
                              uint32_t len);

/* Record a collected source into the kernel-global model (BSP boot path;
 * collectors call this as they run). Quality is clamped: JITTER and TIME
 * never exceed LOW regardless of what the collector claims. */
void entropy_record_source(entropy_src_t src, entropy_quality_t q);

/* Read back the global mask / packed quality (diagnostics, descriptor). */
uint32_t entropy_source_mask(void);
uint32_t entropy_source_quality(void);

/* Emit the one-line boot diagnostics summary:
 *   entropy: fw=.. cpu=.. tpm=.. oem0=.. seed=.. hwrng=.. jitter=.. time=.. (class=..)
 * Logs at WARN when the overall class is DEGRADED, INFO otherwise.
 * Called once from the Phase 3 boot path. */
void entropy_report(void);
