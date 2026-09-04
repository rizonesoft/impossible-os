/* ============================================================================
 * pci_pm.c -- PCI Power Management capability and D0/D1/D2/D3hot state machine
 *
 * See include/kernel/drivers/pci_pm.h for the spec constraints this file is
 * shaped by. The short version: PME_Status is write-1-to-clear so no write may
 * echo it back, every transition has a mandatory recovery time during which the
 * device is unreachable, the only exit from a low-power state is D0, and a
 * device with No_Soft_Reset clear comes back from D3hot uninitialised.
 * ============================================================================ */

#include "kernel/drivers/pci_pm.h"
#include "kernel/drivers/pci.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/irql.h"
#include "kernel/time/mono_clock.h"
#include "kernel/klog.h"
#include "kernel/drivers/serial.h"

#define NS_PER_US   1000ULL
#define US_PER_SEC  1000000ULL

/* ---------------------------------------------------------------------------
 * Per-device transition ownership
 *
 * A transition is not a single register write: it reads the current state,
 * decides legality and the recovery time from it, writes, waits, and verifies.
 * Serialising only the write would let a second caller decide from a state the
 * first has already left -- planning D0 -> D2 while the device is on its way to
 * D3hot performs the prohibited D3hot -> D2 and waits 200 us where 10 ms is
 * required. So the WHOLE sequence claims the device.
 *
 * A spinlock cannot span the claim (the recovery interval is up to 10 ms, far
 * outside the hold budget in spinlock.h), so the lock protects the claim TABLE
 * and the claim itself is what excludes the second caller. A caller that finds
 * the device claimed gets PCI_DX_BUSY rather than blocking: D-state transitions
 * are driver-initiated and rare, and returning is always better than spinning
 * for milliseconds under a lock the owner needs.
 *
 * The claim binds D-state callers. It does NOT stop an unrelated driver reading
 * or writing this device's config space during the recovery interval; that is a
 * per-device gate every config client must honour, which belongs with the
 * per-device power registry rather than here.
 * ------------------------------------------------------------------------- */
static DEFINE_SPINLOCK(s_pci_pm_lock);

static struct {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
    uint8_t busy;
} s_inflight[PCI_PM_MAX_INFLIGHT];

/* Set when a transition ended without observing its recovery interval. Global
 * rather than per-device because the only cause is a qualified hardware counter
 * stopping, which is a property of the machine and not of the device being
 * transitioned. Global also means it cannot be starved: per-device poison in
 * the claim table would have let eight dead devices make every healthy one
 * unclaimable.
 *
 * Guarded by s_pci_pm_lock, not merely atomic, because an ADMISSION GATE has to
 * linearize against the thing it admits. A plain load before the PMCSR write
 * leaves a window: a caller can read "not poisoned", another CPU can poison,
 * and the first caller still writes hardware and reports success. Both the
 * claim and the write below happen under this lock with the flag, so there is
 * no such window. */
static int s_pm_poisoned;

static int pci_pm_claim(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint64_t flags;
    int free_slot = -1;

    spin_lock_irqsave(&s_pci_pm_lock, &flags);
    if (s_pm_poisoned) {
        spin_unlock_irqrestore(&s_pci_pm_lock, flags);
        return PCI_DX_POISONED;
    }
    for (int i = 0; i < PCI_PM_MAX_INFLIGHT; i++) {
        if (!s_inflight[i].busy) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (s_inflight[i].bus == bus && s_inflight[i].dev == dev
            && s_inflight[i].fn == fn) {
            spin_unlock_irqrestore(&s_pci_pm_lock, flags);
            return PCI_DX_BUSY;
        }
    }
    if (free_slot < 0) {
        spin_unlock_irqrestore(&s_pci_pm_lock, flags);
        return PCI_DX_BUSY;
    }
    s_inflight[free_slot].bus  = bus;
    s_inflight[free_slot].dev  = dev;
    s_inflight[free_slot].fn   = fn;
    s_inflight[free_slot].busy = 1;
    spin_unlock_irqrestore(&s_pci_pm_lock, flags);
    return free_slot;
}

static void pci_pm_release(int slot)
{
    uint64_t flags;
    spin_lock_irqsave(&s_pci_pm_lock, &flags);
    s_inflight[slot].busy = 0;
    spin_unlock_irqrestore(&s_pci_pm_lock, flags);
}

/* Refuse everything from here on. The slot itself is released normally: what
 * must not happen is another caller touching a device whose recovery interval
 * went unobserved, and the global refusal below stops that for every device
 * without consuming any bookkeeping that could run out. */
