/* ============================================================================
 * ex_callback.c -- Ex callback objects (TODO-06 S4).
 *
 * A named notification point backed by the Object Manager type "Callback".
 * Consumers register routines; a producer fires them all via ExNotifyCallback.
 * Each registration slot carries its own EX_RUNDOWN_REF (S3): notify acquires a
 * slot's rundown before invoking it (outside the object lock); unregister marks
 * the slot unregistering then drains ALL outstanding references before
 * returning, so the consumer may free its context with no use-after-free.
 *
 * A monotonic per-slot generation in the cookie prevents a stale cookie from
 * touching a reused slot. ExUnregisterCallback must NOT be called from within a
 * callback routine of the same object (Windows contract) -- it would deadlock
 * on its own in-flight reference; the S14 verifier owns detecting that misuse.
 * ============================================================================ */

#include "kernel/ex.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/sched/spinlock.h"
#include "kernel/klog.h"

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* --- Slot + object layout ------------------------------------------------ */

enum { EXPC_FREE = 0, EXPC_ACTIVE = 1, EXPC_UNREGISTERING = 2 };

typedef struct {
    EX_CALLBACK_ROUTINE routine;
    void               *context;
    uint64_t            generation;   /* bumped on each free -> active reuse */
    uint8_t             state;        /* EXPC_FREE / ACTIVE / UNREGISTERING */
    EX_RUNDOWN_REF      rundown;      /* drains in-flight dispatch on unregister */
} expc_slot_t;

struct _EX_CALLBACK_OBJECT {
    spinlock_t   lock;
    bool         allow_multiple;
    uint32_t     active_count;
    expc_slot_t  slots[EX_CALLBACK_MAX_SLOTS];
};

const OBJECT_TYPE *ObpCallbackType;
static void *s_callback_dir;          /* \Callback\ directory body */

/* --- Cookie pack/unpack: (generation << 8) | slot, generation >= 1 -------
 * Slot index occupies the low 8 bits; the 56-bit generation never realistically
 * wraps (2^56 reuses), so a cookie is never 0 (gen >= 1) and a stale cookie can
 * never validate against a reused slot. */
_Static_assert(EX_CALLBACK_MAX_SLOTS <= 256, "slot index must fit in 8 cookie bits");
#define EXPC_GEN_MASK 0x00FFFFFFFFFFFFFFull   /* 56 bits -- the cookie's gen field */
#define EXPC_COOKIE(slot, gen) (((uint64_t)(gen) << 8) | ((uint32_t)(slot) & 0xFFu))
#define EXPC_COOKIE_SLOT(c)    ((uint32_t)((c) & 0xFFu))
#define EXPC_COOKIE_GEN(c)     ((uint64_t)((c) >> 8))   /* 56-bit value */

/* --- Type registration --------------------------------------------------- */

static void callback_on_delete(void *body)
{
    EX_CALLBACK_OBJECT *cb = (EX_CALLBACK_OBJECT *)body;
    /* A callback object reaching refcount 0 with a live registration is a
     * consumer lifetime bug (it should have unregistered first). Surface it
     * instead of silently dropping the registration's context/rundown. */
    if (cb->active_count != 0)
        klog(LOG_WARN, "ex",
             "callback object deleted with %u live registration(s)",
             (uint64_t)cb->active_count);
}

void ex_callback_init(void)
{
    ObpCallbackType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Callback",
        .body_size = sizeof(EX_CALLBACK_OBJECT),
        .on_close  = NULL,
        .on_delete = callback_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });
    if (!ObpCallbackType) {
        klog(LOG_ERROR, "ex", "Failed to register ObpCallbackType");
        return;
    }

    /* Create the \Callback\ directory under the namespace root. */
    extern void *ObpRootDirectory;
    if (ObpRootDirectory) {
        s_callback_dir = ob_ns_create_directory(ObpRootDirectory);
        if (s_callback_dir &&
            ObInsertObject(s_callback_dir, "Callback", ObpRootDirectory) < 0) {
            klog(LOG_ERROR, "ex", "Failed to insert \\Callback directory");
            s_callback_dir = NULL;
        }
    }

    /* Built-in well-known callback OBJECTS only; the PRODUCERS that fire them
     * are wired by the owning subsystems (process/image/registry/power/CI). */
    static const char *const builtins[] = {
        "ProcessCreate", "ThreadCreate", "ImageLoad",
        "RegistryChange", "PowerState", "CodeIntegrity",
    };
    for (uint32_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        (void)ExCreateCallback(builtins[i], true, true);

    klog(LOG_INFO, "ex", "Callback objects initialized (\\Callback\\ + %u built-ins)",
         (uint64_t)(sizeof(builtins) / sizeof(builtins[0])));
}

