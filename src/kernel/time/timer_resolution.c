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
#include "kernel/drivers/lapic.h"
#include "kernel/klog.h"

/* ---- State --------------------------------------------------------------- */

#define MAX_REQUESTS 16

static struct {
    uint32_t pid;
    uint32_t resolution_100ns;
    uint8_t  active;
} s_requests[MAX_REQUESTS];

static uint32_t s_current_resolution = TIMER_RES_DEFAULT;

/* ---- Internal ------------------------------------------------------------ */

static void arbitrate(void)
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
        s_current_resolution = best;

        /* Reprogram LAPIC timer to match new resolution.
         * Resolution is in 100 ns units; Hz = 10000000 / resolution */
        {
            uint32_t new_hz = (uint32_t)(10000000ULL / best);
            if (new_hz > 0)
                lapic_timer_set_hz(new_hz);
        }

        klog(LOG_INFO, "time", "Timer resolution: %u us (%u Hz)",
             (uint64_t)(best / 10),
             (uint64_t)(10000000ULL / best));
    }
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
                break;
            }
        }
    } else {
        /* Release: remove first matching request */
        for (i = 0; i < MAX_REQUESTS; i++) {
            if (s_requests[i].active &&
                s_requests[i].resolution_100ns == desired_100ns) {
                s_requests[i].active = 0;
                break;
            }
        }
    }

    arbitrate();
    return s_current_resolution;
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
