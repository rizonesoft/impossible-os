/* ============================================================================
 * pic.h — 8259 Programmable Interrupt Controller driver
 *
 * Remaps PIC1 (master) to IRQ 32–39, PIC2 (slave) to IRQ 40–47.
 * ============================================================================ */

#pragma once

#include "types.h"

/* PIC I/O ports */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

/* Remapped IRQ base offsets */
#define PIC1_OFFSET  32     /* IRQ 0-7  → INT 32-39 */
#define PIC2_OFFSET  40     /* IRQ 8-15 → INT 40-47 */

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

/* Disable both PICs entirely (for APIC migration later) */
void pic_disable(void);
