/* ============================================================================
 * irq.c -- Dynamic IRQ Registration
 *
 * Provides runtime interrupt handler registration, dynamic vector allocation,
 * and per-vector statistics.  Built on top of the IDT dispatch table.
 *
 * Architecture:
 *   irq_register() installs a wrapper into the IDT handlers[] table.
 *   The wrapper calls irq_eoi(), then the user callback.
 *   This keeps drivers clean -- they just get (vector, ctx).
 *
 * Vectors 0x20–0x2F (32–47): ISA IRQs -- registered by PIT, kbd, mouse
 * Vectors 0x30–0xEF (48–239): dynamic -- MSI/MSI-X, VMBus, STIMER
 * Vectors 0xF0–0xFF: reserved (LAPIC timer, spurious, IPI)
 * ============================================================================ */

#include "kernel/irq.h"
#include "kernel/idt.h"
#include "kernel/drivers/pic.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/irql.h"
#include "kernel/sched/kinterrupt.h"
#include "kernel/smp.h"
#include "kernel/drivers/lapic.h"
#include "kernel/vectors.h"

/* Every irq_chain_lock-taking API is PASSIVE_LEVEL-only: a caller inside
 * an ISR holds its own dispatch_active increment (drain self-deadlock),
 * and a handler merely waiting on the lock deadlocks a concurrent drain
 * holder. current_irql >= DISPATCH_LEVEL is intentionally broader than
 * "in ISR" -- spinlock/DPC contexts must not spin on a drain either
 * (mirrors the NT IoConnectInterrupt PASSIVE_LEVEL contract). */
static int irq_in_isr_context(void)
{
    struct per_cpu_data *p = smp_this_cpu();
    return p && p->current_irql >= DISPATCH_LEVEL;
}

/* ---- Per-vector registration entry ---- */
struct irq_shared_node {
    irq_shared_handler_t       handler;
    void                      *ctx;
    const char                *name;
    struct irq_shared_node    *next;
    uint8_t                    in_use;
};

struct irq_entry {
    irq_handler_t  handler;     /* user callback (NULL = unclaimed) */
    void          *ctx;         /* user context pointer */
    const char    *name;        /* debugging label */
    uint64_t       count;       /* total interrupts on this vector */
    uint8_t        allocated;   /* 1 if allocated via irq_alloc_vector() */
    uint8_t        reserved;    /* 1 = statically owned (ISA IRQ 8-15
                                 * window); never returned by the dynamic
                                 * allocator, survives free/unregister */
    uint8_t        quarantined; /* 1 = auto-masked by storm quarantine */
    uint8_t        chain_shared;/* 1 = chain accepts more sharers; 0 on a
                                 * fresh exclusive (_ex shared=0) route */
    /* Shared-chain state (PCI INTx). chain is published with release
     * ordering and read with acquire in the dispatcher; mutation happens
     * with the GSI masked, under irq_chain_lock, and waits for
     * dispatch_active to drain before a node may be reused. */
    struct irq_shared_node *chain;
    volatile uint32_t       dispatch_active;
    uint32_t                allnone_streak;
    uint8_t                 parked;     /* 1 = torn-down GSI vector held
                                         * back from the allocator (a CPU
                                         * may have accepted it pre-mask
                                         * and not yet read handlers[]);
                                         * revivable for the SAME GSI */
    /* Optional per-interrupt KINTERRUPT sync object. NULL = none; when set,
     * the dispatcher runs this vector's ISR under ki->lock and records the CPU
     * in ki->active_cpu, so KeSynchronizeExecution can synchronize with the ISR
     * and reject a self-ISR call. Opt-in: vectors with no KINTERRUPT take no
     * extra lock on the dispatch hot path. */
    KINTERRUPT             *kinterrupt;
};

static struct irq_entry irq_table[256];

/* Static pool for shared-chain nodes: registration is rare and bounded,
 * the dispatcher must never allocate. */
#define IRQ_SHARED_POOL 16
static struct irq_shared_node shared_pool[IRQ_SHARED_POOL];
static spinlock_t irq_chain_lock = SPINLOCK_INIT;

/* A shared level line where EVERY handler reports IRQ_NONE this many
 * times in a row is storming (stale device state or line noise): mask it
 * rather than livelock the CPU. */
#define IRQ_STORM_ALLNONE_LIMIT  1000u

/* ---- KINTERRUPT dispatch wrap (driver interrupt-sync feature) ----
 * If a KINTERRUPT is bound to this vector, run the ISR under its lock and
 * record the running CPU so KeSynchronizeExecution synchronizes with the ISR
 * and detects a self-ISR call. Opt-in: returns NULL ki for unbound vectors so
 * the common dispatch path takes no extra lock. Already in interrupt context
 * (IRQs off); the lock is for SMP exclusion with KeSynchronizeExecution. */
static inline KINTERRUPT *irq_ki_enter(struct irq_entry *e, uint64_t *flags)
{
    /* Acquire-load: pairs with the release-store in irq_bind_kinterrupt so a
     * dispatch that observes the pointer also observes the fully-initialized
     * KINTERRUPT fields. */
    KINTERRUPT *ki = __atomic_load_n(&e->kinterrupt, __ATOMIC_ACQUIRE);
    if (!ki)
        return (KINTERRUPT *)0;
    spin_lock_irqsave(&ki->lock, flags);
    /* Atomic-release store pairs with the acquire-load in the KeSynchronizeExecution
     * self-guard (keeps active_cpu accesses symmetric + portable off x86 TSO). */
    __atomic_store_n(&ki->active_cpu, smp_this_cpu()->cpu_id, __ATOMIC_RELEASE);
    return ki;
}

