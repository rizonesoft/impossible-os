/* ============================================================================
 * tunables.c -- Runtime Tunable Registry
 *
 * Typed, access-controlled registration surface for mutable kernel policy.
 * See include/kernel/tunables.h for the contract. SMP-safe via one irqsave
 * spinlock; change callbacks always run at PASSIVE_LEVEL (inline when the
 * setter is already passive, otherwise deferred to the system workqueue).
 * ============================================================================ */
#include "kernel/tunables.h"
#include "kernel/klog.h"
#include "kernel/config.h"
#include "kernel/sched/irql.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/workqueue.h"
#include "kernel/sched/task.h"   /* yield() for the unregister quiesce wait */

/* boot_init.h supplies kernel_subsys_t (owner ids for core tunables). */
#include "kernel/boot_init.h"
/* HANDLE_TABLE_DEFAULT_LIMIT: the handle.quota_default tunable's default. */
#include "kernel/ob/handle_table.h"

#define TUNABLE_MAX        64u   /* registry capacity */
_Static_assert(TUNABLE_MAX == KERNEL_TUNABLE_CAPACITY,
    "public KERNEL_TUNABLE_CAPACITY must track the internal registry capacity");
#define TUNABLE_PENDING    32u   /* deferred-callback slot pool */
#define TUNABLE_NAME_CAP   48u   /* incl. NUL */

/* Owner id for builtin tunables without a dedicated subsystem slot. */
#define TUNABLE_OWNER_CORE 0xFEu

typedef struct {
    char         name[TUNABLE_NAME_CAP];
    int64_t      cur;
    int64_t      def;
    int64_t      min;
    int64_t      max;
    tunable_cb_t callback;
    void        *cb_ctx;
    uint64_t     write_gen;     /* global-unique gen of the latest accepted write */
    uint16_t     flags;
    uint8_t      type;
    uint8_t      owner_subsys;
    uint8_t      source;
    uint8_t      in_callback;   /* recursive-self-write guard */
    uint8_t      used;          /* slot occupied */
} tunable_entry_t;

typedef struct {
    uint8_t  used;
    uint8_t  idx;       /* index into s_tunables */
    uint64_t gen;       /* global-unique write generation captured at enqueue */
    int64_t  value;     /* value to publish + pass to the callback */
} tunable_pending_t;

static tunable_entry_t  s_tunables[TUNABLE_MAX];
static uint32_t         s_count;
static tunable_pending_t s_pending[TUNABLE_PENDING];
static DEFINE_SPINLOCK(s_lock);

/* Lock phase: monotonic. Plain aligned byte; advanced once at steady state. */
static volatile uint8_t s_phase = (uint8_t)TUNABLE_PHASE_BOOT;

/* Count of change callbacks currently executing (any owner). Incremented under
 * s_lock just before a callback is invoked outside the lock and decremented
 * after it returns, so kernel_tunable_unregister_owner() can quiesce all live
 * callback pointers before freeing a (possibly module-owned) entry. */
static uint32_t s_active_cb;

/* Monotonic global write-generation source. Every accepted write and every
 * registration draws a fresh value (under s_lock), so a generation is globally
 * unique: a reused table index gets a brand-new gen that no stale queued work
 * item can match, and a 64-bit counter does not wrap in any real runtime. */
static uint64_t s_global_gen;

/* ---- helpers (caller holds s_lock unless noted) ------------------------- */

static int name_eq(const char *a, const char *b)
{
    uint32_t i = 0;
    for (; i < TUNABLE_NAME_CAP; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0)    return 1;
    }
    return 0;
}

static uint32_t name_len(const char *s)
{
    uint32_t i = 0;
    while (i < TUNABLE_NAME_CAP && s[i]) i++;
    return i;
}

static int find_index(const char *name)
{
    for (uint32_t i = 0; i < TUNABLE_MAX; i++) {
        if (s_tunables[i].used && name_eq(s_tunables[i].name, name))
            return (int)i;
    }
    return -1;
}