/* --- Object body init ---------------------------------------------------- */

static void callback_body_init(EX_CALLBACK_OBJECT *cb, bool allow_multiple)
{
    cb->lock = (spinlock_t)SPINLOCK_INIT;
    cb->allow_multiple = allow_multiple;
    cb->active_count = 0;
    for (uint32_t i = 0; i < EX_CALLBACK_MAX_SLOTS; i++) {
        cb->slots[i].routine = (EX_CALLBACK_ROUTINE)0;
        cb->slots[i].context = (void *)0;
        cb->slots[i].generation = 0;
        cb->slots[i].state = EXPC_FREE;
        ExInitializeRundownProtection(&cb->slots[i].rundown);
    }
}

/* --- ExCreateCallback ---------------------------------------------------- */

EX_CALLBACK_OBJECT *ExCreateCallback(const char *name, bool create,
                                     bool allow_multiple)
{
    /* Named: try to open an existing object under \Callback\. */
    if (name) {
        char path[128];
        void *existing = (void *)0;
        snprintf(path, sizeof(path), "\\Callback\\%s", name);
        if (ObLookupObjectByName(path, ObpCallbackType, 0, &existing) == 0
            && existing)
            return (EX_CALLBACK_OBJECT *)existing;   /* lookup ref kept alive */
        if (!create)
            return (EX_CALLBACK_OBJECT *)0;
        /* Fail closed: a named create needs the \Callback\ directory to back
         * it. Without it the object could not be inserted, so returning an
         * un-inserted (effectively anonymous) object under a name would let a
         * later opener miss it and silently lose notifications. */
        if (!s_callback_dir)
            return (EX_CALLBACK_OBJECT *)0;
    }

    if (!ObpCallbackType)
        return (EX_CALLBACK_OBJECT *)0;

    EX_CALLBACK_OBJECT *cb = (EX_CALLBACK_OBJECT *)ob_alloc_object(ObpCallbackType);
    if (!cb)
        return (EX_CALLBACK_OBJECT *)0;
    callback_body_init(cb, allow_multiple);

    if (name && s_callback_dir) {
        if (ObInsertObject(cb, name, s_callback_dir) < 0) {
            /* Name-collision race: another thread won; redirect to the winner. */
            char path[128];
            void *winner = (void *)0;
            snprintf(path, sizeof(path), "\\Callback\\%s", name);
            if (ObLookupObjectByName(path, ObpCallbackType, 0, &winner) == 0
                && winner) {
                ObDereferenceObject(cb);
                return (EX_CALLBACK_OBJECT *)winner;
            }
            ObDereferenceObject(cb);
            return (EX_CALLBACK_OBJECT *)0;
        }
    }
    return cb;
}

/* --- ExRegisterCallback -------------------------------------------------- */

EX_CALLBACK_COOKIE ExRegisterCallback(EX_CALLBACK_OBJECT *cb,
                                      EX_CALLBACK_ROUTINE routine, void *context)
{
    if (!cb || !routine)
        return 0;

    uint64_t flags;
    EX_CALLBACK_COOKIE cookie = 0;
    spin_lock_irqsave(&cb->lock, &flags);

    if (!cb->allow_multiple && cb->active_count > 0) {
        spin_unlock_irqrestore(&cb->lock, flags);
        return 0;                                    /* single-registration cap */
    }
    for (uint32_t i = 0; i < EX_CALLBACK_MAX_SLOTS; i++) {
        if (cb->slots[i].state == EXPC_FREE) {
            /* Free -> active reuse MUST re-arm the rundown: the prior drain
             * left its active bit set, so a fresh acquire would otherwise fail. */
            ExReInitializeRundownProtection(&cb->slots[i].rundown);
            cb->slots[i].routine = routine;
            cb->slots[i].context = context;
            /* Keep the stored generation inside the cookie's 56-bit field so
             * the stored value and EXPC_COOKIE_GEN(cookie) share one domain
             * (no truncation mismatch). >= 1 so a cookie is never 0; wrap at
             * 2^56 reuses is unreachable in practice. */
            cb->slots[i].generation = (cb->slots[i].generation + 1) & EXPC_GEN_MASK;
            if (cb->slots[i].generation == 0)
                cb->slots[i].generation = 1;
            cb->slots[i].state = EXPC_ACTIVE;
            cb->active_count++;
            cookie = EXPC_COOKIE(i, cb->slots[i].generation);
            break;
        }
    }
    spin_unlock_irqrestore(&cb->lock, flags);
    return cookie;                                    /* 0 if array full */
}