static inline void irq_ki_leave(KINTERRUPT *ki, uint64_t flags)
{
    if (!ki)
        return;
    __atomic_store_n(&ki->active_cpu, MAX_CPUS, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&ki->lock, flags);
}

/* ---- IDT-level wrapper ----
 * Installed as the interrupt_handler_t for each registered vector.
 * Dispatches to the irq_handler_t callback after EOI.
 * no_stack_protector: this runs on every hardware interrupt and only takes
 * the address of IRQ-save locals (no stack buffer), so the cookie load/compare
 * is pure ISR-latency cost with no protection value -- the real handlers keep
 * their canaries. */
__attribute__((no_stack_protector))
static uint64_t irq_dispatch_wrapper(struct interrupt_frame *frame)
{
    uint8_t vec = (uint8_t)frame->int_no;
    struct irq_entry *e = &irq_table[vec];
    KINTERRUPT *ki;
    uint64_t ki_flags = 0;

    e->count++;

    if (e->handler) {
        ki = irq_ki_enter(e, &ki_flags);
        e->handler(vec, e->ctx);
        irq_ki_leave(ki, ki_flags);
    }

    /* Send EOI for hardware IRQs (vectors 32+). The ISA irq number only
     * matters on the PIC path; ISA vectors live at PIC1_OFFSET+0..7 and
     * PIC2_OFFSET+0..7 (0x70+ -- NOT contiguous with the master range). */
    if (vec >= 32)
        irq_eoi(irq_vector_to_isa(vec));

    return (uint64_t)frame;
}

/* Translate a vector back to its ISA irq number for PIC EOI routing.
 * Returns 0xFF for non-ISA vectors (LAPIC/IOAPIC routed; LAPIC EOI only). */
uint8_t irq_vector_to_isa(uint8_t vec)
{
    if (vec >= PIC1_OFFSET && vec < PIC1_OFFSET + 8)
        return (uint8_t)(vec - PIC1_OFFSET);
    if (vec >= PIC2_OFFSET && vec < PIC2_OFFSET + 8)
        return (uint8_t)(8 + (vec - PIC2_OFFSET));
    return 0xFF;
}

/* ---- Public API ---- */

/* No-log core: safe to call under irq_chain_lock (logging would hold the
 * lock across serial I/O, violating the spinlock hold-time rule). */
static int irq_register_nolog(uint8_t vector, irq_handler_t handler,
                              void *ctx, const char *name)
{
    if (vector < 32)
        return IRQ_ERR_RANGE;   /* CPU exceptions -- use idt_register_handler */
    if (!handler)
        return IRQ_ERR_RANGE;   /* a NULL handler would silently eat IRQs */

    if (irq_table[vector].handler)
        return IRQ_ERR_BUSY;    /* already claimed */

    irq_table[vector].handler = handler;
    irq_table[vector].ctx     = ctx;
    irq_table[vector].name    = name;
    irq_table[vector].count   = 0;

    /* Install our wrapper into the IDT dispatch table */
    idt_register_handler(vector, irq_dispatch_wrapper);
    return IRQ_OK;
}

int irq_register(uint8_t vector, irq_handler_t handler, void *ctx,
                 const char *name)
{
    int rc = irq_register_nolog(vector, handler, ctx, name);
    if (rc == IRQ_OK)
        klog(LOG_DEBUG, "irq", "  registered vec %u -> \"%s\"",
             (uint64_t)vector, name ? name : "?");
    return rc;
}

/* Bind a KINTERRUPT to a vector so the dispatcher runs that vector's ISR under
 * ki->lock (KeConnectInterrupt path). Release-store pairs with the dispatcher's
 * acquire-load. Returns IRQ_OK, IRQ_ERR_RANGE (vector < 32), or IRQ_ERR_BUSY
 * (a different KINTERRUPT already bound, or the vector is a shared chain).
 * EXCLUSIVE vectors only: a shared GSI line has multiple drivers, so a single
 * vector-wide sync object would force unrelated handlers under one driver's
 * lock -- a per-line/per-node shared KINTERRUPT model is a deferred enhancement.
 * Publish is an atomic CAS (NULL -> ki) so two racing binds cannot both win. */
int irq_bind_kinterrupt(uint8_t vector, KINTERRUPT *ki)
{
    uint64_t flags;
    KINTERRUPT *cur;
    int rc;
    if (vector < 32 || !ki)
        return IRQ_ERR_RANGE;
    /* PASSIVE_LEVEL only: this takes irq_chain_lock, and a chain-drain holder
     * can hold that lock while waiting for in-flight dispatches to drain, so an
     * ISR/DPC caller would deadlock. Same guard as irq_request_gsi_ex. */
    if (irq_in_isr_context())
        return IRQ_ERR_BUSY;
    /* Serialize against shared-route mutation (irq_chain_lock) so the ownership
     * check and the publish are ONE transition -- no window where a shared
     * chain or a fresh allocation appears between the check and the store. */
    spin_lock_irqsave(&irq_chain_lock, &flags);
    /* EXCLUSIVE owned vector only: an installed non-shared handler, no shared
     * chain, not parked. Binding a free/unowned vector is rejected -- the
     * allocator could later hand it to an unrelated handler that the dispatcher
     * would then run under the wrong (or no) KINTERRUPT lock. */
    if (!irq_table[vector].handler || irq_table[vector].chain ||
        irq_table[vector].parked) {
        rc = IRQ_ERR_BUSY;
    } else {
        cur = irq_table[vector].kinterrupt;
        if (cur && cur != ki) {
            rc = IRQ_ERR_BUSY;          /* a different object already bound */
        } else {
            /* Release-store pairs with the dispatcher's acquire-load. */
            __atomic_store_n(&irq_table[vector].kinterrupt, ki, __ATOMIC_RELEASE);
            rc = IRQ_OK;                 /* fresh bind, or idempotent same ki */
        }
    }
    spin_unlock_irqrestore(&irq_chain_lock, flags);
    return rc;
}

