/* ============================================================================
 * entropy.h -- Early entropy source model (inventory + quality + policy)
 *
 * THE MODEL ONLY. Collectors live with their subsystems (bootloader EFI RNG,
 * RDSEED/RDRAND, TPM2_GetRandom, seed file, jitter); the boot_info seed
 * descriptor handoff and CSPRNG seeding are owned by later TODO-12 sections.
 * Runtime reseeding (csprng_add_entropy) is owned by the kernel CSPRNG
 * (02-kernel-core kernel-libraries, Monocypher section).
 *
 * Source mask + quality pack into two u32 values so the pair can later
 * travel inside the random-seed PAYLOAD's own header (the generic
 * boot_payload_desc carries no per-payload metadata fields; the seed
 * handoff section defines that in-payload header):
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

/* Policy gate (boolean accept). Release mode: returns 1 only for
 * >= ENTROPY_CLASS_MINIMUM (at least one hardware-backed HIGH source).
 * Debug mode: ALWAYS returns 1 -- proceeding degraded is allowed there,
 * and the caller must inspect cls itself for the loud DEGRADED WARN
 * (entropy_report() does this for the boot summary). */
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

/* ---- Kernel-side staged transcript ----
 * Collectors that run AFTER the bootloader handoff (jitter sampler,
 * seed file, virtio-rng) append framed records here; the CSPRNG seeding
 * section hashes this together with the boot seed payload, then calls
 * the consume API to zero it. Fixed capacity ENTROPY_STAGE_CAP; staging
 * a record that does not fit FAILS (returns 0, overflow flag set,
 * source quality NOT recorded) -- silent truncation of seed material is
 * forbidden. */
#define ENTROPY_STAGE_CAP 768u

/* Frame (src, data, len) into the staged transcript and, on success,
 * record the source at quality q. Returns 1 staged, 0 refused
 * (overflow, bad args). Safe with interrupts enabled (irqsave lock). */
int entropy_stage_source(entropy_src_t src, const uint8_t *data,
                         uint32_t len, entropy_quality_t q);

/* Atomically drain the staged transcript: copy up to cap bytes into
 * out, zero the staged bytes, reset the position -- all under one
 * irqsave lock so a concurrent producer can never append between the
 * snapshot and the wipe (its record either made the drain or is fully
 * intact for the next one). Returns the copied length, or 0 when out
 * is NULL or cap is too small for the staged content (nothing is
 * consumed in that case). */
uint32_t entropy_staged_drain(uint8_t *out, uint32_t cap);

/* Zero + reset without consuming (test/reset path). */
void entropy_staged_consume_zero(void);

/* 1 when any record was refused for lack of space (diagnostics). */
int entropy_staged_overflowed(void);

/* ---- boot_info seed payload handoff (boot_info seed handoff section) ----
 * The BOOT_PAYLOAD_RANDOM_SEED payload begins with this 32-byte header,
 * followed by exactly transcript_len bytes of framed records (the
 * entropy_frame_source framing above). source_mask / quality are ADVISORY
 * (Linux RANDOM_TRUST_BOOTLOADER analog): the kernel re-derives credit
 * from the records it actually accepts. The header is versioned
 * independently of struct boot_info -- adding fields to reserved[] needs
 * no BOOT_INFO_VERSION bump. The bootloader mirror lives in bootx64.c
 * (struct bl_seed_header); both sides pin the layout with static asserts. */
#define ENTROPY_SEED_MAGIC    0x53525049u  /* "IPRS" little-endian */
#define ENTROPY_SEED_VERSION  1u

struct entropy_seed_header {
    uint32_t magic;
    uint32_t version;
    uint32_t source_mask;     /* advisory ENTROPY_SRC_BIT() union */
    uint32_t quality;         /* advisory packed 2-bit-per-source */
    uint32_t transcript_len;  /* framed bytes following this header */
    uint32_t reserved[3];     /* zero; future fields, no version bump */
};

_Static_assert(sizeof(struct entropy_seed_header) == 32,
    "seed payload header is a bootloader-kernel handoff -- 32 bytes exactly");