static char pci_pm_hex_digit(uint8_t v)
{
    static const char k_hex[] = "0123456789abcdef";
    return k_hex[v & 0xF];
}

static void pci_pm_poison(int slot, int reason)
{
    uint64_t flags;
    uint8_t bus, dev, fn;
    int first;

    spin_lock_irqsave(&s_pci_pm_lock, &flags);
    first = !s_pm_poisoned;
    bus = s_inflight[slot].bus;
    dev = s_inflight[slot].dev;
    fn  = s_inflight[slot].fn;
    s_pm_poisoned         = 1;
    s_inflight[slot].busy = 0;
    spin_unlock_irqrestore(&s_pci_pm_lock, flags);

    /* Once, and on the SERIAL PORT ONLY.
     *
     * The obvious choice here is klog(), and it is the wrong one: klog reaches
     * disk I/O, and the device whose transition just failed may BE the storage
     * controller -- so the diagnostic would submit I/O through a controller in
     * an unobserved transition, to report that the transition was unobserved.
     * serial_write_recoverable() is a port write that cannot reach VFS or any
     * device, AND is bounded: the plain serial_write() polls THRE without a
     * limit, so an absent or wedged UART would turn a bounded clock-stall
     * failure into a permanent hang with the serial lock held. The system
     * survives this event, so it takes the short per-byte wait and the
     * per-call budget rather than the terminal allowance.
     *
     * Emitted outside the lock, and only the first time: the refusal that
     * follows is machine-wide and lasts until something clears it, and
     * releasing the slot discards the only record of which device it was.
     * Silent is the one thing this must not be. */
    if (first) {
        char msg[96];
        char *w = msg;
        const char *pfx = "[pci_pm] recovery interval unobserved on ";
        while (*pfx) *w++ = *pfx++;
        *w++ = pci_pm_hex_digit(bus >> 4);  *w++ = pci_pm_hex_digit(bus);
        *w++ = ':';
        *w++ = pci_pm_hex_digit(dev >> 4);  *w++ = pci_pm_hex_digit(dev);
        *w++ = '.';
        *w++ = pci_pm_hex_digit(fn);
        *w++ = ' '; *w++ = 'r'; *w++ = 'c'; *w++ = '=';
        *w++ = (reason < 0) ? '-' : '+';
        *w++ = pci_pm_hex_digit((uint8_t)(reason < 0 ? -reason : reason));
        const char *sfx = "; PCI PM refused until cleared\n";
        while (*sfx) *w++ = *sfx++;
        *w = '\0';
        serial_write_recoverable(msg);
    }
}

#ifdef KERNEL_TESTS
void pci_pm_set_poisoned_for_test(int on)
{
    uint64_t flags;
    spin_lock_irqsave(&s_pci_pm_lock, &flags);
    s_pm_poisoned = on ? 1 : 0;
    spin_unlock_irqrestore(&s_pci_pm_lock, flags);
}
#endif /* KERNEL_TESTS */

int pci_pm_is_poisoned(void)
{
    uint64_t flags;
    spin_lock_irqsave(&s_pci_pm_lock, &flags);
    int v = s_pm_poisoned;
    spin_unlock_irqrestore(&s_pci_pm_lock, flags);
    return v;
}

void pci_pm_clear_poison(void)
{
    uint64_t flags;
    spin_lock_irqsave(&s_pci_pm_lock, &flags);
    s_pm_poisoned = 0;
    spin_unlock_irqrestore(&s_pci_pm_lock, flags);
}

/* Every config access this module makes goes through these: the poison test and
 * the access happen in ONE critical section, because a gate that tests and acts
 * separately does not gate. Guarding only the write would leave the readback
 * and the readiness poll free to touch hardware after the gate closed, which is
 * the same defect wearing a different call site.
 *
 * Lock order is s_pci_pm_lock then the config-mechanism lock inside the pci_*
 * accessors; nothing in this file ever takes them the other way round, and
 * s_pci_pm_lock is static so no out-of-file holder can exist. Each held region
 * is one config transaction -- short, but see the note in pci.c: a config
 * transaction genuinely exceeds the spinlock hold budget, and this nesting
 * additionally holds the outer lock across the inner lock's spin. */
