/* ============================================================================
 * alpc_port.h -- ALPC_PORT kernel object type + port-lifecycle API
 *
 * Section 2 of TODO-12. Builds on the on-the-wire ABI types from alpc.h
 * (PORT_MESSAGE, ALPC_PORT_ATTRIBUTES, etc.) and wires port objects into
 * the Object Manager so ports live as first-class handles with
 * reference counting, namespace entries under \RPC Control, and
 * DeleteProcedure-driven cleanup.
 *
 * Later sections extend the struct in place:
 *   - Section 3 uses ConnectionQueue to pump the connection handshake
 *   - Section 4 uses MessageQueue/PendingQueue + WaitQueue for sync send
 *   - Section 5 adds async completion list integration
 *   - Section 6 registers port sections into SectionList
 *   - Section 7 captures ClientToken on accept
 *   - Section 11 populates MessageZone with a pre-allocated buffer pool
 *   - Section 12 wires Stats into the latency profiler
 *
 * ABI note: struct alpc_port does NOT embed an OBJECT_HEADER. The Ob
 * header sits at a NEGATIVE offset -- `OB_HEADER_FROM_BODY(port)`
 * recovers it. All Ob types follow this convention (see
 * ob_event.h / ob_section.h).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/ipc/alpc.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/event.h"
#include "kernel/ob/ob_type.h"
#include "kernel/ob/handle_table.h"

/* ---- ALPC port access rights ------------------------------------------ */
/*
 * Per-object-type access masks (no central registry -- see
 * ob_section.h SECTION_MAP_* for the established pattern). §7 wires
 * SeAccessCheck on these with the server port's DACL; until then
 * they're informational.
 */
#define ALPC_PORT_CONNECT         0x00000001u
#define ALPC_PORT_ALL_ACCESS      0x0000001Fu

/* Forward declarations -- pulled via full headers at use-site in .c files. */
struct task;
struct access_token;

/* Forward declarations for future sections -- keep ALPC_PORT stable here
 * so §11 / §12 can extend without redefining the struct layout. */
struct alpc_message_zone;   /* populated in section 11 */

/* ---- ALPC_PORT_TYPE ---------------------------------------------------- */

typedef enum {
    AlpcServerConnectionPort    = 1, /* named, listens for connections */
    AlpcClientCommunicationPort = 2, /* unnamed, client end after connect */
    AlpcServerCommunicationPort = 3, /* unnamed, server end after accept */
} ALPC_PORT_TYPE;

/* ---- PORT_MESSAGE_ENTRY ------------------------------------------------ */
/*
 * Intrusive singly-linked queue node. The kernel has no generic
 * list_head_t yet, so every queue is a `{head, tail}` pair of node
 * pointers and each node carries its own Link_next.
 *
 * Allocation: one kmalloc of sizeof(PORT_MESSAGE_ENTRY) + Header.DataLength.
 * Body bytes follow the struct at offset sizeof(PORT_MESSAGE_ENTRY).
 */
typedef struct port_message_entry {
    struct port_message_entry *Link_next;      /* next entry in queue */
    PORT_MESSAGE               Header;          /* on-the-wire msg header */
    uint32_t                   ChargedSize;     /* bytes counted against
                                                 * owning port's quota at
                                                 * AlpcAllocateMessage time;
                                                 * 0 = uncharged (e.g. the
                                                 * PORT_CLOSED marker queued
                                                 * by AlpcDisconnectPort) */
    uint32_t                   _pad_charged;
    /* uint8_t Body[Header.DataLength]; -- flexible tail (no [] because the
     * struct already has well-defined size; payload is laid out by hand) */
} PORT_MESSAGE_ENTRY;

/* ---- ALPC_PENDING_REPLY ----------------------------------------------- */
/*
 * Sender-owned synchronous-request wait record. Created when the sender
 * issues NtAlpcSendWaitReceivePort with ALPC_MSGFLG_SYNC_REQUEST.
 *
 * The sender allocates this on the heap (charged against its own port's
 * MaxPoolUsage), links it into sender_port->PendingQueue keyed by
 * MessageId, then blocks on ReplyWait. The replier (or the
 * disconnect/destroy path) finds the record by MessageId, REMOVES it
 * from the queue, fills ReplyBody/ReplyType/ReplyStatus, sets Completed,
 * and event_set's ReplyWait -- ALL while still holding sender_port->Lock
 * so the sender (which takes the same lock on wake) cannot free the
 * record before the wake is observed.
 *
 * The reply payload lives inline after the struct: kmalloc allocates
 * sizeof(ALPC_PENDING_REPLY) + ReplyBodyCap bytes; ReplyBody points at
 * the trailing region.
 */
