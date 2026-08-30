/* ============================================================================
 * stack_canary.c -- -fstack-protector-strong kernel cookie
 *
 * The kernel build sets -fstack-protector-strong -mstack-protector-guard=global,
 * so the compiler emits, for every function with stack buffers / address-taken
 * locals, a prologue that loads __stack_chk_guard and an epilogue that compares
 * it; a mismatch (stack-buffer overflow) calls __stack_chk_fail. Because the
 * kernel is -nostdlib there is no libssp, so this file defines the cookie and
 * both routines exactly once.
 *
 * Bootstrap ordering invariant: the cookie write must NOT happen inside a live
 * stack-protected frame -- such a frame saves the old (zero) cookie in its
 * prologue and would compare the new cookie in its epilogue, self-faulting on
 * return. canary_init() is therefore __attribute__((no_stack_protector)) and is
 * called from kernel_main (which never returns) AFTER boot_phase0 (CPUID up ->
 * RDRAND usable) and BEFORE boot_phase1. Functions that complete before the
 * write see zero/zero; those entered after see the real cookie; only kernel_main
 * spans the write and never checks. NT-style single global cookie.
 * ============================================================================ */

#include "kernel/security/stack_canary.h"
#include "kernel/random.h"          /* rdrand_bytes() */
#include "kernel/cpuid.h"           /* cpu_has, CPU_FEATURE_RDRAND/RDSEED */
#include "kernel/bugcheck.h"        /* KeBugCheckEx, BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE */
#include "kernel/boot_info.h"       /* boot_payload_find -- peek the firmware RNG seed */
#include "kernel/entropy.h"          /* boot_seed_length_reservable -- the shared pin contract */
#include "kernel/mm/boot_reserved.h" /* boot_reserved_payload_is_pinned -- what the pass really pinned */
#include "kernel/klog.h"
#include "kernel/nt/ntstatus.h"     /* STATUS_STACK_BUFFER_OVERRUN (canonical home) */

/* The global cookie the compiler reads/compares. Defined exactly once. */
uintptr_t __stack_chk_guard;

/* Kernel load base for the TSC fallback (linker.ld). */
extern char __kernel_start[];

