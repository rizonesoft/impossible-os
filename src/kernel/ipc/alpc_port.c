/* ============================================================================
 * alpc_port.c -- ALPC_PORT object type + \RPC Control + AlpcCreatePort
 *
 * Section 2 of TODO-12. Registers the ALPC Port Object Manager type,
 * creates the \RPC Control namespace directory, and exposes a
 * kernel-side helper that both the NT syscall retrofit
 * (NtAlpcCreatePort) and future in-kernel servers (CSRSS in §10) use.
 *
 * Queue and sync state is initialised empty; messages flow starting
 * in §4. The DeleteProcedure drains all three queues defensively so a
 * port closed before §4 ships (or reached from a fault path) still
 * cleans up without leaking message entries.
 * ============================================================================ */

#include "kernel/ipc/alpc_port.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/sched/task.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/nt/nt_file.h"         /* io_completion_post, validate */
#include "libc/string.h"               /* canonical kernel memcpy */

const OBJECT_TYPE *ObpAlpcPortType;

static void *s_rpc_control_dir;
static uint8_t s_inited;

/* Forward declarations for §5 helpers used by §3 AlpcDisconnectPort and
 * the §4 engine before their definitions appear below. */
static void msg_queue_enqueue_locked(ALPC_PORT *port, PORT_MESSAGE_ENTRY *entry);
static void alpc_notify_completion_port(ALPC_PORT *peer,
                                        uint64_t message_id,
                                        uint32_t data_len);

/* Forward declaration: full definition lives with the §3 connection
 * state machine below. alpc_port_on_delete needs the layout to drain
 * ConnectionQueue correctly; publishing the typedef up here lets on_delete
 * signal waiters without moving the whole block. */
typedef struct alpc_connection_request ALPC_CONNECTION_REQUEST;
struct alpc_connection_request {
    struct alpc_connection_request *Link_next;
    ALPC_PORT                      *ClientCommPort;
    ALPC_PORT                      *ServerCommPort;
    struct task                    *RequesterTask;
    NTSTATUS                        ReplyStatus;
    event_t                         ReplyEvent;
};

/* ---- Queue helpers ----------------------------------------------------- */

static void queue_init(ALPC_MSG_QUEUE *q)
{
    q->Head = (PORT_MESSAGE_ENTRY *)0;
    q->Tail = (PORT_MESSAGE_ENTRY *)0;
    q->Count = 0;
}

static void queue_drain(ALPC_MSG_QUEUE *q)
{
    PORT_MESSAGE_ENTRY *cur = q->Head;
    while (cur) {
        PORT_MESSAGE_ENTRY *next = cur->Link_next;
        /* on_delete drains the queue; the port is being freed, so
         * PoolUsageBytes accounting is moot. Use the uncharged
         * destroy path. */
        kfree(cur);
        cur = next;
    }
    q->Head = (PORT_MESSAGE_ENTRY *)0;
    q->Tail = (PORT_MESSAGE_ENTRY *)0;
    q->Count = 0;
}

static void pending_queue_init(ALPC_PENDING_QUEUE *q)
{
    q->Head = (ALPC_PENDING_REPLY *)0;
    q->Tail = (ALPC_PENDING_REPLY *)0;
    q->Count = 0;
}

static void pending_queue_enqueue(ALPC_PENDING_QUEUE *q, ALPC_PENDING_REPLY *r)
{
    r->Link_next = (ALPC_PENDING_REPLY *)0;
    if (!q->Head) {
        q->Head = r;
        q->Tail = r;
    } else {
        q->Tail->Link_next = r;
        q->Tail = r;
    }
    q->Count++;
}

/* Remove `r` from `q` if present. Caller holds the port's Lock.
 * Returns 1 if found and removed, 0 if not present. Pointer-keyed so
 * the sender's timeout-vs-reply race uses unambiguous identity (no
 * MessageId reuse risk within the port lifetime). */
static int pending_queue_remove(ALPC_PENDING_QUEUE *q, ALPC_PENDING_REPLY *r)
{
    ALPC_PENDING_REPLY **pp = &q->Head;
    ALPC_PENDING_REPLY *prev = (ALPC_PENDING_REPLY *)0;
    while (*pp) {
        if (*pp == r) {
            *pp = r->Link_next;
            if (q->Tail == r)
                q->Tail = prev;
            q->Count--;
            r->Link_next = (ALPC_PENDING_REPLY *)0;
            return 1;
        }
        prev = *pp;
        pp = &(*pp)->Link_next;
    }
    return 0;
}

/* Find a pending record by MessageId. Caller holds the port's Lock.
 * Returns the record (still on the queue) or NULL; reply path then
 * removes it via pending_queue_remove. */
static ALPC_PENDING_REPLY *pending_queue_find(ALPC_PENDING_QUEUE *q,
                                              uint64_t message_id)
{
    ALPC_PENDING_REPLY *cur = q->Head;
    while (cur) {
        if (cur->MessageId == message_id)
            return cur;
        cur = cur->Link_next;
    }
    return (ALPC_PENDING_REPLY *)0;
}

/* ---- DeleteProcedure --------------------------------------------------- */

static void alpc_port_on_delete(void *body)
{
    ALPC_PORT *p = (ALPC_PORT *)body;
    uint64_t irqf;

    /* Snapshot queues under the port's own lock, then release before
     * freeing entries -- kfree may sleep in heap debug builds; the
     * lock-hold-time rule forbids blocking calls while holding a
     * spinlock.
     *
     * Pending sync waiters are SENDER-OWNED (heap allocations on this
     * port's PendingQueue but freed by the sender thread on wake).
     * on_delete must signal them WHILE STILL HOLDING the lock so the
     * sender (which acquires the same lock on wake) cannot free a
     * record before our event_set fires. After the lock is dropped, the
     * pending records are unreachable from this port and any waiter
     * that wakes will simply observe Completed=1 and free its own
     * record. */
    spin_lock_irqsave(&p->Lock, &irqf);
    ALPC_MSG_QUEUE msg = p->MessageQueue;
    ALPC_PENDING_REPLY *pending_head = p->PendingQueue.Head;
    ALPC_CONNECTION_REQUEST *conn_head =
        (ALPC_CONNECTION_REQUEST *)p->ConnectionQueue.Head;
    queue_init(&p->MessageQueue);
    pending_queue_init(&p->PendingQueue);
    p->ConnectionQueue.Head  = (PORT_MESSAGE_ENTRY *)0;
    p->ConnectionQueue.Tail  = (PORT_MESSAGE_ENTRY *)0;
    p->ConnectionQueue.Count = 0;
    ALPC_PORT *peer = p->ConnectedPort;
    ALPC_PORT *listen = p->ConnectionPort;
    p->ConnectedPort   = (ALPC_PORT *)0;
    p->ConnectionPort  = (ALPC_PORT *)0;
    struct access_token *tok = p->ClientToken;
    p->ClientToken = (struct access_token *)0;
    p->Disconnected = 1;

    /* Wake every pending sync waiter under the lock. Contract violation
     * if non-empty (sender should have removed before port hit ref==0)
     * but log + signal defensively so no thread hangs. */
    if (pending_head) {
        klog(LOG_WARN, "alpc",
             "port on_delete with pending sync replies -- contract violation");
        ALPC_PENDING_REPLY *cur = pending_head;
        while (cur) {
            ALPC_PENDING_REPLY *next = cur->Link_next;
            cur->Link_next   = (ALPC_PENDING_REPLY *)0;
            cur->ReplyType   = ALPC_MSG_TYPE_PORT_CLOSED;
            cur->ReplyStatus = STATUS_PORT_DISCONNECTED;
            cur->ReplyBodyLen = 0;
            cur->Completed   = 1;
            event_set(&cur->ReplyWait);
            cur = next;
        }
    }
    spin_unlock_irqrestore(&p->Lock, irqf);

    queue_drain(&msg);

    /* ConnectionQueue carries ALPC_CONNECTION_REQUEST nodes, NOT
     * PORT_MESSAGE_ENTRY -- the list lives on client stacks + heaps and
     * each node is owned/freed by its requesting client AFTER
     * event_wait returns. Contract: by the time on_delete fires,
     * ref_count == 0, and the only references that could possibly have
     * kept the port alive are the client_comm ConnectionPort links +
     * the client's lookup ref. Both are dropped before the client
     * frees its request -- so ConnectionQueue MUST be empty in the
     * normal flow. If it is not, that is a contract violation higher
     * up; we signal every pending client with PORT_DISCONNECTED so at
     * least no one hangs, and let the client's post-wake path free the
     * node. kfree'ing the node ourselves would UAF on the client's
     * kfree. */
    if (conn_head) {
        klog(LOG_WARN, "alpc",
             "port on_delete with pending connects -- contract violation");
        while (conn_head) {
            ALPC_CONNECTION_REQUEST *next = conn_head->Link_next;
            conn_head->ServerCommPort = (ALPC_PORT *)0;
            conn_head->ReplyStatus    = STATUS_PORT_DISCONNECTED;
            event_set(&conn_head->ReplyEvent);
            conn_head = next;
        }
    }

    if (peer)
        ObDereferenceObject(peer);
    if (listen)
        ObDereferenceObject(listen);                  /* drop ConnectionPort ref */

    /* ClientToken is owned by the process (tied to task lifetime), not
     * refcount-owned by the port -- see token.h notes. Just NULL it out
     * here; do not call ObDereferenceObject on it. Future §7 may
     * capture a duplicated token that IS refcount-owned, at which
     * point the drop path becomes ObDereferenceObject. */
    (void)tok;

    /* MessageZone is NULL until §11; the drop path is a no-op today. */
    /* SectionList is empty until §6; no traversal needed. */

    klog(LOG_DEBUG, "alpc", "port deleted: type=%u", (uint64_t)p->PortType);
}

