/* ============================================================================
 * etw.h -- Event Tracing for Windows (ETW) kernel interface
 *
 * Provides high-performance kernel/user tracing through the NtTrace* family
 * of syscalls (SSDT 0x01D0-0x01D6). Trace sessions capture structured events
 * from kernel subsystems and user-mode providers.
 *
 * Architecture:
 *   - Up to ETW_MAX_SESSIONS concurrent trace sessions
 *   - Each session has a circular event buffer (configurable size)
 *   - Events written via NtTraceEvent go to all matching sessions
 *   - NtTraceControl provides session management (start/stop/query/flush)
 *   - Sessions identified by opaque 64-bit handles
 *
 * Integration: events route through klog ring buffer for unified logging.
 * ETW adds structured metadata (provider GUID, event ID, level) on top.
 *
 * Reference: Windows ETW (NtTraceEvent, NtTraceControl, etc.)
 * SSDT indices: 0x01D0-0x01D6 (service_numbers.h)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

/* ---- Constants ----------------------------------------------------------- */

#define ETW_MAX_SESSIONS        8       /* max concurrent trace sessions */
#define ETW_DEFAULT_BUF_SIZE    4096    /* default event buffer size (bytes) */
#define ETW_MAX_BUF_SIZE        4096    /* max buffer -- kmalloc limit */
#define ETW_MAX_EVENT_SIZE      256     /* max single event payload (bytes) */
#define ETW_SESSION_MAGIC       0x45545753  /* "ETWS" */

/* Trace control function codes (NtTraceControl FunctionCode parameter) */
#define ETW_FUNC_START          0       /* start a trace session */
#define ETW_FUNC_STOP           1       /* stop a trace session */
#define ETW_FUNC_QUERY          2       /* query session status */
#define ETW_FUNC_UPDATE         3       /* update session properties */
#define ETW_FUNC_FLUSH          4       /* flush session buffer */

/* Trace information classes (NtQueryTrace TraceInformationClass) */
#define ETW_INFO_BASIC          0       /* basic session info */
#define ETW_INFO_STATISTICS     1       /* event counts and drops */

/* Session state */
#define ETW_STATE_IDLE          0       /* created but not started */
#define ETW_STATE_RUNNING       1       /* actively collecting events */
#define ETW_STATE_STOPPING      2       /* flush in progress */

/* ---- GUID type (128-bit, matches Windows GUID layout) -------------------- */

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} ETW_GUID;

/* ---- Trace event header (written into session buffer) -------------------- */

typedef struct {
    uint32_t timestamp;         /* PIT ticks at event time */
    uint16_t event_id;          /* provider-defined event ID */
    uint8_t  level;             /* event severity (0=critical..5=verbose) */
    uint8_t  cpu_id;            /* CPU that generated the event */
    uint32_t pid;               /* process ID */
    uint32_t payload_size;      /* bytes of payload following this header */
} etw_event_header_t;

_Static_assert(sizeof(etw_event_header_t) == 16,
    "etw_event_header_t must be 16 bytes for buffer alignment");

/* ---- Trace session (internal kernel state) ------------------------------- */

typedef struct {
    uint32_t    magic;          /* ETW_SESSION_MAGIC */
    uint32_t    state;          /* ETW_STATE_* */
    ETW_GUID    guid;           /* session GUID (from NtCreateTrace) */
    uint64_t    handle;         /* opaque handle returned to caller */
    uint8_t    *buffer;         /* circular event buffer */
    uint32_t    buf_size;       /* buffer capacity in bytes */
    uint32_t    buf_head;       /* write offset into buffer */
    uint32_t    events_written; /* total events written */
    uint32_t    events_dropped; /* events dropped (buffer full) */
    uint32_t    flags;          /* session flags from NtCreateTrace */
} etw_session_t;

/* ---- Basic info struct (returned by NtQueryTrace ETW_INFO_BASIC) --------- */

typedef struct {
    ETW_GUID    guid;
    uint32_t    state;
    uint32_t    buf_size;
    uint32_t    events_written;
    uint32_t    events_dropped;
} etw_basic_info_t;

_Static_assert(sizeof(etw_basic_info_t) == 32,
    "etw_basic_info_t must be 32 bytes");

/* ---- Public API ---------------------------------------------------------- */

/* Initialize ETW subsystem. Call once during Phase 3 (after ssdt_init). */
void etw_init(void);

/* Register all 7 NtTrace* handlers with the SSDT. Call after ssdt_init(). */
void etw_register_ssdt(void);

/* ---- NtTrace* syscall handlers (SSDT signature) -------------------------- */

/* 0x01D0: Write a trace event to a session */
NTSTATUS NtTraceEvent(uint64_t trace_handle, uint64_t flags,
                      uint64_t field_size, uint64_t fields,
                      uint64_t a5, uint64_t a6);

/* 0x01D1: Control trace sessions (start/stop/query/update/flush) */
NTSTATUS NtTraceControl(uint64_t function_code, uint64_t in_buffer,
                        uint64_t in_len, uint64_t out_buffer,
                        uint64_t out_len, uint64_t ret_len);

/* 0x01D2: Create a new trace session */
NTSTATUS NtCreateTrace(uint64_t out_handle, uint64_t desired_access,
                       uint64_t object_attributes, uint64_t trace_guid,
                       uint64_t a5, uint64_t a6);

/* 0x01D3: Query trace session information */
NTSTATUS NtQueryTrace(uint64_t trace_handle, uint64_t info_class,
                      uint64_t buffer, uint64_t length,
                      uint64_t a5, uint64_t a6);

/* 0x01D4: Update trace session properties */
NTSTATUS NtUpdateTrace(uint64_t trace_handle, uint64_t instance_name,
                       uint64_t properties, uint64_t a4,
                       uint64_t a5, uint64_t a6);

/* 0x01D5: Stop a trace session */
NTSTATUS NtStopTrace(uint64_t trace_handle, uint64_t instance_name,
                     uint64_t properties, uint64_t a4,
                     uint64_t a5, uint64_t a6);

/* 0x01D6: Flush trace session buffer */
NTSTATUS NtFlushTrace(uint64_t trace_handle, uint64_t instance_name,
                      uint64_t properties, uint64_t a4,
                      uint64_t a5, uint64_t a6);
