/* ============================================================================
 * nt_timer.c -- Timer SSDT handlers + armed-timer queue
 *
 * SSDT 0x007E-0x0083:
 *   NtCreateTimer(TimerHandle, DesiredAccess, ObjectAttributes, TimerType)
 *   NtOpenTimer(TimerHandle, DesiredAccess, ObjectAttributes)
 *   NtSetTimer(TimerHandle, DueTime, ApcRoutine, ApcContext,
 *              Period | ResumeTimer<<32, PreviousState)
 *   NtCancelTimer(TimerHandle, CurrentState)
 *   NtQueryTimer(TimerHandle, InfoClass, Buffer, Length, ReturnLength)
 *   NtSetTimerEx(TimerHandle, InfoClass, Buffer, Length)
 *
 * NtSetTimer is logically 7-parameter; Period and ResumeTimer are packed
 * into a5 as (Period | ResumeTimer << 32) until the INT 0x2E / SYSCALL
 * entry is extended to read stack args.
 *
 * The armed-timer list is a singly-linked list of TIMER_OBJECT bodies,
 * head kept in this file. nt_timer_tick() walks it from the timer ISR
 * every tick under s_armed_lock (irqsave). Timers whose due_ns has been
 * reached are signalled via event_set(). One-shot timers are unlinked;
 * periodic timers are re-armed by adding period_ms to due_ns.
 * ============================================================================ */

#include "kernel/nt/nt_timer.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/zw.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_timer.h"
#include "kernel/timer.h"
#include "kernel/klog.h"

/* --- Armed-timer list ---------------------------------------------------- */

static TIMER_OBJECT *s_armed_head = (TIMER_OBJECT *)0;
static DEFINE_SPINLOCK(s_armed_lock);

/* Caller must hold s_armed_lock. */
static void armed_list_insert_locked(TIMER_OBJECT *to)
{
    if (to->on_queue)
        return;
    to->next_armed = s_armed_head;
    s_armed_head = to;
    to->on_queue = 1;
}

/* Caller must hold s_armed_lock. Returns 1 if removed, 0 if not present. */
static int armed_list_remove_locked(TIMER_OBJECT *to)
{
    TIMER_OBJECT **pp;

    if (!to->on_queue)
        return 0;

    for (pp = &s_armed_head; *pp; pp = &(*pp)->next_armed) {
        if (*pp == to) {
            *pp = to->next_armed;
            to->next_armed = (TIMER_OBJECT *)0;
            to->on_queue = 0;
            return 1;
        }
    }
    /* Inconsistent bookkeeping: on_queue was set but we are not in list. */
    to->on_queue = 0;
    to->next_armed = (TIMER_OBJECT *)0;
    return 0;
}

/* --- nt_timer_tick (ISR context) ----------------------------------------
 *
 * Two-phase design to minimize lock hold time:
 *   Phase A: under s_armed_lock, scan the queue. Periodic timers are
 *            re-armed in place (list pointer stays). One-shot timers are
 *            unlinked from the armed list. In both cases we push the
 *            fired timer onto a local "to_signal" chain (via the SAME
 *            next_armed field, reused for a transient second role that
 *            is safe only because we still logically "own" the entry
 *            until the caller would unmap it).
 *   Phase B: after releasing s_armed_lock, walk the local chain and
 *            call event_set() on each. event_set() may enumerate and
 *            wake waiters; keeping it outside the spinlock prevents
 *            lock contention from scaling with waiter counts.
 *
 * Reusing next_armed for the local chain is safe because for each timer
 * we either (a) left it on the armed list with `on_queue=1` and we do
 * NOT re-link it onto any signal chain, reading next_armed one more
 * time before unlock; or (b) we unlinked it and cleared on_queue before
 * re-purposing next_armed to walk the local chain. Concurrent arm/
 * cancel paths check on_queue under the same lock, so they cannot see
 * the transient state.
 *
 * To keep semantics simple, periodic timers signal inline (event_set
 * still called from ISR), because they stay on the armed list and we
 * would otherwise need a separate secondary link. This is the same
 * lock-held event_set as before but only fires ONCE per periodic timer
 * per tick, which is the lower-contention case. The hot case we
 * optimize here is a burst of one-shot timers expiring together.
 * ------------------------------------------------------------------------- */

