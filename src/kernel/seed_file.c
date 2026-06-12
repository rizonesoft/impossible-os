/* ============================================================================
 * seed_file.c -- random-seed carryover file lifecycle
 *
 * Format helpers + Phase-3 read/reseed/rotate for X:\Boot\random-seed.bin.
 * Contract, threat model, and crash-tolerance argument: kernel/seed_file.h.
 * ============================================================================ */

#include "kernel/seed_file.h"
#include "kernel/csprng.h"
#include "kernel/entropy.h"
#include "kernel/klog.h"
#include "kernel/boot_info.h"
#include "kernel/fs/vfs.h"
#include "kernel/uefi_vars.h"
#include "libc/string.h"
#include "libs/monocypher/monocypher.h"

#define SEED_FILE_PATH_CANONICAL  "X:\\Boot\\random-seed.bin"
#define SEED_FILE_PATH_PARTNER    "X:\\Boot\\random-seed.new"
/* Read cap above SEED_FILE_SIZE so an oversized file is detected as
 * BAD_LEN instead of being silently truncated to a valid-looking blob. */
#define SEED_FILE_READ_CAP        (SEED_FILE_SIZE + 16u)

/* ---- Per-machine NVRAM token ------------------------------------------- */

#define SEED_TOKEN_MAGIC    0x4B545349u  /* "ISTK" little-endian */
#define SEED_TOKEN_VERSION  1u

struct seed_token {
    uint32_t magic;
    uint32_t version;
    uint8_t  secret[SEED_FILE_SECRET_LEN];
    uint64_t last_seen;     /* highest CONSUMED counter (anti-replay) */
} __attribute__((packed));

_Static_assert(sizeof(struct seed_token) == 48,
    "seed token is an NVRAM record -- 48 bytes exactly");

static const efi_guid_t s_seed_guid = IMPOSSIBLE_OS_VENDOR_GUID_INIT;
static const uint16_t s_seed_token_name[] = {
    'I','P','O','S','S','e','e','d','T','o','k','e','n', 0
};

/* ---- Pure format helpers (unit-tested, no VFS / NVRAM) ----------------- */

void seed_file_encode(struct seed_file_blob *out, uint64_t counter,
                      const uint8_t payload[SEED_FILE_PAYLOAD_LEN],
                      const uint8_t secret[SEED_FILE_SECRET_LEN])
{
    out->magic   = SEED_FILE_MAGIC;
    out->version = SEED_FILE_VERSION;
    out->counter = counter;
    memcpy(out->payload, payload, SEED_FILE_PAYLOAD_LEN);
    crypto_blake2b_keyed(out->mac, SEED_FILE_MAC_LEN,
                         secret, SEED_FILE_SECRET_LEN,
                         (const uint8_t *)out, SEED_FILE_MACED_LEN);
}

seed_file_status_t seed_file_accept(const void *buf, uint32_t len,
                                    const uint8_t secret[SEED_FILE_SECRET_LEN],
                                    uint64_t last_seen,
                                    uint64_t *counter_out,
                                    uint8_t payload_out[SEED_FILE_PAYLOAD_LEN])
{
    const struct seed_file_blob *b = (const struct seed_file_blob *)buf;
    uint8_t mac[SEED_FILE_MAC_LEN];

    if (!buf || len != SEED_FILE_SIZE)
        return SEED_FILE_BAD_LEN;
    if (b->magic != SEED_FILE_MAGIC)
        return SEED_FILE_BAD_MAGIC;
    if (b->version != SEED_FILE_VERSION)
        return SEED_FILE_BAD_VERSION;

    crypto_blake2b_keyed(mac, SEED_FILE_MAC_LEN,
                         secret, SEED_FILE_SECRET_LEN,
                         (const uint8_t *)b, SEED_FILE_MACED_LEN);
    if (crypto_verify32(mac, b->mac) != 0) {
        crypto_wipe(mac, sizeof(mac));
        return SEED_FILE_BAD_MAC;
    }
    crypto_wipe(mac, sizeof(mac));

    if (b->counter <= last_seen)
        return SEED_FILE_REPLAY;

    if (counter_out)
        *counter_out = b->counter;
    if (payload_out)
        memcpy(payload_out, b->payload, SEED_FILE_PAYLOAD_LEN);
    return SEED_FILE_OK;
}

