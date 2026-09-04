/* ============================================================================
 * pci_pm.h -- PCI Power Management capability and D0/D1/D2/D3hot state machine
 *
 * Implements the config-space half of PCI device power management: discovery of
 * the PCI Power Management capability (cap ID 0x01) in the capability list, and
 * the D-state transitions the PCI Bus Power Management Interface Specification
 * defines entirely within the PMCSR register.
 *
 * D3cold is NOT here. Removing VCC from a device is a platform action expressed
 * as the ACPI _PS0/_PS3 control methods, which needs an initialised ACPI
 * namespace this kernel does not have yet. It is owned separately.
 *
 * Spec notes that shaped this API
 * -------------------------------
 *   - PMCSR bit 15 (PME_Status) is WRITE-1-TO-CLEAR. A naive read-modify-write
 *     of the power-state field writes the read value of that bit straight back,
 *     which ACKNOWLEDGES and destroys a pending wake event. Every write here
 *     goes through pci_pmcsr_write_value(), which masks PME_Status out.
 *   - Every transition has a mandatory recovery time during which the device
 *     must not be accessed AT ALL, including config space. It is not only the
 *     transitions OUT of a low-power state: entering D3hot needs 10 ms and
 *     entering D2 needs 200 us before the next access, so even reading the
 *     state back to confirm the write must wait.
 *
 *     WHAT THIS MODULE ACTUALLY ENFORCES IS NARROWER, and the gap is stated
 *     rather than papered over: the per-device claim excludes other D-STATE
 *     callers for the whole sequence, and every config access this module makes
 *     goes through its own guarded accessors. It does NOT stop an unrelated
 *     driver reading its own device during the interval, because there is no
 *     per-device state for the rest of the PCI layer to consult yet. That gate
 *     belongs with the device power registry and is filed there.
 *   - A device may not be moved to a shallower state other than D0. D3hot->D1
 *     and D2->D1 are illegal; the only exit from D3hot is D0.
 *   - When PMCSR No_Soft_Reset is clear, a D3hot->D0 transition leaves the
 *     device in "D0 Uninitialized": its config space is undefined and the
 *     driver must fully reinitialise it. A PMCSR readback showing D0 does NOT
 *     mean the device is usable. Callers ask pci_pm_no_soft_reset().
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Capability list walk -------------------------------------------------
 * The Capabilities Pointer lives at a different config offset for CardBus
 * bridges than for ordinary devices and bridges, and a capability structure
 * must be DWORD-aligned and inside the device-specific region of config space.
 */
#define PCI_CAP_ID_PM             0x01  /* Power Management capability ID */
#define PCI_CAP_PTR_TYPE01        0x34  /* Capabilities Pointer, header type 0/1 */
#define PCI_CAP_PTR_TYPE2         0x14  /* Capabilities Pointer, CardBus (type 2) */
#define PCI_CAP_OFF_MIN           0x40  /* first byte past a type 0/1 header */
#define PCI_CAP_OFF_MIN_CARDBUS   0x48  /* type 2 keeps subsystem IDs at 0x40 and
                                         * the legacy-mode base at 0x44, so its
                                         * capabilities cannot start before 0x48 */
#define PCI_CAP_NODE_OFF_MAX      0xFC  /* last DWORD a capability NODE may start
                                         * at: ID and next pointer fit there */
#define PCI_CAP_OFF_MAX           0xF8  /* last offset a PM capability may start
                                         * at, because its PMCSR at +4 is 16 bits
                                         * wide and must end within 0xFF. This is
                                         * a PM FOOTPRINT bound, not a bound on
                                         * capability nodes in general. */
#define PCI_CAP_WALK_MAX            48  /* TTL: bounds a malformed or cyclic list */

/* The TTL is not an arbitrary safety margin -- it is EXACTLY the number of
 * DWORD-aligned positions a capability node can occupy. The walk relies on that
 * to tell a maximal legal list (budget exhausted, next pointer zero) from a
 * cyclic one (budget exhausted, next pointer non-zero); lowering it would start
 * reporting a legal firmware layout as a defect. Pinned so the relation cannot
 * drift silently when one of the three constants is edited. */
_Static_assert(PCI_CAP_WALK_MAX
                   == ((PCI_CAP_NODE_OFF_MAX - PCI_CAP_OFF_MIN) / 4) + 1,
               "capability TTL must equal the count of legal node positions");

/* Header type register (offset 0x0E): low 7 bits are the layout, bit 7 is the
 * multi-function flag and is not part of the type. */
