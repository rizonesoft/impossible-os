/* ============================================================================
 * timer_resolution.c -- Timer resolution management
 *
 * Tracks per-process resolution requests and arbitrates the global
 * active resolution (shortest request wins).
 * ============================================================================ */

#include "kernel/time/timer_resolution.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/timer.h"
#include "kernel/sched/spinlock.h"
#include "kernel/klog.h"

/* ---- State --------------------------------------------------------------- */

#define MAX_REQUESTS 16

static struct {
    uint32_t pid;
    uint32_t resolution_100ns;
    uint8_t  active;
} s_requests[MAX_REQUESTS];

static uint32_t s_current_resolution = TIMER_RES_DEFAULT;

/* Serializes request-table mutation + arbitration: concurrent set and
 * release calls must not interleave around a refused transition. Rare
 * PASSIVE-level path (NtSetTimerResolution), never an ISR. */
static spinlock_t s_res_lock = SPINLOCK_INIT;

/* ---- Internal ------------------------------------------------------------ */

/* Returns 0 when the arbitrated rate is live (or unchanged), -1 when the
 * tick-source transition was refused -- the caller must roll back the
 * request that triggered it so recorded state never diverges from the
 * actual heartbeat. */
static int arbitrate(void)
{
    uint32_t best = TIMER_RES_DEFAULT;
    uint32_t i;

    for (i = 0; i < MAX_REQUESTS; i++) {
        if (s_requests[i].active && s_requests[i].resolution_100ns < best)
            best = s_requests[i].resolution_100ns;
    }

    /* Clamp to hardware minimum */
    if (best < TIMER_RES_MINIMUM)
        best = TIMER_RES_MINIMUM;

    if (best != s_current_resolution) {
        /* Publish the new resolution ONLY after the tick source actually
         * changed rate -- a refused transition (AP caller, fixed-rate
         * PIT backend) must not advertise a resolution the heartbeat
         * does not deliver. NO LOGGING here: the caller holds s_res_lock
         * (and the mode lock nests inside) and klog can flush to disk;
         * diagnostics are emitted by the syscall layer after unlock. */
        uint32_t new_hz = (uint32_t)(10000000ULL / best);
        if (new_hz > 0 && timer_set_tick_hz(new_hz) == 0) {
            s_current_resolution = best;
        } else {
            return -1;
        }
    }
    return 0;
}

/* ---- Kernel API ---------------------------------------------------------- */

void timer_resolution_init(void)
{
    uint32_t i;
    for (i = 0; i < MAX_REQUESTS; i++)
        s_requests[i].active = 0;
    s_current_resolution = TIMER_RES_DEFAULT;
}

uint32_t KeSetTimerResolution(uint32_t desired_100ns, int set)
{
    uint32_t i;
    uint32_t slot = MAX_REQUESTS;
    uint32_t current;
    uint64_t irqf;
    int refused = 0;

    spin_lock_irqsave(&s_res_lock, &irqf);

    if (set) {
        /* Clamp */
        if (desired_100ns < TIMER_RES_MINIMUM)
            desired_100ns = TIMER_RES_MINIMUM;
        if (desired_100ns > TIMER_RES_DEFAULT)
            desired_100ns = TIMER_RES_DEFAULT;

        /* Find free slot */
        for (i = 0; i < MAX_REQUESTS; i++) {
            if (!s_requests[i].active) {
                s_requests[i].pid = 0;  /* TODO: task_current()->pid */
                s_requests[i].resolution_100ns = desired_100ns;
                s_requests[i].active = 1;
                slot = i;
                break;
            }
        }

        /* A request whose tick-source transition was refused (AP caller,
         * fixed-rate PIT backend) must not linger as recorded-but-never-
         * applied state: roll it back so the advertised resolution and
         * the request table stay truthful. BSP delegation for AP-side
         * requests arrives with the cross-CPU call facility. */
        if (arbitrate() != 0) {
            refused = 1;
            if (slot < MAX_REQUESTS)
                s_requests[slot].active = 0;
        }
    } else {
        /* Release: remove first matching request, but RESTORE it if the
         * resulting tick-rate transition is refused -- otherwise the
         * heartbeat stays at the finer rate with no recorded owner */
        for (i = 0; i < MAX_REQUESTS; i++) {
            if (s_requests[i].active &&
                s_requests[i].resolution_100ns == desired_100ns) {
                s_requests[i].active = 0;
                slot = i;
                break;
            }
        }
        if (arbitrate() != 0) {
            refused = 1;
            if (slot < MAX_REQUESTS)
                s_requests[slot].active = 1;
        }
    }

    current = s_current_resolution;
    spin_unlock_irqrestore(&s_res_lock, irqf);

    /* Diagnostics AFTER every lock is released (klog can flush to disk) */
    if (refused)
        klog(LOG_WARN, "time",
             "Timer resolution change refused -- keeping %u us (BSP-only "
             "transition or fixed-rate backend)",
             (uint64_t)(current / 10));
    else
        klog(LOG_INFO, "time", "Timer resolution: %u us",
             (uint64_t)(current / 10));
    return current;
}

void KeQueryTimerResolution(uint32_t *max_time, uint32_t *min_time,
                             uint32_t *current_time)
{
    if (max_time)     *max_time     = TIMER_RES_DEFAULT;
    if (min_time)     *min_time     = TIMER_RES_MINIMUM;
    if (current_time) *current_time = s_current_resolution;
}

/* ---- SSDT handlers ------------------------------------------------------- */

/* NtSetTimerResolution(DesiredTime, SetResolution, ActualTime) -- SSDT 0x00F4 */
static NTSTATUS nt_set_timer_resolution(uint64_t desired, uint64_t set,
                                         uint64_t actual_ptr, uint64_t a4,
                                         uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    uint32_t actual = KeSetTimerResolution((uint32_t)desired, (int)set);

    if (actual_ptr)
        *(uint32_t *)actual_ptr = actual;

    return STATUS_SUCCESS;
}

/* NtQueryTimerResolution(MaximumTime, MinimumTime, CurrentTime) -- SSDT 0x00F3 */
static NTSTATUS nt_query_timer_resolution(uint64_t max_ptr, uint64_t min_ptr,
                                           uint64_t cur_ptr, uint64_t a4,
                                           uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    if (max_ptr) *(uint32_t *)max_ptr = TIMER_RES_DEFAULT;
    if (min_ptr) *(uint32_t *)min_ptr = TIMER_RES_MINIMUM;
    if (cur_ptr) *(uint32_t *)cur_ptr = s_current_resolution;

    return STATUS_SUCCESS;
}

void timer_resolution_register_ssdt(void)
{
    ssdt_register(SSDT_NtSetTimerResolution,
                  (SSDT_HANDLER)nt_set_timer_resolution);
    ssdt_register(SSDT_NtQueryTimerResolution,
                  (SSDT_HANDLER)nt_query_timer_resolution);

    klog(LOG_INFO, "time",
         "Timer resolution syscalls registered (SSDT 0x%03X, 0x%03X)",
         (uint64_t)SSDT_NtSetTimerResolution,
         (uint64_t)SSDT_NtQueryTimerResolution);
}
