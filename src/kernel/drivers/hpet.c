/* ============================================================================
 * hpet.c -- HPET main counter driver
 *
 * Maps the HPET MMIO registers as UC, reads capabilities, enables the
 * free-running 64-bit main counter. Does NOT use timer comparators.
 * ============================================================================ */

#include "kernel/drivers/hpet.h"
#include "kernel/acpi.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"

/* HPET register offsets */
#define HPET_CAP_REG    0x000   /* General Capabilities (64-bit) */
#define HPET_CFG_REG    0x010   /* General Configuration (32-bit) */
#define HPET_COUNTER    0x0F0   /* Main Counter Value (64-bit) */

/* Configuration bits */
#define HPET_ENABLE_CNF 0x01    /* Enable main counter */

/* State */
static volatile uint8_t *s_base;     /* UC-mapped MMIO base */
static uint64_t s_freq_hz;           /* counter frequency in Hz */
static uint64_t s_period_fs;         /* femtoseconds per tick */
static int      s_available;

/* MMIO accessors */
static inline uint32_t hpet_rd32(uint32_t offset)
{
    return *(volatile uint32_t *)(s_base + offset);
}

static inline uint64_t hpet_rd64(uint32_t offset)
{
    return *(volatile uint64_t *)(s_base + offset);
}

static inline void hpet_wr32(uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)(s_base + offset) = val;
}

/* ---- Init ---------------------------------------------------------------- */

int hpet_init(void)
{
    uint64_t hpet_phys;
    uint64_t cap;
    uint32_t period;

    s_available = 0;

    hpet_phys = acpi_get_hpet_base();
    if (hpet_phys == 0) {
        klog(LOG_DEBUG, "hpet", "HPET: not present (no ACPI table)");
        return -1;
    }

    /* Reject bogus addresses */
    if (hpet_phys < 0x100000 || (hpet_phys & 0xFFF) != 0) {
        klog(LOG_WARN, "hpet", "HPET: invalid MMIO address 0x%p",
             hpet_phys);
        return -1;
    }

    /* Map 4 KiB as UC -- HPET MMIO through WB pages causes MCE */
    s_base = (volatile uint8_t *)vmm_map_mmio_uc(hpet_phys, 4096);
    if (!s_base) {
        klog(LOG_WARN, "hpet", "HPET: UC mapping failed");
        return -1;
    }

    /* Probe: reject non-functional hardware */
    {
        uint32_t probe = hpet_rd32(HPET_CAP_REG);
        if (probe == 0xFFFFFFFF || probe == 0x00000000) {
            klog(LOG_WARN, "hpet", "HPET: non-functional (probe=0x%x)",
                 (uint64_t)probe);
            vmm_unmap_mmio((void *)s_base, 4096);
            s_base = (volatile uint8_t *)0;
            return -1;
        }
    }

    /* Read capabilities: upper 32 bits = period in femtoseconds */
    cap = hpet_rd64(HPET_CAP_REG);
    period = (uint32_t)(cap >> 32);
    if (period == 0 || period > 100000000) {
        klog(LOG_WARN, "hpet", "HPET: invalid period %u fs",
             (uint64_t)period);
        vmm_unmap_mmio((void *)s_base, 4096);
        s_base = (volatile uint8_t *)0;
        return -1;
    }

    s_period_fs = (uint64_t)period;
    s_freq_hz = 1000000000000000ULL / s_period_fs;

    /* Enable the main counter */
    hpet_wr32(HPET_CFG_REG, hpet_rd32(HPET_CFG_REG) | HPET_ENABLE_CNF);

    s_available = 1;

    klog(LOG_INFO, "hpet", "HPET: %u MHz (%u fs/tick), counter enabled",
         (uint64_t)(s_freq_hz / 1000000), (uint64_t)s_period_fs);

    return 0;
}

/* ---- API ----------------------------------------------------------------- */

int hpet_available(void)
{  /* INTENTIONAL-STUB: trivial accessor for s_available, a real module-level
   flag set by hpet_init() from actual hardware detection (line 102) and
   cleared on absence/failure (line 51) -- not a placeholder return */
    return s_available;
}

uint64_t hpet_frequency_hz(void)
{
    return s_available ? s_freq_hz : 0;
}

uint64_t hpet_read_counter(void)
{
    if (!s_available) return 0;
    return hpet_rd64(HPET_COUNTER);
}

uint64_t hpet_ns(void)
{
    uint64_t ticks;
    if (!s_available) return 0;

    ticks = hpet_rd64(HPET_COUNTER);
    /* ns = ticks * period_fs / 1000000 (fs -> ns). The naive product
     * ticks * period_fs overflows u64 after ~5 h of uptime (period_fs is up
     * to 1e8) even though the ns result stays in range for centuries. Split
     * the division like mono_clock's tick scaling: the quotient term carries
     * the bulk, the remainder term ( < 1e6 * period_fs ) cannot overflow. */
    return (ticks / 1000000ULL) * s_period_fs
         + ((ticks % 1000000ULL) * s_period_fs) / 1000000ULL;
}