/* Unbind the KINTERRUPT from a vector (KeDisconnectInterrupt path). Only clears
 * if the given object is the bound one. PASSIVE_LEVEL teardown. */
void irq_unbind_kinterrupt(uint8_t vector, KINTERRUPT *ki)
{
    if (vector < 32)
        return;
    if (__atomic_load_n(&irq_table[vector].kinterrupt, __ATOMIC_ACQUIRE) == ki)
        __atomic_store_n(&irq_table[vector].kinterrupt, (KINTERRUPT *)0,
                         __ATOMIC_RELEASE);
}

/* No-log core (see irq_register_nolog rationale) */
static void irq_unregister_nolog(uint8_t vector)
{
    int owned;

    if (vector < 32)
        return;

    /* Only clear the IDT slot the irq table actually owns: vectors like
     * INT 0x80 (Linux syscall) and INT 0x81 (yield) install their
     * handlers straight into the IDT with no irq_table footprint, and a
     * stray unregister/free must not tear those gates down. */
    owned = irq_table[vector].handler != (irq_handler_t)0;

    irq_table[vector].handler   = (irq_handler_t)0;
    irq_table[vector].ctx       = (void *)0;
    /* Drop any KINTERRUPT binding when the exclusive handler goes away, so a
     * stale sync object cannot survive into a later reallocation of this
     * vector (the binding requires an owned handler). */
    __atomic_store_n(&irq_table[vector].kinterrupt, (KINTERRUPT *)0,
                     __ATOMIC_RELEASE);
    if (!irq_table[vector].reserved)
        irq_table[vector].name  = (const char *)0;
    /* Reserved vectors stay allocated: the ISA IRQ 8-15 window and the
     * syscall/yield gates must never fall back into the allocator pool */
    irq_table[vector].allocated = irq_table[vector].reserved ? 1 : 0;

    if (owned)
        idt_register_handler(vector, (interrupt_handler_t)0);
}

void irq_unregister(uint8_t vector)
{
    if (vector >= 32 && irq_table[vector].handler) {
        klog(LOG_DEBUG, "irq", "  unregistered vec %u (\"%s\", %u hits)",
             (uint64_t)vector,
             irq_table[vector].name ? irq_table[vector].name : "?",
             irq_table[vector].count);
    }
    irq_unregister_nolog(vector);
}

int irq_reserve_vector(uint8_t vector, const char *name)
{
    uint64_t irqf;

    if (vector < IRQ_DYNAMIC_BASE || vector > IRQ_DYNAMIC_END)
        return 0;   /* outside the allocator: nothing to reserve */

    spin_lock_irqsave(&irq_chain_lock, &irqf);
    if (irq_table[vector].handler || irq_table[vector].chain ||
        irq_table[vector].allocated) {
        /* Already owned -- by a dynamic registration OR another static
         * reservation (ISA window, syscall/yield gates): refuse. A
         * firmware-derived vector must never displace a static owner. */
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        return -1;
    }
    irq_table[vector].allocated = 1;
    irq_table[vector].reserved  = 1;
    irq_table[vector].name      = name;
    spin_unlock_irqrestore(&irq_chain_lock, irqf);
    return 0;
}

uint8_t irq_alloc_vector(void)
{
    uint32_t vec;
    for (vec = IRQ_DYNAMIC_BASE; vec <= IRQ_DYNAMIC_END; vec++) {
        /* parked check is defense in depth: parked vectors keep
         * allocated=1, but a stale delivery may still dispatch them --
         * they must never reach a new owner through this allocator */
        if (!irq_table[vec].handler && !irq_table[vec].allocated &&
            !irq_table[vec].parked) {
            irq_table[vec].allocated = 1;
            return (uint8_t)vec;
        }
    }
    return 0;   /* no free vectors */
}

void irq_free_vector(uint8_t vector)
{
    if (vector < IRQ_DYNAMIC_BASE || vector > IRQ_DYNAMIC_END)
        return;

    irq_unregister(vector);
    if (!irq_table[vector].reserved)
        irq_table[vector].allocated = 0;
}

const char *irq_get_name(uint8_t vector)
{
    return irq_table[vector].name;
}

uint64_t irq_get_count(uint8_t vector)
{
    return irq_table[vector].count;
}

const uint64_t *irq_get_counts(void)
{
    /* Return pointer to first count field -- but entries are structs,
     * so callers should use irq_get_count() per-vector instead.
     * This is a convenience for bulk stats. */
    static uint64_t count_snapshot[256];
    uint32_t i;
    for (i = 0; i < 256; i++)
        count_snapshot[i] = irq_table[i].count;
    return count_snapshot;
}

