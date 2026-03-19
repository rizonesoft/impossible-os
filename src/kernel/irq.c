/* ============================================================================
 * irq.c — Dynamic IRQ Registration
 *
 * Provides runtime interrupt handler registration, dynamic vector allocation,
 * and per-vector statistics.  Built on top of the IDT dispatch table.
 *
 * Architecture:
 *   irq_register() installs a wrapper into the IDT handlers[] table.
 *   The wrapper calls irq_eoi(), then the user callback.
 *   This keeps drivers clean — they just get (vector, ctx).
 *
 * Vectors 0x20–0x2F (32–47): ISA IRQs — registered by PIT, kbd, mouse
 * Vectors 0x30–0xEF (48–239): dynamic — MSI/MSI-X, VMBus, STIMER
 * Vectors 0xF0–0xFF: reserved (LAPIC timer, spurious, IPI)
 * ============================================================================ */

#include "kernel/irq.h"
#include "kernel/idt.h"
#include "kernel/drivers/pic.h"
#include "kernel/klog.h"

/* ---- Per-vector registration entry ---- */
struct irq_entry {
    irq_handler_t  handler;     /* user callback (NULL = unclaimed) */
    void          *ctx;         /* user context pointer */
    const char    *name;        /* debugging label */
    uint64_t       count;       /* total interrupts on this vector */
    uint8_t        allocated;   /* 1 if allocated via irq_alloc_vector() */
};

static struct irq_entry irq_table[256];

/* ---- IDT-level wrapper ----
 * Installed as the interrupt_handler_t for each registered vector.
 * Dispatches to the irq_handler_t callback after EOI. */
static uint64_t irq_dispatch_wrapper(struct interrupt_frame *frame)
{
    uint8_t vec = (uint8_t)frame->int_no;
    struct irq_entry *e = &irq_table[vec];

    e->count++;

    if (e->handler)
        e->handler(vec, e->ctx);

    /* Send EOI for hardware IRQs (vectors 32+) */
    if (vec >= 32)
        irq_eoi((uint8_t)(vec - 32));

    return (uint64_t)frame;
}

/* ---- Public API ---- */

int irq_register(uint8_t vector, irq_handler_t handler, void *ctx,
                 const char *name)
{
    if (vector < 32)
        return IRQ_ERR_RANGE;   /* CPU exceptions — use idt_register_handler */

    if (irq_table[vector].handler)
        return IRQ_ERR_BUSY;    /* already claimed */

    irq_table[vector].handler = handler;
    irq_table[vector].ctx     = ctx;
    irq_table[vector].name    = name;
    irq_table[vector].count   = 0;

    /* Install our wrapper into the IDT dispatch table */
    idt_register_handler(vector, irq_dispatch_wrapper);

    klog(LOG_DEBUG, "irq", "  registered vec %u -> \"%s\"",
         (uint64_t)vector, name ? name : "?");
    return IRQ_OK;
}

void irq_unregister(uint8_t vector)
{
    if (vector < 32)
        return;

    if (irq_table[vector].handler) {
        klog(LOG_DEBUG, "irq", "  unregistered vec %u (\"%s\", %u hits)",
             (uint64_t)vector,
             irq_table[vector].name ? irq_table[vector].name : "?",
             irq_table[vector].count);
    }

    irq_table[vector].handler   = (irq_handler_t)0;
    irq_table[vector].ctx       = (void *)0;
    irq_table[vector].name      = (const char *)0;
    irq_table[vector].allocated = 0;

    /* Clear the IDT handler so unknown vectors get the default path */
    idt_register_handler(vector, (interrupt_handler_t)0);
}

uint8_t irq_alloc_vector(void)
{
    uint32_t vec;
    for (vec = IRQ_DYNAMIC_BASE; vec <= IRQ_DYNAMIC_END; vec++) {
        if (!irq_table[vec].handler && !irq_table[vec].allocated) {
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
    /* Return pointer to first count field — but entries are structs,
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
    klog(LOG_INFO, "irq", "Dynamic IRQ subsystem ready (vectors 0x30-0xEF allocatable)");
}
