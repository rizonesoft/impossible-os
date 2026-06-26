/* ============================================================================
 * irql.h -- Interrupt Request Level (IRQL) model
 *
 * Defines the KIRQL type and canonical interrupt priority levels used
 * throughout the kernel.  Modeled after Windows NT's KIRQL architecture,
 * this provides a formal contract for what operations are legal at each
 * interrupt priority level.
 *
 * IRQL Levels (0-31):
 *
 *   Level  Name              Context
 *   -----  ----              -------
 *     0    PASSIVE_LEVEL     Normal thread execution.  All APIs legal.
 *     1    APC_LEVEL         APC delivery.  Most APIs legal.
 *     2    DISPATCH_LEVEL    DPC/dispatch.  No blocking, paging, or alloc.
 *   3-14   DIRQL (device)    Device ISR.  Mapped from LAPIC vector priority.
 *  15-27   (reserved)        Reserved for future device/system levels.
 *    28    CLOCK_LEVEL       Clock interrupt (LAPIC timer).
 *    29    IPI_LEVEL         Inter-processor interrupt.
 *    30    POWER_LEVEL       Power failure notification.
 *    31    HIGH_LEVEL        Highest level -- masks everything.
 *
 * API Legality by IRQL:
 *
 *   Operation              PASSIVE  APC  DISPATCH  DIRQL  HIGH
 *   ---------              -------  ---  --------  -----  ----
 *   kmalloc / kfree          Y       Y      -       -     -
 *   Blocking wait            Y       Y      -       -     -
 *   Scheduler yield          Y       Y      -       -     -
 *   Mutex acquire            Y       Y      -       -     -
 *   Spinlock acquire         Y       Y      Y       Y     Y
 *   DPC queue insert         Y       Y      Y       Y     -
 *   Page fault allowed       Y       Y      -       -     -
 *   Access paged memory      Y       Y      -       -     -
 *
 * IRQL transitions must be monotonic raises (KeRaiseIrql) followed by
 * symmetric lowers (KeLowerIrql) in strict LIFO order.  Violations are
 * trapped with diagnostic output in debug builds.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- KIRQL type ---------------------------------------------------------- */

typedef uint8_t KIRQL;

/* ---- Canonical IRQL levels ----------------------------------------------- */

#define PASSIVE_LEVEL    ((KIRQL)0)   /* Normal thread execution */
#define APC_LEVEL        ((KIRQL)1)   /* Asynchronous Procedure Call delivery */
#define DISPATCH_LEVEL   ((KIRQL)2)   /* DPC/dispatch, scheduler, spinlocks */

/* ---- Device IRQL range (DIRQL) ------------------------------------------- *
 *
 * Device IRQLs are derived from LAPIC vector priority groups.  The LAPIC
 * assigns each vector a priority class = vector >> 4.  Higher vectors
 * have higher priority and map to higher DIRQLs.
 *
 * Dynamic device vectors occupy 0x30-0xEF (priority groups 3-14):
 *   Vector 0x30-0x3F  ->  DIRQL 3  (lowest device priority)
 *   Vector 0x40-0x4F  ->  DIRQL 4
 *   ...
 *   Vector 0xE0-0xEF  ->  DIRQL 14 (highest device priority)
 *
 * ISA vectors (0x20-0x2F, priority group 2) are legacy low-priority
 * interrupts treated at DISPATCH_LEVEL.  Modern devices (MSI/MSI-X)
 * use the dynamic range and receive proper DIRQLs.
 */
#define DIRQL_MIN        ((KIRQL)3)   /* Vectors 0x30-0x3F */
#define DIRQL_MAX        ((KIRQL)14)  /* Vectors 0xE0-0xEF */

/* ---- System IRQL levels (above all device priorities) -------------------- */

#define CLOCK_LEVEL      ((KIRQL)28)  /* Clock interrupt (LAPIC timer) */
#define IPI_LEVEL        ((KIRQL)29)  /* Inter-processor interrupt */
#define POWER_LEVEL      ((KIRQL)30)  /* Power failure notification */
#define HIGH_LEVEL       ((KIRQL)31)  /* Highest -- masks everything */

/* Compile-time contract: these values are load-bearing -- vector_to_irql(),
 * irql_to_tpr(), the per-CPU IRQL tracking, and every diagnostic depend on the
 * exact numbers and their strictly-ascending order. Never reorder without
 * understanding the TPR priority-group math (TPR class = irql - 1). */
_Static_assert(PASSIVE_LEVEL == 0,  "PASSIVE_LEVEL must be 0");
_Static_assert(APC_LEVEL == 1,      "APC_LEVEL must be 1");
_Static_assert(DISPATCH_LEVEL == 2, "DISPATCH_LEVEL must be 2");
_Static_assert(DIRQL_MIN == 3 && DIRQL_MAX == 14, "DIRQL range must be 3..14");
_Static_assert(CLOCK_LEVEL == 28 && IPI_LEVEL == 29 && POWER_LEVEL == 30,
               "system levels must be CLOCK=28, IPI=29, POWER=30");