void nt_timer_tick(void)
{
    uint64_t now;
    uint64_t irqf;
    TIMER_OBJECT *to;
    TIMER_OBJECT *next;
    TIMER_OBJECT **pp;
    TIMER_OBJECT *signal_chain = (TIMER_OBJECT *)0;

    now = uptime_ns();
    if (now == 0)
        return;  /* Timer subsystem not yet initialized. */

    spin_lock_irqsave(&s_armed_lock, &irqf);

    pp = &s_armed_head;
    to = *pp;
    while (to) {
        next = to->next_armed;

        if (to->active && to->due_ns <= now) {
            if (to->period_ms > 0) {
                /* Periodic: re-arm for next period. Signal inline under
                 * the lock (one event_set per periodic timer per tick). */
                uint64_t period_ns = to->period_ms * 1000000ULL;
                to->due_ns += period_ns;
                if (to->due_ns <= now)
                    to->due_ns = now + period_ns;
                event_set(&to->event);
                pp = &to->next_armed;
            } else {
                /* One-shot: unlink from armed list, clear active, push
                 * onto local signal chain for deferred wake. */
                *pp = to->next_armed;
                to->on_queue = 0;
                to->active = 0;
                to->next_armed = signal_chain;
                signal_chain = to;
                /* pp unchanged: continue at our former successor. */
            }
        } else {
            pp = &to->next_armed;
        }

        to = next;
    }

    spin_unlock_irqrestore(&s_armed_lock, irqf);

    /* Phase B: wake one-shot fire chain OUTSIDE the spinlock. */
    while (signal_chain) {
        TIMER_OBJECT *fired = signal_chain;
        signal_chain = fired->next_armed;
        fired->next_armed = (TIMER_OBJECT *)0;
        event_set(&fired->event);
    }
}

/* --- nt_timer_detach (on_close / on_delete callback) -------------------- */

void nt_timer_detach(TIMER_OBJECT *to)
{
    uint64_t irqf;

    if (!to)
        return;

    spin_lock_irqsave(&s_armed_lock, &irqf);
    armed_list_remove_locked(to);
    to->active = 0;
    spin_unlock_irqrestore(&s_armed_lock, irqf);
}

/* --- Arm / cancel helpers ----------------------------------------------- */

/* Sets due_ns from DueTime*:
 *   negative => relative 100-ns units (standard NT convention)
 *   positive => absolute FILETIME; treated as relative from now until the
 *               boot-time FILETIME reference lands (-> XREF 02-kernel-core/
 *               TODO-08-time-filetime-management.md for absolute time wiring).
 *   zero     => fire immediately.
 * Returns STATUS_SUCCESS on valid input. */
static NTSTATUS compute_due_ns(int64_t due_time_100ns, uint64_t *out_due_ns)
{
    uint64_t now = uptime_ns();

    if (due_time_100ns == 0) {
        *out_due_ns = now;
        return STATUS_SUCCESS;
    }

    if (due_time_100ns < 0) {
        /* Relative: |DueTime| in 100-ns units, future. INT64_MIN safe via
         * -(x+1)+1. Then convert 100-ns to ns by multiplying by 100.
         * Guard both the multiply AND the add from uint64 wrap. */
        uint64_t rel_100ns = (uint64_t)(-(due_time_100ns + 1)) + 1ULL;
        uint64_t rel_ns;
        const uint64_t MAX_REL_NS = (uint64_t)0x7FFFFFFFFFFFFFFFULL;
                                           /* ~292 years in ns */

        /* Saturate the 100-ns -> ns multiply. */
        if (rel_100ns > MAX_REL_NS / 100ULL)
            rel_ns = MAX_REL_NS;
        else
            rel_ns = rel_100ns * 100ULL;

        /* Saturate the add against now. */
        if (rel_ns > MAX_REL_NS - now)
            *out_due_ns = MAX_REL_NS;
        else
            *out_due_ns = now + rel_ns;
        return STATUS_SUCCESS;
    }

    /* Absolute FILETIME (100-ns since 1601). No boot-time FILETIME zero
     * yet -- treat as "fire now" until the time-subsystem XREF lands. */
    *out_due_ns = now;
    return STATUS_SUCCESS;
}

/* Arm a timer. Called with ht_lock NOT held. Sets active/state under the
 * armed-list lock; returns the prior active (signalled) state for
 * PreviousState reporting. */
