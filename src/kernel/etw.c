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

    /* Copy GUID if provided */
    if (trace_guid) {
        const ETW_GUID *g = (const ETW_GUID *)trace_guid;
        memcpy(&sess->guid, g, sizeof(ETW_GUID));
    } else {
        memset(&sess->guid, 0, sizeof(ETW_GUID));
    }

    uint64_t h = sess->handle;
    spin_unlock_irqrestore(&s_etw_lock, irq_flags);

    /* Write handle to caller */
    *(uint64_t *)out_handle = h;

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
    hdr->timestamp    = (uint32_t)system_get_ticks();
    hdr->event_id     = (uint16_t)(flags & 0xFFFF);
    hdr->level        = (uint8_t)((flags >> 16) & 0xFF);
    hdr->cpu_id       = 0;  /* will be populated from smp_this_cpu() when §9 adds per-entry context */
    hdr->pid          = 0;  /* same as above */
    hdr->payload_size = (uint32_t)field_size;

    /* Copy payload */
    if (field_size > 0 && fields) {
        memcpy(sess->buffer + sess->buf_head + sizeof(etw_event_header_t),
               (const void *)fields, (size_t)field_size);
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
    (void)in_len;
    (void)out_len;
    (void)ret_len;

    if (!in_buffer)
        return STATUS_INVALID_PARAMETER;

    uint64_t trace_handle = *(uint64_t *)in_buffer;

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
            etw_basic_info_t *info = (etw_basic_info_t *)out_buffer;
            memcpy(&info->guid, &sess->guid, sizeof(ETW_GUID));
            info->state          = sess->state;
            info->buf_size       = sess->buf_size;
            info->events_written = sess->events_written;
            info->events_dropped = sess->events_dropped;
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

    switch ((uint32_t)info_class) {
    case ETW_INFO_BASIC:
        if (length < sizeof(etw_basic_info_t)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            etw_basic_info_t *info = (etw_basic_info_t *)buffer;
            memcpy(&info->guid, &sess->guid, sizeof(ETW_GUID));
            info->state          = sess->state;
            info->buf_size       = sess->buf_size;
            info->events_written = sess->events_written;
            info->events_dropped = sess->events_dropped;
        }
        break;

    case ETW_INFO_STATISTICS:
        /* Statistics: write event counts to first two uint32_t slots */
        if (length < sizeof(uint32_t) * 2) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            uint32_t *stats = (uint32_t *)buffer;
            stats[0] = sess->events_written;
            stats[1] = sess->events_dropped;
        }
        break;

    default:
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }

    spin_unlock_irqrestore(&s_etw_lock, irq_flags);
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