void irq_init(void)
{
    uint32_t i;
    for (i = 0; i < 256; i++) {
        irq_table[i].handler   = (irq_handler_t)0;
        irq_table[i].ctx       = (void *)0;
        irq_table[i].name      = (const char *)0;
        irq_table[i].count     = 0;
        irq_table[i].allocated = 0;
    }
    /* Reserve the ISA IRQ 8-15 vector window (PIC2 remap / IOAPIC slave
     * pins) out of the dynamic allocator -- it sits inside 0x30-0xEF. */
    for (i = PIC2_OFFSET; i < (uint32_t)PIC2_OFFSET + 8; i++) {
        irq_table[i].allocated = 1;
        irq_table[i].reserved  = 1;
        irq_table[i].name = "isa-irq8-15";
    }
    /* Reserve the ring-3 software-interrupt gates that also sit inside
     * the dynamic range but register their handlers straight into the
     * IDT (no irq_table footprint): handing either to a device would
     * alias the syscall/yield gate -- the same class of bug as the old
     * ISA IRQ14 aliasing of INT 0x2E. */
    irq_table[VECTOR_LINUX_SYSCALL].allocated = 1;
    irq_table[VECTOR_LINUX_SYSCALL].reserved  = 1;
    irq_table[VECTOR_LINUX_SYSCALL].name      = "int80-linux-syscall";
    irq_table[VECTOR_YIELD].allocated = 1;
    irq_table[VECTOR_YIELD].reserved  = 1;
    irq_table[VECTOR_YIELD].name      = "int81-yield";
    klog(LOG_INFO, "irq",
         "Dynamic IRQ subsystem ready (vectors 0x30-0xEF allocatable, "
         "0x70-0x77 ISA IRQ 8-15 + 0x80/0x81 syscall/yield reserved)");
}

/* ---- High-level GSI-based API ---- */

#include "kernel/acpi.h"
#include "kernel/drivers/ioapic.h"

/* GSI-to-vector mapping table */
static uint8_t gsi_to_vector[256];  /* 0 = not mapped */
static uint32_t gsi_of_vector[256]; /* vector -> gsi + 1; 0 = not mapped */
static uint16_t gsi_route_flags[256]; /* resolved MADT-style flags the
                                       * route was programmed with; a
                                       * shared join must match (one
                                       * line has ONE polarity/trigger) */

uint32_t irq_gsi_for_vector(uint8_t vec)
{
    uint32_t v = gsi_of_vector[vec];
    return v ? (v - 1) : 0xFFFFFFFFu;
}

/* Destination for fresh GSI routes: the BSP's REAL LAPIC ID (not a
 * hardcoded 0 -- the BSP's APIC ID is firmware-assigned on real SMP). */
static uint8_t irq_route_dest(void)
{
    struct per_cpu_data *bsp = smp_get_cpu(0);
    if (bsp)
        return (uint8_t)bsp->lapic_id;
    return (uint8_t)lapic_id();
}

int irq_vector_quarantined(uint8_t vec)
{
    return __atomic_load_n(&irq_table[vec].quarantined, __ATOMIC_ACQUIRE);
}

/* Mask a storming vector at its owning controller. Called from the
 * dispatcher (ISR context): no locks beyond the IOAPIC's own, no alloc. */
static void irq_quarantine_vector(uint8_t vec)
{
    uint8_t  isa = irq_vector_to_isa(vec);
    uint32_t gsi = irq_gsi_for_vector(vec);

    __atomic_store_n(&irq_table[vec].quarantined, 1, __ATOMIC_RELEASE);

    if (gsi != 0xFFFFFFFFu && ioapic_available()) {
        ioapic_mask_irq(gsi);
    } else if (isa != 0xFF) {
        if (ioapic_available())
            ioapic_mask_irq(ioapic_isa_to_gsi(isa));
        else
            pic_mask_irq(isa);
    }
    klog(LOG_ERROR, "irq",
         "vector 0x%x QUARANTINED (interrupt storm, no handler claimed it)",
         (uint64_t)vec);
}

/* Dispatcher for shared chains: every registrant runs and reports a claim
 * status; an all-IRQ_NONE streak marks a storm. Single EOI per interrupt.
 * no_stack_protector for the same ISR-hot-path reason as irq_dispatch_wrapper. */