_Static_assert(__builtin_offsetof(struct entropy_seed_header, transcript_len) == 16,
    "transcript_len at offset 16 -- bootloader mirror depends on this");

typedef enum {
    ENTROPY_SEED_OK = 0,
    ENTROPY_SEED_BAD_ARGS,    /* NULL payload/out */
    ENTROPY_SEED_TOO_SHORT,   /* payload smaller than the header */
    ENTROPY_SEED_BAD_MAGIC,
    ENTROPY_SEED_BAD_VERSION,
    ENTROPY_SEED_BAD_LENGTH,  /* transcript_len != payload len - header */
    ENTROPY_SEED_BAD_CRC,     /* FLAG_CHECKSUMMED set, CRC-32C mismatch */
    ENTROPY_SEED_BAD_RECORD,  /* framing violation inside the transcript */
    ENTROPY_SEED_NO_FIT       /* out cap too small for accepted records */
} entropy_seed_status_t;

/* Seed-file record verifier hook (impure side: NVRAM token + MAC; tests
 * inject a fake). Returns 1 to accept -- payload_out gets the inner
 * 32-byte payload and *counter_out the blob counter -- or 0 to reject
 * (the record is dropped, parsing continues). */
typedef int (*entropy_seed_verify_fn)(const uint8_t *blob, uint32_t len,
                                      uint64_t *counter_out,
                                      uint8_t payload_out[32]);

struct entropy_seed_parse_result {
    uint32_t hdr_mask;        /* advisory mask from the header */
    uint32_t hdr_quality;     /* advisory quality from the header */
    uint32_t records_mask;    /* sources actually present in accepted records */
    uint32_t record_count;    /* framed records copied to out */
    uint32_t seed_file_ok;    /* src-4 records the verifier accepted */
    uint32_t seed_file_rejected;
    uint64_t seed_file_counter; /* highest accepted carryover counter */
};

/* PURE payload parser (no NVRAM, no CSPRNG, no logging): validate the
 * header, the CRC-32C when checksummed != 0, and every framed record;
 * copy accepted records into out re-framed for the CSPRNG transcript.
 * Seed-file (src-4) records carry a raw 80-byte blob and are routed
 * through 'verify' -- accepted ones are re-framed as a 32-byte src-4
 * record (the verified inner payload), rejected ones are dropped without
 * failing the parse. Any structural failure rejects the WHOLE payload
 * (out is not consumed). */
entropy_seed_status_t entropy_seed_parse(
    const uint8_t *payload, uint64_t len,
    int checksummed, uint64_t checksum,
    entropy_seed_verify_fn verify,
    uint8_t *out, uint32_t cap, uint32_t *out_len,
    struct entropy_seed_parse_result *res);

/* Impure Phase 1 consumer (src/kernel/main/boot_seed.c): walk every
 * validated BOOT_PAYLOAD_RANDOM_SEED descriptor, parse it (NVRAM-backed
 * seed-file verification), record accepted sources, and digest-chain
 * each accepted transcript (Blake2b-256 over the concatenation -- one
 * descriptor at the full length cap never overflows a fixed buffer, so
 * the every-descriptor-mixed contract holds for any chain depth). Each
 * payload is wiped after use; frames return to the PMM only when OUR
 * bootloader produced the descriptor with the exclusive-page shape.
 * Writes the BOOT_SEED_DIGEST_LEN digest to out (cap must be >= that)
 * and returns its length, or 0 when no payload contributed bytes (boot
 * continues degraded). BSP boot path only -- runs once in Phase 1
 * BEFORE csprng_init(). */
#define BOOT_SEED_DIGEST_LEN  32u
uint32_t boot_seed_consume(uint8_t *out, uint32_t cap);

/* ---- Entropy diagnostics surfaces (diagnostics + policy gates section) --
 * Registry mirror, BlackBox JSON, and the one-shot admin external-entropy
 * consume (src/kernel/entropy_registry.c). All length/mask/class only --
 * seed bytes never reach any surface. Phase-3 boot path, BSP only. */
#define ENTROPY_EXTERNAL_MAX  512u
void entropy_external_consume(void);
void entropy_populate_registry(void);
void entropy_publish_json(void);

