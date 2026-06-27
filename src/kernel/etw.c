/* ============================================================================
 * etw.c -- Event Tracing for Windows (ETW) kernel implementation
 *
 * Provides the NtTrace* family of syscalls (SSDT 0x01D0-0x01D6) for
 * high-performance kernel/user tracing. Sessions manage circular event
 * buffers; events flow through klog for unified logging.
 *
 * SMP safety: all session state protected by s_etw_lock (irqsave).
 * ============================================================================ */

#include "kernel/etw.h"
#include "kernel/klog.h"
#include "kernel/mm/heap.h"
#include "kernel/timer.h"
#include "kernel/sched/spinlock.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/zw.h"            /* ProbeForRead/WriteIfUser */
#include "kernel/cpu_security.h"     /* copy_from_user / copy_to_user */

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

/* ---- Session pool -------------------------------------------------------- */

static etw_session_t s_sessions[ETW_MAX_SESSIONS];
static uint64_t      s_next_handle = 1;  /* monotonic handle counter */
static spinlock_t    s_etw_lock = SPINLOCK_INIT;

/* ---- Internal helpers ---------------------------------------------------- */

/* Find session by handle. Caller must hold s_etw_lock. */
static etw_session_t *find_session(uint64_t handle)
{
    for (uint32_t i = 0; i < ETW_MAX_SESSIONS; i++) {
        if (s_sessions[i].magic == ETW_SESSION_MAGIC &&
            s_sessions[i].handle == handle)
            return &s_sessions[i];
    }
    return (etw_session_t *)0;
}

/* Find a free session slot. Caller must hold s_etw_lock. */
static etw_session_t *alloc_session(void)
{
    for (uint32_t i = 0; i < ETW_MAX_SESSIONS; i++) {
        if (s_sessions[i].magic != ETW_SESSION_MAGIC)
            return &s_sessions[i];
    }
    return (etw_session_t *)0;
}

/* Validate a user source range and copy `len` bytes into kernel `dst`. noinline:
 * one shared copy keeps the kernel image under its BSS page budget and gives every
 * ETW handler the same probe+copy_from_user contract. */