#define PCI_HEADER_TYPE_MASK      0x7F
#define PCI_HEADER_TYPE_NORMAL    0x00
#define PCI_HEADER_TYPE_BRIDGE    0x01
#define PCI_HEADER_TYPE_CARDBUS   0x02

/* PCI_STATUS (offset 0x06) bit 4: the device implements a capability list. */
#define PCI_STATUS_CAP_LIST       0x0010

/* ---- PM capability structure layout, relative to the capability offset ---- */
#define PCI_PM_CAP_ID_OFF         0x00  /* uint8_t  capability ID (0x01) */
#define PCI_PM_NEXT_OFF           0x01  /* uint8_t  next capability offset */
#define PCI_PM_PMC_OFF            0x02  /* uint16_t PM Capabilities */
#define PCI_PM_PMCSR_OFF          0x04  /* uint16_t PM Control/Status */

/* ---- PMC (PM Capabilities), capability offset +2 ---- */
#define PMC_VERSION_MASK          0x0007  /* PM spec revision implemented */
#define PMC_VERSION_MIN                1    /* 001 = PCI PM 1.0 */
#define PMC_VERSION_MAX                3    /* 011 = PCI PM 1.2; 0 and 4-7 reserved */
#define PMC_D1_SUPPORT            0x0200
#define PMC_D2_SUPPORT            0x0400

/* ---- PMCSR (PM Control/Status), capability offset +4 ---- */
#define PMCSR_POWER_STATE_MASK    0x0003  /* D0=0, D1=1, D2=2, D3hot=3 */
#define PMCSR_NO_SOFT_RESET       0x0008  /* set: D3hot->D0 preserves state */
#define PMCSR_PME_EN              0x0100
#define PMCSR_PME_STATUS          0x8000  /* write-1-to-clear */
/* Bits 7:4 are required to read zero on a device that answered. A degraded
 * response such as 0xFFFB is not 0xFFFF, yet its state bits read as D3hot -- so
 * without this a partially-failed device confirms a suspend it never performed.
 *
 * Bit 2 is deliberately NOT in this mask even though it is unused here: its
 * reset value is device-specific, so requiring it to be zero would reject a
 * legal response and refuse a legitimate suspend. Over-rejecting is not the
 * safe direction when the alternative failure is a device that never sleeps. */
#define PMCSR_RESERVED_MASK       0x00F0

/* ---- D-states this module can reach ---- */
#define PCI_D0                    0
#define PCI_D1                    1
#define PCI_D2                    2
#define PCI_D3HOT                 3

/* ---- Mandatory recovery times, microseconds ----
 * Any transition that involves D3hot requires 10 ms before the next access to
 * the device; any transition that involves D2 requires 200 us. Everything else
 * (the D0 <-> D1 pair) is immediate.
 *
 * Oracle, not transcription: the primary PCI-PM specification text was not
 * available to verify these against, so the rule above matches the Linux
 * reference implementation (pci_raw_set_power_state: 10 ms when either end of
 * the transition is D3hot, 200 us when either end is D2). Note that Linux
 * attributes its own 200 us constant to PCIe Base r4.0 section 5.9.1 rather
 * than to PCI-PM, so treat the D2 figure as the value the reference
 * implementation uses rather than a quoted PCI-PM requirement. */
#define PCI_PM_D3HOT_DELAY_US     10000u
#define PCI_PM_D2_DELAY_US          200u

/* Consecutive identical reads of the monotonic clock that end a wait as a
 * stalled clock rather than spinning forever. Only the hardware-counter sources
 * are permitted to time a recovery interval and they advance on every read, so
 * a million identical samples is not a coarse clock -- it is a dead one. */
#define PCI_PM_CLOCK_STALL_SPINS  1000000u

/* Bounded readiness poll after a D3hot->D0 transition. The recovery delay is
 * the spec minimum before the device may be ACCESSED; a device that came back
 * uninitialised can still take longer to answer config cycles at all, and a
 * vendor ID of 0xFFFF is how that shows up. */
#define PCI_PM_D0_READY_MAX_US  1000000u  /* 1 s, the conventional-reset allowance
                                           * the Linux PCI core uses; 60 ms was
                                           * short enough to declare a compliant
                                           * storage or USB controller failed */
#define PCI_PM_D0_READY_STEP_US    1000u