__attribute__((no_stack_protector))
static uint64_t irq_shared_dispatch_wrapper(struct interrupt_frame *frame)
{
    uint8_t vec = (uint8_t)frame->int_no;
    struct irq_entry *e = &irq_table[vec];
    struct irq_shared_node *n;
    int handled = IRQ_NONE;

    e->count++;

    /* Entry protocol (closes the accepted-but-not-yet-counted window):
     * the increment is published under irq_chain_lock, so a mutator that
     * holds the lock and calls irq_chain_drain() either (a) waits for
     * this dispatch because the increment already landed, or (b) forces
     * this dispatch to spin here until the mutation finishes, after
     * which the chain load below sees the post-mutation list (and bails
     * on an empty one). Same shape as Linux handle_fasteoi_irq taking
     * desc->lock before setting IRQD_IRQ_INPROGRESS. Hold: 1 increment.
     *
     * dispatch_active then stays elevated until the VERY END of the
     * wrapper -- the drain postcondition covers the chain walk, the
     * streak/quarantine update, AND the EOI, so no mutator can clear
     * quarantine state this ISR is about to set. */
    {
        uint64_t entryf;
        spin_lock_irqsave(&irq_chain_lock, &entryf);
        __atomic_fetch_add(&e->dispatch_active, 1, __ATOMIC_ACQUIRE);
        spin_unlock_irqrestore(&irq_chain_lock, entryf);
    }
    n = __atomic_load_n(&e->chain, __ATOMIC_ACQUIRE);
    if (!n) {
        /* Torn down while this dispatch was in flight: acknowledge and
         * leave -- no storm accounting on a vector we no longer own */
        if (vec >= 32)
            irq_eoi(irq_vector_to_isa(vec));
        __atomic_fetch_sub(&e->dispatch_active, 1, __ATOMIC_RELEASE);
        return (uint64_t)frame;
    }
    /* No KINTERRUPT wrap on the shared path: KINTERRUPT binds EXCLUSIVE vectors
     * only (irq_bind_kinterrupt rejects shared chains), since one vector-wide
     * lock cannot correctly serialize independent drivers on a shared GSI line.
     * A per-line/per-node shared synchronization object is a deferred follow-up. */
    for (; n; n = n->next) {
        if (n->handler(vec, n->ctx) == IRQ_HANDLED)
            handled = IRQ_HANDLED;
    }

    if (handled == IRQ_HANDLED) {
        e->allnone_streak = 0;
    } else if (!__atomic_load_n(&e->quarantined, __ATOMIC_ACQUIRE) &&
               ++e->allnone_streak >= IRQ_STORM_ALLNONE_LIMIT) {
        irq_quarantine_vector(vec);
    }

    if (vec >= 32)
        irq_eoi(irq_vector_to_isa(vec));

    __atomic_fetch_sub(&e->dispatch_active, 1, __ATOMIC_RELEASE);

    return (uint64_t)frame;
}

/* Wait for in-flight shared dispatches to finish. Callers hold
 * irq_chain_lock AND have already masked the line, so no new dispatch can
 * start; once this returns, no CPU is walking the chain, so list mutation,
 * node reuse, and quarantine-state changes are all race-free. */
static void irq_chain_drain(uint8_t vec)
{
    while (__atomic_load_n(&irq_table[vec].dispatch_active,
                           __ATOMIC_ACQUIRE) != 0)
        __asm__ volatile("pause");
}

/* Pool helpers -- callers hold irq_chain_lock */
static struct irq_shared_node *shared_node_take(void)
{
    uint32_t i;
    for (i = 0; i < IRQ_SHARED_POOL; i++) {
        if (!shared_pool[i].in_use) {
            shared_pool[i].in_use = 1;
            return &shared_pool[i];
        }
    }
    return (struct irq_shared_node *)0;
}

/* Park a torn-down GSI vector instead of returning it to the allocator.
 * dispatch_active cannot cover a CPU that accepted the interrupt but has
 * not yet read handlers[vec] in the common IDT path, so handing the
 * vector to a NEW owner could run that owner's handler for the stale
 * interrupt. Parked vectors keep a tombstone (shared wrapper with a NULL
 * chain: EOI and exit) and are revived only for the SAME GSI, where a
 * cross-teardown delivery is a legitimate interrupt of that line.
 * Callers hold irq_chain_lock; the line is already masked. */
static void irq_park_vector(uint8_t vec, uint32_t gsi)
{
    irq_table[vec].handler       = (irq_handler_t)0;
    irq_table[vec].ctx           = (void *)0;
    irq_table[vec].name          = "(parked)";
    irq_table[vec].chain_shared  = 0;
    irq_table[vec].quarantined   = 0;
    irq_table[vec].allnone_streak = 0;
    irq_table[vec].parked        = 1;
    /* Authoritative, not inherited: irq_unregister_nolog() zeroes
     * allocated for non-reserved vectors, and several park sites run it
     * first -- force the never-allocatable state here */
    irq_table[vec].allocated     = 1;
    __atomic_store_n(&irq_table[vec].chain,
                     (struct irq_shared_node *)0, __ATOMIC_RELEASE);
    idt_register_handler(vec, irq_shared_dispatch_wrapper);
    gsi_to_vector[gsi] = 0;
    gsi_route_flags[gsi] = 0;
    /* gsi_of_vector[vec] keeps gsi + 1: the revive key */
}

/* The parked vector previously routed for this GSI, or 0. Caller holds
 * irq_chain_lock. */
static uint8_t irq_parked_vector_for(uint32_t gsi)
{
    uint32_t vec;
    for (vec = 32; vec < 256; vec++) {
        if (irq_table[vec].parked && gsi_of_vector[vec] == gsi + 1)
            return (uint8_t)vec;
    }
    return 0;
}