typedef struct alpc_pending_reply {
    struct alpc_pending_reply *Link_next;
    uint64_t                   MessageId;       /* matches request */
    uint32_t                   ChargedSize;     /* against sender port quota */
    uint16_t                   ReplyBodyCap;    /* allocated capacity */
    uint16_t                   ReplyBodyLen;    /* bytes the replier wrote */
    uint16_t                   ReplyType;       /* ALPC_MSG_TYPE_REPLY or
                                                 * ALPC_MSG_TYPE_PORT_CLOSED */
    uint8_t                    Completed;       /* 0 waiting, 1 signalled */
    uint8_t                    _pad[5];
    NTSTATUS                   ReplyStatus;     /* set by replier/disconnect */
    event_t                    ReplyWait;       /* sender blocks here */
    uint8_t                   *ReplyBody;       /* points to inline tail */
} ALPC_PENDING_REPLY;

/* ---- Pending-reply queue (separate from message queue) ---------------- */

typedef struct alpc_pending_queue {
    ALPC_PENDING_REPLY *Head;
    ALPC_PENDING_REPLY *Tail;
    uint32_t            Count;
} ALPC_PENDING_QUEUE;

/* ---- ALPC_PORT_STATS stub --------------------------------------------- */
/*
 * Populated by section 12 (port monitor / latency profiler). Defined as
 * a concrete small struct here so the ALPC_PORT body has a stable size
 * while later sections grow it; the internal layout is NOT an ABI
 * contract -- consumers treat ALPC_PORT as opaque and only the public
 * API in this header crosses translation units. No `_Static_assert`
 * on sizeof(ALPC_PORT) until layout stabilises after §4-§12.
 */
typedef struct alpc_port_stats {
    uint64_t MessagesReceived;
    uint64_t MessagesSent;
    uint64_t BytesTransferred;
    uint64_t ConnectionsAccepted;
} ALPC_PORT_STATS;

/* ---- Intrusive queue head --------------------------------------------- */

typedef struct alpc_msg_queue {
    PORT_MESSAGE_ENTRY *Head;
    PORT_MESSAGE_ENTRY *Tail;
    uint32_t            Count;
} ALPC_MSG_QUEUE;

/* ---- ALPC_PORT body --------------------------------------------------- */

typedef struct alpc_port {
    ALPC_PORT_TYPE         PortType;
    uint32_t               _pad_type;          /* align next pointer field */

    struct alpc_port      *ConnectedPort;     /* peer (comm ports) */
    struct alpc_port      *ConnectionPort;   /* server listen port (comm ports) */

    spinlock_t             Lock;               /* protects queues + refs +
                                                 * PoolUsageBytes */

    ALPC_MSG_QUEUE         MessageQueue;       /* pending inbound */
    ALPC_PENDING_QUEUE     PendingQueue;       /* awaiting reply -- §4 */
    ALPC_MSG_QUEUE         ConnectionQueue;   /* connection requests (server) */

    uint64_t               PoolUsageBytes;     /* tracked under Lock; charged
                                                 * by AlpcAllocateMessage and
                                                 * AlpcAllocatePendingReply,
                                                 * uncharged on free */

    uint64_t               NextMessageId;     /* monotonically increasing */
    void                  *PortContext;       /* opaque user data */

    ALPC_PORT_ATTRIBUTES   Attributes;         /* 80 bytes, §1 */

    struct task           *OwnerTask;         /* creator/connector */
    event_t                WaitQueue;          /* cross-task wakeup (server:
                                                 * connection arrived;
                                                 * client/comm: message
                                                 * arrived); auto-reset */
    struct access_token   *ClientToken;       /* captured on accept -- §7 */

    /* Future extensions: keep these at the end so adding to them does
     * not disturb earlier field offsets. Section 6 grows SectionList,
     * section 11 fills MessageZone, section 12 populates Stats. */
    struct {
        void *Head;
        void *Tail;
        uint32_t Count;
    }                      SectionList;       /* empty until §6 */
    struct alpc_message_zone *MessageZone;   /* NULL until §11 */
    ALPC_PORT_STATS        Stats;              /* zero-init until §12 */

    /* §5 Async delivery & waitable port.
     * CompletionPortHandle is set (non-INVALID) when a user-space I/O
     * Completion Port has been associated via
     * NtAlpcSetInformation(AlpcAssociateCompletionPortInformation). Once
     * set, every message enqueued on THIS port's MessageQueue also
     * causes io_completion_post() to enqueue a notification packet on
     * the associated IOCP; the ALPC message stays on MessageQueue so
     * the receiver can still pull the body via NtAlpcSendWaitReceivePort.
     * A failed IOCP post is logged and ignored (best-effort wake).
     *
     * IsWaitable mirrors ALPC_PORTFLG_WAITABLE_PORT on Attributes.Flags;
     * cached here so wait_on_handle does not have to re-read attrs.
     * SignalledEvent is a MANUAL_RESET event that mirrors
     * MessageQueue.Count > 0 -- set on 0->1 transition, reset on 1->0,
     * both under port->Lock so the event state is never out of sync
     * with the queue count. */
    HANDLE                 CompletionPortHandle;
    uint64_t               CompletionKey;
    uint64_t               DroppedNotifications;  /* increments when
                                                   * io_completion_post
                                                   * fails; first failure
                                                   * is the only one that
                                                   * logs to avoid the
                                                   * kernel log flooding
                                                   * under IOCP
                                                   * backpressure */
    uint8_t                IsWaitable;
    uint8_t                _pad_waitable[7];
    event_t                SignalledEvent;

    uint8_t                Disconnected;
    uint8_t                _pad_tail[7];
} ALPC_PORT;