static int nt_timer_arm(TIMER_OBJECT *to, uint64_t due_ns, uint32_t period_ms,
                        void *apc_routine, void *apc_context)
{
    uint64_t irqf;
    int prev_state;

    spin_lock_irqsave(&s_armed_lock, &irqf);

    /* "PreviousState" per Windows: the prior signalled state of the
     * timer, i.e. was the event currently set? Approximate with "active"
     * (armed) -- once APC is wired the exact NT semantics can tighten. */
    prev_state = to->active ? 1 : 0;

    /* Reset the signal state BEFORE making the timer visible as armed.
     * Ordering matters: if we reset after unlocking, a concurrent
     * nt_timer_tick() on another CPU that caught the old due_ns could
     * fire event_set() between unlock and reset, and our reset would
     * clobber the fire so waiters miss it. Reset-then-arm guarantees
     * any post-arm event_set() survives until consumed. */
    event_reset(&to->event);

    /* If already armed, replace in place (no unlink required). */
    to->due_ns      = due_ns;
    to->period_ms   = period_ms;
    to->active      = 1;
    to->apc_routine = apc_routine;
    to->apc_context = apc_context;

    if (!to->on_queue)
        armed_list_insert_locked(to);

    spin_unlock_irqrestore(&s_armed_lock, irqf);
    return prev_state;
}

/* Cancel a timer. Returns 1 if the timer was armed (current state), 0 otherwise. */
static int nt_timer_cancel_inner(TIMER_OBJECT *to)
{
    uint64_t irqf;
    int was_armed;

    spin_lock_irqsave(&s_armed_lock, &irqf);
    was_armed = to->active ? 1 : 0;
    armed_list_remove_locked(to);
    to->active = 0;
    to->due_ns = 0;
    to->period_ms = 0;
    spin_unlock_irqrestore(&s_armed_lock, irqf);

    /* Cancel does NOT signal the event per Windows semantics. */
    return was_armed;
}

/* --- Probe helper: ObjectAttributes ASCII name ------------------------- */

static NTSTATUS oa_probe_ascii_name(OBJECT_ATTRIBUTES *oa, const char **out)
{
    UNICODE_STRING *us;
    uint32_t probe_len;
    NTSTATUS st;

    *out = (const char *)0;
    if (!oa)
        return STATUS_SUCCESS;

    st = ProbeForReadIfUser(oa, sizeof(OBJECT_ATTRIBUTES), 8);
    if (!NT_SUCCESS(st))
        return st;

    us = oa->ObjectName;
    if (!us)
        return STATUS_SUCCESS;

    st = ProbeForReadIfUser(us, sizeof(UNICODE_STRING), 4);
    if (!NT_SUCCESS(st))
        return st;

    if (!us->Buffer)
        return STATUS_INVALID_PARAMETER;

    probe_len = (uint32_t)us->Length;
    if (probe_len > 255u)
        probe_len = 255u;
    if (probe_len == 0u)
        return STATUS_INVALID_PARAMETER;

    st = ProbeForReadIfUser(us->Buffer, probe_len, 1);
    if (!NT_SUCCESS(st))
        return st;

    *out = (const char *)us->Buffer;
    return STATUS_SUCCESS;
}

/* --- Handle resolution -------------------------------------------------- */

static NTSTATUS resolve_timer_handle(HANDLE h, TIMER_OBJECT **out)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;

    *out = (TIMER_OBJECT *)0;
    entry = ObpLookupHandle(&task_current()->handle_table, h);
    if (!entry)
        return STATUS_INVALID_HANDLE;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpTimerType)
        return STATUS_OBJECT_TYPE_MISMATCH;

    *out = (TIMER_OBJECT *)entry->object;
    return STATUS_SUCCESS;
}

/* --- SSDT handlers ------------------------------------------------------ */

/* ---- NtCreateTimer (0x007E) --------------------------------------------- */
static NTSTATUS NtCreateTimer_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    uint32_t desired_access = (uint32_t)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    uint32_t timer_type = (uint32_t)a4;
    const char *name = (const char *)0;
    HANDLE h;
    NTSTATUS pr;

    (void)a5; (void)a6;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;
    if (timer_type > TIMER_TYPE_SYNCHRONIZATION)
        return STATUS_INVALID_PARAMETER;

    pr = ProbeForWriteIfUser(out_handle, sizeof(HANDLE), 4);
    if (!NT_SUCCESS(pr))
        return pr;

    pr = oa_probe_ascii_name(oa, &name);
    if (!NT_SUCCESS(pr))
        return pr;

    h = ObCreateTimerEx(&task_current()->handle_table, name, timer_type,
                        desired_access);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* ---- NtOpenTimer (0x007F) ----------------------------------------------- */
