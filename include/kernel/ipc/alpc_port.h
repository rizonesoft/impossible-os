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
    struct alpc_port          *ReplyPort;       /* reply target (NULL until §4) */
    uint64_t                   ReplyMessageId;  /* request this is a reply for */
    uint8_t                    WaitingForReply; /* sender blocked on reply */
    uint8_t                    _pad[3];         /* align next field */
    event_t                    ReplySyncWait;   /* sender sleeps here -- §4 */
    /* uint8_t Body[Header.DataLength]; -- flexible tail (no [] because the
     * struct already has well-defined size; payload is laid out by hand) */
} PORT_MESSAGE_ENTRY;

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

    spinlock_t             Lock;               /* protects queues + refs */

    ALPC_MSG_QUEUE         MessageQueue;       /* pending inbound */
    ALPC_MSG_QUEUE         PendingQueue;       /* awaiting reply */
    ALPC_MSG_QUEUE         ConnectionQueue;   /* connection requests (server) */

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

    uint8_t                Disconnected;
    uint8_t                _pad_tail[7];
} ALPC_PORT;

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