/* §5 NtAlpcSetInformation: ALPC_PORT_INFORMATION_CLASS values.
 * AlpcAssociateCompletionPortInformation wires an IOCP to the port so
 * future sends post a completion notification in addition to landing on
 * MessageQueue. Other values land with §9 (NtAlpcQueryInformation). */
typedef enum {
    AlpcAssociateCompletionPortInformation = 0,
} ALPC_PORT_INFORMATION_CLASS;

/* Payload for AlpcAssociateCompletionPortInformation. */
typedef struct {
    HANDLE    CompletionPort;     /* IOCP handle (idx + 0x10000) */
    uint64_t  CompletionKey;      /* opaque value returned to receiver */
} ALPC_PORT_ASSOCIATE_COMPLETION_PORT;

/* ---- API -------------------------------------------------------------- */

/* Registered by alpc_port_init(). NULL until init runs; tests may assert
 * on non-NULL after alpc_init() has returned BOOT_OK. */
extern const OBJECT_TYPE *ObpAlpcPortType;

/*
 * alpc_port_init -- register ObpAlpcPortType and create \RPC Control.
 *
 * Called from alpc_init() during boot_phase3. Returns BOOT_OK on
 * success, BOOT_FATAL on type-registration failure or if the
 * \RPC Control directory cannot be created -- neither is recoverable
 * because later ALPC sections assume both resources exist.
 *
 * Idempotent: safe to call twice (does nothing on the second call).
 */
boot_result_t alpc_port_init(void);

/*
 * AlpcCreatePort -- create a server connection port.
 *
 * Parameters:
 *   ht           -- handle table to receive the new handle (usually
 *                   &task_current()->handle_table)
 *   name         -- component name under \RPC Control (leaf only,
 *                   no `\\` or `/`), or NULL for an unnamed
 *                   handle-only port (client/inline use cases)
 *   attrs        -- caller-supplied port attributes (copied by value),
 *                   or NULL to use zero-init defaults
 *   out_handle   -- receives the HANDLE on success; untouched on
 *                   failure
 *
 * Returns an NTSTATUS so callers can distinguish duplicate-name
 * collisions (STATUS_OBJECT_NAME_COLLISION), invalid leaves
 * (STATUS_OBJECT_NAME_INVALID), missing \RPC Control
 * (STATUS_OBJECT_NAME_NOT_FOUND), and resource exhaustion
 * (STATUS_INSUFFICIENT_RESOURCES). Every failure path rolls back
 * state so the namespace never holds a half-created entry and the
 * handle table never holds a dangling entry.
 */
NTSTATUS AlpcCreatePort(HANDLE_TABLE *ht, const char *name,
                        const ALPC_PORT_ATTRIBUTES *attrs,
                        HANDLE *out_handle);