static int pci_pm_guarded_write16(uint8_t bus, uint8_t dev, uint8_t fn,
                                  uint8_t off, uint16_t value)
{
    uint64_t flags;
    spin_lock_irqsave(&s_pci_pm_lock, &flags);
    if (s_pm_poisoned) {
        spin_unlock_irqrestore(&s_pci_pm_lock, flags);
        return PCI_DX_POISONED;
    }
    pci_write16(bus, dev, fn, off, value);
    spin_unlock_irqrestore(&s_pci_pm_lock, flags);
    return PCI_DX_OK;
}

static int pci_pm_guarded_read16(uint8_t bus, uint8_t dev, uint8_t fn,
                                 uint8_t off, uint16_t *out)
{
    uint64_t flags;
    spin_lock_irqsave(&s_pci_pm_lock, &flags);
    if (s_pm_poisoned) {
        spin_unlock_irqrestore(&s_pci_pm_lock, flags);
        return PCI_DX_POISONED;
    }
    *out = pci_read16(bus, dev, fn, off);
    spin_unlock_irqrestore(&s_pci_pm_lock, flags);
    return PCI_DX_OK;
}

static int pci_pm_guarded_read8(uint8_t bus, uint8_t dev, uint8_t fn,
                                uint8_t off, uint8_t *out)
{
    uint64_t flags;
    spin_lock_irqsave(&s_pci_pm_lock, &flags);
    if (s_pm_poisoned) {
        spin_unlock_irqrestore(&s_pci_pm_lock, flags);
        return PCI_DX_POISONED;
    }
    *out = pci_read8(bus, dev, fn, off);
    spin_unlock_irqrestore(&s_pci_pm_lock, flags);
    return PCI_DX_OK;
}

static inline void pci_pm_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

/* ---------------------------------------------------------------------------
 * Pure helpers
 * ------------------------------------------------------------------------- */

int pci_pm_pmcsr_plausible(uint16_t pmcsr)
{
    if (pmcsr == PCI_CFG_NO_RESPONSE)
        return 0;                                /* nothing answered the cycle */
    if (pmcsr & PMCSR_RESERVED_MASK)
        return 0;                                /* a device that answered reads these as zero */
    return 1;
}

int pci_pm_pmc_version_supported(uint16_t pmc)
{
    uint16_t ver = (uint16_t)(pmc & PMC_VERSION_MASK);
    return (ver >= PMC_VERSION_MIN && ver <= PMC_VERSION_MAX) ? 1 : 0;
}

int pci_pm_bdf_valid(uint8_t bus, uint8_t dev, uint8_t fn)
{
    (void)bus;   /* every uint8_t bus number is addressable */
    return (dev < PCI_MAX_DEV && fn < PCI_MAX_FUNC) ? 1 : 0;
}

uint8_t pci_cap_ptr_offset(uint8_t header_type)
{
    switch (header_type & PCI_HEADER_TYPE_MASK) {
    case PCI_HEADER_TYPE_NORMAL:
    case PCI_HEADER_TYPE_BRIDGE:
        return PCI_CAP_PTR_TYPE01;
    case PCI_HEADER_TYPE_CARDBUS:
        return PCI_CAP_PTR_TYPE2;
    default:
        /* An undefined header type has no layout this code can trust. Refusing
         * is the honest answer; guessing 0x34 would read whatever the vendor
         * put there and follow it as a pointer. */
        return 0;
    }
}

int pci_pm_cap_offset_valid(uint16_t off, uint8_t header_type)
{
    /* Capability structures are DWORD-aligned. A misaligned pointer is a
     * malformed list, not something to normalise with a mask: masking would
     * silently redirect the walk to an unrelated register. */
    if (off & 0x3)
        return 0;

    /* The lower bound is the end of the FIXED header, which differs by type. A
     * CardBus bridge holds subsystem IDs at 0x40 and the legacy-mode base
     * address at 0x44, so a type-2 pointer of 0x40 whose first byte happens to
     * read 0x01 would otherwise be accepted as a PM capability sitting on top
     * of the subsystem vendor ID. */
    uint16_t min_off = ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_CARDBUS)
                       ? PCI_CAP_OFF_MIN_CARDBUS : PCI_CAP_OFF_MIN;
    if (off < min_off)
        return 0;

    /* A capability node needs only its ID and next-pointer bytes, so the last
     * DWORD of the header is a legal place for one. The stricter footprint a PM
     * capability needs is pci_pm_cap_fits_pmcsr()'s job. */
    if (off > PCI_CAP_NODE_OFF_MAX)
        return 0;
    return 1;
}

int pci_pm_cap_fits_pmcsr(uint16_t off)
{
    return (off <= PCI_CAP_OFF_MAX) ? 1 : 0;
}