static int64_t clamp_val(int64_t v, int64_t lo, int64_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Allocate a pending-callback slot. Caller holds s_lock. */
static tunable_pending_t *pending_alloc(uint8_t idx, uint64_t gen, int64_t value)
{
    for (uint32_t i = 0; i < TUNABLE_PENDING; i++) {
        if (!s_pending[i].used) {
            s_pending[i].used  = 1;
            s_pending[i].idx   = idx;
            s_pending[i].gen   = gen;
            s_pending[i].value = value;
            return &s_pending[i];
        }
    }
    return (tunable_pending_t *)0;
}

/* Workqueue trampoline: publish cur + run the callback, both at PASSIVE_LEVEL.
 * Publishing cur here (not in set()) keeps the stored value and the callback
 * effect atomic with respect to scheduling failure. A stale work item (a newer
 * write superseded this one) is dropped without publishing or invoking the
 * callback, so deferred writes stay monotonic with later inline/deferred sets. */
static void tunable_trampoline(void *arg)
{
    tunable_pending_t *slot = (tunable_pending_t *)arg;
    uint64_t flags;
    tunable_cb_t cb = (tunable_cb_t)0;
    void *ctx = (void *)0;
    int64_t value;
    const char *name = (const char *)0;
    uint8_t idx;
    int run = 0;

    spin_lock_irqsave(&s_lock, &flags);
    idx = slot->idx;
    value = slot->value;
    uint64_t gen = slot->gen;
    slot->used = 0;                       /* free the slot */
    if (idx < TUNABLE_MAX && s_tunables[idx].used &&
        s_tunables[idx].write_gen == gen) {   /* still the latest write */
        tunable_entry_t *t = &s_tunables[idx];
        t->cur = value;                   /* publish now */
        cb   = t->callback;
        ctx  = t->cb_ctx;
        name = t->name;
        if (cb) { t->in_callback = 1; s_active_cb++; run = 1; }
    }
    spin_unlock_irqrestore(&s_lock, flags);

    if (run) {
        cb(name, value, ctx);
        spin_lock_irqsave(&s_lock, &flags);
        if (idx < TUNABLE_MAX && s_tunables[idx].used)
            s_tunables[idx].in_callback = 0;
        s_active_cb--;
        spin_unlock_irqrestore(&s_lock, flags);
    }
}

/* ---- public API --------------------------------------------------------- */

NTSTATUS kernel_tunable_register(const char *name, tunable_type_t type,
                                 uint16_t flags, int64_t min, int64_t max,
                                 int64_t def, tunable_cb_t callback, void *cb_ctx,
                                 uint8_t owner_subsys, tunable_source_t source)
{
    if (!name) return STATUS_INVALID_PARAMETER;
    uint32_t nlen = name_len(name);
    if (nlen == 0 || nlen >= TUNABLE_NAME_CAP) return STATUS_INVALID_PARAMETER;
    if (min > max || def < min || def > max)   return STATUS_INVALID_PARAMETER;

    uint64_t irq;
    spin_lock_irqsave(&s_lock, &irq);

    if (find_index(name) >= 0) {
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    int slot = -1;
    for (uint32_t i = 0; i < TUNABLE_MAX; i++) {
        if (!s_tunables[i].used) { slot = (int)i; break; }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    tunable_entry_t *t = &s_tunables[slot];
    /* Zero the whole name buffer first: a reused slot must not leak stale tail
     * bytes from a prior (longer) name into kernel_tunable_dump output. */
    for (uint32_t i = 0; i < TUNABLE_NAME_CAP; i++) t->name[i] = 0;
    for (uint32_t i = 0; i < TUNABLE_NAME_CAP - 1; i++) {
        if (name[i] == 0) break;
        t->name[i] = name[i];
    }
    t->cur = def; t->def = def; t->min = min; t->max = max;
    t->callback = callback; t->cb_ctx = cb_ctx;
    t->flags = flags; t->type = (uint8_t)type;
    t->owner_subsys = owner_subsys; t->source = (uint8_t)source;
    t->in_callback = 0; t->write_gen = ++s_global_gen; t->used = 1;
    s_count++;

    spin_unlock_irqrestore(&s_lock, irq);
    return STATUS_SUCCESS;
}

NTSTATUS kernel_tunable_set(const char *name, int64_t value, uint32_t set_flags)
{
    if (!name) return STATUS_INVALID_PARAMETER;

    /* Capture the CALLER's IRQL before acquiring the lock -- spin_lock_irqsave
     * raises to DISPATCH_LEVEL, so reading it inside the lock would always
     * report non-passive and force every callback to defer. */
    int caller_passive = (KeGetCurrentIrql() == PASSIVE_LEVEL) &&
                         !(set_flags & TUNABLE_SET_DEFER);

    uint64_t irq;
    spin_lock_irqsave(&s_lock, &irq);

    int idx = find_index(name);
    if (idx < 0) {
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_NOT_FOUND;
    }
    tunable_entry_t *t = &s_tunables[idx];

    /* Access enforcement. */
    if (t->flags & TUNABLE_READONLY) {
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_ACCESS_DENIED;
    }
    if ((t->flags & TUNABLE_PRIVILEGED) && !(set_flags & TUNABLE_SET_PRIVILEGED)) {
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_ACCESS_DENIED;
    }
    if ((s_phase >= (uint8_t)TUNABLE_PHASE_LOCKED) ||
        ((t->flags & TUNABLE_BOOT_ONLY) && s_phase > (uint8_t)TUNABLE_PHASE_BOOT)) {
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_ACCESS_DENIED;
    }
    if (t->flags & TUNABLE_DEBUG_ONLY) {
        const kernel_config_t *kc = kernel_config_get();
        if (!kc || !kc->debug_enabled) {
            spin_unlock_irqrestore(&s_lock, irq);
            return STATUS_ACCESS_DENIED;
        }
    }
    /* Recursive self-write guard. */
    if (t->in_callback) {
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_UNSUCCESSFUL;
    }

    int64_t clamped = clamp_val(value, t->min, t->max);
    int did_clamp = (clamped != value);

    /* Stamp this write with a fresh global generation so a later writer
     * supersedes any still-queued work; remember the prior gen so a failed
     * deferred dispatch can roll back without dropping an earlier queued write. */
    uint64_t prev_gen = t->write_gen;
    uint64_t my_gen = ++s_global_gen;
    t->write_gen = my_gen;

    /* No callback: publish inline. */
    if (!t->callback) {
        t->cur = clamped;
        spin_unlock_irqrestore(&s_lock, irq);
        if (did_clamp)
            klog(LOG_WARN, "CONF", "[CONF] tunable clamp: %s", name);
        return STATUS_SUCCESS;
    }

    if (caller_passive) {
        /* Run inline: publish then invoke the callback outside the lock. */
        tunable_cb_t cb = t->callback;
        void *ctx = t->cb_ctx;
        t->cur = clamped;
        t->in_callback = 1;
        s_active_cb++;
        spin_unlock_irqrestore(&s_lock, irq);

        if (did_clamp)
            klog(LOG_WARN, "CONF", "[CONF] tunable clamp: %s", name);
        cb(name, clamped, ctx);

        spin_lock_irqsave(&s_lock, &irq);
        if (s_tunables[idx].used) s_tunables[idx].in_callback = 0;
        s_active_cb--;
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_SUCCESS;
    }

    /* Deferred: reserve a slot and enqueue while STILL holding s_lock so the
     * generation only advances for a write that is actually scheduled. On any
     * failure, roll the generation back (safe: the lock is held continuously,
     * so no other writer advanced it) -- otherwise a failed write would
     * supersede and silently drop an earlier accepted-but-queued write.
     * workqueue_enqueue is IRQ-safe and non-blocking; the trampoline takes
     * s_lock only after the worker dequeues, so the lock order is consistent. */
    tunable_pending_t *slot = pending_alloc((uint8_t)idx, my_gen, clamped);
    if (!slot) {
        t->write_gen = prev_gen;                 /* write not accepted */
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_INSUFFICIENT_RESOURCES;    /* cur unchanged */
    }
    if (workqueue_enqueue(sys_wq, tunable_trampoline, slot) == 0) {
        slot->used = 0;                          /* release slot */
        t->write_gen = prev_gen;                 /* write not accepted */
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_INSUFFICIENT_RESOURCES;    /* cur unchanged */
    }
    spin_unlock_irqrestore(&s_lock, irq);
    if (did_clamp)
        klog(LOG_WARN, "CONF", "[CONF] tunable clamp: %s", name);
    return STATUS_PENDING;
}

NTSTATUS kernel_tunable_get(const char *name, int64_t *out_value)
{
    if (!name || !out_value) return STATUS_INVALID_PARAMETER;
    uint64_t irq;
    spin_lock_irqsave(&s_lock, &irq);
    int idx = find_index(name);
    if (idx < 0) {
        spin_unlock_irqrestore(&s_lock, irq);
        return STATUS_NOT_FOUND;
    }
    *out_value = s_tunables[idx].cur;
    spin_unlock_irqrestore(&s_lock, irq);
    return STATUS_SUCCESS;
}

uint64_t kernel_tunable_get_u64(const char *name, uint64_t fallback)
{
    int64_t v;
    if (kernel_tunable_get(name, &v) == STATUS_SUCCESS && v >= 0)
        return (uint64_t)v;
    return fallback;
}

uint32_t kernel_tunable_unregister_owner(uint8_t owner_subsys)
{
    uint32_t removed = 0;
    uint64_t irq;

    /* Quiesce first: wait until no change callback is executing, so no copied
     * callback pointer for the owner can still be invoked once we free its
     * entries. A new dispatch cannot slip in during the free below because it
     * must take s_lock (held here) to increment s_active_cb. Yields rather than
     * busy-spins so a concurrent callback makes progress. PRECONDITION: must NOT
     * be called from the workqueue worker thread NOR from inside a tunable
     * change callback -- either would wait on its own callback forever; the
     * module loader flushes the work queue and calls this from a normal thread. */
    for (;;) {
        spin_lock_irqsave(&s_lock, &irq);
        if (s_active_cb == 0)
            break;
        spin_unlock_irqrestore(&s_lock, irq);
        yield();
    }

    for (uint32_t i = 0; i < TUNABLE_MAX; i++) {
        if (s_tunables[i].used && s_tunables[i].owner_subsys == owner_subsys) {
            /* Free the entry only. Any queued work item still referencing a
             * pending slot for this index frees its OWN slot when it runs and
             * no-ops safely (the trampoline re-checks used + write_gen under
             * the lock, so a freed or reused entry is never callback-invoked).
             * Clearing slots here would let a new write reuse a slot a stale
             * work item still points at. */
            s_tunables[i].used = 0;
            s_tunables[i].callback = (tunable_cb_t)0;
            s_count--;
            removed++;
        }
    }
    spin_unlock_irqrestore(&s_lock, irq);
    return removed;
}

tunable_phase_t kernel_tunable_lock_phase_get(void)
{
    return (tunable_phase_t)s_phase;
}

void kernel_tunable_lock_phase_advance(tunable_phase_t to)
{
    /* Ignore any value that is not exactly one of the defined phases -- check
     * the full enum value (not the narrowed low byte) so a bad cast like
     * (tunable_phase_t)257 cannot alias a valid phase and seal the registry. */
    if (to != TUNABLE_PHASE_BOOT && to != TUNABLE_PHASE_RUNTIME &&
        to != TUNABLE_PHASE_LOCKED)
        return;

    uint64_t irq;
    int advanced = 0;
    spin_lock_irqsave(&s_lock, &irq);
    if ((uint8_t)to > s_phase) {
        s_phase = (uint8_t)to;
        advanced = 1;
    }
    spin_unlock_irqrestore(&s_lock, irq);

    /* klog AFTER unlock: it writes serial/framebuffer/disk and must never run
     * under the registry spinlock with IRQs disabled. */
    if (advanced)
        klog(LOG_INFO, "CONF", "[CONF] tunable lock phase -> %u", (uint32_t)to);
}

uint32_t kernel_tunable_count(void)
{
    uint64_t irq;
    spin_lock_irqsave(&s_lock, &irq);
    uint32_t c = s_count;
    spin_unlock_irqrestore(&s_lock, irq);
    return c;
}

uint32_t kernel_tunable_dump(tunable_snapshot_t *out, uint32_t max_rows)
{
    if (!out || max_rows == 0) return 0;
    uint32_t n = 0;
    uint64_t irq;
    spin_lock_irqsave(&s_lock, &irq);
    for (uint32_t i = 0; i < TUNABLE_MAX && n < max_rows; i++) {
        if (!s_tunables[i].used) continue;
        tunable_entry_t *t = &s_tunables[i];
        tunable_snapshot_t *r = &out[n++];
        for (uint32_t j = 0; j < TUNABLE_NAME_CAP; j++) r->name[j] = t->name[j];
        r->cur = t->cur; r->def = t->def; r->min = t->min; r->max = t->max;
        r->flags = t->flags; r->type = t->type;
        r->owner_subsys = t->owner_subsys; r->source = t->source;
        r->_pad[0] = r->_pad[1] = r->_pad[2] = 0;
    }
    spin_unlock_irqrestore(&s_lock, irq);
    return n;
}

/* ---- core tunables ------------------------------------------------------ */

/* Live consumer: klog global minimum level. */
static void cb_klog_level(const char *name, int64_t v, void *ctx)
{
    (void)name; (void)ctx;
    klog_set_level((const char *)0, (log_level_t)v);
}

/* Live consumer: panic auto-restart seconds. The panic path reads this plain
 * aligned 32-bit global locklessly (panic-path safe: no spinlock acquire). */
volatile int32_t g_panic_tunable_restart_secs = -1;   /* -1 = unset */

static void cb_panic_timeout(const char *name, int64_t v, void *ctx)
{
    (void)name; (void)ctx;
    g_panic_tunable_restart_secs = (int32_t)v;
}

void kernel_tunables_register_core(void)
{
    static int registered;
    if (registered) return;
    registered = 1;

    /* klog global minimum level (LOG_DEBUG=0 .. LOG_FATAL=4). Live-wired.
     * Default 0 matches klog's own "keep all" default so registration is a
     * no-op until explicitly set. */
    kernel_tunable_register("klog.level", TUNABLE_UINT, TUNABLE_RUNTIME,
                            0, 4, 0, cb_klog_level, (void *)0,
                            (uint8_t)SUBSYS_KLOG, TUNABLE_SRC_BUILTIN);

    /* panic auto-restart seconds (0 = disabled, max 1 hour). Live-wired. */
    kernel_tunable_register("panic.timeout", TUNABLE_UINT, TUNABLE_RUNTIME,
                            0, 3600, 30, cb_panic_timeout, (void *)0,
                            TUNABLE_OWNER_CORE, TUNABLE_SRC_BUILTIN);

    /* Per-process default handle-table limit. Consumed by handle_table_init()
     * via kernel_tunable_get_u64() when a process handle table is created. */
    kernel_tunable_register("handle.quota_default", TUNABLE_UINT, TUNABLE_RUNTIME,
                            64, 1048576, HANDLE_TABLE_DEFAULT_LIMIT,
                            (tunable_cb_t)0, (void *)0,
                            TUNABLE_OWNER_CORE, TUNABLE_SRC_BUILTIN);

    klog(LOG_INFO, "CONF", "[CONF] tunable registry: %u core tunables",
         kernel_tunable_count());
}