/*
 * AlpcConnectPort -- client-side connection initiation.
 *
 *   ht          -- handle table to receive the new client-comm handle
 *   port_name   -- full path, typically "\\RPC Control\\<name>"
 *   timeout_ms  -- 0 = block indefinitely, otherwise bounded wait
 *   out_handle  -- on success, receives the client communication-port
 *                  handle; on failure untouched
 *
 * Returns STATUS_SUCCESS on accept, STATUS_PORT_CONNECTION_REFUSED
 * on server reject, STATUS_OBJECT_NAME_NOT_FOUND if no port at
 * port_name, STATUS_OBJECT_TYPE_MISMATCH if the path resolves to a
 * non-server-connection port, STATUS_TIMEOUT if the server did not
 * accept/reject within timeout_ms, STATUS_INSUFFICIENT_RESOURCES on
 * allocation failure. Rolls back every partial state on failure.
 */
NTSTATUS AlpcConnectPort(HANDLE_TABLE *ht, const char *port_name,
                         uint32_t timeout_ms, HANDLE *out_handle);

/*
 * AlpcAcceptConnectPort -- server-side connection completion.
 *
 *   ht                    -- handle table to receive the new
 *                            server-comm handle
 *   conn_port_handle      -- server connection port (the thing
 *                            AlpcCreatePort returned)
 *   accept                -- 1 = accept, 0 = reject
 *   timeout_ms            -- max wait for a pending connection
 *                            request; 0 = block indefinitely
 *   out_server_comm_handle -- on accept success, receives the server
 *                             communication-port handle
 *
 * Returns STATUS_SUCCESS on both accept and reject (the server's
 * decision succeeded); rejection propagates as
 * STATUS_PORT_CONNECTION_REFUSED to the blocked client. Returns
 * STATUS_TIMEOUT if no request arrived in time,
 * STATUS_INVALID_PORT_HANDLE if the handle is not a server-connection
 * port, STATUS_INSUFFICIENT_RESOURCES on allocation failure.
 */
NTSTATUS AlpcAcceptConnectPort(HANDLE_TABLE *ht,
                               HANDLE conn_port_handle, int accept,
                               uint32_t timeout_ms,
                               HANDLE *out_server_comm_handle);

/*
 * AlpcDisconnectPort -- tear down the cross-link and notify the peer.
 *
 * Queues a PORT_CLOSED message on the peer's MessageQueue so any
 * receiver sees the close, then clears this side's ConnectedPort
 * reference. The caller still owns their handle; a subsequent
 * NtClose is what finally frees the port object (via the refcount).
 *
 * Returns STATUS_SUCCESS whether a peer existed or not;
 * STATUS_INVALID_PORT_HANDLE if the handle is not an ALPC port.
 */
NTSTATUS AlpcDisconnectPort(HANDLE_TABLE *ht, HANDLE port_handle);

/* ---- §4 Synchronous Send+Wait+Receive Engine -------------------------- */

/*
 * AlpcAllocateMessage -- allocate a queueable message entry charged
 * against `charge_port`'s MaxPoolUsage.
 *
 * Single kmalloc for sizeof(PORT_MESSAGE_ENTRY) + data_length, with the
 * inline body region following the struct. Callers fill Header.* and
 * write the payload into ALPC_MSG_BODY(entry); ChargedSize is set by
 * this helper so AlpcFreeMessage can decrement the same amount.
 *
 * Returns NULL when data_length exceeds ALPC_MAX_ALLOWED_MESSAGE_LENGTH,
 * when MaxPoolUsage > 0 and the allocation would push PoolUsageBytes
 * past the cap, or when kmalloc fails. MaxPoolUsage == 0 means
 * "no per-port limit" (still bounded by ALPC_MAX_ALLOWED_MESSAGE_LENGTH
 * and overall heap availability).
 */
PORT_MESSAGE_ENTRY *AlpcAllocateMessage(struct alpc_port *charge_port,
                                        uint32_t data_length);

/*
 * AlpcFreeMessage -- decrement charge_port->PoolUsageBytes by entry's
 * ChargedSize and kfree the entry. Pass the SAME charge_port that was
 * passed to AlpcAllocateMessage. The entry MUST NOT be on any queue
 * when this is called.
 */
