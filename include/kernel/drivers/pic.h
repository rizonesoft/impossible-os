/* ============================================================================
 * pic.h -- 8259 Programmable Interrupt Controller driver
 *
 * Remaps PIC1 (master) to IRQ 32–39, PIC2 (slave) to IRQ 40–47.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* PIC I/O ports */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

/* Remapped IRQ base offsets.
 * PIC2 lives at 0x70-0x77 (NOT the historical 0x28-0x2F): IRQ14 at
 * 0x2E would alias the DPL=3 Windows NT syscall gate (ntdll INT 2Eh).
 * Must match VECTOR_ISA_IRQ8_BASE in kernel/vectors.h; irq.c reserves
 * the range out of the dynamic allocator. */
#define PIC1_OFFSET  32     /* IRQ 0-7  -> INT 0x20-0x27 */
#define PIC2_OFFSET  0x70   /* IRQ 8-15 -> INT 0x70-0x77 */

/* The ONLY correct ISA-irq-to-vector mapping. The two windows are NOT
 * contiguous: PIC1_OFFSET + irq is wrong for irq >= 8. Returns 0 for
 * out-of-range irq numbers (no valid ISA vector). */
static inline uint8_t isa_irq_to_vector(uint8_t irq)
{
    if (irq < 8)
        return (uint8_t)(PIC1_OFFSET + irq);
    if (irq < 16)
        return (uint8_t)(PIC2_OFFSET + (irq - 8));
    return 0;
}

/* IRQ numbers (after remapping, these are the interrupt vector numbers) */
#define IRQ_TIMER     0
#define IRQ_KEYBOARD  1
#define IRQ_CASCADE   2
#define IRQ_COM2      3
#define IRQ_COM1      4
#define IRQ_LPT2      5
#define IRQ_FLOPPY    6
#define IRQ_LPT1      7
#define IRQ_RTC       8
#define IRQ_MOUSE     12
#define IRQ_FPU       13
#define IRQ_ATA1      14
#define IRQ_ATA2      15

/* Initialize and remap the PIC */
void pic_init(void);

/* Send End-Of-Interrupt to the PIC(s) */
void pic_send_eoi(uint8_t irq);

/* Mask (disable) a specific IRQ line */
void pic_mask_irq(uint8_t irq);

/* Unmask (enable) a specific IRQ line */
void pic_unmask_irq(uint8_t irq);

/* Read a specific IRQ line's current mask bit from the 8259 IMR, for
 * callers that mask a line temporarily and must restore it exactly.
 * Returns 1 (masked), 0 (unmasked), or -1 when no PIC is active. */
int pic_irq_masked(uint8_t irq);

/* Disable both PICs entirely (for APIC migration later) */
void pic_disable(void);

/* Check if the PIC is initialized and active.
 * Returns 0 on APIC-only platforms (PCAT_COMPAT=0). */
int  pic_available(void);

/* Returns 1 when the given ISA IRQ's in-service bit is set in the 8259 ISR.
 * Spurious IRQ7/IRQ15 deliveries have NO ISR bit -- check before EOI. */
int  pic_irq_in_service(uint8_t irq);

/* ---- Unified EOI ----
 * Use irq_eoi() in IRQ handlers instead of pic_send_eoi() directly.
 * When the LAPIC is active, this sends EOI to the LAPIC; otherwise
 * it falls back to the legacy PIC. This makes drivers transparent
 * to the PIC→APIC migration. */
void irq_eoi(uint8_t irq);