uint8_t irq_request_gsi(uint32_t gsi, irq_handler_t handler, void *ctx,
                         const char *name)
{
    uint64_t irqf;
    uint8_t vec;
    int rc;
    int revived = 0;

    if (gsi >= 256 || !handler) return 0;

    /* PASSIVE_LEVEL-only (NT IoConnectInterrupt contract): taking
     * irq_chain_lock from a shared-chain handler deadlocks against a
     * concurrent drain (drain holder waits on dispatch_active, the
     * handler waits on the lock). Applies to every chain-lock API. */
    if (irq_in_isr_context()) {
        klog(LOG_ERROR, "irq",
             "irq_request_gsi(%u) called at DISPATCH_LEVEL or above -- refused",
             (uint64_t)gsi);
        return 0;
    }

    spin_lock_irqsave(&irq_chain_lock, &irqf);
    if (gsi_to_vector[gsi]) {
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        klog(LOG_ERROR, "irq", "irq_request_gsi(%u): GSI already routed",
             (uint64_t)gsi);
        return 0;
    }

    /* Revive the vector previously parked for this GSI (safe: a stale
     * pre-mask delivery is a real interrupt of this same line), else
     * allocate a fresh one */
    vec = irq_parked_vector_for(gsi);
    if (vec) {
        irq_table[vec].parked = 0;
        revived = 1;
    } else {
        vec = irq_alloc_vector();
        if (!vec) {
            spin_unlock_irqrestore(&irq_chain_lock, irqf);
            klog(LOG_ERROR, "irq", "irq_request_gsi(%u): no free vectors", (uint64_t)gsi);
            return 0;
        }
    }

    /* Register handler on the allocated vector (no-log under the lock) */
    rc = irq_register_nolog(vec, handler, ctx, name);
    if (rc != IRQ_OK) {
        irq_unregister_nolog(vec);
        if (revived)
            irq_park_vector(vec, gsi);
        else if (!irq_table[vec].reserved)
            irq_table[vec].allocated = 0;
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        klog(LOG_ERROR, "irq", "irq_request_gsi(%u): register failed (%d)",
             (uint64_t)gsi, (uint64_t)rc);
        return 0;
    }

    /* Program IOAPIC redirection: GSI → vector, routed to BSP */
    if (ioapic_available()) {
        /* Check MADT overrides for polarity/trigger flags */
        uint16_t flags = 0;
        uint32_t i;
        uint32_t ov_count = acpi_get_override_count();
        for (i = 0; i < ov_count; i++) {
            const struct madt_int_override *ovr = acpi_get_override(i);
            if (ovr && ovr->gsi == gsi) {
                flags = ovr->flags;
                break;
            }
        }
        if (ioapic_route_irq(gsi, vec, irq_route_dest(), flags) != 0 ||
            ioapic_unmask_irq(gsi) != 0) {
            /* GSI outside the IOAPIC routing domain: roll back so the
             * caller sees failure instead of a vector that never fires.
             * A fresh vector was never routed (no acceptance hazard) and
             * may go back to the allocator; a revived one re-parks. */
            irq_unregister_nolog(vec);
            if (revived)
                irq_park_vector(vec, gsi);
            else if (!irq_table[vec].reserved)
                irq_table[vec].allocated = 0;
            spin_unlock_irqrestore(&irq_chain_lock, irqf);
            klog(LOG_ERROR, "irq",
                 "irq_request_gsi(%u): GSI not routable on this IOAPIC",
                 (uint64_t)gsi);
            return 0;
        }
        gsi_route_flags[gsi] = flags;
    }

    gsi_to_vector[gsi] = vec;
    gsi_of_vector[vec] = gsi + 1;
    spin_unlock_irqrestore(&irq_chain_lock, irqf);

    klog(LOG_DEBUG, "irq", "GSI %u -> vec 0x%x (%s)",
         (uint64_t)gsi, (uint64_t)vec, name ? name : "?");

    return vec;
}

/* Presence-based MADT override lookup: returns 1 and writes the flags
 * (which may legitimately be 0 = conforms-to-bus) when an override names
 * the GSI, 0 when none does. Flags 0 must stay distinguishable from
 * "no override" -- firmware overrides are authoritative either way. */
static int gsi_override_lookup(uint32_t gsi, uint16_t *out_flags)
{
    uint32_t i, n = acpi_get_override_count();
    for (i = 0; i < n; i++) {
        const struct madt_int_override *ovr = acpi_get_override(i);
        if (ovr && ovr->gsi == gsi) {
            *out_flags = ovr->flags;
            return 1;
        }
    }
    return 0;
}