/* ---- VFS / NVRAM glue --------------------------------------------------- */

/* Read one candidate file. Returns number of bytes read into buf (cap
 * SEED_FILE_READ_CAP), 0 when absent/unreadable. */
static uint32_t seed_read_file(const char *path,
                               uint8_t buf[SEED_FILE_READ_CAP])
{
    struct vfs_node *node = vfs_open(path, VFS_O_READ);
    int rd;

    if (!node)
        return 0;
    rd = vfs_read(node, 0, SEED_FILE_READ_CAP, buf);
    vfs_close(node);
    return (rd > 0) ? (uint32_t)rd : 0;
}

/* Durably write one 80-byte blob: open(CREATE|TRUNC) + write + flush +
 * close, success ONLY when every step succeeds (the flush reaches
 * blkdev_sync via vfs_flush). Returns 0 on durable success. */
static int seed_write_file(const char *path,
                           const struct seed_file_blob *blob)
{
    struct vfs_node *node;
    int wr, fl, cl;

    node = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!node)
        return -1;
    wr = vfs_write(node, 0, SEED_FILE_SIZE, (const uint8_t *)blob);
    fl = vfs_flush(node);
    cl = vfs_close(node);
    if (wr != (int)SEED_FILE_SIZE || fl != 0 || cl != 0)
        return -1;
    return 0;
}

/* Load + validate the NVRAM token. Returns 1 when a well-formed token is
 * present; 0 on any failure (absent variable, short read, bad magic or
 * version, runtime services unavailable) -- callers treat every failure as
 * token-absent and take the recovery path. */
static int seed_token_load(struct seed_token *tok)
{
    size_t size = sizeof(*tok);
    NTSTATUS st = uefi_var_get(s_seed_token_name, &s_seed_guid,
                               tok, &size, NULL);
    if (st != STATUS_SUCCESS || size != sizeof(*tok) ||
        tok->magic != SEED_TOKEN_MAGIC ||
        tok->version != SEED_TOKEN_VERSION) {
        /* Firmware may have filled the buffer (e.g. right size, bad
         * magic) -- never hand a possibly-real secret back on failure;
         * callers treat failure paths as secret-free. */
        crypto_wipe(tok, sizeof(*tok));
        return 0;
    }
    return 1;
}

static NTSTATUS seed_token_store(const struct seed_token *tok)
{
    return uefi_var_set(s_seed_token_name, &s_seed_guid,
                        tok, sizeof(*tok), UEFI_VAR_NV_BOOT_RUNTIME);
}

/* ---- Early (Phase 1) payload verification ------------------------------- */

/* Highest counter the early consumer ABSORBED into the first seed this
 * boot. BSP boot path only (Phase 1 runs before APs exist; Phase 3 is
 * the single other reader) -- no lock needed, documented per the SMP
 * gate. Set ONLY via seed_file_early_mark_consumed() AFTER the whole
 * payload was accepted into the transcript: a record that merely
 * VERIFIED but whose payload was later rejected (BAD_RECORD / NO_FIT)
 * never reached the CSPRNG, and marking it consumed would make
 * seed_file_phase3() skip the only absorption it gets. */
static uint64_t g_early_counter;
static int      g_early_consumed;

void seed_file_early_mark_consumed(uint64_t counter)
{
    if (!g_early_consumed || counter > g_early_counter)
        g_early_counter = counter;
    g_early_consumed = 1;
}

/* Per-boot budget on NVRAM-backed verify attempts. The bootloader can
 * publish at most the two rotation names; a small safety margin covers
 * a legitimate earlier-stage payload. Without the cap, a malformed
 * payload packed with src-4 records could force thousands of slow UEFI
 * variable reads in Phase 1 before the first CSPRNG seed. BSP boot
 * path only -- plain counter per the SMP gate. */