uint32_t pci_pm_recovery_delay_us(uint8_t from, uint8_t to)
{
    if (from == to)
        return 0;
    /* Any transition that involves D3hot in either direction needs 10 ms; any
     * transition that involves D2 needs 200 us. D0 <-> D1 is immediate. */
    if (from == PCI_D3HOT || to == PCI_D3HOT)
        return PCI_PM_D3HOT_DELAY_US;
    if (from == PCI_D2 || to == PCI_D2)
        return PCI_PM_D2_DELAY_US;
    return 0;
}

int pci_pm_transition_legal(uint8_t from, uint8_t to)
{
    if (from > PCI_D3HOT || to > PCI_D3HOT)
        return 0;
    if (from == to)
        return 1;
    /* A device may only be moved to a DEEPER state, or to D0. Waking part-way
     * (D3hot -> D2, D3hot -> D1, D2 -> D1) is not defined. */
    if (to == PCI_D0)
        return 1;
    return to > from;
}

int pci_pm_state_supported(uint16_t pmc, uint8_t state)
{
    switch (state) {
    case PCI_D0:
    case PCI_D3HOT:
        /* Mandatory for every PM-capable device. */
        return 1;
    case PCI_D1:
        return (pmc & PMC_D1_SUPPORT) ? 1 : 0;
    case PCI_D2:
        return (pmc & PMC_D2_SUPPORT) ? 1 : 0;
    default:
        return 0;
    }
}

uint16_t pci_pmcsr_write_value(uint16_t old, uint8_t state)
{
    /* Clear the power-state field so the new state can be inserted, and clear
     * PME_Status so the write does not acknowledge a pending wake event.
     * Writing 0 to a write-1-to-clear bit has no effect. Everything else --
     * PME_En, Data_Select, Data_Scale, reserved bits -- is preserved. */
    uint16_t v = (uint16_t)(old & (uint16_t)~(PMCSR_POWER_STATE_MASK | PMCSR_PME_STATUS));
    return (uint16_t)(v | (state & PMCSR_POWER_STATE_MASK));
}

int pci_pm_no_soft_reset(uint16_t pmcsr)
{
    return (pmcsr & PMCSR_NO_SOFT_RESET) ? 1 : 0;
}

int pci_pm_plan_transition(uint16_t pmc, uint16_t pmcsr, uint8_t target,
                           int timebase_ready, pci_pm_plan_t *out)
{
    if (!out)
        return PCI_DX_INVALID;

    uint8_t from = (uint8_t)(pmcsr & PMCSR_POWER_STATE_MASK);

    out->from         = from;
    out->to           = target;
    out->write_value  = pmcsr;
    out->delay_us     = 0;
    out->needs_write  = 0;
    out->reinit_after = 0;

    if (target > PCI_D3HOT)
        return PCI_DX_INVALID;
    if (!pci_pm_state_supported(pmc, target))
        return PCI_DX_UNSUPPORTED;
    if (!pci_pm_transition_legal(from, target))
        return PCI_DX_INVALID;

    if (from == target)
        return PCI_DX_OK;   /* already there: no write, no delay, nothing to verify */

    out->delay_us    = pci_pm_recovery_delay_us(from, target);
    out->write_value = pci_pmcsr_write_value(pmcsr, target);
    out->needs_write = 1;

    /* A device whose No_Soft_Reset is clear does not keep its configuration
     * across D3hot, so leaving D3hot lands in D0 Uninitialized. The bit is read
     * from the PRE-write value, because after the transition config space is
     * exactly what cannot be trusted. */
    out->reinit_after = (from == PCI_D3HOT && target == PCI_D0
                         && !pci_pm_no_soft_reset(pmcsr)) ? 1 : 0;

    /* Refuse BEFORE anything is written. Transitioning and only then finding the
     * mandatory delay cannot be honoured would leave the device in a state
     * nothing is allowed to touch, which is worse than not transitioning. */
    if (out->delay_us != 0 && !timebase_ready) {
        out->needs_write  = 0;
        out->reinit_after = 0;
        return PCI_DX_NO_TIMEBASE;
    }
    return PCI_DX_OK;
}