/* PURE one-shot core (unit-testable): copy min(len, cap) bytes of src to
 * dst, then WIPE the full src buffer (the source must never be readable
 * after the copy is taken). Returns the copied length; 0 on NULL/empty
 * input (nothing copied, src still wiped when non-NULL). */
uint32_t entropy_external_oneshot(uint8_t *src, uint32_t len,
                                  uint8_t *dst, uint32_t cap);

/* Human-readable class name ("degraded" / "minimum" / "good"). */
const char *entropy_class_str(entropy_class_t cls);

/* The SINGLE named early-entropy init point (kernel early CSPRNG seeding
 * section): boot_seed_consume -> csprng_init -> wipe, then logs the
 * credited class + release crypto-gate verdict. Runs once on the BSP in
 * Phase 1, BEFORE every randomness consumer (AT_RANDOM, AP canaries,
 * KUSD cookie, GUID generation, future KASLR). */
void early_entropy_init(void);

/* PURE consumability classifier for one RANDOM_SEED descriptor (the
 * load-bearing gate inside boot_seed_consume, exported for tests).
 * NOT_RESERVED, OUT_OF_MAP and BAD_LENGTH descriptors are RETIRED
 * UNTOUCHED: none of them was pinned by the reservation pass, so the
 * range may already be allocator-owned, and an out-of-map range is not
 * dereferenceable at all.
 *
 * NO_CAPABILITY is different and the distinction is load-bearing for
 * anyone writing a new consumer: it retires NOTHING and abandons the
 * whole walk, leaving every descriptor FLAG_VALID-set for the rest of
 * the boot. Clearing the flag would be mutating a handoff we just
 * declined to trust. So do NOT assume boot_payload_find() can no longer
 * rediscover a seed descriptor after boot_seed_consume() has run --
 * on a capability-refused boot it still can, and your consumer needs
 * its own capability gate exactly as this one does. */
typedef enum {
    BOOT_SEED_DESC_CONSUMABLE = 0,
    BOOT_SEED_DESC_NOT_RESERVED,  /* FLAG_RESERVED missing -- PMM never pinned it */
    BOOT_SEED_DESC_OUT_OF_MAP,    /* outside the 4 GiB boot identity map */
    BOOT_SEED_DESC_BAD_LENGTH,    /* below header size or above the payload cap */
    BOOT_SEED_DESC_NO_CAPABILITY  /* BOOT_CAP_PAYLOAD_DESCRIPTORS not negotiated */
} boot_seed_desc_class_t;

/* Payload sanity cap shared by the classifier and its tests: one
 * descriptor larger than this is malformed for the seed type. */
#define BOOT_SEED_PAYLOAD_CAP  16384ull

/* caps_present is FIRST and is checked FIRST, mirroring
 * boot_headless_authz_classify(). src/kernel/mm/boot_reserved.c gates
 * the ENTIRE payload reservation pass on BOOT_CAP_PAYLOAD_DESCRIPTORS,
 * so without that bit BOOT_PAYLOAD_FLAG_RESERVED is a claim nobody
 * acted on and the frames may already belong to the allocator. The
 * capability describes the HANDOFF, not one descriptor, so
 * NO_CAPABILITY means abandon the whole walk rather than reject one
 * entry the way the three classes above it do. */
boot_seed_desc_class_t boot_seed_desc_classify(uint64_t caps_present,
                                               uint32_t flags,
                                               uint64_t phys_start,
                                               uint64_t length);

/* PURE disposition helpers -- the OBSERVABLE SEAM for these guards.
 * A test that only drove the consume loop could not tell a guard that
 * fired from one that failed open: the loop dereferences phys_start
 * through the Phase-1 boot identity map, which is dead by the time the
 * suite runs, so a fail-open read lands somewhere that is simply not
 * the fixture's buffer and every assertion still passes. Expressing the
 * decision as DATA removes the ambiguity -- delete the capability
 * branch and these return different values, so the tests fail. Measured
 * precedent: deleting the equivalent branch in the TPM headless-
 * authorization consumer left the whole security suite green. */