#define SEED_FILE_EARLY_VERIFY_BUDGET  8u
static uint32_t g_early_verify_attempts;

int seed_file_early_verify(const uint8_t *blob, uint32_t len,
                           uint64_t *counter_out,
                           uint8_t payload_out[SEED_FILE_PAYLOAD_LEN])
{
    struct seed_token tok;
    seed_file_status_t st;
    uint64_t counter = 0;

    if (!blob || !counter_out || !payload_out)
        return 0;
    /* Length gate BEFORE any NVRAM I/O: a record that is not exactly
     * the on-disk blob size can never verify, and the reject must not
     * cost a firmware variable read. */
    if (len != (uint32_t)SEED_FILE_SIZE)
        return 0;
    if (g_early_verify_attempts >= SEED_FILE_EARLY_VERIFY_BUDGET) {
        if (g_early_verify_attempts == SEED_FILE_EARLY_VERIFY_BUDGET) {
            g_early_verify_attempts++;
            klog(LOG_WARN, "entropy",
                 "early seed: verify budget exhausted (%u attempts) -- "
                 "remaining carryover records dropped",
                 (uint64_t)SEED_FILE_EARLY_VERIFY_BUDGET);
        }
        return 0;
    }
    g_early_verify_attempts++;
    if (!seed_token_load(&tok)) {
        /* Absent/foreign token: fail closed (anti-clone). Recovery --
         * minting a fresh token + file -- is the Phase-3 lifecycle's
         * job once the CSPRNG is seeded. */
        klog(LOG_WARN, "entropy",
             "early seed: NVRAM token absent -- carryover rejected "
             "(fail closed; Phase 3 re-mints)");
        return 0;
    }
    st = seed_file_accept(blob, len, tok.secret, tok.last_seen,
                          &counter, payload_out);
    crypto_wipe(&tok, sizeof(tok));
    if (st != SEED_FILE_OK) {
        klog(LOG_WARN, "entropy",
             "early seed: carryover rejected (status=%u) -- fail closed",
             (uint64_t)st);
        return 0;
    }
    /* Side-effect-free on success: the caller (boot_seed_consume) marks
     * the counter consumed ONLY after the whole payload is accepted into
     * the first-seed transcript. */
    *counter_out = counter;
    return 1;
}

int seed_file_hw_provenance(void)
{
    /* At least one HARDWARE RNG class credited HIGH this boot. The
     * seed-file class is excluded by construction: a seed file must
     * never launder itself forward. csprng_is_seeded() alone is NOT
     * provenance -- csprng_init sets it even on degraded boots whose
     * output is only personalization-grade. */
    static const entropy_src_t hw[4] = {
        ENTROPY_SRC_FW_RNG, ENTROPY_SRC_CPU_RNG,
        ENTROPY_SRC_TPM_RNG, ENTROPY_SRC_ACPI_OEM0
    };
    uint32_t mask = entropy_source_mask();
    uint32_t q = entropy_source_quality();
    int i;

    for (i = 0; i < 4; i++) {
        if ((mask & ENTROPY_SRC_BIT(hw[i])) &&
            entropy_quality_get(q, hw[i]) == ENTROPY_Q_HIGH)
            return 1;
    }
    return 0;
}

/* ---- Phase-3 lifecycle --------------------------------------------------
 * Single boot-path caller (boot_desktop.c); all state is stack-local and
 * wiped before return. One NVRAM write per boot: the token store that is
 * either the first-boot create or the consumed-counter commit point. */