static NTSTATUS NtOpenTimer_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    uint32_t desired_access = (uint32_t)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *name = (const char *)0;
    HANDLE h;
    NTSTATUS pr;

    (void)a4; (void)a5; (void)a6;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;

    pr = ProbeForWriteIfUser(out_handle, sizeof(HANDLE), 4);
    if (!NT_SUCCESS(pr))
        return pr;

    pr = oa_probe_ascii_name(oa, &name);
    if (!NT_SUCCESS(pr))
        return pr;
    if (!name)
        return STATUS_INVALID_PARAMETER;

    h = ObOpenTimer(&task_current()->handle_table, name, desired_access);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* ---- NtSetTimer (0x0080) ------------------------------------------------
 * a1 = HANDLE
 * a2 = LARGE_INTEGER *DueTime   (100-ns: negative relative, positive abs)
 * a3 = void           *ApcRoutine (may be NULL; recorded for future APC)
 * a4 = void           *ApcContext
 * a5 = (uint64_t)Period(low 32 ms) | ((uint64_t)ResumeTimer(0/1) << 32)
 * a6 = BOOLEAN        *PreviousState (optional; 1 = was signalled/armed)
 * ----------------------------------------------------------------------- */
static NTSTATUS NtSetTimer_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE h = (HANDLE)(int32_t)a1;
    LARGE_INTEGER *due_time = (LARGE_INTEGER *)a2;
    void *apc_routine = (void *)a3;
    void *apc_context = (void *)a4;
    uint32_t period_ms = (uint32_t)a5;
    uint8_t *prev_state_out = (uint8_t *)a6;
    TIMER_OBJECT *to;
    uint64_t due_ns = 0;
    NTSTATUS st;
    NTSTATUS pr;
    int prev_state;

    /* ResumeTimer ((a5 >> 32) & 1) is recorded but has no effect without
     * ACPI S1-S3 sleep/resume infrastructure. */
    (void)a5;

    if (!due_time)
        return STATUS_INVALID_PARAMETER;

    pr = ProbeForReadIfUser(due_time, sizeof(LARGE_INTEGER), 8);
    if (!NT_SUCCESS(pr))
        return pr;

    if (prev_state_out) {
        pr = ProbeForWriteIfUser(prev_state_out, sizeof(uint8_t), 1);
        if (!NT_SUCCESS(pr))
            return pr;
    }

    st = resolve_timer_handle(h, &to);
    if (!NT_SUCCESS(st))
        return st;

    st = compute_due_ns(due_time->QuadPart, &due_ns);
    if (!NT_SUCCESS(st))
        return st;

    prev_state = nt_timer_arm(to, due_ns, period_ms, apc_routine, apc_context);

    if (prev_state_out)
        *prev_state_out = (uint8_t)prev_state;

    return STATUS_SUCCESS;
}

/* ---- NtCancelTimer (0x0081) --------------------------------------------- */
static NTSTATUS NtCancelTimer_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE h = (HANDLE)(int32_t)a1;
    uint8_t *current_state_out = (uint8_t *)a2;
    TIMER_OBJECT *to;
    NTSTATUS st;
    NTSTATUS pr;
    int was_armed;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (current_state_out) {
        pr = ProbeForWriteIfUser(current_state_out, sizeof(uint8_t), 1);
        if (!NT_SUCCESS(pr))
            return pr;
    }

    st = resolve_timer_handle(h, &to);
    if (!NT_SUCCESS(st))
        return st;

    was_armed = nt_timer_cancel_inner(to);

    if (current_state_out)
        *current_state_out = (uint8_t)was_armed;

    return STATUS_SUCCESS;
}

