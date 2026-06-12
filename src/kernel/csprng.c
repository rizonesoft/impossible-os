/* ============================================================================
 * csprng.c -- Kernel CSPRNG (ChaCha20 fast-key-erasure, Blake2b conditioner)
 *
 * See include/kernel/csprng.h for the design contract. Built on the
 * vendored Monocypher primitives; zero heap allocation (all state is
 * static or stack-resident, matching Monocypher's own allocation model).
 *
 * Locking: one irqsave spinlock guards the global core. The lock is held
 * only for bounded work (the 64-byte ratchet or a 32-byte digest mix),
 * never across bulk keystream generation or user-memory access -- the
 * spinlock contract in kernel/sched/spinlock.h demands very short holds.
 * ============================================================================ */

#include "kernel/csprng.h"
#include "kernel/random.h"
#include "kernel/acpi.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"
#include "kernel/time/mono_clock.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/zw.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/cpu_security.h"
#include "libc/string.h"
#include "libs/monocypher/monocypher.h"

/* Seed transcript: RDRAND (37) + TSC/addr (29) + PM timer (9) frames plus
 * the staged kernel transcript (jitter + TPM RNG, up to 768 bytes). */
#define CSPRNG_TRANSCRIPT_CAP   (ENTROPY_STAGE_CAP + 128u)

/* Per-iteration kernel bounce buffer for NtGetRandom user copies. */
#define CSPRNG_COPY_CHUNK       256u

static csprng_core_t g_core;                 /* guarded by g_lock */
static spinlock_t    g_lock = SPINLOCK_INIT;
static int           g_seeded;               /* guarded by g_lock */
static uint64_t      g_fill_calls;           /* guarded by g_lock */

/* ---- Pure core ---- */

void csprng_core_seed(csprng_core_t *c, const uint8_t *transcript,
                      uint32_t len)
{
    crypto_blake2b(c->key, CSPRNG_KEY_SIZE, transcript, len);
    c->ctr = 0;
}

void csprng_core_ratchet(csprng_core_t *c, uint8_t out_key[CSPRNG_KEY_SIZE])
{
    static const uint8_t nonce[8] = {0};  /* key is single-epoch: no reuse */
    uint8_t block[CSPRNG_RATCHET_BYTES];

    crypto_chacha20_djb(block, NULL, sizeof(block), c->key, nonce, c->ctr);
    memcpy(c->key, block, CSPRNG_KEY_SIZE);          /* forward secrecy */
    memcpy(out_key, block + CSPRNG_KEY_SIZE, CSPRNG_KEY_SIZE);
    c->ctr = 0;                                      /* fresh key epoch */
    crypto_wipe(block, sizeof(block));
}

void csprng_core_stream(const uint8_t key[CSPRNG_KEY_SIZE], void *out,
                        size_t len)
{
    static const uint8_t nonce[8] = {0};  /* request key is single-use */

    crypto_chacha20_djb((uint8_t *)out, NULL, len, key, nonce, 0);
}

void csprng_core_reseed(csprng_core_t *c,
                        const uint8_t digest[CSPRNG_KEY_SIZE])
{
    uint8_t cat[2 * CSPRNG_KEY_SIZE];

    memcpy(cat, c->key, CSPRNG_KEY_SIZE);
    memcpy(cat + CSPRNG_KEY_SIZE, digest, CSPRNG_KEY_SIZE);
    crypto_blake2b(c->key, CSPRNG_KEY_SIZE, cat, sizeof(cat));
    c->ctr = 0;
    crypto_wipe(cat, sizeof(cat));
}

int csprng_core_absorb_digest(csprng_core_t *c, int seeded,
                              const uint8_t digest[CSPRNG_KEY_SIZE],
                              entropy_quality_t quality)
{
    if (seeded) {
        csprng_core_reseed(c, digest);
        return 1;
    }
    /* First material wins the seed slot; only HIGH quality marks seeded. */
    csprng_core_seed(c, digest, CSPRNG_KEY_SIZE);
    return quality == ENTROPY_Q_HIGH ? 1 : 0;
}

/* ---- Seeding ---- */

/* Frame the always-available local sources (RDRAND, TSC + link address,
 * ACPI PM timer) into 'buf'. Returns the new transcript position and
 * sets *had_hw to 1 when RDRAND contributed. Shared by the Phase 1 init
 * and the defensive emergency path. */
