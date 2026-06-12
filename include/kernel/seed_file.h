/* ============================================================================
 * seed_file.h -- random-seed carryover file (X:\Boot\random-seed.bin)
 *
 * Persists 32 bytes of CSPRNG output across boots so the next boot can mix
 * prior-session randomness (systemd-boot / Windows registry-seed parity).
 * Early-entropy TODO section 6 owns this module: the on-disk FORMAT, the
 * per-machine UEFI NVRAM anti-clone token, the crash-tolerant rotation, and
 * the Phase-3 csprng_add_entropy() reseed. The EARLY first-seed read of the
 * same format (bootloader, pre-ExitBootServices, into the boot_info seed
 * payload) is owned by section 7 -- this module never feeds csprng_init().
 *
 * On-disk blob (little-endian, 80 bytes):
 *     offset  0  u32  magic    "IPSD" (0x44535049)
 *     offset  4  u32  version  SEED_FILE_VERSION
 *     offset  8  u64  counter  monotonic rotation counter (anti-replay)
 *     offset 16  u8[32] payload  CSPRNG output from the writing boot
 *     offset 48  u8[32] mac      keyed Blake2b-256 over bytes 0..47
 *
 * The MAC key is a 32-byte per-machine secret kept in the IPOSSeedToken
 * UEFI NVRAM variable together with the highest CONSUMED counter
 * (last_seen). A seed file copied from another machine fails the MAC
 * (anti-clone); a restored old file fails counter > last_seen (anti-replay).
 * Both fail CLOSED: the payload is never credited, and once the CSPRNG is
 * seeded the lifecycle writes a fresh valid file so the machine never stays
 * degraded (mismatch recovery). Persisting last_seen is the COMMIT POINT:
 * the payload is credited HIGH only after the NVRAM update succeeds; on
 * persist failure it is mixed UNCREDITED (LOW -- conditioning never hurts)
 * so a replayable counter is never advertised as fresh.
 *
 * Rotation is crash-tolerant WITHOUT rename (FAT32 LFN rename is not
 * power-fail-atomic -- boot-policy doctrine): write partner file + flush +
 * close-success, delete canonical, rewrite canonical, delete partner.
 * Durability comes from vfs_flush() reaching blkdev_sync() (FAT32 flush
 * propagates sector-cache write errors). Every crash point leaves at least
 * one file whose MAC verifies; the reader scans both names and consumes the
 * highest fresh counter.
 *
 * The encode/accept helpers are PURE (no VFS, no NVRAM) so unit tests cover
 * format, MAC, clone, and replay rejection without live boot infrastructure.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

#define SEED_FILE_MAGIC        0x44535049u  /* "IPSD" little-endian */
#define SEED_FILE_VERSION      1u
#define SEED_FILE_PAYLOAD_LEN  32u
#define SEED_FILE_MAC_LEN      32u
#define SEED_FILE_SECRET_LEN   32u
#define SEED_FILE_MACED_LEN    48u          /* magic..payload, MAC input */
#define SEED_FILE_SIZE         80u

struct seed_file_blob {
    uint32_t magic;
    uint32_t version;
    uint64_t counter;
    uint8_t  payload[SEED_FILE_PAYLOAD_LEN];
    uint8_t  mac[SEED_FILE_MAC_LEN];
} __attribute__((packed));

_Static_assert(sizeof(struct seed_file_blob) == SEED_FILE_SIZE,
    "seed file blob is an on-disk format -- 80 bytes exactly");
_Static_assert(__builtin_offsetof(struct seed_file_blob, counter) == 8,
    "seed file counter at offset 8 -- on-disk format contract");
_Static_assert(__builtin_offsetof(struct seed_file_blob, payload) == 16,
    "seed file payload at offset 16 -- on-disk format contract");
_Static_assert(__builtin_offsetof(struct seed_file_blob, mac) == SEED_FILE_MACED_LEN,
    "seed file MAC at offset 48 -- MAC covers every byte before it");

typedef enum {
    SEED_FILE_OK = 0,
    SEED_FILE_BAD_LEN,      /* not exactly SEED_FILE_SIZE bytes */
    SEED_FILE_BAD_MAGIC,
    SEED_FILE_BAD_VERSION,
    SEED_FILE_BAD_MAC,      /* tampered, torn write, or other machine's token */
    SEED_FILE_REPLAY        /* valid MAC but counter <= last consumed */
} seed_file_status_t;

/* Build an 80-byte blob: header + payload + keyed-Blake2b MAC. Pure. */
void seed_file_encode(struct seed_file_blob *out, uint64_t counter,
                      const uint8_t payload[SEED_FILE_PAYLOAD_LEN],
                      const uint8_t secret[SEED_FILE_SECRET_LEN]);

/* Validate a candidate blob read from disk: length, magic, version, MAC
 * (constant-time compare), then anti-replay against last_seen. On
 * SEED_FILE_OK copies counter/payload to the out params (either may be
 * NULL). Checks run in fixed order so a tampered counter fails BAD_MAC,
 * never REPLAY. Pure. */
seed_file_status_t seed_file_accept(const void *buf, uint32_t len,
                                    const uint8_t secret[SEED_FILE_SECRET_LEN],
                                    uint64_t last_seen,
                                    uint64_t *counter_out,
                                    uint8_t payload_out[SEED_FILE_PAYLOAD_LEN]);

/* 1 when at least one HARDWARE RNG class (FW/CPU/TPM/OEM0) is credited
 * HIGH in the global entropy record this boot. Token mint and rotation
 * are gated on this (or on a freshly credited carryover) so a degraded
 * boot's predictable CSPRNG output is never persisted and laundered into
 * a HIGH-credited source on the next boot. Reads the global record. */
int seed_file_hw_provenance(void);

/* Phase-3 boot lifecycle: read + verify + reseed + rotate. Called once from
 * the Phase 3 boot path after X: is mounted and the CSPRNG is seeded; not
 * SMP-shared (single boot-path caller, no retained state). Degrades with a
 * WARN and never blocks boot: missing X:, absent file, NVRAM-less firmware,
 * and write failures all log and return. boot.conf seed_file=off skips
 * everything. */
void seed_file_phase3(void);