static NTSTATUS __attribute__((noinline))
etw_copy_in(void *dst, uint64_t user_src, uint32_t len, uint32_t align)
{
    NTSTATUS st = ProbeForReadIfUser((const void *)user_src, len, align);
    if (!NT_SUCCESS(st))
        return st;
    if (copy_from_user(dst, (const void *)user_src, len) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* Validate a user destination range and copy `len` bytes from kernel `src`. */
static NTSTATUS __attribute__((noinline))
etw_copy_out(uint64_t user_dst, const void *src, uint32_t len, uint32_t align)
{
    NTSTATUS st = ProbeForWriteIfUser((void *)user_dst, len, align);
    if (!NT_SUCCESS(st))
        return st;
    if (copy_to_user((void *)user_dst, src, len) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* ---- NtCreateTrace (SSDT 0x01D2) ---------------------------------------- */

NTSTATUS NtCreateTrace(uint64_t out_handle, uint64_t desired_access,
                       uint64_t object_attributes, uint64_t trace_guid,
                       uint64_t a5, uint64_t a6)
{
    (void)desired_access;
    (void)object_attributes;
    (void)a5;
    (void)a6;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;

    /* Copy the GUID in BEFORE allocating a session so no user pointer is
     * dereferenced under the IRQ-off spinlock or used to read kernel memory. The
     * out-handle is validated by etw_copy_out at the end (a bad handle there tears
     * the session back down). */
    ETW_GUID local_guid;
    if (trace_guid) {
        NTSTATUS pst = etw_copy_in(&local_guid, trace_guid, sizeof(ETW_GUID), 1);
        if (!NT_SUCCESS(pst))
            return pst;
    } else {
        memset(&local_guid, 0, sizeof(ETW_GUID));
    }

    uint64_t irq_flags;
    spin_lock_irqsave(&s_etw_lock, &irq_flags);

    etw_session_t *sess = alloc_session();
    if (!sess) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        klog(LOG_WARN, "etw", "NtCreateTrace: no free session slots");
        return STATUS_NO_MEMORY;
    }

    /* Allocate event buffer */
    uint8_t *buf = (uint8_t *)kmalloc(ETW_DEFAULT_BUF_SIZE);
    if (!buf) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        klog(LOG_ERROR, "etw", "NtCreateTrace: buffer allocation failed");
        return STATUS_NO_MEMORY;
    }
    memset(buf, 0, ETW_DEFAULT_BUF_SIZE);

    /* Initialize session */
    sess->magic          = ETW_SESSION_MAGIC;
    sess->state          = ETW_STATE_IDLE;
    sess->handle         = s_next_handle++;
    sess->buffer         = buf;
    sess->buf_size       = ETW_DEFAULT_BUF_SIZE;
    sess->buf_head       = 0;
    sess->events_written = 0;
    sess->events_dropped = 0;
    sess->flags          = 0;

    /* GUID was already validated + copied into local_guid before the lock. */
    memcpy(&sess->guid, &local_guid, sizeof(ETW_GUID));

    uint64_t h = sess->handle;
    spin_unlock_irqrestore(&s_etw_lock, irq_flags);

    /* Return the handle via copy_to_user (probed above). If the write still
     * faults (e.g. the page was unmapped after the probe), tear the just-created
     * session down so it does not leak. */
    NTSTATUS cs = etw_copy_out(out_handle, &h, sizeof(uint64_t), 8);
    if (cs != STATUS_SUCCESS) {
        uint8_t *tofree;
        spin_lock_irqsave(&s_etw_lock, &irq_flags);
        etw_session_t *s2 = find_session(h);
        tofree = s2 ? s2->buffer : (uint8_t *)0;
        if (s2) { s2->buffer = (uint8_t *)0; s2->magic = 0; }  /* magic=0 frees the slot */
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        if (tofree)
            kfree(tofree);
        /* Propagate the real status (probe alignment/range failure vs copy fault)
         * so it matches the nt_alpc / nt_timer probe-status contract. */
        return cs;
    }

    klog(LOG_DEBUG, "etw", "NtCreateTrace: session %u created", (uint64_t)h);
    return STATUS_SUCCESS;
}

/* ---- NtTraceEvent (SSDT 0x01D0) ----------------------------------------- */

NTSTATUS NtTraceEvent(uint64_t trace_handle, uint64_t flags,
                      uint64_t field_size, uint64_t fields,
                      uint64_t a5, uint64_t a6)
{
    (void)flags;
    (void)a5;
    (void)a6;

    if (field_size > ETW_MAX_EVENT_SIZE)
        return STATUS_INVALID_PARAMETER;
    if (field_size > 0 && !fields)
        return STATUS_INVALID_PARAMETER;

    /* Copy the user payload into a bounded kernel bounce buffer BEFORE taking the
     * IRQ-off spinlock: a user pointer must never be dereferenced under the lock
     * (a fault there would be in IRQ-disabled context with the global ETW lock
     * held), and it must be validated as a readable user-range buffer first so a
     * caller cannot copy kernel memory into the trace buffer. */
    uint8_t bounce[ETW_MAX_EVENT_SIZE];
    if (field_size > 0) {
        NTSTATUS pst = etw_copy_in(bounce, fields, (uint32_t)field_size, 1);
        if (!NT_SUCCESS(pst))
            return pst;
    }

    uint64_t irq_flags;
    spin_lock_irqsave(&s_etw_lock, &irq_flags);

    etw_session_t *sess = find_session(trace_handle);
    if (!sess) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        return STATUS_INVALID_HANDLE;
    }

    if (sess->state != ETW_STATE_RUNNING) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        return STATUS_INVALID_PARAMETER;
    }

    /* Compute total event record size */
    uint32_t record_size = (uint32_t)(sizeof(etw_event_header_t) + field_size);

    /* Check if event fits in buffer */
    if (sess->buf_head + record_size > sess->buf_size) {
        /* Wrap around -- overwrite oldest events */
        sess->buf_head = 0;
    }

    if (record_size > sess->buf_size) {
        sess->events_dropped++;
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Write event header */
    etw_event_header_t *hdr = (etw_event_header_t *)(sess->buffer + sess->buf_head);
    hdr->timestamp    = (uint32_t)(uptime_ns() / 1000000ULL);  /* ms, rate-change-safe */
    hdr->event_id     = (uint16_t)(flags & 0xFFFF);
    hdr->level        = (uint8_t)((flags >> 16) & 0xFF);
    hdr->cpu_id       = 0;  /* will be populated from smp_this_cpu() when per-entry context is added */
    hdr->pid          = 0;  /* same as above */
    hdr->payload_size = (uint32_t)field_size;

    /* Copy payload from the pre-validated bounce buffer (no user deref here). */
    if (field_size > 0) {
        memcpy(sess->buffer + sess->buf_head + sizeof(etw_event_header_t),
               bounce, (size_t)field_size);
    }

    sess->buf_head += record_size;
    sess->events_written++;

    spin_unlock_irqrestore(&s_etw_lock, irq_flags);
    return STATUS_SUCCESS;
}

/* ---- NtTraceControl (SSDT 0x01D1) --------------------------------------- */

NTSTATUS NtTraceControl(uint64_t function_code, uint64_t in_buffer,
                        uint64_t in_len, uint64_t out_buffer,
                        uint64_t out_len, uint64_t ret_len)
{
    (void)ret_len;

    if (!in_buffer || in_len < sizeof(uint64_t))
        return STATUS_INVALID_PARAMETER;

    /* Validate + copy the handle out of the user input buffer before touching
     * session state (in_len was previously ignored, allowing an 8-byte overread;
     * a raw deref also let a kernel address be read as a handle). */
    uint64_t trace_handle;
    NTSTATUS pst = etw_copy_in(&trace_handle, in_buffer, sizeof(uint64_t), 8);
    if (!NT_SUCCESS(pst))
        return pst;

    /* Query output is built into a local under the lock, then copied to the user
     * buffer after the lock drops (no user write under the IRQ-off spinlock). */
    etw_basic_info_t q_info;
    int q_copy = 0;

    uint64_t irq_flags;
    spin_lock_irqsave(&s_etw_lock, &irq_flags);

    etw_session_t *sess = find_session(trace_handle);
    if (!sess) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        return STATUS_INVALID_HANDLE;
    }

    NTSTATUS status = STATUS_SUCCESS;
    /* -9 LEAK retrofit: ETW_FUNC_STOP(IDLE) now releases the buffer.
     * Defer kfree to after the spinlock drop so the allocator is not
     * called under an IRQ-off spinlock. NULL means "no release
     * needed" (all other cases). */
    uint8_t *release_buf = (uint8_t *)0;

    switch ((uint32_t)function_code) {
    case ETW_FUNC_START:
        if (sess->state == ETW_STATE_RUNNING) {
            status = STATUS_UNSUCCESSFUL;
        } else {
            sess->state = ETW_STATE_RUNNING;
            klog(LOG_DEBUG, "etw", "session %u started", (uint64_t)sess->handle);
        }
        break;

    case ETW_FUNC_STOP:
        /* -9 LEAK retrofit (Codex follow-up): unify teardown semantics
         * with NtStopTrace. Three states worth handling:
         *   RUNNING -> IDLE: transition; keep buffer so the session
         *                    can be restarted via ETW_FUNC_START.
         *                    Same behavior as before this fix.
         *   IDLE   -> RELEASED: free buffer + clear magic so the slot
         *                    is reusable. Without this path, a caller
         *                    that stopped once then stopped again (or
         *                    never started) had no way to free the
         *                    4 KiB buffer -- fixed by NtStopTrace(IDLE)
         *                    but ETW_FUNC_STOP(IDLE) still rejected.
         *                    Now both surfaces behave identically.
         *   STOPPING -> rejected: transient, may be mid-flush on
         *                    another CPU. Same rejection as before. */
        if (sess->state == ETW_STATE_RUNNING) {
            sess->state = ETW_STATE_STOPPING;
            klog(LOG_DEBUG, "etw", "session %u stopped (%u events, %u dropped)",
                 (uint64_t)sess->handle,
                 (uint64_t)sess->events_written,
                 (uint64_t)sess->events_dropped);
            sess->state = ETW_STATE_IDLE;
        } else if (sess->state == ETW_STATE_IDLE) {
            /* Release buffer + clear magic; defer kfree until after
             * the spinlock is dropped (matches NtStopTrace's pattern
             * and avoids holding an IRQ-off spinlock across the
             * heap allocator's free path). */
            release_buf = sess->buffer;
            sess->buffer = (uint8_t *)0;
            sess->magic  = 0;
            klog(LOG_DEBUG, "etw",
                 "session %u released from IDLE via NtTraceControl(STOP)",
                 (uint64_t)sess->handle);
        } else {
            status = STATUS_UNSUCCESSFUL;
        }
        break;

    case ETW_FUNC_QUERY:
        if (out_buffer && out_len >= sizeof(etw_basic_info_t)) {
            memcpy(&q_info.guid, &sess->guid, sizeof(ETW_GUID));
            q_info.state          = sess->state;
            q_info.buf_size       = sess->buf_size;
            q_info.events_written = sess->events_written;
            q_info.events_dropped = sess->events_dropped;
            q_copy = 1;  /* copy_to_user after the lock drops */
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    case ETW_FUNC_UPDATE:
        /* Currently no updatable properties -- accept as no-op */
        break;

    case ETW_FUNC_FLUSH:
        /* Reset buffer head to drain events */
        sess->buf_head = 0;
        klog(LOG_DEBUG, "etw", "session %u flushed", (uint64_t)sess->handle);
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    spin_unlock_irqrestore(&s_etw_lock, irq_flags);
    /* -9 LEAK retrofit: kfree outside the spinlock (see release_buf
     * declaration comment). NULL when no release was needed. */
    if (release_buf)
        kfree(release_buf);

    /* Copy the query result to the user buffer outside the lock, with a write
     * probe so a kernel/invalid out_buffer cannot be used as a write primitive. */
    if (q_copy && NT_SUCCESS(status)) {
        pst = etw_copy_out(out_buffer, &q_info, sizeof(etw_basic_info_t), 4);
        if (!NT_SUCCESS(pst))
            return pst;
    }
    return status;
}

/* ---- NtQueryTrace (SSDT 0x01D3) ----------------------------------------- */

NTSTATUS NtQueryTrace(uint64_t trace_handle, uint64_t info_class,
                      uint64_t buffer, uint64_t length,
                      uint64_t a5, uint64_t a6)
{
    (void)a5;
    (void)a6;

    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    uint64_t irq_flags;
    spin_lock_irqsave(&s_etw_lock, &irq_flags);

    etw_session_t *sess = find_session(trace_handle);
    if (!sess) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        return STATUS_INVALID_HANDLE;
    }

    NTSTATUS status = STATUS_SUCCESS;
    /* Build the output into locals under the lock, then copy_to_user after the
     * lock drops -- never write through a user-controlled pointer under the
     * IRQ-off spinlock, and probe it first so it cannot be a write primitive. */
    etw_basic_info_t out_basic;
    uint32_t out_stats[2];
    uint32_t out_len = 0;   /* nonzero => copy that many bytes after unlock */
    const void *out_src = (const void *)0;

    switch ((uint32_t)info_class) {
    case ETW_INFO_BASIC:
        if (length < sizeof(etw_basic_info_t)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            memcpy(&out_basic.guid, &sess->guid, sizeof(ETW_GUID));
            out_basic.state          = sess->state;
            out_basic.buf_size       = sess->buf_size;
            out_basic.events_written = sess->events_written;
            out_basic.events_dropped = sess->events_dropped;
            out_src = &out_basic;
            out_len = sizeof(etw_basic_info_t);
        }
        break;

    case ETW_INFO_STATISTICS:
        /* Statistics: event counts in the first two uint32_t slots */
        if (length < sizeof(uint32_t) * 2) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            out_stats[0] = sess->events_written;
            out_stats[1] = sess->events_dropped;
            out_src = out_stats;
            out_len = sizeof(uint32_t) * 2;
        }
        break;

    default:
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }

    spin_unlock_irqrestore(&s_etw_lock, irq_flags);

    if (NT_SUCCESS(status) && out_len > 0) {
        NTSTATUS pst = etw_copy_out(buffer, out_src, out_len, 4);
        if (!NT_SUCCESS(pst))
            return pst;
    }
    return status;
}