/* A PCIe function that is not ready yet answers a config read with Request
 * Retry Status, which surfaces as vendor ID 0x0001 -- NOT as 0xFFFF. Treating
 * "anything but 0xFFFF" as ready therefore declares an explicitly
 * not-ready device ready. */
#define PCI_VENDOR_ID_RRS         0x0001
/* What a config read returns when nothing on the bus answered it. Named because
 * it is this module's central trust predicate, decided at three separate sites,
 * and a bare literal at a trust boundary is the shape that drifts. */
#define PCI_CFG_NO_RESPONSE       0xFFFF

/* Concurrent D-state transitions this module tracks. A transition claims its
 * device for the whole read -> decide -> write -> recover -> verify sequence, so
 * this bounds how many DIFFERENT devices can be transitioning at once, not how
 * often any one of them may be. */
#define PCI_PM_MAX_INFLIGHT       8

/* ---- Status codes ----
 * Negative so a function that returns either an offset/state or a failure can
 * use one return value. */
/* Status semantics, stated exhaustively because several paths can produce the
 * same code and a caller has to be able to tell them apart:
 *
 *   PCI_DX_OK          the operation completed and its result is meaningful.
 *   PCI_DX_UNSUPPORTED the device is fine, the REQUEST is not: no PM capability
 *                      at all, a D1/D2 the PMC bits do not advertise, a header
 *                      type with no walkable capability list, or a PM revision
 *                      outside the 1..3 this code understands.
 *   PCI_DX_INVALID     the CALLER is wrong: a NULL out-pointer, a state above
 *                      D3hot, a device or function number that would alias a
 *                      different device, or an illegal shallower transition.
 *   PCI_DX_FAILED      the DEVICE did not behave: it stopped answering config
 *                      cycles (all-ones or implausible register contents), it
 *                      never came back within the readiness allowance, or the
 *                      requested state did not latch. All three mean "do not
 *                      trust this device right now"; they are one code because
 *                      the caller's response is the same.
 *   PCI_DX_MALFORMED   the capability LIST is structurally invalid: a
 *                      misaligned or out-of-range pointer, a cyclic list, or a
 *                      PM capability that cannot hold its own PMCSR.
 *   PCI_DX_BUSY        another D-state transition owns this device, or no
 *                      transition slot is free. Retryable.
 *   PCI_DX_POISONED    a previous transition could not observe its recovery
 *                      interval, so nothing here may touch any device until
 *                      pci_pm_clear_poison(). Not retryable without it.
 *   PCI_DX_CLOCK_STALLED  this call's own recovery wait could not be observed.
 *   PCI_DX_IRQL        the caller is above PASSIVE_LEVEL, where the millisecond
 *                      waits a transition needs would block interrupts.
 */
typedef enum {
    PCI_DX_OK           =  0,
    PCI_DX_UNSUPPORTED  = -1,  /* no PM capability, or a D1/D2 the device does
                                * not advertise in PMC */
    PCI_DX_INVALID      = -2,  /* state out of range, or an illegal transition
                                * to a shallower state other than D0 */
    PCI_DX_NO_TIMEBASE  = -3,  /* a mandatory recovery delay cannot be honoured
                                * because no monotonic time source is running */
    PCI_DX_FAILED       = -4,  /* the requested state did not latch */
    PCI_DX_MALFORMED    = -5,  /* the capability list is structurally invalid */
    PCI_DX_BUSY         = -6,  /* another transition owns this device, or no
                                * transition slot is free */
    PCI_DX_POISONED     = -8,  /* a transition ended without observing its
                                * recovery interval, so this module refuses to
                                * drive any device until pci_pm_clear_poison() */
    PCI_DX_IRQL         = -9,  /* called above PASSIVE_LEVEL, where a
                                * millisecond-scale busy wait would hold off
                                * interrupts and DPCs */
    PCI_DX_CLOCK_STALLED = -7, /* the recovery interval could not be completed
                                * because the clock stopped advancing mid-wait.
                                * The write already happened, so the device is
                                * mid-transition and must be treated as unusable
                                * until something re-establishes its state */
} pci_dx_status_t;

/* The PM capability structure as it sits in config space. Field widths are the
 * hardware's, not uniform 16-bit words: the ID and next pointer are bytes. */
typedef struct {
    uint8_t  cap_id;    /* PCI_CAP_ID_PM */
    uint8_t  next_cap;  /* offset of the next capability, 0 = end of list */
    uint16_t pmc;       /* PM Capabilities */
    uint16_t pmcsr;     /* PM Control/Status */
    uint8_t  cap_off;   /* config offset this structure was read from */
} PCI_PMCAP;