/* --- ExUnregisterCallback ------------------------------------------------ */

void ExUnregisterCallback(EX_CALLBACK_OBJECT *cb, EX_CALLBACK_COOKIE cookie)
{
    if (!cb || cookie == 0)
        return;
    uint32_t slot = EXPC_COOKIE_SLOT(cookie);
    uint64_t gen  = EXPC_COOKIE_GEN(cookie);
    if (slot >= EX_CALLBACK_MAX_SLOTS)
        return;

    uint64_t flags;
    spin_lock_irqsave(&cb->lock, &flags);
    if (cb->slots[slot].state != EXPC_ACTIVE ||
        cb->slots[slot].generation != gen) {
        spin_unlock_irqrestore(&cb->lock, flags);
        return;                                      /* stale/invalid cookie */
    }
    cb->slots[slot].state = EXPC_UNREGISTERING;      /* notify skips it now */
    spin_unlock_irqrestore(&cb->lock, flags);

    /* Drain all in-flight dispatch (any CPU) before returning so the caller can
     * free its context. MUST NOT run from within this slot's own callback. */
    ExWaitForRundownProtectionRelease(&cb->slots[slot].rundown);

    spin_lock_irqsave(&cb->lock, &flags);
    cb->slots[slot].routine = (EX_CALLBACK_ROUTINE)0;
    cb->slots[slot].context = (void *)0;
    cb->slots[slot].state = EXPC_FREE;
    if (cb->active_count > 0)
        cb->active_count--;
    spin_unlock_irqrestore(&cb->lock, flags);
}

/* --- ExNotifyCallback ---------------------------------------------------- */

void ExNotifyCallback(EX_CALLBACK_OBJECT *cb, void *arg1, void *arg2)
{
    if (!cb)
        return;

    /* Snapshot active slots + take a rundown reference on each UNDER the lock,
     * so an unregister that lands after this cannot free a context we are about
     * to invoke (its drain waits for the reference we hold). */
    struct { EX_CALLBACK_ROUTINE routine; void *context; uint32_t slot; }
        snap[EX_CALLBACK_MAX_SLOTS];
    uint32_t n = 0;
    uint64_t flags;

    spin_lock_irqsave(&cb->lock, &flags);
    for (uint32_t i = 0; i < EX_CALLBACK_MAX_SLOTS; i++) {
        if (cb->slots[i].state == EXPC_ACTIVE &&
            ExAcquireRundownProtection(&cb->slots[i].rundown)) {
            snap[n].routine = cb->slots[i].routine;
            snap[n].context = cb->slots[i].context;
            snap[n].slot = i;
            n++;
        }
    }
    spin_unlock_irqrestore(&cb->lock, flags);

    /* Invoke outside the lock (callbacks may take locks / allocate / re-enter),
     * then release each rundown reference so a pending unregister can drain. */
    for (uint32_t i = 0; i < n; i++) {
        snap[i].routine(snap[i].context, arg1, arg2);
        ExReleaseRundownProtection(&cb->slots[snap[i].slot].rundown);
    }
}

/* --- ExpEnumerateCallback (verifier / debug) ----------------------------- */

uint32_t ExpEnumerateCallback(EX_CALLBACK_OBJECT *cb, void **out, uint32_t max)
{
    if (!cb || !out || max == 0)
        return 0;
    uint32_t n = 0;
    uint64_t flags;
    spin_lock_irqsave(&cb->lock, &flags);
    for (uint32_t i = 0; i < EX_CALLBACK_MAX_SLOTS && n < max; i++) {
        if (cb->slots[i].state == EXPC_ACTIVE)
            out[n++] = cb->slots[i].context;
    }
    spin_unlock_irqrestore(&cb->lock, flags);
    return n;
}