/* ---------------------------------------------------------------------------
 * Recovery delay
 *
 * sleep_ms() is deliberately NOT used: it returns immediately before the timer
 * HAL is up, and its backends can depend on timer interrupts, which a device
 * transition may run with disabled.
 *
 * The wait is a plain elapsed-subtraction spin on mono_ns(), with NO second
 * counter. What makes that safe is pci_pm_timebase_ready() below, which admits
 * only the sources read directly from hardware; the tick-derived source, which
 * stops advancing when interrupts are off, is refused before the write rather
 * than compensated for afterwards.
 *
 * Source identity proves the counter is read from hardware, not that the
 * hardware is alive, so the loop still ends on PCI_PM_CLOCK_STALL_SPINS
 * identical samples -- as a FAILURE (PCI_DX_CLOCK_STALLED), because the
 * interval genuinely was not observed. It does not substitute a second estimate
 * for the clock; it refuses.
 * ------------------------------------------------------------------------- */
static int pci_pm_delay_us(uint32_t us)
{
    if (us == 0)
        return PCI_DX_OK;

    uint64_t ns_start = mono_ns();
    uint64_t need_ns  = (uint64_t)us * NS_PER_US;
    uint64_t last     = ns_start;
    uint32_t stalled  = 0;

    /* Elapsed-subtraction rather than an absolute deadline: it cannot be
     * defeated by a counter that wraps. mono_ns() is per-CPU corrected and
     * globally floored, so migrating mid-wait cannot move it backwards. */
    for (;;) {
        uint64_t now = mono_ns();
        if ((now - ns_start) >= need_ns)
            return PCI_DX_OK;

        /* Qualifying the SOURCE proves it is read from hardware rather than
         * banked by an interrupt; it does not prove the hardware is alive. A
         * counter that has stopped would otherwise spin this CPU forever with
         * the device claimed, so the wait ends -- as a failure, because the
         * interval genuinely was not observed. */
        if (now != last) {
            last = now;
            stalled = 0;
        } else if (++stalled > PCI_PM_CLOCK_STALL_SPINS) {
            return PCI_DX_CLOCK_STALLED;
        }
        pci_pm_relax();
    }
}

/* Non-zero when a recovery interval can actually be MEASURED here.
 *
 * Existence of a clock is not the test. The tick-derived source advances only
 * when a timer interrupt fires, so it stops dead in exactly the context a
 * device transition may run in -- and a wait against a stopped clock is either
 * an infinite spin or a delay that never happened. The hardware-counter sources
 * are read directly and keep moving regardless, so those are the ones that
 * qualify.
 *
 * An earlier version accepted any clock and bounded the spin with the raw TSC
 * instead. That watchdog had to be right about per-CPU TSC offsets, migration
 * during the sample, and a ceiling that only binds a QUALIFIED counter -- three
 * ways to under-wait on a device that must not be touched yet. Refusing up
 * front is one condition instead of three, and it refuses BEFORE the write. */
/* Refuse above PASSIVE_LEVEL. A transition busy-waits for the mandatory
 * recovery interval and, leaving D3hot, may then poll for readiness; at raised
 * IRQL that holds off lower-priority interrupts and DPCs for the whole
 * duration. Checked BEFORE anything is claimed or written, so a refusal costs
 * the caller nothing but the return value. */
static int pci_pm_irql_ok(void)
{
    return KeGetCurrentIrql() == PASSIVE_LEVEL;
}

static int pci_pm_timebase_ready(void)
{
    uint32_t src = mono_clock_source_id();
    return (src == MONO_SRC_TSC || src == MONO_SRC_HPET || src == MONO_SRC_PMTMR);
}

/* ---------------------------------------------------------------------------
 * Capability discovery
 * ------------------------------------------------------------------------- */

/* The capability walk itself. Reads config space, so every caller must already
 * hold the device's claim; the public entry points below take it. */
