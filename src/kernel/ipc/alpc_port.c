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

const OBJECT_TYPE *ObpAlpcPortType;

static void *s_rpc_control_dir;
static uint8_t s_inited;

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
        kfree(cur);
        cur = next;
    }
    q->Head = (PORT_MESSAGE_ENTRY *)0;
    q->Tail = (PORT_MESSAGE_ENTRY *)0;
    q->Count = 0;
}

/* ---- DeleteProcedure --------------------------------------------------- */

static void alpc_port_on_delete(void *body)
{
    ALPC_PORT *p = (ALPC_PORT *)body;
    uint64_t irqf;

    /* Snapshot queues under the port's own lock, then release before
     * freeing entries -- kfree may sleep in heap debug builds; the
     * lock-hold-time rule forbids blocking calls while holding a
     * spinlock. */
    spin_lock_irqsave(&p->Lock, &irqf);
    ALPC_MSG_QUEUE msg = p->MessageQueue;
    ALPC_MSG_QUEUE pending = p->PendingQueue;
    ALPC_CONNECTION_REQUEST *conn_head =
        (ALPC_CONNECTION_REQUEST *)p->ConnectionQueue.Head;
    queue_init(&p->MessageQueue);
    queue_init(&p->PendingQueue);
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
    spin_unlock_irqrestore(&p->Lock, irqf);

    queue_drain(&msg);
    queue_drain(&pending);

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

    if (attrs)
        port->Attributes = *attrs;              /* copy by value */

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
static ALPC_PORT *alpc_alloc_comm_port(ALPC_PORT_TYPE type)
{
    ALPC_PORT *p = (ALPC_PORT *)ob_alloc_object(ObpAlpcPortType);
    if (!p)
        return (ALPC_PORT *)0;
    p->PortType      = type;
    p->Lock.flag     = 0;
    p->OwnerTask     = task_current();
    p->NextMessageId = 1;
    event_init(&p->WaitQueue, "alpc_comm_wait", EVENT_AUTO_RESET, 0);
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
     * ALPC_PORT_CONNECT pending T11 §5 (security reference monitor).
     * Concrete retrofit tracked at 11-security-reference-monitor/TODO-01 §5. */

    /* 2. Allocate client communication port. */
    client_comm = alpc_alloc_comm_port(AlpcClientCommunicationPort);
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

    /* Accept path. */
    server_comm = alpc_alloc_comm_port(AlpcServerCommunicationPort);
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
        /* Zero the message entry body to avoid passing uninitialised
         * stack bytes to consumers (event_init below overwrites its
         * slot). */
        uint8_t *zp = (uint8_t *)close_msg;
        for (uint32_t i = 0; i < sizeof(*close_msg); i++)
            zp[i] = 0;
        close_msg->Header.TotalLength = sizeof(PORT_MESSAGE);
        close_msg->Header.DataLength  = 0;
        close_msg->Header.Type        = ALPC_MSG_TYPE_PORT_CLOSED;
        event_init(&close_msg->ReplySyncWait, "alpc_closed",
                   EVENT_AUTO_RESET, 0);

        spin_lock_irqsave(&peer->Lock, &irqf);
        /* Append to peer's MessageQueue. */
        if (!peer->MessageQueue.Head) {
            peer->MessageQueue.Head = close_msg;
            peer->MessageQueue.Tail = close_msg;
        } else {
            peer->MessageQueue.Tail->Link_next = close_msg;
            peer->MessageQueue.Tail = close_msg;
        }
        peer->MessageQueue.Count++;
        peer->Disconnected = 1;
        spin_unlock_irqrestore(&peer->Lock, irqf);

        event_set(&peer->WaitQueue);
    } else {
        /* Couldn't allocate the marker. Still mark peer Disconnected so
         * any subsequent send fails fast. */
        spin_lock_irqsave(&peer->Lock, &irqf);
        peer->Disconnected = 1;
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