/* ---- Pure helpers ----
 * No hardware access, no locking; the decision logic of the state machine, so
 * it can be tested directly rather than through a device that may not exist. */

/* Required recovery time in microseconds for `from` -> `to`. */
uint32_t pci_pm_recovery_delay_us(uint8_t from, uint8_t to);

/* Non-zero when `from` -> `to` is a transition the spec permits. */
int pci_pm_transition_legal(uint8_t from, uint8_t to);

/* Non-zero when `state` is reachable given the device's PMC capability bits.
 * D0 and D3hot are mandatory for every PM-capable device; D1 and D2 are
 * optional and must be advertised. */
int pci_pm_state_supported(uint16_t pmc, uint8_t state);

/* The value to WRITE to PMCSR to select `state`, given the value just read.
 * Preserves PME_En and every reserved bit, and never writes 1 to the
 * write-1-to-clear PME_Status bit. */
uint16_t pci_pmcsr_write_value(uint16_t old, uint8_t state);

/* Non-zero when the device keeps its state across D3hot->D0. When zero, that
 * transition yields D0 Uninitialized and the driver must reinitialise. */
int pci_pm_no_soft_reset(uint16_t pmcsr);

/* Non-zero when a PMCSR value could have come from a device that answered.
 * An all-ones config read is what a bus returns when nothing responded, and its
 * low two bits read as D3hot -- so an absent, removed or wedged device would
 * otherwise CONFIRM a suspend that never happened. Reserved fields are required
 * to read zero, which catches the partial-response values that are not 0xFFFF
 * but are just as untrustworthy. */
int pci_pm_pmcsr_plausible(uint16_t pmcsr);

/* Non-zero when the PM Capabilities register names a PM revision this code
 * understands. A structure whose version field is reserved is not a PM
 * capability that can be driven, whether that is a corrupt read or genuinely
 * unsupported silicon. */
int pci_pm_pmc_version_supported(uint16_t pmc);

/* Non-zero when a bus/device/function triple is addressable at all. The CF8
 * address format packs the fields adjacently, so an out-of-range device or
 * function does not fail -- it ALIASES another device (device 32 lands on the
 * next bus). Every entry point validates before it claims or accesses. */
int pci_pm_bdf_valid(uint8_t bus, uint8_t dev, uint8_t fn);

/* Non-zero when `off` is a structurally valid location for ANY capability node
 * on a device with the given header type. The lower bound is header-type-aware:
 * a CardBus bridge's fixed fields run to 0x47, so a type-2 pointer of 0x40 or
 * 0x44 names a subsystem-ID or legacy-base register, not a capability. */
int pci_pm_cap_offset_valid(uint16_t off, uint8_t header_type);

/* Non-zero when a PM capability starting at `off` fits: its PMCSR lives at +4
 * and is 16 bits wide. Deliberately separate from the node check above -- a
 * non-PM capability may legally occupy the final DWORD at 0xFC, and rejecting
 * the whole list as malformed because of it would report a firmware defect for
 * a legal layout. */
int pci_pm_cap_fits_pmcsr(uint16_t off);

/* Config offset of the Capabilities Pointer for a given header type, or 0 when
 * the header type has no capability list this code can walk. */
uint8_t pci_cap_ptr_offset(uint8_t header_type);

/* What a transition will do, decided entirely from register values. Separating
 * the decision from the config accesses is what makes the state machine
 * testable on a machine with no PM-capable device on the bus. */
typedef struct {
    uint8_t  from;         /* state read out of PMCSR */
    uint8_t  to;           /* state requested */
    uint16_t write_value;  /* value to write to PMCSR; valid when needs_write */
    uint32_t delay_us;     /* recovery time to observe after the write */
    int      needs_write;  /* 0 when the device is already in the target state */
    int      reinit_after; /* the transition lands in D0 Uninitialized, so the
                            * caller must restore BARs, command register and
                            * interrupt line before using the device */
} pci_pm_plan_t;

/* Decide a transition from the device's PMC and PMCSR values.
 *
 * `timebase_ready` is the caller's answer to "can I actually wait?"; when a
 * delay is required and it is 0, the plan is refused with PCI_DX_NO_TIMEBASE so
 * the refusal happens BEFORE anything is written. Returns PCI_DX_OK,
 * PCI_DX_UNSUPPORTED, PCI_DX_INVALID or PCI_DX_NO_TIMEBASE. */