static int pci_pmcap_find_claimed(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint16_t status;
    int rc = pci_pm_guarded_read16(bus, dev, fn, PCI_STATUS, &status);
    if (rc != PCI_DX_OK)
        return rc;

    /* 0xFFFF from a config read means no device responded. Treat it as absent
     * rather than reading a capability list out of the float value. */
    if (status == PCI_CFG_NO_RESPONSE)
        return PCI_DX_UNSUPPORTED;
    if (!(status & PCI_STATUS_CAP_LIST))
        return PCI_DX_UNSUPPORTED;

    uint8_t header_type;
    rc = pci_pm_guarded_read8(bus, dev, fn, PCI_HEADER_TYPE, &header_type);
    if (rc != PCI_DX_OK)
        return rc;

    uint8_t ptr_off = pci_cap_ptr_offset(header_type);
    if (ptr_off == 0)
        return PCI_DX_UNSUPPORTED;

    uint8_t first;
    rc = pci_pm_guarded_read8(bus, dev, fn, ptr_off, &first);
    if (rc != PCI_DX_OK)
        return rc;
    uint16_t off = first;

    for (uint32_t hops = 0; hops < PCI_CAP_WALK_MAX; hops++) {
        /* A zero pointer terminates a well-formed list: the device simply has
         * no PM capability. */
        if (off == 0)
            return PCI_DX_UNSUPPORTED;
        if (!pci_pm_cap_offset_valid(off, header_type))
            return PCI_DX_MALFORMED;

        /* ID and next pointer are adjacent bytes, so ONE 16-bit read fetches
         * both. Two byte reads would double the config transactions and the
         * lock acquisitions for every node walked, on a list bounded at 48. */
        uint16_t node;
        rc = pci_pm_guarded_read16(bus, dev, fn,
                                   (uint8_t)(off + PCI_PM_CAP_ID_OFF), &node);
        if (rc != PCI_DX_OK)
            return rc;

        if ((node & 0xFF) == PCI_CAP_ID_PM) {
            /* A PM capability that cannot hold its own PMCSR is a malformed
             * structure, not an absent one. */
            if (!pci_pm_cap_fits_pmcsr(off))
                return PCI_DX_MALFORMED;
            return (int)off;
        }

        off = (uint16_t)((node >> 8) & 0xFF);
    }

    /* The hop budget is exactly the number of DWORD-aligned node positions, so
     * a MAXIMAL legal list consumes all of them and leaves a zero next pointer
     * behind. Distinguish that from a list still running, which is cyclic or
     * otherwise malformed -- reporting a firmware defect for a legal layout is
     * as wrong as missing a real one. */
    return (off == 0) ? PCI_DX_UNSUPPORTED : PCI_DX_MALFORMED;
}

int pci_pmcap_find(uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (pci_pm_is_poisoned())
        return PCI_DX_POISONED;
    if (!pci_pm_bdf_valid(bus, dev, fn))
        return PCI_DX_INVALID;

    int slot = pci_pm_claim(bus, dev, fn);
    if (slot < 0)
        return slot;
    int rc = pci_pmcap_find_claimed(bus, dev, fn);
    pci_pm_release(slot);
    return rc;
}

int pci_pmcap_read(uint8_t bus, uint8_t dev, uint8_t fn, PCI_PMCAP *out)
{
    if (!out)
        return PCI_DX_INVALID;
    if (pci_pm_is_poisoned())
        return PCI_DX_POISONED;
    if (!pci_pm_bdf_valid(bus, dev, fn))
        return PCI_DX_INVALID;

    int slot = pci_pm_claim(bus, dev, fn);
    if (slot < 0)
        return slot;

    int off = pci_pmcap_find_claimed(bus, dev, fn);
    if (off < 0) {
        pci_pm_release(slot);
        return off;
    }

    /* Four independent config transactions, so the device can stop answering
     * partway through and leave a mixture of real and floating values. Build
     * the snapshot locally and validate it before the caller ever sees it. */
    PCI_PMCAP cap;
    cap.cap_off = (uint8_t)off;
    int rc = pci_pm_guarded_read8(bus, dev, fn,
                                  (uint8_t)(off + PCI_PM_CAP_ID_OFF), &cap.cap_id);
    if (rc == PCI_DX_OK)
        rc = pci_pm_guarded_read8(bus, dev, fn,
                                  (uint8_t)(off + PCI_PM_NEXT_OFF), &cap.next_cap);
    if (rc == PCI_DX_OK)
        rc = pci_pm_guarded_read16(bus, dev, fn,
                                   (uint8_t)(off + PCI_PM_PMC_OFF), &cap.pmc);
    if (rc == PCI_DX_OK)
        rc = pci_pm_guarded_read16(bus, dev, fn,
                                   (uint8_t)(off + PCI_PM_PMCSR_OFF), &cap.pmcsr);
    if (rc != PCI_DX_OK) {
        pci_pm_release(slot);
        return rc;
    }
    pci_pm_release(slot);

    /* Non-response first, for the same reason the getter checks it first: a
     * device that vanished mid-snapshot answers 0xFF/0xFFFF to everything, and
     * that is a failed device rather than a malformed capability. */
    if (cap.cap_id != PCI_CAP_ID_PM || !pci_pm_pmcsr_plausible(cap.pmcsr))
        return PCI_DX_FAILED;
    if (!pci_pm_pmc_version_supported(cap.pmc))
        return PCI_DX_UNSUPPORTED;

    *out = cap;
    return PCI_DX_OK;
}

