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
#include "kernel/nt/zw.h"
#include "kernel/cpu_security.h"
#include "kernel/sched/task.h"
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

/* Published arbitrated resolution. Written only under s_res_lock; read
 * unlocked by the query paths via __atomic_load_n (a plain read would be an
 * unsynchronized access of shared mutable state). */
static uint32_t s_current_resolution = TIMER_RES_DEFAULT;

/* Serializes request-table mutation + arbitration: concurrent set and
 * release calls must not interleave around a refused transition. Rare
 * PASSIVE-level path (NtSetTimerResolution), never an ISR.
 *
 * LOCK ORDER: s_res_lock -> s_timer_mode_lock (arbitrate() calls
 * timer_set_tick_hz() while holding s_res_lock). Any path taking both MUST
 * acquire s_res_lock first. */
static spinlock_t s_res_lock = SPINLOCK_INIT;

/* Owner pid of the current process (0 when no task context, e.g. a kernel-mode
 * caller or early boot). */
static uint32_t current_owner_pid(void)
{
    struct task *t = task_current();
    return t ? t->pid : 0;
}

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
            __atomic_store_n(&s_current_resolution, best, __ATOMIC_SEQ_CST);
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
    __atomic_store_n(&s_current_resolution, TIMER_RES_DEFAULT, __ATOMIC_SEQ_CST);
}