/* ---- Initialization ---------------------------------------------------- */

boot_result_t alpc_port_init(void)
{
    if (s_inited)
        return BOOT_OK;

    if (!ObpDirectoryType) {
        klog(LOG_ERROR, "alpc", "alpc_port_init: Object Manager not ready");
        return BOOT_FATAL;
    }
    if (!ObpRootDirectory) {
        klog(LOG_ERROR, "alpc", "alpc_port_init: Ob namespace root missing");
        return BOOT_FATAL;
    }

    ObpAlpcPortType = ob_create_type(&(OBJECT_TYPE){
        .name      = "ALPC Port",
        .body_size = sizeof(ALPC_PORT),
        .on_close  = (void (*)(void *, uint32_t))0,
        .on_delete = alpc_port_on_delete,
        .on_open   = (int (*)(void *, uint32_t))0,
        .on_parse  = (int (*)(void *, const char *, void **))0,
    });
    if (!ObpAlpcPortType) {
        klog(LOG_ERROR, "alpc", "ob_create_type(ALPC Port) failed");
        return BOOT_FATAL;
    }
    klog(LOG_INFO, "alpc", "ALPC Port type registered");

    /* Create \RPC Control */
    s_rpc_control_dir = ob_ns_create_directory(ObpRootDirectory);
    if (!s_rpc_control_dir) {
        klog(LOG_ERROR, "alpc", "failed to create RPC Control directory");
        return BOOT_FATAL;
    }
    if (ObInsertObject(s_rpc_control_dir, "RPC Control",
                       ObpRootDirectory) != 0) {
        klog(LOG_ERROR, "alpc", "failed to insert RPC Control in namespace");
        /* The directory was marked OB_FLAG_PERMANENT by
         * ob_ns_create_directory; clear it so the deref actually frees
         * the orphaned directory body. */
        OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(s_rpc_control_dir);
        hdr->flags &= ~OB_FLAG_PERMANENT;
        ObDereferenceObject(s_rpc_control_dir);
        s_rpc_control_dir = (void *)0;
        return BOOT_FATAL;
    }
    klog(LOG_INFO, "alpc", "\\RPC Control directory created");

    s_inited = 1;
    return BOOT_OK;
}

/* ---- AlpcCreatePort ---------------------------------------------------- */

/*
 * alpc_validate_attrs -- gate user-reachable ALPC_PORT_ATTRIBUTES.
 *
 * Called from AlpcCreatePort before the port is allocated. Rejects:
 *   - unknown Flags bits (anything outside the 4 declared ALPC_PORTFLG_*)
 *   - ALPC_PORTFLG_SYSTEM_PROCESS: reserved for a privileged kernel
 *     path; will gain an SeAccessCheck in §7 once token capture is up.
 *     For §2, the only legitimate caller is the kernel itself, and the
 *     kernel does not set this flag today -- so any caller passing it
 *     is either confused or malicious.
 *   - non-zero reserved pad fields (Pad0/Pad1/Pad2): catch uninitialised
 *     stack bytes from user mode; ABI contract is pad-must-be-zero.
 */
