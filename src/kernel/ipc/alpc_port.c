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
    ALPC_MSG_QUEUE conn = p->ConnectionQueue;
    queue_init(&p->MessageQueue);
    queue_init(&p->PendingQueue);
    queue_init(&p->ConnectionQueue);
    ALPC_PORT *peer = p->ConnectedPort;
    p->ConnectedPort = (ALPC_PORT *)0;
    struct access_token *tok = p->ClientToken;
    p->ClientToken = (struct access_token *)0;
    p->Disconnected = 1;
    spin_unlock_irqrestore(&p->Lock, irqf);

    queue_drain(&msg);
    queue_drain(&pending);
    queue_drain(&conn);

    if (peer)
        ObDereferenceObject(peer);

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
HANDLE AlpcCreatePort(HANDLE_TABLE *ht, const char *name,
                      const ALPC_PORT_ATTRIBUTES *attrs)
{
    ALPC_PORT *port;
    HANDLE h;
    void *rpc_dir = (void *)0;

    if (!ht || !ObpAlpcPortType)
        return INVALID_HANDLE_VALUE;

    /* Leaf-name length check: ObInsertObject rejects empty or > OB_NAME_MAX-1,
     * but we also want to refuse embedded backslashes here so a caller
     * cannot smuggle a path ("foo\\bar") past the namespace's component
     * contract. Kernel helpers (tests, CSRSS) pass pure leaf names;
     * syscall callers split at the syscall boundary. */
    if (name) {
        const char *c;
        uint32_t len = 0;
        for (c = name; *c; c++) {
            if (*c == '\\' || *c == '/')
                return INVALID_HANDLE_VALUE;
            if (++len >= 64)                    /* OB_NAME_MAX */
                return INVALID_HANDLE_VALUE;
        }
        if (len == 0)
            return INVALID_HANDLE_VALUE;
    }

    port = (ALPC_PORT *)ob_alloc_object(ObpAlpcPortType);
    if (!port)
        return INVALID_HANDLE_VALUE;

    /* ob_alloc_object zero-fills the body; explicit field setup below
     * captures only the non-zero initial state. */
    port->PortType      = AlpcServerConnectionPort;
    port->Lock.flag     = 0;                    /* SPINLOCK_INIT */
    port->OwnerTask     = task_current();
    port->NextMessageId = 1;
    cond_init(&port->WaitQueue, "alpc_port_wait");

    if (attrs)
        port->Attributes = *attrs;              /* copy by value */
    /* else leave zero-init: Flags=0, SecurityQos zeroed, sizes 0 -- the
     * syscall path will layer policy on top; unit tests rely on the
     * zero defaults. */

    /* 3. Allocate the handle FIRST. On failure, the creation ref is the
     * only reference; a simple deref frees the port. No namespace
     * entry has been created yet, so no unlink is needed. */
    h = ObpAllocateHandle(ht, port, /* access */ 0, /* attrs */ 0);
    if (h == INVALID_HANDLE_VALUE) {
        ObDereferenceObject(port);              /* drops creation ref */
        return INVALID_HANDLE_VALUE;
    }

    /* 4. If named, insert under \RPC Control. Failure path frees the
     * handle (which drops a ref) and then drops the creation ref. */
    if (name) {
        if (ObLookupObjectByName("\\RPC Control", ObpDirectoryType, 0,
                                 &rpc_dir) != 0 || !rpc_dir) {
            ObpFreeHandle(ht, h);               /* drops handle ref */
            ObDereferenceObject(port);          /* drops creation ref */
            return INVALID_HANDLE_VALUE;
        }

        if (ObInsertObject(port, name, rpc_dir) != 0) {
            ObDereferenceObject(rpc_dir);       /* drop lookup ref */
            ObpFreeHandle(ht, h);               /* drops handle ref */
            ObDereferenceObject(port);          /* drops creation ref */
            return INVALID_HANDLE_VALUE;
        }

        ObDereferenceObject(rpc_dir);            /* drop lookup ref */
    }

    ObDereferenceObject(port);                   /* drop creation ref */
    return h;
}