uint8_t irq_request_gsi_ex(uint32_t gsi, irq_shared_handler_t handler,
                           void *ctx, const char *name, uint16_t flags,
                           int shared)
{
    uint64_t irqf;
    uint8_t vec;
    struct irq_shared_node *node;
    int revived = 0;

    if (gsi >= 256 || !handler)
        return 0;
    if (!ioapic_available())
        return 0;   /* shared/flagged routing is IOAPIC-only */

    /* Joining an existing chain drains in-flight dispatches: deadlocks
     * when the caller is itself inside the dispatcher. Refuse from ISR. */
    if (irq_in_isr_context()) {
        klog(LOG_ERROR, "irq",
             "irq_request_gsi_ex(%u) called from interrupt context -- refused",
             (uint64_t)gsi);
        return 0;
    }

    /* A MADT interrupt source override is authoritative for the line's
     * polarity/trigger -- firmware knows the board, even when the
     * override says 0 (conforms to bus). Caller flags apply only when
     * no override names this GSI. */
    {
        uint16_t ovr_flags;
        if (gsi_override_lookup(gsi, &ovr_flags))
            flags = ovr_flags;
    }

    spin_lock_irqsave(&irq_chain_lock, &irqf);

    vec = gsi_to_vector[gsi];
    if (vec) {
        /* GSI already routed: joining is only legal when both the
         * existing route and the new caller opted into sharing */
        if (!shared || !irq_table[vec].chain ||
            !irq_table[vec].chain_shared) {
            spin_unlock_irqrestore(&irq_chain_lock, irqf);
            klog(LOG_ERROR, "irq",
                 "irq_request_gsi_ex(%u): GSI busy (non-shared owner)",
                 (uint64_t)gsi);
            return 0;
        }
        if (flags != gsi_route_flags[gsi]) {
            /* One physical line has ONE polarity/trigger: a join whose
             * resolved flags disagree with the programmed route is an
             * electrical misconfiguration -- refuse loudly rather than
             * silently dispatch with the wrong sense */
            spin_unlock_irqrestore(&irq_chain_lock, irqf);
            klog(LOG_ERROR, "irq",
                 "irq_request_gsi_ex(%u): flags 0x%x conflict with routed 0x%x -- refused",
                 (uint64_t)gsi, (uint64_t)flags,
                 (uint64_t)gsi_route_flags[gsi]);
            return 0;
        }
        node = shared_node_take();
        if (!node) {
            spin_unlock_irqrestore(&irq_chain_lock, irqf);
            klog(LOG_ERROR, "irq", "shared IRQ pool exhausted");
            return 0;
        }
        node->handler = handler;
        node->ctx     = ctx;
        node->name    = name;
        /* Mask the line and drain in-flight dispatches so no CPU is
         * walking the chain during the splice (and so the quarantine
         * state cannot change concurrently -- quarantine fires from the
         * dispatcher, which the drain excludes). */
        ioapic_mask_irq(gsi);
        irq_chain_drain(vec);
        node->next = irq_table[vec].chain;
        __atomic_store_n(&irq_table[vec].chain, node, __ATOMIC_RELEASE);
        /* A new sharer is a storm-recovery point (the joining device may
         * be the one whose pending state was storming): clear quarantine
         * and restart the streak, like Linux's spurious-disabled
         * re-enable on a new action in __setup_irq(). */
        __atomic_store_n(&irq_table[vec].quarantined, 0, __ATOMIC_RELEASE);
        irq_table[vec].allnone_streak = 0;
        ioapic_unmask_irq(gsi);
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        return vec;
    }

    /* Fresh GSI: revive this GSI's parked vector or allocate a new one,
     * then start a chain */
    vec = irq_parked_vector_for(gsi);
    if (vec) {
        irq_table[vec].parked = 0;
        revived = 1;
    } else {
        vec = irq_alloc_vector();
        if (!vec) {
            spin_unlock_irqrestore(&irq_chain_lock, irqf);
            klog(LOG_ERROR, "irq", "irq_request_gsi_ex(%u): no free vectors",
                 (uint64_t)gsi);
            return 0;
        }
    }
    node = shared_node_take();
    if (!node) {
        if (revived)
            irq_park_vector(vec, gsi);
        else
            irq_table[vec].allocated = irq_table[vec].reserved ? 1 : 0;
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        klog(LOG_ERROR, "irq", "shared IRQ pool exhausted");
        return 0;
    }
    node->handler = handler;
    node->ctx     = ctx;
    node->name    = name;
    node->next    = (struct irq_shared_node *)0;

    irq_table[vec].name = name;
    irq_table[vec].quarantined = 0;
    irq_table[vec].allnone_streak = 0;
    irq_table[vec].chain_shared = shared ? 1 : 0;
    __atomic_store_n(&irq_table[vec].chain, node, __ATOMIC_RELEASE);
    idt_register_handler(vec, irq_shared_dispatch_wrapper);

    if (ioapic_route_irq(gsi, vec, irq_route_dest(), flags) != 0 ||
        ioapic_unmask_irq(gsi) != 0) {
        __atomic_store_n(&irq_table[vec].chain,
                         (struct irq_shared_node *)0, __ATOMIC_RELEASE);
        node->in_use = 0;
        if (revived) {
            irq_park_vector(vec, gsi);
        } else {
            /* Never routed: no acceptance hazard, free to the allocator */
            idt_register_handler(vec, (interrupt_handler_t)0);
            irq_table[vec].name = (const char *)0;
            irq_table[vec].allocated = irq_table[vec].reserved ? 1 : 0;
        }
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        klog(LOG_ERROR, "irq",
             "irq_request_gsi_ex(%u): GSI not routable", (uint64_t)gsi);
        return 0;
    }

    gsi_to_vector[gsi] = vec;
    gsi_of_vector[vec] = gsi + 1;
    gsi_route_flags[gsi] = flags;
    spin_unlock_irqrestore(&irq_chain_lock, irqf);

    klog(LOG_DEBUG, "irq", "GSI %u -> vec 0x%x (%s, shared=%u, flags=0x%x)",
         (uint64_t)gsi, (uint64_t)vec, name ? name : "?",
         (uint64_t)(shared ? 1 : 0), (uint64_t)flags);
    return vec;
}