/* ---------------------------------------------------------------------------
 * D-state transitions
 * ------------------------------------------------------------------------- */

int pci_get_d_state(uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (pci_pm_is_poisoned())
        return PCI_DX_POISONED;
    /* Claimed for the same reason the setter is: capability discovery reads
     * config space, and doing that inside another CPU's recovery interval is
     * exactly the access the interval forbids. A read that collides with a
     * transition in flight also has no trustworthy answer to give, so
     * PCI_DX_BUSY is the honest one. */
    if (!pci_pm_bdf_valid(bus, dev, fn))
        return PCI_DX_INVALID;

    int slot = pci_pm_claim(bus, dev, fn);
    if (slot < 0)
        return slot;

    int off = pci_pmcap_find_claimed(bus, dev, fn);
    if (off < 0) {
        pci_pm_release(slot);
        return off;
    }

    uint16_t pmc = 0, pmcsr = 0;
    int rc = pci_pm_guarded_read16(bus, dev, fn,
                                   (uint8_t)(off + PCI_PM_PMC_OFF), &pmc);
    if (rc == PCI_DX_OK)
        rc = pci_pm_guarded_read16(bus, dev, fn,
                                   (uint8_t)(off + PCI_PM_PMCSR_OFF), &pmcsr);
    pci_pm_release(slot);
    if (rc != PCI_DX_OK)
        return rc;

    /* Non-response is checked FIRST. A device that disappeared between
     * discovery and these reads answers 0xFFFF to both, and classifying that as
     * an unsupported PM revision would tell a caller the capability is wrong
     * when the truth is the device is gone. */
    if (!pci_pm_pmcsr_plausible(pmcsr))
        return PCI_DX_FAILED;

    /* Same rule as the setter: reading a power state out of a register whose
     * revision this code does not understand is interpreting bits it has no
     * contract for. */
    if (!pci_pm_pmc_version_supported(pmc))
        return PCI_DX_UNSUPPORTED;

    return (int)(pmcsr & PMCSR_POWER_STATE_MASK);
}