/* ---- NtUpdateTrace (SSDT 0x01D4) ---------------------------------------- */

NTSTATUS NtUpdateTrace(uint64_t trace_handle, uint64_t instance_name,
                       uint64_t properties, uint64_t a4,
                       uint64_t a5, uint64_t a6)
{
    (void)instance_name;
    (void)properties;
    (void)a4;
    (void)a5;
    (void)a6;

    uint64_t irq_flags;
    spin_lock_irqsave(&s_etw_lock, &irq_flags);

    etw_session_t *sess = find_session(trace_handle);
    if (!sess) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        return STATUS_INVALID_HANDLE;
    }

    /* No updatable properties yet -- validate handle and succeed */
    spin_unlock_irqrestore(&s_etw_lock, irq_flags);
    return STATUS_SUCCESS;
}

/* ---- NtStopTrace (SSDT 0x01D5) ------------------------------------------ */

NTSTATUS NtStopTrace(uint64_t trace_handle, uint64_t instance_name,
                     uint64_t properties, uint64_t a4,
                     uint64_t a5, uint64_t a6)
{
    (void)instance_name;
    (void)properties;
    (void)a4;
    (void)a5;
    (void)a6;

    uint64_t irq_flags;
    spin_lock_irqsave(&s_etw_lock, &irq_flags);

    etw_session_t *sess = find_session(trace_handle);
    if (!sess) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        return STATUS_INVALID_HANDLE;
    }

    /* -9 LEAK retrofit: previously this returned STATUS_UNSUCCESSFUL
     * for any non-RUNNING state, which meant a caller that created a
     * session via NtCreateTrace and then never started it had NO way
     * to free its 4 KiB buffer -- NtStopTrace refused, no NtCloseTrace
     * exists, and slot reuse only happens when magic is cleared.
     * Result: 4096-byte leak per orphan session. test_etw_create_trace
     * and test_etw_event_not_running both hit this path on KVM.
     *
     * Fix: NtStopTrace is now permissive for IDLE sessions too. An
     * IDLE session has no pending writes so the release path simply
     * frees the buffer + clears magic without a STATE_STOPPING
     * transition. Matches Windows' "stop releases the session" docs
     * and Linux's kfree-on-any-state cleanup convention. STOPPING
     * transient remains rejected because its buffer may be mid-
     * flush by another CPU. */
    if (sess->state != ETW_STATE_RUNNING && sess->state != ETW_STATE_IDLE) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        return STATUS_UNSUCCESSFUL;
    }

    uint8_t running = (sess->state == ETW_STATE_RUNNING);
    sess->state = ETW_STATE_STOPPING;
    if (running)
        klog(LOG_DEBUG, "etw", "session %u stopped (%u events, %u dropped)",
             (uint64_t)sess->handle,
             (uint64_t)sess->events_written,
             (uint64_t)sess->events_dropped);
    else
        klog(LOG_DEBUG, "etw", "session %u stopped from IDLE (never started)",
             (uint64_t)sess->handle);

    /* Release session: free buffer and clear magic so slot can be reused.
     * This makes the 8-session limit concurrent, not lifetime-per-boot. */
    {
        uint8_t *buf = sess->buffer;
        sess->buffer = (uint8_t *)0;
        sess->magic  = 0;
        sess->state  = ETW_STATE_IDLE;
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        if (buf)
            kfree(buf);
    }
    return STATUS_SUCCESS;
}