static inline uint64_t canary_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* RDSEED into *out; returns 1 on success (CF=1). Gate on CPU_FEATURE_RDSEED. */
static inline int canary_rdseed(uint64_t *out)
{
    uint64_t v;
    uint8_t ok;
    for (int try = 0; try < 8; try++) {
        __asm__ volatile ("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

/* Mix entropy bytes from the bootloader's firmware-RNG seed payload WITHOUT
 * consuming/retiring it (the CSPRNG still consumes it later in Phase 1). The
 * descriptor is gated with the SAME safety contract the canonical consumer
 * (boot_seed_desc_classify) applies before dereferencing: the negotiated
 * BOOT_CAP_PAYLOAD_DESCRIPTORS capability (without it the PMM reservation pass
 * never ran, so FLAG_RESERVED certifies nothing), FLAG_RESERVED itself (an
 * unreserved range may be allocator-owned) and the [phys_start, phys_start+
 * length) range fully inside the 4 GiB boot identity map -- canary_init runs
 * pre-IDT, so an out-of-map read would #PF into the UEFI IDT and hang. The
 * payload contents are NOT credited as high-quality entropy here (this peek
 * does not parse/verify the seed records); the bytes are mixed only as
 * personalization, so the caller still warns + treats the cookie as degraded
 * unless a verified hardware source (RDRAND/RDSEED) contributed. Returns 1 if
 * bytes were mixed. */
int canary_seed_desc_ok(uint64_t caps_present, uint32_t flags,
                        uint64_t phys_start, uint64_t length)
{
    if ((caps_present & (uint64_t)BOOT_CAP_PAYLOAD_DESCRIPTORS) == 0)
        return 0;                       /* handoff never negotiated descriptors */
    if (phys_start == 0)
        return 0;                       /* absent */
    /* The SAME length contract the Phase-0 reservation pass applies, not a
     * local `length >= 16`. boot_reserved.c declines to PIN a seed
     * descriptor outside this contract while leaving FLAG_RESERVED set, so
     * a local rule that accepted more than the pass pins would dereference
     * memory nothing reserved -- and this peek is the worst place for that:
     * canary_init runs after pmm/vmm/heap init and after the boot-stack
     * guard page is unmapped (boot_hw.c:509-564) but BEFORE the IDT exists,
     * so a fault here hangs the boot with no handler. Found by the round-2
     * re-adversarial: bounding the reservation without bounding this
     * predicate created exactly that gap. */
    if (!boot_seed_length_reservable(length))
        return 0;                       /* outside the type contract -- never pinned */
    if ((flags & BOOT_PAYLOAD_FLAG_RESERVED) == 0)
        return 0;                       /* may be allocator-owned -- untouchable */
    if (phys_start >= BOOT_INFO_EARLY_MAP_END ||
        length > BOOT_INFO_EARLY_MAP_END - phys_start)
        return 0;                       /* outside the 4 GiB identity map */
    return 1;
}

static int canary_mix_boot_seed(uint64_t *acc)
{
    const struct boot_payload_desc *d =
        boot_payload_find(&g_boot_info, (uint32_t)BOOT_PAYLOAD_RANDOM_SEED, 0);
    if (!d || !canary_seed_desc_ok(g_boot_info.caps_present, d->flags,
                                   d->phys_start, d->length))
        return 0;
    /* ASK THE PASS WHAT IT ACTUALLY PINNED, do not re-derive it.
     *
     * canary_seed_desc_ok() re-checks every condition a consumer CAN
     * reconstruct from the handoff. The aggregate reservation budget and the
     * singleton rule are not among them: both depend on the other
     * descriptors in the table and on the order the pass walked it, so a
     * seed can be perfectly in contract, carry FLAG_RESERVED, and still
     * never have been pinned. This peek runs pre-IDT, where reading a frame
     * the allocator already owns has no handler at all. */
    if (!boot_reserved_payload_is_pinned(
            (uint32_t)(d - &g_boot_info.payload_descriptors[0]),
            d->phys_start, d->length))
        return 0;

    /* Read the 8 tail bytes (past any seed header) -- raw firmware RNG bytes. */
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)d->phys_start;
    uint64_t fw = 0;
    for (unsigned i = 0; i < 8; i++)
        fw |= (uint64_t)p[d->length - 8 + i] << (i * 8);
    *acc ^= fw;
    return 1;
}

uintptr_t canary_massage(uint64_t raw)
{
    raw &= ~(uint64_t)0xFF;       /* low byte 0: a string overflow that stops at
                                   * a NUL terminator cannot forge the low byte */
    raw |= (uint64_t)1 << 63;     /* bit 63 set: cookie is never 0, high byte != 0 */
    return (uintptr_t)raw;
}

__attribute__((no_stack_protector))
void canary_init(void)
{
    uint64_t raw = 0;
    const char *src;
    int credited = 0;   /* 1 once a hardware/firmware entropy source contributed */

    /* RDRAND is hardware crypto entropy and is usable as soon as CPUID has run
     * (Phase 0), well before the CSPRNG is seeded; re-seeding the cookie later
     * is unsafe because a live function frame would hold the old cookie, so a
     * single early seed is used. The source is reported by what ACTUALLY
     * succeeded, not by the CPUID bit (RDRAND can transiently fail). */
    if (cpu_has(CPU_FEATURE_RDRAND) && rdrand_bytes((uint8_t *)&raw, sizeof raw)) {
        src = "RDRAND";
        credited = 1;
    } else {
        /* No usable RDRAND. Combine every credited early source we can: RDSEED
         * (true hardware entropy) and the bootloader's firmware-RNG seed
         * payload, with a TSC + kernel-base value as the non-credited base so
         * the cookie is at least never constant. */
        uint64_t mix = canary_rdtsc() ^ (uint64_t)(uintptr_t)__kernel_start
                     ^ 0x9E3779B97F4A7C15ULL;
        src = "TSC (degraded)";

        uint64_t seed;
        if (cpu_has(CPU_FEATURE_RDSEED) && canary_rdseed(&seed)) {
            mix ^= seed;
            src = "RDSEED";
            credited = 1;     /* RDSEED is a verified hardware entropy source */
        }
        /* Mix the bootloader firmware-RNG seed as additional personalization.
         * It is NOT credited as a high-quality source: this peek does not parse
         * or verify the seed records, so the payload could be timing-only or
         * unverified carryover. It improves the cookie but does not suppress the
         * degraded warning unless RDSEED above already credited. */
        if (canary_mix_boot_seed(&mix) && !credited)
            src = "boot-seed (unverified)";
        raw = mix;
    }

    __stack_chk_guard = canary_massage(raw);
    klog(LOG_INFO, "sec", "Stack canary seeded (%s)", src);
    if (!credited)
        klog(LOG_WARN, "sec",
             "Stack canary: no hardware/firmware RNG credited -- cookie is "
             "predictable on this platform (degraded)");
}

__attribute__((noreturn))
void __stack_chk_fail(void)
{
    /* Cast to uint32_t: NTSTATUS is signed, so a bare widen to the uint64_t
     * bugcheck parameter would sign-extend 0xC0000409 to 0xFFFFFFFFC0000409.
     * Keep the historical zero-extended 0x00000000C0000409 param value. */
    KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE,
                 (uint32_t)STATUS_STACK_BUFFER_OVERRUN, 0, 0, 0);
    __builtin_unreachable();
}