int pci_set_d_state(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t state,
                    int *reinit_required)
{
    if (!reinit_required)
        return PCI_DX_INVALID;
    *reinit_required = 0;

    if (state > PCI_D3HOT)
        return PCI_DX_INVALID;
    if (!pci_pm_bdf_valid(bus, dev, fn))
        return PCI_DX_INVALID;
    if (!pci_pm_irql_ok())
        return PCI_DX_IRQL;
    if (pci_pm_is_poisoned())
        return PCI_DX_POISONED;

    /* Claim BEFORE any config access to this device, capability discovery
     * included: discovery reads STATUS, the header type and the capability
     * list, and those reads are as forbidden inside another CPU's recovery
     * interval as the PMCSR readback is. Every exit below releases. */
    int slot = pci_pm_claim(bus, dev, fn);
    if (slot < 0)
        return slot;

    int off = pci_pmcap_find_claimed(bus, dev, fn);
    if (off < 0) {
        pci_pm_release(slot);
        return off;
    }

    uint8_t  pmcsr_off = (uint8_t)(off + PCI_PM_PMCSR_OFF);
    uint16_t pmc = 0, pmcsr = 0;
    int prc = pci_pm_guarded_read16(bus, dev, fn,
                                    (uint8_t)(off + PCI_PM_PMC_OFF), &pmc);
    if (prc == PCI_DX_OK)
        prc = pci_pm_guarded_read16(bus, dev, fn, pmcsr_off, &pmcsr);
    if (prc != PCI_DX_OK) {
        pci_pm_release(slot);
        return prc;
    }

    if (!pci_pm_pmcsr_plausible(pmcsr)) {
        pci_pm_release(slot);
        return PCI_DX_FAILED;   /* the device is not answering; plan nothing */
    }
    if (!pci_pm_pmc_version_supported(pmc)) {
        /* A reserved revision is not a capability this code knows how to drive,
         * whether that is corrupt silicon or a future encoding. Discovery is
         * structural and may pass it; anything that INTERPRETS or WRITES PMCSR
         * may not. */
        pci_pm_release(slot);
        return PCI_DX_UNSUPPORTED;
    }

    pci_pm_plan_t plan;
    int rc = pci_pm_plan_transition(pmc, pmcsr, state, pci_pm_timebase_ready(), &plan);
    if (rc != PCI_DX_OK || !plan.needs_write) {
        /* A genuine no-op reports NO obligation, deliberately.
         *
         * Claiming one here on the grounds that No_Soft_Reset is clear was
         * tried and withdrawn: that bit describes what a D3hot->D0 transition
         * WOULD do, not that this device's current D0 came from one, so a
         * freshly booted or actively running controller would be handed an
         * instruction to restore its BARs, command register and interrupt line.
         * Against live DMA and live queues that is destructive, not a harmless
         * extra write, which is what made over-reporting look like the safe
         * direction. Telling them apart needs history this module does not
         * keep; the per-device pending bit belongs with the power registry. */
        pci_pm_release(slot);
        return rc;
    }

    rc = pci_pm_guarded_write16(bus, dev, fn, pmcsr_off, plan.write_value);
    if (rc != PCI_DX_OK) {
        pci_pm_release(slot);
        return rc;   /* poisoned between admission and the write; nothing written */
    }

    /* From here the write HAS happened, so the obligation is real whatever
     * follows. Reporting it on the failure paths too is what stops a caller
     * that retries after a timeout from seeing a D0 no-op and concluding the
     * device was never reset. */
    *reinit_required = plan.reinit_after;

    /* The device is unreachable until the recovery time elapses, including for
     * the readback below. */
    rc = pci_pm_delay_us(plan.delay_us);
    if (rc != PCI_DX_OK) {
        pci_pm_poison(slot, rc);
        return rc;
    }

    /* Leaving D3hot without No_Soft_Reset means config space came back
     * undefined, and a device in that state can take longer than the spec
     * minimum to answer config cycles at all. Poll until it responds before
     * treating any read of it as evidence. */
    if (plan.reinit_after) {
        uint32_t waited = 0;
        for (;;) {
            uint16_t vendor = 0;
            int vrc = pci_pm_guarded_read16(bus, dev, fn, PCI_VENDOR_ID, &vendor);
            if (vrc != PCI_DX_OK) {
                pci_pm_release(slot);
                return vrc;
            }
            /* Two distinct not-ready answers, and only one of them is silence.
             * A PCIe function still initialising answers Request Retry Status,
             * which surfaces as vendor 0x0001 -- a real value, so "anything but
             * all-ones is ready" would call an explicitly not-ready device
             * ready and touch it mid-reset. */
            if (vendor != PCI_CFG_NO_RESPONSE && vendor != PCI_VENDOR_ID_RRS)
                break;
            if (waited >= PCI_PM_D0_READY_MAX_US) {
                pci_pm_release(slot);
                klog(LOG_WARN, "pci_pm", "%02x:%02x.%u did not answer after D3hot->D0",
                     (uint64_t)bus, (uint64_t)dev, (uint64_t)fn);
                return PCI_DX_FAILED;
            }
            /* A backoff that could not be observed is not a reason to resume
             * touching the device: return the exact status so the caller sees
             * PCI_DX_CLOCK_STALLED rather than a generic failure derived from a
             * read taken too early. */
            int drc = pci_pm_delay_us(PCI_PM_D0_READY_STEP_US);
            if (drc != PCI_DX_OK) {
                pci_pm_poison(slot, drc);
                return drc;
            }
            waited += PCI_PM_D0_READY_STEP_US;
        }
    }

    uint16_t after = 0;
    rc = pci_pm_guarded_read16(bus, dev, fn, pmcsr_off, &after);
    pci_pm_release(slot);
    if (rc != PCI_DX_OK)
        return rc;

    /* Check BEFORE masking. 0xFFFF is a device that did not answer, and its low
     * two bits are exactly D3hot -- so a removed or failed device would confirm
     * the very suspend it failed to perform, and the caller would carry on
     * believing DMA and interrupts were quiesced. */
    if (!pci_pm_pmcsr_plausible(after)) {
        klog(LOG_WARN, "pci_pm", "%02x:%02x.%u did not answer after D%u->D%u",
             (uint64_t)bus, (uint64_t)dev, (uint64_t)fn,
             (uint64_t)plan.from, (uint64_t)state);
        return PCI_DX_FAILED;
    }

    if ((after & PMCSR_POWER_STATE_MASK) != (state & PMCSR_POWER_STATE_MASK)) {
        klog(LOG_WARN, "pci_pm", "%02x:%02x.%u D%u->D%u did not latch (PMCSR=%04x)",
             (uint64_t)bus, (uint64_t)dev, (uint64_t)fn,
             (uint64_t)plan.from, (uint64_t)state, (uint64_t)after);
        return PCI_DX_FAILED;
    }

    return PCI_DX_OK;
}