void seed_file_phase3(void)
{
    struct seed_token tok;
    struct seed_file_blob blob;
    uint8_t rdbuf[SEED_FILE_READ_CAP];
    uint8_t payload[SEED_FILE_PAYLOAD_LEN];
    uint64_t best_counter = 0;
    int have_payload = 0, best_idx = -1;
    int token_ok, token_fresh = 0;
    int saw_bad_mac = 0, saw_replay = 0, saw_any_file = 0;
    int saw_malformed = 0;
    static const char *const s_paths[2] = {
        SEED_FILE_PATH_CANONICAL, SEED_FILE_PATH_PARTNER
    };
    int i;

    if (!g_boot_info.config.seed_file) {
        /* Zero means boot.conf seed_file=off OR a pre-seed_file
         * bootloader (zero-filled _reserved byte) -- name both so a
         * binary-skew boot is not misread as operator intent. */
        klog(LOG_INFO, "entropy",
             "seed file: disabled (boot.conf seed_file=off or "
             "pre-seed_file bootloader)");
        return;
    }
    if (!csprng_is_seeded()) {
        /* Rotation and token creation both need CSPRNG output; by Phase 3
         * csprng_init() has always run, so this is a defensive gate. */
        klog(LOG_WARN, "entropy", "seed file: CSPRNG not seeded -- skipped");
        return;
    }
    if (!vfs_is_mounted('X')) {
        klog(LOG_WARN, "entropy",
             "seed file: X: not mounted -- seed=none, boot continues");
        return;
    }

    token_ok = seed_token_load(&tok);
    if (!token_ok) {
        if (!seed_file_hw_provenance()) {
            /* Degraded boot (no HIGH hardware source): csprng_fill output
             * may be predictable, so a token secret or seed payload minted
             * now would launder this boot into a HIGH-credited source on
             * the next boot. Defer until a boot with real provenance. */
            klog(LOG_WARN, "entropy",
                 "seed file: degraded boot (no HIGH hardware source) -- "
                 "token mint and rotation deferred");
            return;
        }
        /* First boot, firmware reset, or board swap: mint a fresh token.
         * The old seed file (if any) can no longer verify -- that is the
         * fail-closed anti-clone property, and the rotation below writes a
         * fresh valid file so the machine never stays degraded. */
        tok.magic   = SEED_TOKEN_MAGIC;
        tok.version = SEED_TOKEN_VERSION;
        tok.last_seen = 0;
        csprng_fill(tok.secret, SEED_FILE_SECRET_LEN);
        if (seed_token_store(&tok) != STATUS_SUCCESS) {
            klog(LOG_WARN, "entropy",
                 "seed file: NVRAM token write failed -- seed=none");
            crypto_wipe(&tok, sizeof(tok));
            return;
        }
        token_fresh = 1;
        klog(LOG_INFO, "entropy",
             "seed file: fresh NVRAM token minted (first boot or "
             "firmware reset)");
    }

    /* Read BOTH names; highest fresh counter wins (a crash mid-rotation
     * can leave either or both, possibly with DIFFERENT fresh counters).
     * Accept into a per-candidate scratch and promote to the kept payload
     * only when the counter actually wins -- otherwise a lower-counter
     * second file would overwrite the payload while best_counter keeps
     * the higher value (counter/payload mismatch at the commit). */
    for (i = 0; i < 2; i++) {
        uint64_t c;
        uint8_t scratch[SEED_FILE_PAYLOAD_LEN];
        uint32_t got = seed_read_file(s_paths[i], rdbuf);
        seed_file_status_t st;

        if (got == 0)
            continue;
        saw_any_file = 1;
        st = seed_file_accept(rdbuf, got, tok.secret, tok.last_seen,
                              &c, scratch);
        if (st == SEED_FILE_OK && c > best_counter) {
            best_counter = c;
            memcpy(payload, scratch, SEED_FILE_PAYLOAD_LEN);
            have_payload = 1;
            best_idx = i;
        } else if (st == SEED_FILE_BAD_MAC) {
            saw_bad_mac = 1;
        } else if (st == SEED_FILE_REPLAY) {
            saw_replay = 1;
        } else if (st != SEED_FILE_OK) {
            saw_malformed = 1;  /* BAD_LEN / BAD_MAGIC / BAD_VERSION */
        }
        crypto_wipe(scratch, sizeof(scratch));
    }
    crypto_wipe(rdbuf, sizeof(rdbuf));

    if (have_payload) {
        /* Pre-mix the accepted carryover UNCREDITED before anything else
         * draws from the CSPRNG: the rotation's replacement payload must
         * inherit this entropy (on a degraded boot the pre-carryover
         * generator state may be predictable -- a replacement drawn from
         * it would launder forward). Credit (HIGH accounting) waits for
         * the replacement-write + NVRAM commit below; the bytes
         * themselves are absorbed exactly once -- here, UNLESS the
         * boot_info seed handoff already folded this exact counter into
         * the Phase-1 first seed (boot_seed_consume + csprng_init). */
        if (g_early_consumed && best_counter == g_early_counter) {
            /* EXACT counter match means the same blob: a strictly LOWER
             * disk winner is a different blob whose bytes never reached
             * the first seed and must still be absorbed. */
            klog(LOG_INFO, "entropy",
                 "seed file: carryover already in the first seed "
                 "(counter=%llu) -- pre-mix skipped",
                 (unsigned long long)best_counter);
        } else {
            csprng_add_entropy(payload, SEED_FILE_PAYLOAD_LEN, ENTROPY_Q_LOW);
        }
        crypto_wipe(payload, sizeof(payload));
    }

    if (!have_payload) {
        if (saw_bad_mac && !token_fresh) {
            klog(LOG_WARN, "entropy",
                 "seed file: MAC mismatch (cloned file or token reset) -- "
                 "rejected, rewriting fresh");
        } else if (saw_replay) {
            klog(LOG_WARN, "entropy",
                 "seed file: stale counter (replay) -- rejected, rotating");
        } else if (saw_malformed) {
            klog(LOG_WARN, "entropy",
                 "seed file: malformed (torn write or corruption) -- "
                 "rejected, rewriting fresh");
        } else if (!saw_any_file && !token_fresh) {
            klog(LOG_INFO, "entropy",
                 "seed file: absent -- writing first seed file");
        }
    }

    /* Rotation provenance gate: write a new seed only when this boot's
     * CSPRNG output is trustworthy -- a HIGH hardware source collected
     * this boot, OR a fresh valid carryover (a rescued boot may re-seed
     * forward; the chain root was a provenance-OK boot because mint and
     * rotation are both gated). The EARLY-consumed carryover (boot_info
     * seed handoff, verified against the same NVRAM token) satisfies the
     * gate identically. Skipping leaves any prior file valid: last_seen
     * advances only after a durable replacement exists. */
    if (!have_payload && !g_early_consumed && !seed_file_hw_provenance()) {
        klog(LOG_WARN, "entropy",
             "seed file: degraded boot -- rotation deferred, prior file "
             "kept");
        crypto_wipe(&tok, sizeof(tok));
        return;
    }

    /* Counter base = the value the new file must exceed. Wrap guard:
     * unreachable in practice (2^64 rotations), but base + 1 wrapping to
     * 0 would replay-reject every future file -- skip instead of writing
     * a poisoned counter. */
    {
        /* The consumed counter that must become unreplayable covers BOTH
         * absorption paths: the Phase-3 disk winner AND the Phase-1
         * early-consumed payload counter -- whichever is higher. Leaving
         * the early counter out of last_seen would keep an
         * already-absorbed blob fresh for a later boot (replay). */
        uint64_t consumed_counter = have_payload ? best_counter : 0;
        int consumed_any = have_payload;
        uint64_t base;
        int first_ok;

        if (g_early_consumed) {
            consumed_any = 1;
            if (g_early_counter > consumed_counter)
                consumed_counter = g_early_counter;
        }
        base = consumed_any ? consumed_counter : tok.last_seen;
        /* NEVER truncate the file that supplied the accepted payload
         * until a higher-counter replacement is durable elsewhere: the
         * replacement goes to the OTHER name first (partner by default;
         * canonical when the partner held the winner -- the
         * crash-after-partner-write recovery shape). */
        const char *first_path = (best_idx == 1)
            ? SEED_FILE_PATH_CANONICAL : SEED_FILE_PATH_PARTNER;
        uint8_t fresh[SEED_FILE_PAYLOAD_LEN];

        if (base == (uint64_t)0xFFFFFFFFFFFFFFFFULL) {
            klog(LOG_WARN, "entropy",
                 "seed file: counter exhausted -- rotation skipped");
            crypto_wipe(payload, sizeof(payload));
            crypto_wipe(&tok, sizeof(tok));
            return;
        }

        /* REPLACEMENT BEFORE CONSUME: the base+1 file must be durable
         * BEFORE last_seen advances or the carryover is credited; a
         * failed replacement write must never burn the only fresh file
         * (it stays acceptable on the next boot). */
        csprng_fill(fresh, SEED_FILE_PAYLOAD_LEN);
        seed_file_encode(&blob, base + 1, fresh, tok.secret);
        crypto_wipe(fresh, sizeof(fresh));
        first_ok = (seed_write_file(first_path, &blob) == 0);

        if (consumed_any) {
            /* The payload bytes were already absorbed (Phase-3 pre-mix
             * above and/or the Phase-1 first seed); what remains is the
             * ACCOUNTING. COMMIT POINT: persist the consumed counter
             * BEFORE recording HIGH so a counter that was not durably
             * recorded is never advertised as fresh (anti-replay), and
             * only after the durable replacement exists. */
            if (first_ok) {
                tok.last_seen = consumed_counter;
                if (seed_token_store(&tok) == STATUS_SUCCESS) {
                    entropy_record_source(ENTROPY_SRC_SEED_FILE,
                                          ENTROPY_Q_HIGH);
                    klog(LOG_INFO, "entropy",
                         "seed file: carryover accepted (counter=%llu)",
                         (unsigned long long)consumed_counter);
                } else {
                    klog(LOG_WARN, "entropy",
                         "seed file: NVRAM commit failed -- carryover "
                         "stays uncredited (counter=%llu)",
                         (unsigned long long)consumed_counter);
                }
            } else {
                klog(LOG_WARN, "entropy",
                     "seed file: replacement write failed -- carryover "
                     "stays uncredited, prior file kept fresh");
            }
        }

        if (!first_ok) {
            if (!consumed_any)
                klog(LOG_WARN, "entropy",
                     "seed file: rotation write failed (read-only or "
                     "full media?) -- boot continues");
        } else if (best_idx == 1) {
            /* Replacement already lives at the canonical name; drop the
             * consumed partner (failure harmless -- it is replay-stale
             * after the commit above). */
            if (vfs_unlink(SEED_FILE_PATH_PARTNER) != 0)
                klog(LOG_WARN, "entropy",
                     "seed file: stale partner cleanup failed (harmless "
                     "-- replay-rejected next boot)");
            klog(LOG_INFO, "entropy",
                 "seed file: rotated (counter=%llu)",
                 (unsigned long long)(base + 1));
        } else {
            /* Unlink result intentionally ignored: absent on first
             * boot, and a torn canonical rewrite is caught by the MAC
             * with the partner copy still durable. */
            vfs_unlink(SEED_FILE_PATH_CANONICAL);
            if (seed_write_file(SEED_FILE_PATH_CANONICAL, &blob) == 0) {
                if (vfs_unlink(SEED_FILE_PATH_PARTNER) != 0)
                    klog(LOG_WARN, "entropy",
                         "seed file: partner cleanup failed (harmless "
                         "-- same counter both names)");
                klog(LOG_INFO, "entropy",
                     "seed file: rotated (counter=%llu)",
                     (unsigned long long)(base + 1));
            } else {
                klog(LOG_WARN, "entropy",
                     "seed file: canonical rewrite failed -- partner "
                     "copy is the durable seed");
            }
        }
    }

    crypto_wipe(&blob, sizeof(blob));
    crypto_wipe(&tok, sizeof(tok));
}