static NTSTATUS alpc_validate_attrs(const ALPC_PORT_ATTRIBUTES *attrs)
{
    uint32_t allowed_flags;

    if (!attrs)
        return STATUS_SUCCESS;

    allowed_flags = ALPC_PORTFLG_LPC_MODE
                  | ALPC_PORTFLG_WAITABLE_PORT
                  | ALPC_PORTFLG_ALLOW_DUP_OBJECT;
    /* ALPC_PORTFLG_SYSTEM_PROCESS intentionally excluded: privileged
     * bit, §7 gates it via SeAccessCheck once token capture ships. */

    if (attrs->Flags & ~allowed_flags)
        return STATUS_INVALID_PARAMETER;

    if (attrs->Pad0 != 0 || attrs->Pad1 != 0 || attrs->Pad2 != 0)
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

/*
 * Ordering (follows NtCreateDirectoryObject idiom in nt_namespace.c):
 *   1. ob_alloc_object         (ref_count = 1; creation ref)
 *   2. Initialize fields        (port not yet published)
 *   3. ObpAllocateHandle       (takes +1 ref; publishes to caller)
 *   4. If named: ObInsertObject(takes +1 ref; publishes to namespace)
 *      On ObInsertObject failure: ObpFreeHandle (-1 ref) then deref
 *      creation ref. No half-created namespace entry is possible.
 *   5. ObDereferenceObject     (drops creation ref; handle+namespace keep it alive)
 *
 * The previous ordering (ObInsertObject before ObpAllocateHandle) left
 * a reachable half-created port in \RPC Control when handle-allocation
 * failed -- there is no caller-visible unlink primitive yet. Allocating
 * the handle first means handle failure rolls back cleanly without
 * touching the namespace, and namespace insert failure only needs a
 * handle free.
 */
NTSTATUS AlpcCreatePort(HANDLE_TABLE *ht, const char *name,
                        const ALPC_PORT_ATTRIBUTES *attrs,
                        HANDLE *out_handle)
{
    ALPC_PORT *port;
    HANDLE h;
    void *rpc_dir = (void *)0;
    NTSTATUS st;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;
    *out_handle = INVALID_HANDLE_VALUE;

    if (!ht || !ObpAlpcPortType)
        return STATUS_INVALID_PARAMETER;

    st = alpc_validate_attrs(attrs);
    if (!NT_SUCCESS(st))
        return st;

    /* Leaf-name validation: ObInsertObject rejects empty or >
     * OB_NAME_MAX-1, but we also refuse embedded backslashes here so a
     * caller cannot smuggle a path ("foo\\bar") past the namespace's
     * component contract. Kernel helpers (tests, CSRSS) pass pure leaf
     * names; syscall callers split at the syscall boundary. */
    if (name) {
        const char *c;
        uint32_t len = 0;
        for (c = name; *c; c++) {
            if (*c == '\\' || *c == '/')
                return STATUS_OBJECT_NAME_INVALID;
            if (++len >= 64)                    /* OB_NAME_MAX */
                return STATUS_OBJECT_NAME_INVALID;
        }
        if (len == 0)
            return STATUS_OBJECT_NAME_INVALID;
    }

    port = (ALPC_PORT *)ob_alloc_object(ObpAlpcPortType);
    if (!port)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* ob_alloc_object zero-fills the body; explicit field setup below
     * captures only the non-zero initial state. */
    port->PortType      = AlpcServerConnectionPort;
    port->Lock.flag     = 0;                    /* SPINLOCK_INIT */
    port->OwnerTask     = task_current();
    port->NextMessageId = 1;
    event_init(&port->WaitQueue, "alpc_port_wait", EVENT_AUTO_RESET, 0);
    /* §5.3: SignalledEvent is manual-reset so NtWaitForSingleObject on
     * the port sees "queue non-empty" until the consumer explicitly
     * drains. event_reset on the 1 -> 0 dequeue transition clears it. */
    event_init(&port->SignalledEvent, "alpc_port_signal",
               EVENT_MANUAL_RESET, 0);

    if (attrs) {
        port->Attributes = *attrs;              /* copy by value */
        if (attrs->Flags & ALPC_PORTFLG_WAITABLE_PORT)
            port->IsWaitable = 1;
    }

    /* 3. Allocate the handle FIRST. On failure, the creation ref is the
     * only reference; a simple deref frees the port. No namespace
     * entry has been created yet, so no unlink is needed. */
    h = ObpAllocateHandle(ht, port, /* access */ 0, /* attrs */ 0);
    if (h == INVALID_HANDLE_VALUE) {
        ObDereferenceObject(port);              /* drops creation ref */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* 4. If named, insert under \RPC Control. Failure path frees the
     * handle (which drops a ref) and then drops the creation ref. */
    if (name) {
        if (ObLookupObjectByName("\\RPC Control", ObpDirectoryType, 0,
                                 &rpc_dir) != 0 || !rpc_dir) {
            ObpFreeHandle(ht, h);               /* drops handle ref */
            ObDereferenceObject(port);          /* drops creation ref */
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        if (ObInsertObject(port, name, rpc_dir) != 0) {
            /* ObInsertObject fails on duplicate or directory-full; both
             * collapse into OBJECT_NAME_COLLISION at the syscall
             * boundary, matching Windows NT behaviour. */
            ObDereferenceObject(rpc_dir);       /* drop lookup ref */
            ObpFreeHandle(ht, h);               /* drops handle ref */
            ObDereferenceObject(port);          /* drops creation ref */
            return STATUS_OBJECT_NAME_COLLISION;
        }

        ObDereferenceObject(rpc_dir);            /* drop lookup ref */
    }

    ObDereferenceObject(port);                   /* drop creation ref */
    *out_handle = h;
    return STATUS_SUCCESS;
}

/* =========================================================================
 * §3 Connection state machine
 *
 * Design:
 *   - Client pre-allocates its own client_comm port + handle so the accept
 *     path never touches the client's handle table (cross-process
 *     handle-table access would require privileges we don't have yet).
 *   - Connection request is a file-local node, not a PORT_MESSAGE_ENTRY,
 *     because it carries cross-port pointers + a reply event rather than
 *     wire-format bytes.
 *   - One spinlock per port is enough today: the client only mutates the
 *     server_conn->ConnectionQueue while holding server_conn->Lock; the
 *     server drains the same queue under the same lock. Cross-link
 *     assignment (server_comm <-> client_comm) happens before the
 *     request is published to the client via event_set, so the client
 *     observes a consistent cross-link.
 * =======================================================================*/

/* ALPC_CONNECTION_REQUEST layout is declared at the top of this file
 * so alpc_port_on_delete can signal pending waiters. The helpers
 * below operate on that shared definition. */

/*
 * Server-side FIFO for ALPC_CONNECTION_REQUEST. Reuses the port's
 * ConnectionQueue field (which is declared as `ALPC_MSG_QUEUE` for
 * PORT_MESSAGE_ENTRY nodes) by the common-initial-sequence rule:
 * PORT_MESSAGE_ENTRY and ALPC_CONNECTION_REQUEST both start with a
 * `struct X *Link_next` field, and ALPC_MSG_QUEUE's Head/Tail slots
 * are plain pointers we can carry either node type in. The helpers
 * below do the cast in one place so the rest of the file stays
 * type-safe. No separate ALPC_CONN_Q struct is needed. */

static void conn_queue_enqueue(ALPC_MSG_QUEUE *q, ALPC_CONNECTION_REQUEST *r)
{
    r->Link_next = (ALPC_CONNECTION_REQUEST *)0;
    if (!q->Head) {
        q->Head = (PORT_MESSAGE_ENTRY *)r;
        q->Tail = (PORT_MESSAGE_ENTRY *)r;
    } else {
        ((ALPC_CONNECTION_REQUEST *)q->Tail)->Link_next = r;
        q->Tail = (PORT_MESSAGE_ENTRY *)r;
    }
    q->Count++;
}

static ALPC_CONNECTION_REQUEST *conn_queue_dequeue(ALPC_MSG_QUEUE *q)
{
    ALPC_CONNECTION_REQUEST *r = (ALPC_CONNECTION_REQUEST *)q->Head;
    if (!r)
        return (ALPC_CONNECTION_REQUEST *)0;
    q->Head = (PORT_MESSAGE_ENTRY *)r->Link_next;
    if (!q->Head)
        q->Tail = (PORT_MESSAGE_ENTRY *)0;
    q->Count--;
    r->Link_next = (ALPC_CONNECTION_REQUEST *)0;
    return r;
}

/* ---- Internal helper: allocate + init a bare comm port ---------------- */
/*
 * Comm ports (client + server communication) are unnamed. We bypass
 * AlpcCreatePort because that path is named-port-focused and does a
 * namespace lookup the comm case does not need. The init matches
 * AlpcCreatePort's "new port" state exactly.
 */
static ALPC_PORT *alpc_alloc_comm_port(ALPC_PORT_TYPE type,
                                       const ALPC_PORT_ATTRIBUTES *inherit)
{
    ALPC_PORT *p = (ALPC_PORT *)ob_alloc_object(ObpAlpcPortType);
    if (!p)
        return (ALPC_PORT *)0;
    p->PortType      = type;
    p->Lock.flag     = 0;
    p->OwnerTask     = task_current();
    p->NextMessageId = 1;
    event_init(&p->WaitQueue, "alpc_comm_wait", EVENT_AUTO_RESET, 0);
    event_init(&p->SignalledEvent, "alpc_comm_signal",
               EVENT_MANUAL_RESET, 0);
    /* Comm ports inherit the connection port's Attributes so per-port
     * caps (MaxMessageLength, MaxPoolUsage, MaxViewSize, etc.) apply to
     * messages flowing through this side of the cross-link. Matches
     * Windows ALPC behaviour where the connection-port attrs govern
     * the whole port pair. Waitable bit inherits for the same reason:
     * if the listener was created waitable, both comm ports can be
     * wait targets for NtWaitForSingleObject. */
    if (inherit) {
        p->Attributes = *inherit;
        if (inherit->Flags & ALPC_PORTFLG_WAITABLE_PORT)
            p->IsWaitable = 1;
    }
    return p;
}

/* ---- AlpcConnectPort --------------------------------------------------- */

NTSTATUS AlpcConnectPort(HANDLE_TABLE *ht, const char *port_name,
                         uint32_t timeout_ms, HANDLE *out_handle)
{
    void *server_body = (void *)0;
    ALPC_PORT *server_conn, *client_comm;
    ALPC_CONNECTION_REQUEST *req;
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t irqf;
    NTSTATUS st;
    int waited_ok;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;
    *out_handle = INVALID_HANDLE_VALUE;
    if (!ht || !port_name || !ObpAlpcPortType)
        return STATUS_INVALID_PARAMETER;

    /* 1. Look up server connection port by name (refcount +1). */
    if (ObLookupObjectByName(port_name, ObpAlpcPortType, 0,
                             &server_body) != 0 || !server_body)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    server_conn = (ALPC_PORT *)server_body;
    if (server_conn->PortType != AlpcServerConnectionPort) {
        ObDereferenceObject(server_body);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    /* SCOPE-GAP-ALLOWED: SeAccessCheck on server DACL with
     * ALPC_PORT_CONNECT pending security reference monitor integration.
     * Concrete retrofit tracked under 11-security-reference-monitor/
     * 02-kernel-core/TODO-15-security-reference-monitor.md (SeAccessCheck wiring). */

    /* 2. Allocate client communication port; inherit attributes from
     * the named server connection port so MaxMessageLength /
     * MaxPoolUsage caps apply to client-side sends too. */
    client_comm = alpc_alloc_comm_port(AlpcClientCommunicationPort,
                                       &server_conn->Attributes);
    if (!client_comm) {
        ObDereferenceObject(server_body);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ObReferenceObject(server_conn);                   /* for ConnectionPort field */
    client_comm->ConnectionPort = server_conn;

    /* 3. Allocate handle for client_comm in caller's table. */
    h = ObpAllocateHandle(ht, client_comm, ALPC_PORT_ALL_ACCESS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        ObDereferenceObject(server_conn);             /* undo ConnectionPort ref */
        ObDereferenceObject(client_comm);             /* drops creation ref */
        ObDereferenceObject(server_body);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* 4. Build the connection request. */
    req = (ALPC_CONNECTION_REQUEST *)kmalloc(sizeof(*req));
    if (!req) {
        ObpFreeHandle(ht, h);
        ObDereferenceObject(server_conn);
        ObDereferenceObject(client_comm);
        ObDereferenceObject(server_body);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    req->Link_next      = (ALPC_CONNECTION_REQUEST *)0;
    req->ClientCommPort = client_comm;
    req->ServerCommPort = (ALPC_PORT *)0;
    req->RequesterTask  = task_current();
    req->ReplyStatus    = STATUS_PORT_CONNECTION_REFUSED;  /* safe default */
    event_init(&req->ReplyEvent, "alpc_conn_reply", EVENT_AUTO_RESET, 0);

    /* 5. Publish to server + wake it. */
    spin_lock_irqsave(&server_conn->Lock, &irqf);
    if (server_conn->Disconnected) {
        spin_unlock_irqrestore(&server_conn->Lock, irqf);
        kfree(req);
        ObpFreeHandle(ht, h);
        ObDereferenceObject(server_conn);
        ObDereferenceObject(client_comm);
        ObDereferenceObject(server_body);
        return STATUS_PORT_DISCONNECTED;
    }
    conn_queue_enqueue(&server_conn->ConnectionQueue, req);
    spin_unlock_irqrestore(&server_conn->Lock, irqf);
    event_set(&server_conn->WaitQueue);

    klog(LOG_DEBUG, "alpc", "connect: server=%s timeout=%u",
         port_name, (uint64_t)timeout_ms);

    /* 6. Wait for server's accept/reject. */
    if (timeout_ms == 0) {
        event_wait(&req->ReplyEvent);
        waited_ok = 1;
    } else {
        waited_ok = event_wait_timeout(&req->ReplyEvent, timeout_ms);
    }

    if (!waited_ok) {
        /* Timeout. The request may still be sitting on the server's queue
         * with a dangling pointer to `req`; we must remove it before
         * freeing, otherwise the server will dereference freed memory. */
        st = STATUS_TIMEOUT;
        spin_lock_irqsave(&server_conn->Lock, &irqf);
        ALPC_CONNECTION_REQUEST **pp =
            (ALPC_CONNECTION_REQUEST **)&server_conn->ConnectionQueue.Head;
        ALPC_CONNECTION_REQUEST *prev = (ALPC_CONNECTION_REQUEST *)0;
        int found = 0;
        while (*pp) {
            if (*pp == req) {
                *pp = req->Link_next;
                if (server_conn->ConnectionQueue.Tail == (PORT_MESSAGE_ENTRY *)req)
                    server_conn->ConnectionQueue.Tail = (PORT_MESSAGE_ENTRY *)prev;
                server_conn->ConnectionQueue.Count--;
                found = 1;
                break;
            }
            prev = *pp;
            pp = &(*pp)->Link_next;
        }
        spin_unlock_irqrestore(&server_conn->Lock, irqf);
        if (!found) {
            /* Server already dequeued and is racing to set ReplyEvent.
             * Do one more blocking wait so we don't free `req` while the
             * server still holds a pointer to it. */
            event_wait(&req->ReplyEvent);
            st = req->ReplyStatus;
            /* Respect the reply we just got, even though we already
             * returned STATUS_TIMEOUT intent -- actually adopt the real
             * result to keep the handle/ref accounting honest. */
        }
    } else {
        st = req->ReplyStatus;
    }

    kfree(req);

    /* Only STATUS_SUCCESS means the server accepted and cross-linked
     * us; STATUS_TIMEOUT / STATUS_PORT_DISCONNECTED /
     * STATUS_PORT_CONNECTION_REFUSED all mean the handshake failed
     * even though their severity bit is zero. NT_SUCCESS is too loose
     * here. */
    if (st == STATUS_SUCCESS) {
        *out_handle = h;
        /* Drop the creation ref: handle + cross-link from server_comm
         * are the remaining references that keep client_comm alive.
         * This mirrors AlpcCreatePort's final ObDereferenceObject and
         * stops every successful connect from leaking one port body. */
        ObDereferenceObject(client_comm);
    } else {
        /* Rollback client-side state on reject / disconnect / timeout. */
        ObpFreeHandle(ht, h);
        /* ConnectionPort ref + creation ref both drop here: handle close
         * fired the handle_count->0 path in ObpFreeHandle, but the
         * creation ref (ref_count +1 from ob_alloc_object) is still
         * held. Drop it and the ConnectionPort ref we added. */
        spin_lock_irqsave(&client_comm->Lock, &irqf);
        ALPC_PORT *cp = client_comm->ConnectionPort;
        client_comm->ConnectionPort = (ALPC_PORT *)0;
        spin_unlock_irqrestore(&client_comm->Lock, irqf);
        if (cp)
            ObDereferenceObject(cp);                   /* drop ConnectionPort ref */
        ObDereferenceObject(client_comm);             /* drop creation ref */
    }

    ObDereferenceObject(server_body);                 /* drop lookup ref */
    return st;
}

/* ---- AlpcAcceptConnectPort -------------------------------------------- */

NTSTATUS AlpcAcceptConnectPort(HANDLE_TABLE *ht,
                               HANDLE conn_port_handle, int accept,
                               uint32_t timeout_ms,
                               HANDLE *out_server_comm_handle)
{
    HANDLE_TABLE_ENTRY *entry;
    ALPC_PORT *server_conn;
    ALPC_CONNECTION_REQUEST *req;
    ALPC_PORT *server_comm = (ALPC_PORT *)0;
    ALPC_PORT *client_comm;
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t irqf;
    int waited_ok;

    if (out_server_comm_handle)
        *out_server_comm_handle = INVALID_HANDLE_VALUE;
    if (!ht)
        return STATUS_INVALID_PARAMETER;

    /* Lookup the server connection port by handle. */
    entry = ObpLookupHandle(ht, conn_port_handle);
    if (!entry || !entry->object)
        return STATUS_INVALID_HANDLE;
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpAlpcPortType)
        return STATUS_OBJECT_TYPE_MISMATCH;
    server_conn = (ALPC_PORT *)entry->object;
    if (server_conn->PortType != AlpcServerConnectionPort)
        return STATUS_INVALID_PORT_HANDLE;
    ObReferenceObject(server_conn);                   /* pin across wait + accept */

    /* Dequeue a request (or wait). */
    for (;;) {
        spin_lock_irqsave(&server_conn->Lock, &irqf);
        req = conn_queue_dequeue(&server_conn->ConnectionQueue);
        spin_unlock_irqrestore(&server_conn->Lock, irqf);
        if (req)
            break;
        /* Queue empty. Wait for producer to signal. */
        if (timeout_ms == 0) {
            event_wait(&server_conn->WaitQueue);
            waited_ok = 1;
        } else {
            waited_ok = event_wait_timeout(&server_conn->WaitQueue,
                                           timeout_ms);
        }
        if (!waited_ok) {
            ObDereferenceObject(server_conn);
            return STATUS_TIMEOUT;
        }
        /* Re-check the queue after wake -- a concurrent accept on another
         * thread may have stolen the request. */
    }

    client_comm = req->ClientCommPort;

    if (!accept) {
        /* Reject path: the server's syscall succeeds; the client's
         * blocked AlpcConnectPort sees STATUS_PORT_CONNECTION_REFUSED. */
        req->ServerCommPort = (ALPC_PORT *)0;
        req->ReplyStatus    = STATUS_PORT_CONNECTION_REFUSED;
        event_set(&req->ReplyEvent);
        /* req is freed by the client. */
        ObDereferenceObject(server_conn);
        klog(LOG_DEBUG, "alpc", "accept: rejected");
        return STATUS_SUCCESS;
    }

    /* Accept path. Inherit attributes from the connection port so the
     * server-side comm port enforces the same caps as the listener. */
    server_comm = alpc_alloc_comm_port(AlpcServerCommunicationPort,
                                       &server_conn->Attributes);
    if (!server_comm) {
        req->ReplyStatus = STATUS_INSUFFICIENT_RESOURCES;
        event_set(&req->ReplyEvent);
        ObDereferenceObject(server_conn);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Cross-link: server_comm <-> client_comm (each takes +1 ref on peer
     * so cross-link survives until either side disconnects). */
    ObReferenceObject(client_comm);
    server_comm->ConnectedPort = client_comm;
    ObReferenceObject(server_conn);
    server_comm->ConnectionPort = server_conn;        /* pin listen port */

    ObReferenceObject(server_comm);
    client_comm->ConnectedPort = server_comm;

    /* Allocate server-side handle. */
    h = ObpAllocateHandle(ht, server_comm, ALPC_PORT_ALL_ACCESS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        /* Undo cross-link. */
        ObDereferenceObject(server_comm);             /* client's ref */
        client_comm->ConnectedPort = (ALPC_PORT *)0;
        ObDereferenceObject(client_comm);             /* server_comm's ref */
        server_comm->ConnectedPort = (ALPC_PORT *)0;
        ObDereferenceObject(server_conn);             /* server_comm's ConnectionPort ref */
        server_comm->ConnectionPort = (ALPC_PORT *)0;
        ObDereferenceObject(server_comm);             /* creation ref */
        req->ReplyStatus = STATUS_INSUFFICIENT_RESOURCES;
        event_set(&req->ReplyEvent);
        ObDereferenceObject(server_conn);             /* server_conn pin */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Snapshot any field we want to log BEFORE signalling the client:
     * once event_set fires, the client thread is free to return from
     * event_wait and kfree(req), so req pointers cease to be valid on
     * this CPU immediately after the signal. */
    uint32_t requester_pid =
        req->RequesterTask ? req->RequesterTask->pid : 0u;

    req->ServerCommPort = server_comm;
    req->ReplyStatus    = STATUS_SUCCESS;
    event_set(&req->ReplyEvent);                      /* client may now free req */
    req = (ALPC_CONNECTION_REQUEST *)0;               /* poison: do not dereference */

    ObDereferenceObject(server_comm);                 /* drop creation ref */
    ObDereferenceObject(server_conn);                 /* drop pin */

    klog(LOG_DEBUG, "alpc", "accept: client-pid=%u",
         (uint64_t)requester_pid);

    if (out_server_comm_handle)
        *out_server_comm_handle = h;
    return STATUS_SUCCESS;
}

/* ---- AlpcDisconnectPort ----------------------------------------------- */

NTSTATUS AlpcDisconnectPort(HANDLE_TABLE *ht, HANDLE port_handle)
{
    HANDLE_TABLE_ENTRY *entry;
    ALPC_PORT *port, *peer;
    PORT_MESSAGE_ENTRY *close_msg;
    ALPC_CONNECTION_REQUEST *pending_head = (ALPC_CONNECTION_REQUEST *)0;
    uint64_t irqf;

    if (!ht)
        return STATUS_INVALID_PARAMETER;

    entry = ObpLookupHandle(ht, port_handle);
    if (!entry || !entry->object)
        return STATUS_INVALID_HANDLE;
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpAlpcPortType)
        return STATUS_INVALID_PORT_HANDLE;
    port = (ALPC_PORT *)entry->object;

    /* Mark this side disconnected + snapshot peer under the port lock.
     * Server connection ports additionally drain ConnectionQueue so any
     * client blocked in AlpcConnectPort unblocks with
     * STATUS_PORT_DISCONNECTED instead of waiting for its per-request
     * timeout (or forever on timeout_ms=0). We pin the peer with an
     * extra ref so it cannot be freed between dropping our lock and
     * queuing the PORT_CLOSED notification. */
    spin_lock_irqsave(&port->Lock, &irqf);
    port->Disconnected = 1;
    peer = port->ConnectedPort;
    if (peer)
        ObReferenceObject(peer);
    port->ConnectedPort = (ALPC_PORT *)0;
    if (port->PortType == AlpcServerConnectionPort) {
        /* Detach the whole ConnectionQueue list head; signal outside the lock. */
        pending_head = (ALPC_CONNECTION_REQUEST *)port->ConnectionQueue.Head;
        port->ConnectionQueue.Head  = (PORT_MESSAGE_ENTRY *)0;
        port->ConnectionQueue.Tail  = (PORT_MESSAGE_ENTRY *)0;
        port->ConnectionQueue.Count = 0;
    }
    /* Drain MY PendingQueue: any sync waiter that I sent a request from
     * gets STATUS_PORT_DISCONNECTED. Signal under the lock so the
     * sender (which acquires the same lock on wake) cannot free the
     * record before our event_set is observed. */
    {
        ALPC_PENDING_REPLY *pr = port->PendingQueue.Head;
        while (pr) {
            ALPC_PENDING_REPLY *next = pr->Link_next;
            pr->Link_next   = (ALPC_PENDING_REPLY *)0;
            pr->ReplyType   = ALPC_MSG_TYPE_PORT_CLOSED;
            pr->ReplyStatus = STATUS_PORT_DISCONNECTED;
            pr->ReplyBodyLen = 0;
            pr->Completed   = 1;
            event_set(&pr->ReplyWait);
            pr = next;
        }
        pending_queue_init(&port->PendingQueue);
    }
    spin_unlock_irqrestore(&port->Lock, irqf);

    /* Wake every pending connect caller with PORT_DISCONNECTED. Each
     * request's lifetime is owned by its client -- we only set status
     * and event_set; the client frees the node after event_wait returns. */
    while (pending_head) {
        ALPC_CONNECTION_REQUEST *next = pending_head->Link_next;
        pending_head->ServerCommPort = (ALPC_PORT *)0;
        pending_head->ReplyStatus    = STATUS_PORT_DISCONNECTED;
        event_set(&pending_head->ReplyEvent);
        pending_head = next;
    }

    if (!peer) {
        klog(LOG_DEBUG, "alpc", "disconnect: port=%u (no peer)",
             (uint64_t)port->PortType);
        return STATUS_SUCCESS;
    }

    /* Allocate a PORT_CLOSED marker (zero-body message). On allocation
     * failure the peer still gets its Disconnected side cleared; we log
     * and drop the extra ref. */
    close_msg = (PORT_MESSAGE_ENTRY *)kmalloc(sizeof(PORT_MESSAGE_ENTRY));
    if (close_msg) {
        /* Zero-fill so PORT_MESSAGE header bytes and ChargedSize start
         * at 0. ChargedSize=0 is the documented "uncharged" sentinel:
         * AlpcFreeMessage will skip the PoolUsageBytes decrement when
         * the receiver eventually dequeues this marker, so the marker
         * cannot corrupt quota that belongs to other entries. */
        uint8_t *zp = (uint8_t *)close_msg;
        for (uint32_t i = 0; i < sizeof(*close_msg); i++)
            zp[i] = 0;
        close_msg->Header.TotalLength = sizeof(PORT_MESSAGE);
        close_msg->Header.DataLength  = 0;
        close_msg->Header.Type        = ALPC_MSG_TYPE_PORT_CLOSED;

        spin_lock_irqsave(&peer->Lock, &irqf);
        /* Route through the shared enqueue helper so §5.3's SignalledEvent
         * tracks MessageQueue.Count transitions across every producer,
         * including disconnect's PORT_CLOSED marker. A waitable peer
         * must wake on teardown, not only on normal sends. */
        msg_queue_enqueue_locked(peer, close_msg);
        peer->Disconnected = 1;
        /* Drain peer's PendingQueue too: peer may have sync requests
         * waiting for replies from US, which will never arrive. Signal
         * under peer->Lock so peer's senders can safely free on wake. */
        {
            ALPC_PENDING_REPLY *pr = peer->PendingQueue.Head;
            while (pr) {
                ALPC_PENDING_REPLY *next = pr->Link_next;
                pr->Link_next   = (ALPC_PENDING_REPLY *)0;
                pr->ReplyType   = ALPC_MSG_TYPE_PORT_CLOSED;
                pr->ReplyStatus = STATUS_PORT_DISCONNECTED;
                pr->ReplyBodyLen = 0;
                pr->Completed   = 1;
                event_set(&pr->ReplyWait);
                pr = next;
            }
            pending_queue_init(&peer->PendingQueue);
        }
        spin_unlock_irqrestore(&peer->Lock, irqf);

        event_set(&peer->WaitQueue);
    } else {
        /* Couldn't allocate the marker. Still mark peer Disconnected and
         * drain peer PendingQueue so any subsequent send fails fast and
         * any blocked sync waiter wakes. */
        spin_lock_irqsave(&peer->Lock, &irqf);
        peer->Disconnected = 1;
        {
            ALPC_PENDING_REPLY *pr = peer->PendingQueue.Head;
            while (pr) {
                ALPC_PENDING_REPLY *next = pr->Link_next;
                pr->Link_next   = (ALPC_PENDING_REPLY *)0;
                pr->ReplyType   = ALPC_MSG_TYPE_PORT_CLOSED;
                pr->ReplyStatus = STATUS_PORT_DISCONNECTED;
                pr->ReplyBodyLen = 0;
                pr->Completed   = 1;
                event_set(&pr->ReplyWait);
                pr = next;
            }
            pending_queue_init(&peer->PendingQueue);
        }
        spin_unlock_irqrestore(&peer->Lock, irqf);
        event_set(&peer->WaitQueue);
        klog(LOG_WARN, "alpc",
             "disconnect: PORT_CLOSED alloc failed; peer still marked");
    }

    /* Drop the pin AND the cross-link reference the peer held on us.
     * The cross-link on the PEER side still points at our port; the
     * port object itself will survive until that side also disconnects
     * or closes. Our ConnectedPort cleared above drops our reference to
     * the peer. */
    ObDereferenceObject(peer);                        /* drop pin */
    ObDereferenceObject(peer);                        /* drop our ConnectedPort ref */

    klog(LOG_DEBUG, "alpc", "disconnect: port=%u",
         (uint64_t)port->PortType);
    return STATUS_SUCCESS;
}

/* =========================================================================
 * §4 Synchronous Send+Wait+Receive Engine
 *
 * Ownership model (resolves the v1/v2 design-review criticals):
 *   - Inbound message entry (PORT_MESSAGE_ENTRY): queue-owned. Lives on
 *     a single ALPC_MSG_QUEUE (MessageQueue). Receiver dequeues + frees
 *     via AlpcFreeMessage. on_delete drains and frees defensively.
 *   - Pending sync record (ALPC_PENDING_REPLY): sender-owned. Lives on
 *     one ALPC_PENDING_QUEUE (sender_port->PendingQueue). The sender is
 *     the SOLE freer. Replier and disconnect may signal it but only
 *     after removing it from the queue (so no other path reaches it),
 *     and only WHILE STILL HOLDING the port lock the sender will
 *     re-acquire on wake (so the wake observation is serialized).
 *
 * Lock order for cross-port enqueue (sync request only): lower port
 * address first, then higher. All other paths (reply, receive, timeout,
 * disconnect drain) hold at most one port lock at a time.
 *
 * Pool accounting: AlpcAllocateMessage and the equivalent helper for
 * pending records charge under the owning port's Lock, then kmalloc
 * outside the lock. ChargedSize is stored in the entry/record so the
 * free path uncharges the SAME amount regardless of whether
 * Header.DataLength has been mutated. AlpcFreeMessage uncharges; on
 * port destruction the queue_drain in on_delete simply kfree's without
 * uncharging (the port is going away).
 * ======================================================================= */

/* ---- AlpcAllocateMessage / AlpcFreeMessage ---------------------------- */

PORT_MESSAGE_ENTRY *AlpcAllocateMessage(ALPC_PORT *charge_port,
                                        uint32_t data_length)
{
    uint32_t alloc_size;
    uint64_t cap;
    uint64_t irqf;
    PORT_MESSAGE_ENTRY *entry;

    if (!charge_port)
        return (PORT_MESSAGE_ENTRY *)0;
    if (data_length > ALPC_MAX_ALLOWED_MESSAGE_LENGTH)
        return (PORT_MESSAGE_ENTRY *)0;

    alloc_size = (uint32_t)sizeof(PORT_MESSAGE_ENTRY) + data_length;

    /* Reserve quota under the lock. Drop the lock before kmalloc so
     * heap allocation never runs under a spinlock. */
    spin_lock_irqsave(&charge_port->Lock, &irqf);
    if (charge_port->Disconnected) {
        spin_unlock_irqrestore(&charge_port->Lock, irqf);
        return (PORT_MESSAGE_ENTRY *)0;
    }
    cap = charge_port->Attributes.MaxPoolUsage;
    if (cap > 0 && (charge_port->PoolUsageBytes + alloc_size) > cap) {
        spin_unlock_irqrestore(&charge_port->Lock, irqf);
        return (PORT_MESSAGE_ENTRY *)0;
    }
    charge_port->PoolUsageBytes += alloc_size;
    spin_unlock_irqrestore(&charge_port->Lock, irqf);

    entry = (PORT_MESSAGE_ENTRY *)kmalloc(alloc_size);
    if (!entry) {
        spin_lock_irqsave(&charge_port->Lock, &irqf);
        if (charge_port->PoolUsageBytes >= alloc_size)
            charge_port->PoolUsageBytes -= alloc_size;
        spin_unlock_irqrestore(&charge_port->Lock, irqf);
        return (PORT_MESSAGE_ENTRY *)0;
    }

    /* Zero header + bookkeeping. Body bytes are caller-filled. */
    {
        uint8_t *zp = (uint8_t *)entry;
        for (uint32_t i = 0; i < sizeof(PORT_MESSAGE_ENTRY); i++)
            zp[i] = 0;
    }
    entry->ChargedSize = alloc_size;
    return entry;
}

void AlpcFreeMessage(ALPC_PORT *charge_port, PORT_MESSAGE_ENTRY *entry)
{
    uint64_t irqf;

    if (!entry)
        return;
    /* ChargedSize == 0 means the entry was allocated outside the pool
     * accounting path (e.g. the PORT_CLOSED marker AlpcDisconnectPort
     * queues with raw kmalloc when the peer is being torn down). Skip
     * the decrement in that case so we don't undercount quota that
     * belongs to other entries. */
    if (charge_port && entry->ChargedSize > 0) {
        uint32_t size = entry->ChargedSize;
        spin_lock_irqsave(&charge_port->Lock, &irqf);
        if (charge_port->PoolUsageBytes >= size)
            charge_port->PoolUsageBytes -= size;
        spin_unlock_irqrestore(&charge_port->Lock, irqf);
    }
    kfree(entry);
}

/* ---- Pending-reply allocator (file-local) ----------------------------- */

static ALPC_PENDING_REPLY *alpc_alloc_pending_reply(ALPC_PORT *sender_port,
                                                    uint16_t reply_body_cap)
{
    uint32_t alloc_size;
    uint64_t cap;
    uint64_t irqf;
    ALPC_PENDING_REPLY *r;

    if (!sender_port)
        return (ALPC_PENDING_REPLY *)0;
    if (reply_body_cap > ALPC_MAX_ALLOWED_MESSAGE_LENGTH)
        return (ALPC_PENDING_REPLY *)0;

    alloc_size = (uint32_t)sizeof(ALPC_PENDING_REPLY) + reply_body_cap;

    spin_lock_irqsave(&sender_port->Lock, &irqf);
    if (sender_port->Disconnected) {
        spin_unlock_irqrestore(&sender_port->Lock, irqf);
        return (ALPC_PENDING_REPLY *)0;
    }
    cap = sender_port->Attributes.MaxPoolUsage;
    if (cap > 0 && (sender_port->PoolUsageBytes + alloc_size) > cap) {
        spin_unlock_irqrestore(&sender_port->Lock, irqf);
        return (ALPC_PENDING_REPLY *)0;
    }
    sender_port->PoolUsageBytes += alloc_size;
    spin_unlock_irqrestore(&sender_port->Lock, irqf);

    r = (ALPC_PENDING_REPLY *)kmalloc(alloc_size);
    if (!r) {
        spin_lock_irqsave(&sender_port->Lock, &irqf);
        if (sender_port->PoolUsageBytes >= alloc_size)
            sender_port->PoolUsageBytes -= alloc_size;
        spin_unlock_irqrestore(&sender_port->Lock, irqf);
        return (ALPC_PENDING_REPLY *)0;
    }

    {
        uint8_t *zp = (uint8_t *)r;
        for (uint32_t i = 0; i < sizeof(ALPC_PENDING_REPLY); i++)
            zp[i] = 0;
    }
    r->ChargedSize  = alloc_size;
    r->ReplyBodyCap = reply_body_cap;
    r->ReplyBody    = (uint8_t *)r + sizeof(ALPC_PENDING_REPLY);
    event_init(&r->ReplyWait, "alpc_reply_wait", EVENT_AUTO_RESET, 0);
    return r;
}

static void alpc_free_pending_reply(ALPC_PORT *sender_port,
                                    ALPC_PENDING_REPLY *r)
{
    uint32_t size;
    uint64_t irqf;

    if (!r)
        return;
    size = r->ChargedSize ? r->ChargedSize
                          : (uint32_t)sizeof(ALPC_PENDING_REPLY);
    if (sender_port) {
        spin_lock_irqsave(&sender_port->Lock, &irqf);
        if (sender_port->PoolUsageBytes >= size)
            sender_port->PoolUsageBytes -= size;
        spin_unlock_irqrestore(&sender_port->Lock, irqf);
    }
    kfree(r);
}

/* ---- Engine helpers --------------------------------------------------- */

/* Effective per-port message length cap. MaxMessageLength == 0 means
 * "use the hard ceiling"; non-zero values cap below the ceiling. */
static uint32_t alpc_effective_max_message(const ALPC_PORT *p)
{
    uint64_t v = p->Attributes.MaxMessageLength;
    if (v == 0 || v > ALPC_MAX_ALLOWED_MESSAGE_LENGTH)
        return ALPC_MAX_ALLOWED_MESSAGE_LENGTH;
    return (uint32_t)v;
}

/* Append `entry` to `port->MessageQueue` under `port->Lock`. Caller
 * holds the lock.
 *
 * Side effect for §5.3: on the 0 -> 1 transition, set port->SignalledEvent
 * (manual-reset) iff the port is waitable. The event is strictly tied
 * to MessageQueue.Count > 0; edge transitions happen under the same
 * lock that guards Count so the event state can never disagree with the
 * queue state a waiter would observe. */
static void msg_queue_enqueue_locked(ALPC_PORT *port, PORT_MESSAGE_ENTRY *entry)
{
    entry->Link_next = (PORT_MESSAGE_ENTRY *)0;
    uint32_t was = port->MessageQueue.Count;
    if (!port->MessageQueue.Head) {
        port->MessageQueue.Head = entry;
        port->MessageQueue.Tail = entry;
    } else {
        port->MessageQueue.Tail->Link_next = entry;
        port->MessageQueue.Tail = entry;
    }
    port->MessageQueue.Count++;
    if (port->IsWaitable && was == 0)
        event_set(&port->SignalledEvent);
}

/* Dequeue front of `port->MessageQueue` under `port->Lock`. Returns
 * NULL if empty. Caller holds the lock.
 *
 * Side effect for §5.3: on the 1 -> 0 transition, clear
 * port->SignalledEvent so subsequent NtWaitForSingleObject blocks again. */
static PORT_MESSAGE_ENTRY *msg_queue_dequeue_locked(ALPC_PORT *port)
{
    PORT_MESSAGE_ENTRY *e = port->MessageQueue.Head;
    if (!e)
        return (PORT_MESSAGE_ENTRY *)0;
    port->MessageQueue.Head = e->Link_next;
    if (!port->MessageQueue.Head)
        port->MessageQueue.Tail = (PORT_MESSAGE_ENTRY *)0;
    port->MessageQueue.Count--;
    e->Link_next = (PORT_MESSAGE_ENTRY *)0;
    if (port->IsWaitable && port->MessageQueue.Count == 0)
        event_reset(&port->SignalledEvent);
    return e;
}

/* Resolve a HANDLE to an ALPC_PORT and pin it with a reference. The
 * caller drops via ObDereferenceObject. Returns NTSTATUS. */
static NTSTATUS resolve_alpc_port(HANDLE_TABLE *ht, HANDLE h,
                                  ALPC_PORT **out)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    ALPC_PORT *p;

    *out = (ALPC_PORT *)0;
    if (!ht)
        return STATUS_INVALID_PARAMETER;
    entry = ObpLookupHandle(ht, h);
    if (!entry || !entry->object)
        return STATUS_INVALID_HANDLE;
    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpAlpcPortType)
        return STATUS_INVALID_PORT_HANDLE;
    p = (ALPC_PORT *)entry->object;
    ObReferenceObject(p);
    *out = p;
    return STATUS_SUCCESS;
}

/* Address-ordered double lock: take both port spinlocks in a stable
 * global order so concurrent send/disconnect on opposite endpoints
 * cannot deadlock. Save IRQ flags from the FIRST acquired lock. */
static void alpc_lock_two(ALPC_PORT *a, ALPC_PORT *b,
                          uint64_t *irqf_a, uint64_t *irqf_b,
                          ALPC_PORT **out_first, ALPC_PORT **out_second)
{
    if ((uintptr_t)a < (uintptr_t)b) {
        *out_first = a; *out_second = b;
        spin_lock_irqsave(&a->Lock, irqf_a);
        spin_lock_irqsave(&b->Lock, irqf_b);
    } else {
        *out_first = b; *out_second = a;
        spin_lock_irqsave(&b->Lock, irqf_a);
        spin_lock_irqsave(&a->Lock, irqf_b);
    }
}

static void alpc_unlock_two(ALPC_PORT *first, ALPC_PORT *second,
                            uint64_t irqf_a, uint64_t irqf_b)
{
    spin_unlock_irqrestore(&second->Lock, irqf_b);
    spin_unlock_irqrestore(&first->Lock, irqf_a);
}

/* Fill the standard ClientId from the current task. */
static void alpc_fill_client_id(CLIENT_ID *cid)
{
    struct task *t = task_current();
    cid->UniqueProcess = t ? (uint64_t)t->pid : 0u;
    cid->UniqueThread  = 0;  /* §4 reserves thread id; full value lands
                              * with §7 token capture / TEB integration */
}

/* ---- Receive-only path ------------------------------------------------ */

static NTSTATUS alpc_receive_only(ALPC_PORT *port, PORT_MESSAGE *recv_msg,
                                  uint32_t recv_buf_len, uint32_t timeout_ms)
{
    PORT_MESSAGE_ENTRY *entry;
    uint64_t irqf;
    int waited_ok;
    uint32_t copy_len;

    if (!recv_msg || recv_buf_len < sizeof(PORT_MESSAGE))
        return STATUS_BUFFER_TOO_SMALL;

    for (;;) {
        spin_lock_irqsave(&port->Lock, &irqf);
        if (port->Disconnected && port->MessageQueue.Count == 0) {
            spin_unlock_irqrestore(&port->Lock, irqf);
            return STATUS_PORT_DISCONNECTED;
        }
        entry = msg_queue_dequeue_locked(port);
        spin_unlock_irqrestore(&port->Lock, irqf);
        if (entry)
            break;
        if (timeout_ms == 0)
            return STATUS_TIMEOUT;
        waited_ok = event_wait_timeout(&port->WaitQueue, timeout_ms);
        if (!waited_ok)
            return STATUS_TIMEOUT;
        /* Re-check disconnect + queue under lock on next iteration. */
    }

    /* Copy header. Body bounded by the smaller of TotalLength and
     * the caller's recv_buf_len; truncation is normal at this layer. */
    memcpy(recv_msg, &entry->Header, sizeof(PORT_MESSAGE));
    copy_len = entry->Header.DataLength;
    {
        uint32_t avail = recv_buf_len - (uint32_t)sizeof(PORT_MESSAGE);
        if (copy_len > avail)
            copy_len = avail;
    }
    if (copy_len > 0)
        memcpy((uint8_t *)recv_msg + sizeof(PORT_MESSAGE),
                    ALPC_MSG_BODY(entry), copy_len);

    AlpcFreeMessage(port, entry);
    return STATUS_SUCCESS;
}

/* ---- Datagram send path ---------------------------------------------- */

static NTSTATUS alpc_datagram_send(ALPC_PORT *sender_port,
                                   PORT_MESSAGE *send_msg)
{
    ALPC_PORT *peer;
    PORT_MESSAGE_ENTRY *entry;
    uint32_t data_len;
    uint64_t irqf;
    uint64_t local_msg_id;
    int local_disc, peer_disc;

    if (!send_msg)
        return STATUS_INVALID_PARAMETER;
    data_len = send_msg->DataLength;
    if (data_len > alpc_effective_max_message(sender_port))
        return STATUS_BUFFER_TOO_SMALL;

    spin_lock_irqsave(&sender_port->Lock, &irqf);
    local_disc = sender_port->Disconnected;
    peer = sender_port->ConnectedPort;
    if (peer)
        ObReferenceObject(peer);
    spin_unlock_irqrestore(&sender_port->Lock, irqf);
    if (local_disc) {
        if (peer)
            ObDereferenceObject(peer);
        return STATUS_PORT_DISCONNECTED;
    }
    if (!peer)
        return STATUS_PORT_DISCONNECTED;

    /* Pre-check peer Disconnected so a tear-down maps to PORT_DISCONNECTED
     * rather than the INSUFFICIENT_RESOURCES that AlpcAllocateMessage would
     * surface (it also rejects when the charge port is Disconnected). */
    spin_lock_irqsave(&peer->Lock, &irqf);
    peer_disc = peer->Disconnected;
    spin_unlock_irqrestore(&peer->Lock, irqf);
    if (peer_disc) {
        ObDereferenceObject(peer);
        return STATUS_PORT_DISCONNECTED;
    }

    entry = AlpcAllocateMessage(peer, data_len);
    if (!entry) {
        ObDereferenceObject(peer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Snapshot the MessageId locally BEFORE enqueue so the post-signal
     * klog never touches `entry` (the receiver may dequeue+free it as
     * soon as event_set fires). */
    local_msg_id = __atomic_fetch_add(&sender_port->NextMessageId, 1u,
                                      __ATOMIC_RELAXED);
    entry->Header.TotalLength    = (uint16_t)(sizeof(PORT_MESSAGE) + data_len);
    entry->Header.DataLength     = (uint16_t)data_len;
    entry->Header.Type           = ALPC_MSG_TYPE_DATAGRAM;
    entry->Header.DataInfoOffset = 0;
    entry->Header.MessageId      = local_msg_id;
    entry->Header.CallbackId     = 0;
    alpc_fill_client_id(&entry->Header.ClientId);

    if (data_len > 0)
        memcpy(ALPC_MSG_BODY(entry),
                    (const uint8_t *)send_msg + sizeof(PORT_MESSAGE),
                    data_len);

    spin_lock_irqsave(&peer->Lock, &irqf);
    if (peer->Disconnected) {
        spin_unlock_irqrestore(&peer->Lock, irqf);
        AlpcFreeMessage(peer, entry);
        ObDereferenceObject(peer);
        return STATUS_PORT_DISCONNECTED;
    }
    msg_queue_enqueue_locked(peer, entry);
    spin_unlock_irqrestore(&peer->Lock, irqf);
    /* `entry` may be freed by the receiver any time after this point. */
    entry = (PORT_MESSAGE_ENTRY *)0;

    event_set(&peer->WaitQueue);
    /* §5.2 async wake: if the peer associated an IOCP, push a notification
     * packet in addition to the MessageQueue enqueue. Must run AFTER the
     * regular event_set so a receiver that observed WaitQueue still sees
     * the matching IOCP entry. */
    alpc_notify_completion_port(peer, local_msg_id, data_len);

    klog(LOG_DEBUG, "alpc", "msg sent: id=%u type=DATAGRAM len=%u",
         local_msg_id, (uint64_t)data_len);

    ObDereferenceObject(peer);
    return STATUS_SUCCESS;
}

/* ---- Sync request+wait path ----------------------------------------- */

static NTSTATUS alpc_sync_request(ALPC_PORT *sender_port,
                                  PORT_MESSAGE *send_msg,
                                  PORT_MESSAGE *recv_msg,
                                  uint32_t recv_buf_len,
                                  uint32_t timeout_ms)
{
    ALPC_PORT *peer;
    PORT_MESSAGE_ENTRY *entry;
    ALPC_PENDING_REPLY *pending;
    uint32_t data_len;
    uint16_t reply_cap;
    uint64_t message_id;
    uint64_t irqf, irqf2;
    ALPC_PORT *first, *second;
    int waited_ok;
    NTSTATUS final_status;

    if (!send_msg || !recv_msg || recv_buf_len < sizeof(PORT_MESSAGE))
        return STATUS_BUFFER_TOO_SMALL;

    data_len = send_msg->DataLength;
    if (data_len > alpc_effective_max_message(sender_port))
        return STATUS_BUFFER_TOO_SMALL;

    /* Reply capacity = the user's RecvMsg payload region after the
     * header. Capped at the inline-message ceiling so the pending
     * record's inline body never exceeds the protocol max. */
    {
        uint32_t cap = recv_buf_len - (uint32_t)sizeof(PORT_MESSAGE);
        if (cap > ALPC_MAX_ALLOWED_MESSAGE_LENGTH)
            cap = ALPC_MAX_ALLOWED_MESSAGE_LENGTH;
        reply_cap = (uint16_t)cap;
    }

    int local_disc, peer_disc;
    spin_lock_irqsave(&sender_port->Lock, &irqf);
    local_disc = sender_port->Disconnected;
    peer = sender_port->ConnectedPort;
    if (peer)
        ObReferenceObject(peer);
    spin_unlock_irqrestore(&sender_port->Lock, irqf);
    if (local_disc) {
        if (peer)
            ObDereferenceObject(peer);
        return STATUS_PORT_DISCONNECTED;
    }
    if (!peer)
        return STATUS_PORT_DISCONNECTED;

    /* Pre-check peer Disconnected so a remote teardown maps to
     * PORT_DISCONNECTED rather than INSUFFICIENT_RESOURCES. */
    spin_lock_irqsave(&peer->Lock, &irqf);
    peer_disc = peer->Disconnected;
    spin_unlock_irqrestore(&peer->Lock, irqf);
    if (peer_disc) {
        ObDereferenceObject(peer);
        return STATUS_PORT_DISCONNECTED;
    }

    pending = alpc_alloc_pending_reply(sender_port, reply_cap);
    if (!pending) {
        ObDereferenceObject(peer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    entry = AlpcAllocateMessage(peer, data_len);
    if (!entry) {
        alpc_free_pending_reply(sender_port, pending);
        ObDereferenceObject(peer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    message_id = __atomic_fetch_add(&sender_port->NextMessageId, 1u,
                                    __ATOMIC_RELAXED);

    entry->Header.TotalLength    = (uint16_t)(sizeof(PORT_MESSAGE) + data_len);
    entry->Header.DataLength     = (uint16_t)data_len;
    entry->Header.Type           = ALPC_MSG_TYPE_REQUEST;
    entry->Header.DataInfoOffset = 0;
    entry->Header.MessageId      = message_id;
    entry->Header.CallbackId     = 0;
    alpc_fill_client_id(&entry->Header.ClientId);
    /* The reply destination is implicit: the receiver replies via
     * its own port handle and the engine routes the reply to
     * port->ConnectedPort (which is the original requester). The
     * MessageId carries the correlation. No per-entry ReplyPort
     * field is needed. */

    if (data_len > 0)
        memcpy(ALPC_MSG_BODY(entry),
                    (const uint8_t *)send_msg + sizeof(PORT_MESSAGE),
                    data_len);

    pending->MessageId = message_id;

    /* Address-ordered double lock for atomic enqueue across BOTH ports.
     * If either side is Disconnected, abort cleanly without exposing the
     * partial state. */
    alpc_lock_two(sender_port, peer, &irqf, &irqf2, &first, &second);
    if (sender_port->Disconnected || peer->Disconnected) {
        alpc_unlock_two(first, second, irqf, irqf2);
        AlpcFreeMessage(peer, entry);
        alpc_free_pending_reply(sender_port, pending);
        ObDereferenceObject(peer);
        return STATUS_PORT_DISCONNECTED;
    }
    pending_queue_enqueue(&sender_port->PendingQueue, pending);
    msg_queue_enqueue_locked(peer, entry);
    alpc_unlock_two(first, second, irqf, irqf2);

    event_set(&peer->WaitQueue);
    /* §5.2: also notify peer's associated IOCP if any. Sync request +
     * completion port is a valid combination; the IOCP wake is purely
     * an advisory signal to the receiver that a message (request) is
     * ready. The sender still blocks on pending->ReplyWait as usual. */
    alpc_notify_completion_port(peer, message_id, data_len);

    klog(LOG_DEBUG, "alpc", "msg sent: id=%u type=REQUEST len=%u",
         (uint64_t)message_id, (uint64_t)data_len);

    /* Block on the pending record's wait event. event_wait_timeout uses
     * cooperative yield (see event.c) -- the pending record stays
     * reachable from sender_port->PendingQueue until completion or
     * timeout, and the replier/disconnect path signals under the lock. */
    if (timeout_ms == 0)
        timeout_ms = 0xFFFFFFFFu;     /* "block forever" -- huge sentinel */
    waited_ok = event_wait_timeout(&pending->ReplyWait, timeout_ms);

    /* Acquire sender_port->Lock to serialize with replier/disconnect.
     * If pending is still on the queue, we won the race over the replier
     * (timeout) and we remove + own the record. If pending has already
     * been removed (Completed=1), the replier/disconnect set the result
     * and event_set'd us. */
    spin_lock_irqsave(&sender_port->Lock, &irqf);
    if (pending->Completed) {
        spin_unlock_irqrestore(&sender_port->Lock, irqf);
        if (pending->ReplyStatus == STATUS_SUCCESS) {
            /* Build a reply header at recv_msg + copy reply body. */
            PORT_MESSAGE hdr;
            uint8_t *zp = (uint8_t *)&hdr;
            for (uint32_t i = 0; i < sizeof(hdr); i++) zp[i] = 0;
            hdr.TotalLength = (uint16_t)(sizeof(PORT_MESSAGE)
                                         + pending->ReplyBodyLen);
            hdr.DataLength  = pending->ReplyBodyLen;
            hdr.Type        = pending->ReplyType;
            hdr.MessageId   = message_id;
            memcpy(recv_msg, &hdr, sizeof(hdr));
            if (pending->ReplyBodyLen > 0)
                memcpy((uint8_t *)recv_msg + sizeof(PORT_MESSAGE),
                            pending->ReplyBody, pending->ReplyBodyLen);
            final_status = STATUS_SUCCESS;
        } else {
            final_status = pending->ReplyStatus;
        }
    } else {
        /* Timeout: remove from PendingQueue ourselves. */
        if (pending_queue_remove(&sender_port->PendingQueue, pending)) {
            final_status = waited_ok ? STATUS_TIMEOUT : STATUS_TIMEOUT;
        } else {
            /* Edge case: another path removed pending but didn't set
             * Completed before we acquired the lock. In practice this
             * is impossible (replier/disconnect set Completed under the
             * lock atomically with removal), but defend. */
            final_status = STATUS_TIMEOUT;
        }
        spin_unlock_irqrestore(&sender_port->Lock, irqf);
    }
    (void)waited_ok;

    alpc_free_pending_reply(sender_port, pending);
    ObDereferenceObject(peer);
    return final_status;
}

/* ---- Reply path ------------------------------------------------------- */

static NTSTATUS alpc_reply(ALPC_PORT *replier_port, PORT_MESSAGE *send_msg)
{
    ALPC_PORT *peer;
    ALPC_PENDING_REPLY *pending;
    uint64_t message_id;
    uint16_t copy_len;
    uint64_t irqf;

    if (!send_msg)
        return STATUS_INVALID_PARAMETER;
    message_id = send_msg->MessageId;
    if (message_id == 0)
        return STATUS_REPLY_MESSAGE_MISMATCH;

    spin_lock_irqsave(&replier_port->Lock, &irqf);
    peer = replier_port->ConnectedPort;
    if (peer)
        ObReferenceObject(peer);
    spin_unlock_irqrestore(&replier_port->Lock, irqf);
    if (!peer)
        return STATUS_PORT_DISCONNECTED;

    /* Walk PEER's PendingQueue (the original requester is the peer).
     * Hold peer->Lock for the full search + removal + signal so the
     * sender (which acquires the same lock on wake) cannot free the
     * record before our event_set is observed. */
    spin_lock_irqsave(&peer->Lock, &irqf);
    pending = pending_queue_find(&peer->PendingQueue, message_id);
    if (!pending) {
        spin_unlock_irqrestore(&peer->Lock, irqf);
        ObDereferenceObject(peer);
        return STATUS_REPLY_MESSAGE_MISMATCH;
    }
    pending_queue_remove(&peer->PendingQueue, pending);

    copy_len = send_msg->DataLength;
    if (copy_len > pending->ReplyBodyCap)
        copy_len = pending->ReplyBodyCap;
    if (copy_len > 0)
        memcpy(pending->ReplyBody,
                    (const uint8_t *)send_msg + sizeof(PORT_MESSAGE),
                    copy_len);
    pending->ReplyBodyLen = copy_len;
    pending->ReplyType    = ALPC_MSG_TYPE_REPLY;
    pending->ReplyStatus  = STATUS_SUCCESS;
    pending->Completed    = 1;
    event_set(&pending->ReplyWait);
    spin_unlock_irqrestore(&peer->Lock, irqf);

    klog(LOG_DEBUG, "alpc", "msg sent: id=%u type=REPLY len=%u",
         (uint64_t)message_id, (uint64_t)copy_len);

    ObDereferenceObject(peer);
    return STATUS_SUCCESS;
}

/* ---- §5 Async delivery: IOCP notification helper --------------------
 *
 * Called after a successful enqueue on peer->MessageQueue to wake a
 * server that associated an IOCP. The ALPC message itself STAYS on
 * MessageQueue -- this is a notification channel only, not a delivery
 * substitute. A failed post (IOCP full, stale handle) is logged and
 * ignored; the sender does not learn about it because the receiver can
 * still observe the message through normal ALPC receive.
 *
 * KeyContext  = port->CompletionKey (opaque identifier chosen at
 *               association time)
 * ApcContext  = MessageId of the ALPC message just enqueued
 * IoStatus    = STATUS_SUCCESS
 * Information = DataLength of the message body
 *
 * Caller must NOT hold any port->Lock (io_completion_post takes the
 * IOCP's own spinlock). */
static void alpc_notify_completion_port(ALPC_PORT *peer,
                                        uint64_t message_id,
                                        uint32_t data_len)
{
    HANDLE cp;
    uint64_t key;
    uint64_t irqf;

    spin_lock_irqsave(&peer->Lock, &irqf);
    cp  = peer->CompletionPortHandle;
    key = peer->CompletionKey;
    spin_unlock_irqrestore(&peer->Lock, irqf);

    if (cp == (HANDLE)0 || cp == INVALID_HANDLE_VALUE)
        return;

    NTSTATUS st = io_completion_post(cp, key, message_id,
                                     STATUS_SUCCESS,
                                     (uint64_t)data_len);
    if (!NT_SUCCESS(st)) {
        /* Best-effort wake: the message is still on MessageQueue and
         * will be delivered via the normal receive path. Notification
         * dropping IS backpressure (typically a full IOCP ring after a
         * slow receiver); per-send logging would flood the kernel log
         * once the IOCP is saturated. Count every drop, but only log
         * the FIRST failure per port -- future drops bump the counter
         * silently and stay observable via ALPC_PORT.DroppedNotifications
         * (surfaced by §12 alpcmon / ALPC_PORT_STATS). */
        uint64_t dropped;
        spin_lock_irqsave(&peer->Lock, &irqf);
        dropped = peer->DroppedNotifications++;
        spin_unlock_irqrestore(&peer->Lock, irqf);
        if (dropped == 0) {
            klog(LOG_WARN, "alpc",
                 "completion port post failed (status=0x%x) id=%u len=%u; "
                 "further drops counted silently",
                 (uint64_t)(uint32_t)st, (uint64_t)message_id,
                 (uint64_t)data_len);
        }
    }
}

/* ---- AlpcAssociateCompletionPort (§5.1 public helper) --------------- */

NTSTATUS AlpcAssociateCompletionPort(HANDLE_TABLE *ht, HANDLE port_handle,
                                     HANDLE completion_port, uint64_t key)
{
    ALPC_PORT *port;
    NTSTATUS st;
    uint64_t irqf;

    /* Validate IOCP handle before pinning the ALPC port so a bad IOCP
     * never leaves the port in a half-associated state. */
    st = io_completion_validate_handle(completion_port);
    if (!NT_SUCCESS(st))
        return st;

    st = resolve_alpc_port(ht, port_handle, &port);
    if (!NT_SUCCESS(st))
        return st;

    spin_lock_irqsave(&port->Lock, &irqf);
    port->CompletionPortHandle = completion_port;
    port->CompletionKey        = key;
    spin_unlock_irqrestore(&port->Lock, irqf);

    klog(LOG_DEBUG, "alpc",
         "port associated with IOCP: port_type=%u key=0x%lx",
         (uint64_t)port->PortType, key);

    ObDereferenceObject(port);
    return STATUS_SUCCESS;
}

/* ---- AlpcSendWaitReceivePort ----------------------------------------- */

NTSTATUS AlpcSendWaitReceivePort(HANDLE_TABLE *ht, HANDLE port_handle,
                                 uint32_t flags,
                                 PORT_MESSAGE *send_msg,
                                 PORT_MESSAGE *recv_msg,
                                 uint32_t recv_buf_len,
                                 uint32_t timeout_ms)
{
    ALPC_PORT *port;
    NTSTATUS st;

    st = resolve_alpc_port(ht, port_handle, &port);
    if (!NT_SUCCESS(st))
        return st;

    /* Reply path takes precedence over the SYNC_REQUEST flag. */
    if (send_msg && (flags & ALPC_MSGFLG_REPLY_MESSAGE)) {
        st = alpc_reply(port, send_msg);
        ObDereferenceObject(port);
        return st;
    }

    if (!send_msg) {
        st = alpc_receive_only(port, recv_msg, recv_buf_len, timeout_ms);
        ObDereferenceObject(port);
        return st;
    }

    if (flags & ALPC_MSGFLG_SYNC_REQUEST) {
        st = alpc_sync_request(port, send_msg, recv_msg,
                               recv_buf_len, timeout_ms);
        ObDereferenceObject(port);
        return st;
    }

    st = alpc_datagram_send(port, send_msg);
    ObDereferenceObject(port);
    return st;
}