/* ---- NtFlushTrace (SSDT 0x01D6) ----------------------------------------- */

NTSTATUS NtFlushTrace(uint64_t trace_handle, uint64_t instance_name,
                      uint64_t properties, uint64_t a4,
                      uint64_t a5, uint64_t a6)
{
    (void)instance_name;
    (void)properties;
    (void)a4;
    (void)a5;
    (void)a6;

    uint64_t irq_flags;
    spin_lock_irqsave(&s_etw_lock, &irq_flags);

    etw_session_t *sess = find_session(trace_handle);
    if (!sess) {
        spin_unlock_irqrestore(&s_etw_lock, irq_flags);
        return STATUS_INVALID_HANDLE;
    }

    /* Drain the buffer -- reset write head */
    sess->buf_head = 0;
    klog(LOG_DEBUG, "etw", "session %u flushed (%u events total)",
         (uint64_t)sess->handle, (uint64_t)sess->events_written);

    spin_unlock_irqrestore(&s_etw_lock, irq_flags);
    return STATUS_SUCCESS;
}

/* ---- Kernel-local event emission ---------------------------------------- */

#include "desktop/wm.h"   /* for struct wm_frame_stats in etw_emit_wm_frame_presented */

void etw_emit_kernel_event(uint16_t event_id, uint8_t level,
                           const void *payload, uint32_t payload_size)
{
    if (payload_size > ETW_MAX_EVENT_SIZE)
        return;  /* oversized payload: drop silently (emit path must not fail) */

    uint64_t irq_flags;
    spin_lock_irqsave(&s_etw_lock, &irq_flags);

    /* Walk every session and write into the ones that are RUNNING.
     * Hot-path idle case (no running session) pays one cache miss +
     * one atomic lock pair -- the compositor can afford that per
     * frame. Subsystems that fire faster (per-IRQ) should gate on a
     * global "any-session-running" atomic before calling. */
    for (uint32_t i = 0; i < ETW_MAX_SESSIONS; i++) {
        etw_session_t *sess = &s_sessions[i];
        if (sess->magic != ETW_SESSION_MAGIC)
            continue;
        if (sess->state != ETW_STATE_RUNNING)
            continue;

        uint32_t record_size = (uint32_t)(sizeof(etw_event_header_t) + payload_size);

        /* Wrap around on overflow: overwrite oldest events, matching
         * the circular-buffer discipline in NtTraceEvent. */
        if (sess->buf_head + record_size > sess->buf_size)
            sess->buf_head = 0;

        if (record_size > sess->buf_size) {
            sess->events_dropped++;
            continue;
        }

        etw_event_header_t *hdr = (etw_event_header_t *)(sess->buffer + sess->buf_head);
        hdr->timestamp    = (uint32_t)(uptime_ns() / 1000000ULL);  /* ms, rate-change-safe */
        hdr->event_id     = event_id;
        hdr->level        = level;
        hdr->cpu_id       = 0;   /* populated when smp_this_cpu is wired into ETW */
        hdr->pid          = 0;
        hdr->payload_size = payload_size;

        if (payload_size > 0 && payload) {
            memcpy(sess->buffer + sess->buf_head + sizeof(etw_event_header_t),
                   payload, (size_t)payload_size);
        }

        sess->buf_head += record_size;
        sess->events_written++;
    }

    spin_unlock_irqrestore(&s_etw_lock, irq_flags);
}