NTSTATUS KeSetTimerResolution(uint32_t desired_100ns, int set,
                              uint32_t *actual_100ns)
{
    uint32_t i;
    uint32_t slot = MAX_REQUESTS;
    uint32_t pid = current_owner_pid();
    uint32_t prev_res, current;
    uint64_t irqf;
    int refused = 0;
    int exhausted = 0;
    uint8_t  saved_active = 0;
    uint32_t saved_pid = 0;
    uint32_t saved_res = 0;

    /* Normalize the request value the SAME way for set AND release so a
     * clamped-on-set value (e.g. a sub-minimum request stored as MINIMUM) can
     * be released with the same caller value instead of matching nothing. */
    if (desired_100ns < TIMER_RES_MINIMUM)
        desired_100ns = TIMER_RES_MINIMUM;
    if (desired_100ns > TIMER_RES_DEFAULT)
        desired_100ns = TIMER_RES_DEFAULT;

    spin_lock_irqsave(&s_res_lock, &irqf);
    prev_res = s_current_resolution;

    if (set) {
        /* Coalesce per process: a process holds at most ONE slot (its current
         * requested resolution), so a single caller cannot exhaust the table
         * with duplicate requests. (Nested begin/end refcount + process-exit
         * reaping of leaked requests is the deferred per-process model.) */
        for (i = 0; i < MAX_REQUESTS; i++) {
            if (s_requests[i].active && s_requests[i].pid == pid) {
                slot = i;
                break;
            }
        }
        if (slot == MAX_REQUESTS) {
            for (i = 0; i < MAX_REQUESTS; i++) {
                if (!s_requests[i].active) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot == MAX_REQUESTS) {
            exhausted = 1;          /* table full, distinct owners */
        } else {
            /* Snapshot the FULL prior slot (active/pid/resolution) so a refused
             * transition restores it exactly -- restoring only `active` would
             * leave a coalesced slot holding the new, never-applied resolution,
             * making the owner's later release-by-value miss. */
            saved_active = s_requests[slot].active;
            saved_pid = s_requests[slot].pid;
            saved_res = s_requests[slot].resolution_100ns;
            s_requests[slot].pid = pid;
            s_requests[slot].resolution_100ns = desired_100ns;
            s_requests[slot].active = 1;
            /* A refused tick-source transition (AP caller, fixed-rate PIT
             * backend) must not linger as recorded-but-never-applied state. */
            if (arbitrate() != 0) {
                refused = 1;
                s_requests[slot].active = saved_active;
                s_requests[slot].pid = saved_pid;
                s_requests[slot].resolution_100ns = saved_res;
            }
        }
    } else {
        /* Release: match the CALLER's own request (pid + value), never another
         * process's slot. Restore it if the resulting transition is refused. */
        for (i = 0; i < MAX_REQUESTS; i++) {
            if (s_requests[i].active && s_requests[i].pid == pid &&
                s_requests[i].resolution_100ns == desired_100ns) {
                s_requests[i].active = 0;
                slot = i;
                break;
            }
        }
        if (slot < MAX_REQUESTS && arbitrate() != 0) {
            refused = 1;
            s_requests[slot].active = 1;
        }
    }

    current = s_current_resolution;
    spin_unlock_irqrestore(&s_res_lock, irqf);

    if (actual_100ns)
        *actual_100ns = current;

    /* Diagnostics AFTER the lock is released (klog can flush to disk), and only
     * on an actual state transition -- a user looping no-op set/release calls
     * must not force kernel log I/O. */
    if (refused)
        klog(LOG_WARN, "time",
             "Timer resolution change refused -- keeping %u us (BSP-only "
             "transition or fixed-rate backend)",
             (uint64_t)(current / 10));
    else if (current != prev_res)
        klog(LOG_INFO, "time", "Timer resolution: %u us",
             (uint64_t)(current / 10));

    return exhausted ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
}

uint32_t timer_resolution_release_process(uint32_t pid)
{
    uint32_t i;
    uint32_t released = 0;
    uint64_t irqf;

    spin_lock_irqsave(&s_res_lock, &irqf);

    /* Clear EVERY active slot owned by this pid. The set path coalesces one slot
     * per pid (see KeSetTimerResolution), so this is normally 0 or 1, but the
     * full sweep is robust if that invariant ever changes. */
    for (i = 0; i < MAX_REQUESTS; i++) {
        if (s_requests[i].active && s_requests[i].pid == pid) {
            s_requests[i].active = 0;
            s_requests[i].pid = 0;
            s_requests[i].resolution_100ns = 0;
            released++;
        }
    }

    /* Re-arbitrate toward the coarsest remaining request. Unlike the live-caller
     * release path in KeSetTimerResolution, a refused transition is NOT rolled
     * back: the owner is dead and will never call release again, so restoring the
     * slot would strand a phantom fast-tick request permanently. A refused
     * transition on release can only occur for (a) an AP caller -- tasks run
     * BSP-only today, so process death always reaps on the BSP where a coarser
     * transition succeeds; AP-side reconciliation lands with per-CPU scheduling
     * (-> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md) -- or (b) a
     * fixed-rate PIT backend, whose heartbeat is fixed regardless, so nothing is
     * stranded. Log-free: callers may run at raised IRQL on the death path and
     * klog can flush to disk (diagnostics belong to the reap-point caller). */
    if (released)
        (void)arbitrate();

    spin_unlock_irqrestore(&s_res_lock, irqf);
    return released;
}

void KeQueryTimerResolution(uint32_t *max_time, uint32_t *min_time,
                             uint32_t *current_time)
{
    if (max_time)     *max_time     = TIMER_RES_DEFAULT;
    if (min_time)     *min_time     = TIMER_RES_MINIMUM;
    if (current_time)
        *current_time = __atomic_load_n(&s_current_resolution, __ATOMIC_SEQ_CST);
}

/* ---- SSDT handlers ------------------------------------------------------- */

/* Probe (when the caller is user-mode) and copy a uint32_t out to a syscall
 * pointer. Returns STATUS_SUCCESS, or a fault status the handler propagates. */
static NTSTATUS write_u32_out(uint64_t ptr, uint32_t value)
{
    NTSTATUS pst;
    if (!ptr)
        return STATUS_SUCCESS;   /* optional out-param */
    pst = ProbeForWriteIfUser((void *)ptr, (uint32_t)sizeof(uint32_t), 4);
    if (pst != STATUS_SUCCESS)
        return pst;
    if (copy_to_user((void *)ptr, &value, (uint32_t)sizeof(uint32_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* NtSetTimerResolution(DesiredTime, SetResolution, ActualTime) -- SSDT 0x00F4 */
static NTSTATUS nt_set_timer_resolution(uint64_t desired, uint64_t set,
                                         uint64_t actual_ptr, uint64_t a4,
                                         uint64_t a5, uint64_t a6)
{
    uint32_t actual = 0;
    NTSTATUS st;
    (void)a4; (void)a5; (void)a6;

    /* Probe the out-pointer BEFORE mutating ownership/tick rate so an invalid
     * pointer fails the syscall without leaving a dangling request set. */
    if (actual_ptr) {
        st = ProbeForWriteIfUser((void *)actual_ptr,
                                 (uint32_t)sizeof(uint32_t), 4);
        if (st != STATUS_SUCCESS)
            return st;
    }

    st = KeSetTimerResolution((uint32_t)desired, (int)set, &actual);

    /* Non-transactional contract: the request-table/tick-rate mutation is
     * authoritative once KeSetTimerResolution returns, so the syscall reports
     * the MUTATION status. Writing ActualResolution is best-effort -- the
     * address was already probed, so a copy fault here is a rare post-probe
     * TOCTOU (concurrent unmap), and converting it into a failed syscall would
     * leave the caller believing it owns no request when it actually does. */
    if (actual_ptr)
        (void)copy_to_user((void *)actual_ptr, &actual,
                           (uint32_t)sizeof(uint32_t));

    return st;
}

/* NtQueryTimerResolution(MaximumTime, MinimumTime, CurrentTime) -- SSDT 0x00F3 */
static NTSTATUS nt_query_timer_resolution(uint64_t max_ptr, uint64_t min_ptr,
                                           uint64_t cur_ptr, uint64_t a4,
                                           uint64_t a5, uint64_t a6)
{
    uint32_t cur = __atomic_load_n(&s_current_resolution, __ATOMIC_SEQ_CST);
    NTSTATUS st;
    (void)a4; (void)a5; (void)a6;

    st = write_u32_out(max_ptr, TIMER_RES_DEFAULT);
    if (st != STATUS_SUCCESS)
        return st;
    st = write_u32_out(min_ptr, TIMER_RES_MINIMUM);
    if (st != STATUS_SUCCESS)
        return st;
    return write_u32_out(cur_ptr, cur);
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