_Static_assert(HIGH_LEVEL == 31, "HIGH_LEVEL must be 31 and fit KIRQL (uint8_t)");
_Static_assert(PASSIVE_LEVEL < APC_LEVEL && APC_LEVEL < DISPATCH_LEVEL &&
               DISPATCH_LEVEL < DIRQL_MIN && DIRQL_MAX < CLOCK_LEVEL &&
               CLOCK_LEVEL < IPI_LEVEL && IPI_LEVEL < POWER_LEVEL &&
               POWER_LEVEL < HIGH_LEVEL,
               "IRQL levels must be strictly ascending");

/* ---- Vector-to-IRQL mapping ---------------------------------------------- */

/* Map a hardware interrupt vector to its effective device IRQL.
 *
 * Uses LAPIC vector priority: DIRQL = vector >> 4 for vectors in the
 * dynamic device range (0x30-0xEF).
 *
 * Special cases:
 *   - CPU exceptions (0x00-0x1F):  not device interrupts, returns PASSIVE_LEVEL
 *   - ISA vectors (0x20-0x2F):    legacy low-priority, returns DISPATCH_LEVEL
 *   - System vectors (0xF0-0xFF): returns HIGH_LEVEL; callers for LAPIC timer
 *     and IPI should use CLOCK_LEVEL / IPI_LEVEL directly instead */
static inline KIRQL vector_to_irql(uint8_t vector)
{
    uint8_t group = vector >> 4;

    /* CPU exceptions -- not hardware device interrupts */
    if (group < 2)
        return PASSIVE_LEVEL;

    /* ISA legacy range (PIT, keyboard, mouse, etc.) */
    if (group < 3)
        return DISPATCH_LEVEL;

    /* Dynamic device vectors: DIRQL = priority group */
    if (group <= DIRQL_MAX)
        return (KIRQL)group;

    /* System vectors (LAPIC timer, IPI, spurious) */
    return HIGH_LEVEL;
}

/* ---- IRQL-to-LAPIC TPR mapping ------------------------------------------- *
 *
 * The LAPIC Task Priority Register (TPR) masks interrupts at or below the
 * specified priority class.  Setting TPR[7:4] = N blocks all vectors with
 * priority group <= N.
 *
 * Mapping:
 *   PASSIVE/APC:   TPR = 0x00  (accept all hardware interrupts)
 *   DISPATCH:      TPR = 0x20  (block ISA legacy range, groups 0-1)
 *   DIRQL N:       TPR = (N-1) << 4  (block all vectors below group N)
 *   CLOCK/IPI/...: TPR = 0xFF  (block all interrupts)
 *   HIGH:          TPR = 0xFF  (block all interrupts)
 */

/* Convert IRQL to LAPIC TPR value.  Used internally by KeRaiseIrql/KeLowerIrql
 * and available to HAL code that needs direct TPR computation. */
uint32_t irql_to_tpr(KIRQL irql);

/* ---- Debug assertions ---------------------------------------------------- */

/* Check that the current IRQL is at most max_irql.  Logs a diagnostic on
 * violation.  Called from blocking primitives to catch illegal waits at
 * elevated IRQL.  Full enforcement (hard traps) deferred. */
void _irql_check_max(KIRQL max_irql, const char *caller);

/* Assert: current IRQL must allow blocking (at most APC_LEVEL).
 * Use at the top of mutex_lock, sem_wait, event_wait, yield, etc. */
#define ASSERT_IRQL_PASSIVE_OR_APC() \
    _irql_check_max(APC_LEVEL, __func__)

/* ---- IRQL query and transition API --------------------------------------- */

/* Return the IRQL of the current CPU.
 * May be called at any IRQL.  Lock-free per-CPU read. */
KIRQL KeGetCurrentIrql(void);

/* Raise the current CPU's IRQL to new_irql.
 * Stores the previous IRQL in *old_irql for later restoration.
 *
 * Constraints:
 *   - new_irql must be >= current IRQL (monotonic raise)
 *   - Violation triggers debug assertion and diagnostic log
 *
 * Programs the LAPIC TPR to mask interrupts below the new level. */
void KeRaiseIrql(KIRQL new_irql, KIRQL *old_irql);

/* Lower the current CPU's IRQL to old_irql (previously saved by KeRaiseIrql).
 *
 * Constraints:
 *   - old_irql must be <= current IRQL (symmetric lower)
 *   - Violation triggers a diagnostic log and is clamped (no crash)
 *
 * NOTE: strict LIFO raise/lower pairing is NOT yet enforced here -- only the
 * monotonic old_irql <= current check runs. True LIFO ordering will be enforced
 * by the planned per-CPU IRQL transition-stack validator (IRQL-violation
 * telemetry work).
 *
 * KeLowerIrql drains pending DPCs when crossing below DISPATCH_LEVEL and
 * delivers pending kernel APCs when crossing below APC_LEVEL. */
void KeLowerIrql(KIRQL old_irql);

/* Lower the current IRQL to old_irql and deliver pending kernel APCs (if
 * crossing below APC_LEVEL), but WITHOUT the DPC drain-on-lower phase. Used by
 * KiDispatchDpc to restore IRQL after its own explicit single-batch drain
 * (avoids a redundant second drain) while still delivering APCs -- and, unlike
 * holding a per-CPU drain guard across KeLowerIrql, never spans the yieldable
 * APC NormalRoutine with a guard set. Internal to the IRQL/DPC subsystem. */
void irql_lower_deliver(KIRQL old_irql);