void etw_emit_wm_frame_presented(const struct wm_frame_stats *stats)
{
    if (!stats)
        return;
    /* Level 4 = "information" in the 0=critical..5=verbose scale.
     * WM_FRAME_PRESENTED is high-volume observability, not an alert. */
    etw_emit_kernel_event(ETW_EVT_WM_FRAME_PRESENTED, 4,
                          stats, (uint32_t)sizeof(*stats));
}

/* ---- SSDT registration --------------------------------------------------- */

void etw_register_ssdt(void)
{
    ssdt_register(SSDT_NtTraceEvent,   (SSDT_HANDLER)NtTraceEvent);
    ssdt_register(SSDT_NtTraceControl, (SSDT_HANDLER)NtTraceControl);
    ssdt_register(SSDT_NtCreateTrace,  (SSDT_HANDLER)NtCreateTrace);
    ssdt_register(SSDT_NtQueryTrace,   (SSDT_HANDLER)NtQueryTrace);
    ssdt_register(SSDT_NtUpdateTrace,  (SSDT_HANDLER)NtUpdateTrace);
    ssdt_register(SSDT_NtStopTrace,    (SSDT_HANDLER)NtStopTrace);
    ssdt_register(SSDT_NtFlushTrace,   (SSDT_HANDLER)NtFlushTrace);

    klog(LOG_INFO, "etw", "ETW tracing syscalls registered (SSDT 0x01D0-0x01D6)");
}

/* ---- Init ---------------------------------------------------------------- */

void etw_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    s_next_handle = 1;

    klog(LOG_INFO, "etw", "ETW subsystem initialized (%u max sessions)",
         (uint64_t)ETW_MAX_SESSIONS);
}