static uint32_t collect_local_sources(uint8_t *buf, uint32_t cap,
                                      uint32_t pos, int *had_hw)
{
    uint8_t  rd[CSPRNG_KEY_SIZE];
    uint64_t t[3];
    uint32_t pm;

    *had_hw = 0;
    if (rdrand_bytes(rd, sizeof(rd))) {
        uint32_t npos = entropy_frame_source(buf, cap, pos,
                                             ENTROPY_SRC_CPU_RNG,
                                             rd, sizeof(rd));
        if (npos != 0) {
            pos = npos;
            *had_hw = 1;
            entropy_record_source(ENTROPY_SRC_CPU_RNG, ENTROPY_Q_HIGH);
        }
        crypto_wipe(rd, sizeof(rd));
    }

    /* Timing/layout personalization: TSC sample, the link-time address of
     * this function, one PM timer tick. LOW by definition -- never
     * credited alone (the entropy model clamps TIME regardless). */
    t[0] = rdtsc_ns();
    t[1] = (uint64_t)(uintptr_t)&collect_local_sources;
    pm   = acpi_pmtimer_read_value();
    t[2] = ((uint64_t)pm << 32) ^ rdtsc_ns();
    {
        uint32_t npos = entropy_frame_source(buf, cap, pos,
                                             ENTROPY_SRC_TIME,
                                             (const uint8_t *)t, sizeof(t));
        if (npos != 0) {
            pos = npos;
            entropy_record_source(ENTROPY_SRC_TIME, ENTROPY_Q_LOW);
        }
    }
    crypto_wipe(t, sizeof(t));
    return pos;
}

void csprng_init(void)
{
    uint8_t  transcript[CSPRNG_TRANSCRIPT_CAP];
    uint32_t pos = 0;
    uint32_t staged;
    int      had_hw = 0;
    uint64_t fl;
    entropy_class_t cls;

    pos = collect_local_sources(transcript, sizeof(transcript), pos, &had_hw);

    /* Kernel-side staged boot transcript (interrupt jitter, TPM RNG).
     * Records are already source-framed; drain zeroes the staging area.
     * The boot_info seed PAYLOAD (bootloader EFI RNG / RDSEED / OEM0
     * collection) arrives via the TODO-12 seed handoff sections, which
     * feed csprng_add_entropy() -- reseeding, not first-seed-or-nothing. */
    staged = entropy_staged_drain(transcript + pos,
                                  (uint32_t)sizeof(transcript) - pos);
    pos += staged;

    spin_lock_irqsave(&g_lock, &fl);
    csprng_core_seed(&g_core, transcript, pos);
    g_seeded = 1;
    spin_unlock_irqrestore(&g_lock, fl);
    crypto_wipe(transcript, sizeof(transcript));

    cls = entropy_classify(entropy_source_mask(), entropy_source_quality());
    if (cls == ENTROPY_CLASS_DEGRADED) {
        klog(LOG_WARN, "csprng",
             "seeded DEGRADED: no hardware-backed source (rdrand=%u, "
             "staged=%u bytes) -- output is personalization-grade only",
             (uint64_t)had_hw, (uint64_t)staged);
    } else {
        klog(LOG_INFO, "csprng",
             "seeded: transcript %u bytes (staged %u), class=%s",
             (uint64_t)pos, (uint64_t)staged,
             cls == ENTROPY_CLASS_GOOD ? "good" : "minimum");
    }
}

/* Defensive: a consumer ran before Phase 1 seeding. Seed from the local
 * sources so output is never the all-zero-key stream. Caller holds g_lock. */
static void emergency_seed_locked(void)
{
    uint8_t  transcript[160];
    uint32_t pos = 0;
    int      had_hw = 0;

    pos = collect_local_sources(transcript, sizeof(transcript), pos, &had_hw);
    csprng_core_seed(&g_core, transcript, pos);
    g_seeded = 1;
    crypto_wipe(transcript, sizeof(transcript));
    klog(LOG_ERROR, "csprng",
         "emergency seed: csprng_fill() before csprng_init() "
         "(rdrand=%u) -- check boot init ordering", (uint64_t)had_hw);
}

/* ---- Global output path ---- */