int irq_release_gsi_shared(uint32_t gsi, irq_shared_handler_t handler,
                           void *ctx)
{
    uint64_t irqf;
    uint8_t vec;
    struct irq_shared_node **pp, *n;

    if (gsi >= 256)
        return -1;

    /* Drain-wait deadlocks when called from inside the dispatcher */
    if (irq_in_isr_context()) {
        klog(LOG_ERROR, "irq",
             "irq_release_gsi_shared(%u) called from interrupt context -- refused",
             (uint64_t)gsi);
        return -1;
    }

    spin_lock_irqsave(&irq_chain_lock, &irqf);
    vec = gsi_to_vector[gsi];
    if (!vec || !irq_table[vec].chain) {
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        return -1;
    }

    /* Mask the line and drain in-flight dispatches BEFORE touching the
     * chain: a CPU that accepted the interrupt before the mask may still
     * be walking the list, and unlinking under its feet is a data race.
     * After the drain no dispatcher can run (line masked + lock held). */
    ioapic_mask_irq(gsi);
    irq_chain_drain(vec);

    for (pp = &irq_table[vec].chain; (n = *pp); pp = &n->next) {
        if (n->handler == handler && n->ctx == ctx)
            break;
    }
    if (!n) {
        /* Never revive a quarantined line on an error path */
        if (!irq_table[vec].quarantined)
            ioapic_unmask_irq(gsi);
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        return -1;
    }
    *pp = n->next;   /* unlink -- drained above, no concurrent walker */
    n->in_use = 0;

    if (!irq_table[vec].chain) {
        /* Last sharer gone: park the vector (tombstone EOI wrapper, not
         * allocator-reusable -- pre-mask accepted deliveries may still
         * dispatch it); the line stays masked */
        irq_park_vector(vec, gsi);
    } else if (!irq_table[vec].quarantined) {
        /* Removing a sharer is NOT a recovery event: a quarantined line
         * stays masked until a new sharer joins (recovery point) */
        ioapic_unmask_irq(gsi);
    }
    spin_unlock_irqrestore(&irq_chain_lock, irqf);
    return 0;
}

int irq_set_affinity(uint32_t gsi, uint64_t cpu_mask)
{
    uint64_t irqf;
    uint32_t cpu;
    struct per_cpu_data *p;
    int rc;

    if (!ioapic_available() || gsi >= 256 || cpu_mask == 0)
        return -1;

    /* PASSIVE_LEVEL-only: irq_chain_lock from a shared-chain handler
     * deadlocks against a concurrent drain (see irq_request_gsi) */
    if (irq_in_isr_context()) {
        klog(LOG_ERROR, "irq",
             "irq_set_affinity(%u) called at DISPATCH_LEVEL or above -- refused",
             (uint64_t)gsi);
        return -1;
    }

    cpu = (uint32_t)__builtin_ctzll(cpu_mask);
    /* MAX_CPUS is the slot bound, NEVER a CPU COUNT (TODO-10 S21). Slots are
     * sparse: with slots 0 and 2 online the live count is 2, so a count-bound
     * check rejected CPU2 -- a valid target -- and the caller could not steer
     * an interrupt to the CPU that survived. Membership is a mask test. */
    if (cpu >= MAX_CPUS)
        return -1;
    p = smp_get_cpu(cpu);
    if (!p)
        return -1;
    if (!smp_cpu_is_online(cpu))
        return -1;

    /* Route-lifetime lock: a concurrent release/free must not be able to
     * retire (or reassign) the GSI between the lookup and the IOAPIC RMW */
    spin_lock_irqsave(&irq_chain_lock, &irqf);
    if (!gsi_to_vector[gsi]) {
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        return -1;
    }
    rc = ioapic_set_destination(gsi, (uint8_t)p->lapic_id);
    spin_unlock_irqrestore(&irq_chain_lock, irqf);
    return rc;
}

void irq_free_gsi(uint32_t gsi)
{
    uint64_t irqf;
    uint8_t vec;
    if (gsi >= 256) return;

    /* Shared-chain teardown drains in-flight dispatches: deadlocks when
     * the caller is itself inside the dispatcher. Refuse from ISR. */
    if (irq_in_isr_context()) {
        klog(LOG_ERROR, "irq",
             "irq_free_gsi(%u) called from interrupt context -- refused",
             (uint64_t)gsi);
        return;
    }

    spin_lock_irqsave(&irq_chain_lock, &irqf);
    vec = gsi_to_vector[gsi];
    if (!vec) {
        spin_unlock_irqrestore(&irq_chain_lock, irqf);
        return;
    }

    /* Mask the IOAPIC entry */
    if (ioapic_available())
        ioapic_mask_irq(gsi);

    /* A shared-chain vector reached through the legacy free API: tear the
     * whole chain down (mask, drain in-flight dispatches, return nodes)
     * so pool entries never leak. */
    if (irq_table[vec].chain) {
        struct irq_shared_node *n;
        /* Drain BEFORE mutating: an already-accepted dispatch may still
         * be walking the chain (line mask does not stop it) */
        irq_chain_drain(vec);
        n = irq_table[vec].chain;
        __atomic_store_n(&irq_table[vec].chain,
                         (struct irq_shared_node *)0, __ATOMIC_RELEASE);
        while (n) {
            struct irq_shared_node *next = n->next;
            n->in_use = 0;
            n = next;
        }
        irq_park_vector(vec, gsi);
    } else if (irq_table[vec].reserved) {
        /* Statically owned ISA window vector: stays reserved; a stale
         * dispatch lands in the idt.c unhandled path (controller-aware
         * EOI), and only the same ISA line ever reuses the slot */
        irq_unregister_nolog(vec);
        gsi_to_vector[gsi] = 0;
        gsi_of_vector[vec] = 0;
        gsi_route_flags[gsi] = 0;
    } else {
        /* Routed legacy vector: same pre-mask acceptance hazard as the
         * shared path -- park, never hand it to a new owner */
        irq_unregister_nolog(vec);
        irq_park_vector(vec, gsi);
    }
    spin_unlock_irqrestore(&irq_chain_lock, irqf);
}

uint64_t irq_gsi_count(uint32_t gsi)
{
    if (gsi >= 256) return 0;
    uint8_t vec = gsi_to_vector[gsi];
    if (!vec) return 0;
    return irq_get_count(vec);
}