/* Bytes that may be wiped at phys_start; 0 for every class whose range
 * must not be touched at all -- which now includes BAD_LENGTH, a
 * deliberate reversal of this section's first draft. A bad-length
 * descriptor is precisely the one the reservation pass refused to pin
 * (see boot_seed_length_reservable below), so its frames may already be
 * allocator-owned by Phase 1 and wiping even a clamped prefix would
 * corrupt the new owner. Leaving a malformed producer's bytes in RAM is
 * the lesser harm. Found by the post-commit adversarial round, which
 * showed that merely refusing to FREE an over-cap descriptor pinned it
 * forever instead. */
uint64_t boot_seed_desc_wipe_len(boot_seed_desc_class_t cls, uint64_t length);

/* The seed type's OWN length contract, in ONE place so the Phase-0
 * reservation pass and the Phase-1 consumer cannot drift apart. Returns
 * 1 when the declared length is inside [header size, BOOT_SEED_PAYLOAD_CAP].
 *
 * src/kernel/mm/boot_reserved.c refuses to PIN a RANDOM_SEED descriptor
 * that fails this, so a malformed handoff declaring gigabytes cannot
 * reserve an arbitrary span of RAM and starve the PMM. The consumer then
 * refuses to TOUCH the same descriptor, for exactly the reason it refuses
 * a NOT_RESERVED one. The resulting invariant is the one worth
 * remembering: the consumer touches only what the reservation pass
 * actually pinned. */
int boot_seed_length_reservable(uint64_t length);

/* Whether the descriptor's frames may be returned to the PMM at all.
 * ONLY a CONSUMABLE descriptor qualifies. BAD_LENGTH deliberately does
 * NOT: the page count could only come from the same `length` field the
 * classifier just rejected, so an overlong descriptor would free a long
 * UNWIPED suffix (and could issue ~1M pmm_free_frame calls below the
 * 4 GiB ceiling). Wiping a clamped prefix never licenses freeing an
 * unclamped range. The ownership contract in boot_seed_release_payload
 * still applies on top of this. */
int boot_seed_desc_may_free(boot_seed_desc_class_t cls);

/* PURE release helper for one consumed payload (unit-testable half of
 * the consume path): wipe 'length' bytes at 'payload' and return 1 ONLY
 * when the FULL ownership contract holds -- producer_id is our own
 * bootloader (BOOT_PRODUCER_UEFI, whose RANDOM_SEED publish contract is
 * an AllocatePages-exclusive page) AND the page shape is certified
 * (4 KiB-aligned phys_start, alignment == 4096). Alignment alone is a
 * natural-alignment hint, never ownership: a foreign producer's
 * page-aligned descriptor may point into a shared allocation. 0 =
 * wiped but frames stay reserved. */
int boot_seed_release_payload(uint8_t *payload, uint64_t length,
                              uint64_t phys_start, uint64_t alignment,
                              uint32_t producer_id);

/* The boot-seed credit TRUST BOUNDARY (exported for tests): records ONLY the
 * sources present in r->records_mask (re-derived from accepted records),
 * never the advisory r->hdr_mask -- so a rejected/cloned carryover, whose
 * records_mask bit is absent, can never be laundered into credit by an
 * adversarial all-HIGH header. SEED_FILE is force-clamped to LOW; every other
 * accepted source takes its advisory hdr_quality (entropy_record_source then
 * applies the conservative JITTER/TIME clamps). */
void boot_seed_record_sources(const struct entropy_seed_parse_result *r);

/* Collect TPM RNG output (TPM2_GetRandom over the TPM2 command
 * transport) into the staged transcript as ENTROPY_SRC_TPM_RNG.
 * Requires a 32-byte minimum before crediting HIGH and rejects
 * stuck-RNG output; absent/failed TPMs degrade silently (the report
 * line shows tpm=none). Called once from boot Phase 1 after
 * tpm_transport_init(). */
void entropy_collect_tpm(void);

/* Emit the one-line boot diagnostics summary:
 *   entropy: fw=.. cpu=.. tpm=.. oem0=.. seed=.. hwrng=.. jitter=.. time=.. (class=..)
 * Logs at WARN when the overall class is DEGRADED, INFO otherwise.
 * Called once from the Phase 3 boot path. */
void entropy_report(void);