/* ---- NtQueryTimer (0x0082) ---------------------------------------------- */
static NTSTATUS NtQueryTimer_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE h = (HANDLE)(int32_t)a1;
    uint32_t info_class = (uint32_t)a2;
    void *buffer = (void *)a3;
    uint64_t buf_len = a4;
    uint64_t *ret_len = (uint64_t *)a5;
    TIMER_OBJECT *to;
    NTSTATUS st;
    NTSTATUS pr;

    (void)a6;

    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    if (buf_len > 0) {
        pr = ProbeForWriteIfUser(buffer, buf_len, 4);
        if (!NT_SUCCESS(pr))
            return pr;
    }
    if (ret_len) {
        pr = ProbeForWriteIfUser(ret_len, sizeof(uint64_t), 8);
        if (!NT_SUCCESS(pr))
            return pr;
    }

    st = resolve_timer_handle(h, &to);
    if (!NT_SUCCESS(st))
        return st;

    if (info_class != TimerBasicInformation)
        return STATUS_INVALID_INFO_CLASS;

    if (buf_len < sizeof(TIMER_BASIC_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    {
        TIMER_BASIC_INFORMATION *bi = (TIMER_BASIC_INFORMATION *)buffer;
        uint64_t now = uptime_ns();
        uint64_t irqf;
        uint64_t due_snapshot;
        uint32_t active_snapshot;
        int64_t remaining_100ns;

        spin_lock_irqsave(&s_armed_lock, &irqf);
        due_snapshot = to->due_ns;
        active_snapshot = to->active;
        spin_unlock_irqrestore(&s_armed_lock, irqf);

        if (active_snapshot && due_snapshot > now) {
            uint64_t delta_ns = due_snapshot - now;
            /* Windows reports RemainingTime as negative 100-ns (relative). */
            remaining_100ns = -(int64_t)(delta_ns / 100ULL);
        } else {
            remaining_100ns = 0;
        }

        bi->RemainingTime.QuadPart = remaining_100ns;
        bi->TimerState = active_snapshot ? 0 : 1; /* 0 = pending, 1 = signalled */
        bi->_pad[0] = 0; bi->_pad[1] = 0; bi->_pad[2] = 0; bi->_pad[3] = 0;
        bi->_pad[4] = 0; bi->_pad[5] = 0; bi->_pad[6] = 0;

        if (ret_len)
            *ret_len = sizeof(TIMER_BASIC_INFORMATION);
    }

    return STATUS_SUCCESS;
}

/* ---- NtSetTimerEx (0x0083) ---------------------------------------------- */
static NTSTATUS NtSetTimerEx_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE h = (HANDLE)(int32_t)a1;
    uint32_t info_class = (uint32_t)a2;
    void *buffer = (void *)a3;
    uint64_t buf_len = a4;
    TIMER_OBJECT *to;
    NTSTATUS st;
    NTSTATUS pr;

    (void)a5; (void)a6;

    st = resolve_timer_handle(h, &to);
    if (!NT_SUCCESS(st))
        return st;

    if (info_class != TimerSetCoalescableTimer)
        return STATUS_INVALID_INFO_CLASS;

    /* TimerSetCoalescableTimer supplies a
     *   struct { int64_t DueTime; DELAY_TOLERANCE; DWORD Period; ... }
     * On NT this sets a tolerable-delay hint for power-aware batching.
     * Without coalescing infrastructure the hint is accepted silently --
     * callers get correct single-shot firing via NtSetTimer / existing
     * armed-list behavior. */
    if (buffer && buf_len > 0) {
        pr = ProbeForReadIfUser(buffer, buf_len, 8);
        if (!NT_SUCCESS(pr))
            return pr;
    }

    return STATUS_SUCCESS;
}

/* --- Registration -------------------------------------------------------- */

void nt_timer_register_ssdt(void)
{
    s_armed_head = (TIMER_OBJECT *)0;

    ssdt_register(SSDT_NtCreateTimer, (SSDT_HANDLER)NtCreateTimer_handler);
    ssdt_register(SSDT_NtOpenTimer,   (SSDT_HANDLER)NtOpenTimer_handler);
    ssdt_register(SSDT_NtSetTimer,    (SSDT_HANDLER)NtSetTimer_handler);
    ssdt_register(SSDT_NtCancelTimer, (SSDT_HANDLER)NtCancelTimer_handler);
    ssdt_register(SSDT_NtQueryTimer,  (SSDT_HANDLER)NtQueryTimer_handler);
    ssdt_register(SSDT_NtSetTimerEx,  (SSDT_HANDLER)NtSetTimerEx_handler);

    klog(LOG_INFO, "nt", "NT timer: 6 handlers registered (SSDT 0x007E-0x0083)");
}