void AlpcFreeMessage(struct alpc_port *charge_port, PORT_MESSAGE_ENTRY *entry);

/*
 * Inline body region of an allocated message entry. Body starts
 * immediately after the struct; the entry was allocated with one extra
 * `data_length` bytes for the payload.
 */
static inline uint8_t *ALPC_MSG_BODY(PORT_MESSAGE_ENTRY *entry)
{
    return (uint8_t *)entry + sizeof(PORT_MESSAGE_ENTRY);
}

/*
 * AlpcSendWaitReceivePort -- the §4 core. Routes to one of four paths
 * based on (send_msg, flags):
 *
 *   receive-only  : send_msg == NULL. Dequeues from port->MessageQueue,
 *                   blocking on port->WaitQueue with timeout_ms (0 = no
 *                   wait; UINT32_MAX = block forever). Copies header +
 *                   body into recv_msg, capped by recv_buf_len. Returns
 *                   STATUS_SUCCESS on dequeue, STATUS_TIMEOUT if empty,
 *                   STATUS_PORT_DISCONNECTED if the port was torn down
 *                   while waiting.
 *
 *   datagram-send : flags has neither SYNC_REQUEST nor REPLY_MESSAGE,
 *                   send_msg != NULL. Allocates an entry charged on the
 *                   peer's port, copies the body, enqueues on peer's
 *                   MessageQueue, signals peer->WaitQueue. Returns
 *                   STATUS_SUCCESS without blocking.
 *
 *   sync request  : ALPC_MSGFLG_SYNC_REQUEST set, send_msg != NULL.
 *                   Allocates an inbound entry on peer's queue AND a
 *                   sender-owned ALPC_PENDING_REPLY on this port. Blocks
 *                   on the pending record's ReplyWait until the replier
 *                   completes it (STATUS_SUCCESS + reply body in
 *                   recv_msg) or disconnect/destroy fires
 *                   (STATUS_PORT_DISCONNECTED). Returns STATUS_TIMEOUT
 *                   if no reply arrived in time.
 *
 *   reply         : ALPC_MSGFLG_REPLY_MESSAGE set, send_msg has the
 *                   reply body and Header.MessageId equal to the
 *                   request's MessageId. Walks the peer's PendingQueue
 *                   under peer->Lock, removes the matching record,
 *                   copies the reply body into the record's inline
 *                   buffer, sets Completed and event_set's ReplyWait
 *                   while STILL HOLDING the lock so the sender cannot
 *                   free the record before the wake is observed.
 *                   Returns STATUS_REPLY_MESSAGE_MISMATCH if no record
 *                   matches the MessageId.
 *
 * Buffer rules:
 *   - Inline body is bounded by ALPC_MAX_ALLOWED_MESSAGE_LENGTH (65528).
 *   - Per-port MaxMessageLength caps each send (0 = use the hard
 *     ceiling). Send of a larger message returns STATUS_BUFFER_TOO_SMALL.
 *   - Receive truncates: if recv_buf_len is smaller than the queued
 *     entry's TotalLength, copy what fits; the entry is still consumed.
 */
NTSTATUS AlpcSendWaitReceivePort(HANDLE_TABLE *ht, HANDLE port_handle,
                                 uint32_t flags,
                                 PORT_MESSAGE *send_msg,
                                 PORT_MESSAGE *recv_msg,
                                 uint32_t recv_buf_len,
                                 uint32_t timeout_ms);

/* ---- §5 Asynchronous delivery & completion list ---------------------- */

/*
 * AlpcAssociateCompletionPort -- attach an I/O completion port to an
 * ALPC port so subsequent sends post a completion notification in
 * addition to landing the message on MessageQueue.
 *
 *   ht              -- handle table holding `port_handle`
 *   port_handle     -- ALPC_PORT handle (client or server comm port)
 *   completion_port -- IOCP handle returned by NtCreateIoCompletion
 *   key             -- opaque value returned verbatim to the consumer
 *                      via NtRemoveIoCompletion's KeyContext
 *
 * Returns STATUS_SUCCESS on association, STATUS_INVALID_HANDLE for a
 * bad port or IOCP handle, STATUS_INVALID_PORT_HANDLE if the handle is
 * not an ALPC port.
 */
NTSTATUS AlpcAssociateCompletionPort(HANDLE_TABLE *ht, HANDLE port_handle,
                                     HANDLE completion_port, uint64_t key);