int pci_pm_plan_transition(uint16_t pmc, uint16_t pmcsr, uint8_t target,
                           int timebase_ready, pci_pm_plan_t *out);

/* ---- Hardware operations ---- */

#ifdef KERNEL_TESTS
/* Test-only poison override (mirrors the PMM fault-injection primitives in
 * include/kernel/mm/pmm.h). Forces or clears the machine-wide poisoned state so
 * a test can prove the admission gate actually refuses: reaching that state for
 * real needs a qualified hardware counter to stop mid-wait, which no test can
 * arrange, and without this the only provable claims are about the status code
 * values -- an implementation whose poison flag was never set would satisfy all
 * of them.
 *
 * Guarded because clearing poison is exactly what a real caller must NOT do:
 * the invariant is that only an explicit re-establishment of the affected
 * devices lifts the refusal. The release build flavor (`make KERNEL_TESTS=off`)
 * compiles this away, same as the PMM surface. */
void pci_pm_set_poisoned_for_test(int on);
#endif /* KERNEL_TESTS */

/* Non-zero once a transition has ended without observing its recovery interval.
 * While set, every operation in this module refuses with PCI_DX_POISONED. */
int pci_pm_is_poisoned(void);

/* Clear that state and let this module drive devices again.
 *
 * The scope is the MACHINE, not one device, and deliberately so: the only way
 * to reach it is a qualified hardware counter (TSC, HPET or PM timer) stopping
 * mid-wait, which is not a property of the device being transitioned -- every
 * other transition would hit the same stopped clock. Refusing globally matches
 * the scope of the fault, and it means the refusal cannot be starved by running
 * out of per-device bookkeeping.
 *
 * There is no automatic expiry: nothing here can know when an unobserved
 * recovery interval finished, so the only honest way back is a caller asserting
 * the affected devices have been re-established by other means (a bus rescan, a
 * driver re-probe, a reset). */
void pci_pm_clear_poison(void);

/* Walk the capability list and return the PM capability's config offset, or a
 * negative pci_dx_status_t (UNSUPPORTED when there is no PM capability,
 * MALFORMED when the list itself is invalid). */
int pci_pmcap_find(uint8_t bus, uint8_t dev, uint8_t fn);

/* Read the PM capability structure. Returns PCI_DX_OK or a negative status. */
int pci_pmcap_read(uint8_t bus, uint8_t dev, uint8_t fn, PCI_PMCAP *out);

/* Read the current D-state back from PMCSR. Returns 0..3 or a negative
 * pci_dx_status_t. The hardware is the source of truth; nothing caches it. */
int pci_get_d_state(uint8_t bus, uint8_t dev, uint8_t fn);

/* Move the device to `state`, honouring the mandatory recovery delay before
 * verifying the write. Returns PCI_DX_OK or a negative pci_dx_status_t.
 *
 * Fails closed: when the transition needs a recovery delay and no time source
 * that can measure it is running, it returns PCI_DX_NO_TIMEBASE WITHOUT
 * touching PMCSR, rather than transitioning and then accessing the device too
 * early. "Can measure it" is stricter than "exists": the tick-derived clock
 * source does not advance while interrupts are off, and a device transition may
 * run in exactly that context, so it does not qualify.
 *
 * PASSIVE_LEVEL ONLY, refused with PCI_DX_IRQL otherwise. A transition
 * busy-waits for the mandatory recovery interval and, leaving D3hot, then polls
 * for readiness -- up to a second in total. At DISPATCH_LEVEL or above that
 * holds off every lower-priority interrupt and DPC for the duration.
 *
 * This is a CONTRACT ON CONSUMERS, not just a check. An ordinary KDPC callback
 * runs at DISPATCH_LEVEL, so a runtime-idle timer that calls a driver's
 * suspend hook directly from DPC expiry would have every autosuspend refused
 * here. Such a consumer must hand off to a threaded DPC or a PASSIVE_LEVEL
 * worker first.
 *
 * Serialised per device: a transition claims the device for the whole read ->
 * decide -> write -> recovery -> verify sequence, so a second caller cannot
 * plan from a state the first has already left, and gets PCI_DX_BUSY. The claim
 * binds D-state callers only; a driver doing unrelated config access to the
 * same device during the recovery interval is not prevented by it, and that
 * gate belongs with the per-device power registry. */
int pci_set_d_state(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t state,
                    int *reinit_required);