void csprng_fill(void *buf, size_t len)
{
    uint8_t  rkey[CSPRNG_KEY_SIZE];
    uint64_t fl;

    if (buf == NULL || len == 0)
        return;

    spin_lock_irqsave(&g_lock, &fl);
    if (!g_seeded)
        emergency_seed_locked();
    csprng_core_ratchet(&g_core, rkey);
    g_fill_calls++;
    spin_unlock_irqrestore(&g_lock, fl);

    csprng_core_stream(rkey, buf, len);
    crypto_wipe(rkey, sizeof(rkey));
}

uint64_t csprng_u64(void)
{
    uint64_t v;

    csprng_fill(&v, sizeof(v));
    return v;
}

void csprng_add_entropy(const void *buf, uint32_t len,
                        entropy_quality_t quality)
{
    uint8_t  digest[CSPRNG_KEY_SIZE];
    uint64_t fl;

    if (buf == NULL || len == 0)
        return;

    /* Condition the input OUTSIDE the lock (input length is unbounded);
     * only the bounded digest absorb runs locked. */
    crypto_blake2b(digest, sizeof(digest), (const uint8_t *)buf, len);

    spin_lock_irqsave(&g_lock, &fl);
    g_seeded = csprng_core_absorb_digest(&g_core, g_seeded, digest, quality);
    spin_unlock_irqrestore(&g_lock, fl);
    crypto_wipe(digest, sizeof(digest));
}

int csprng_is_seeded(void)
{
    uint64_t fl;
    int s;

    spin_lock_irqsave(&g_lock, &fl);
    s = g_seeded;
    spin_unlock_irqrestore(&g_lock, fl);
    return s;
}

/* ---- NtGetRandom SSDT handler ----
 *
 * NTSTATUS NtGetRandom(PVOID Buffer, ULONG Length, ULONG Flags)
 *
 * Fills exactly Length bytes (no Linux-style short reads -- the CSPRNG
 * never blocks, so partial fills have nothing to signal). Flags must be 0;
 * the parameter exists so getrandom(2) compat flags (GRND_*) can map onto
 * it without an ABI break. Generation goes through a kernel bounce buffer
 * in bounded chunks; the CSPRNG lock is never held during the user copy
 * (csprng_fill releases it before streaming), and the copy itself goes
 * through copy_to_user (SMAP-safe, fault-recoverable) -- never a raw
 * memcpy into a ring-3 pointer.
 *
 * Destination validation is the standard NT contract: ProbeForWriteIfUser
 * range/alignment check (UserMode callers) + copy_to_user. Tighter
 * per-page User-PTE validation is a property of the whole NtXxx user-copy
 * surface (today user CR3s still share kernel identity frames below
 * MM_USER_PROBE_ADDRESS); it is owned by the per-process-isolation /
 * SMAP-enablement work, not special-cased here.
 * -> XREF: 02-kernel-core/TODO-10-kernel-security-hardening.md (SMEP/SMAP
 *    + KPTI) -- hardened copy_to_user that rejects supervisor destinations. */
NTSTATUS NtGetRandom(void *user_buf, uint64_t len, uint64_t flags)
{
    uint8_t  chunk[CSPRNG_COPY_CHUNK];
    uint8_t *dst = (uint8_t *)user_buf;
    NTSTATUS st;

    if (user_buf == NULL || len == 0 ||
        len > CSPRNG_GETRANDOM_MAX || flags != 0)
        return STATUS_INVALID_PARAMETER;

    st = ProbeForWriteIfUser(user_buf, len, 1);
    if (st != STATUS_SUCCESS)
        return st;

    while (len > 0) {
        uint32_t n = (len < sizeof(chunk)) ? (uint32_t)len
                                           : (uint32_t)sizeof(chunk);

        csprng_fill(chunk, n);
        if (copy_to_user(dst, chunk, n) != 0) {
            crypto_wipe(chunk, sizeof(chunk));
            return STATUS_ACCESS_VIOLATION;
        }
        dst += n;
        len -= n;
    }
    crypto_wipe(chunk, sizeof(chunk));
    return STATUS_SUCCESS;
}

/* 6-arg SSDT dispatch shape (avoids a function-type-mismatch cast). */
static NTSTATUS nt_get_random_ssdt(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    return NtGetRandom((void *)a1, a2, a3);
}

int csprng_register_ssdt(void)
{
    return ssdt_register(SSDT_NtGetRandom, nt_get_random_ssdt);
}
