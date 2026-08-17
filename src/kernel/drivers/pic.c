/* ============================================================================
 * pic.c -- 8259 Programmable Interrupt Controller driver
 *
 * Initializes the PIC in cascade mode and remaps IRQs:
 *   Master (PIC1): IRQ 0-7  -> interrupt vectors 0x20-0x27
 *   Slave  (PIC2): IRQ 8-15 -> interrupt vectors 0x70-0x77
 *
 * The slave window is NOT contiguous with the master: the historical
 * 0x28-0x2F placement put IRQ14 on the DPL=3 NT syscall gate (INT 0x2E).
 * Use isa_irq_to_vector() for the mapping; irq.c reserves 0x70-0x77.
 * ============================================================================ */

#include "kernel/drivers/pic.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/klog.h"
/* PIC state -- set by pic_init(), cleared by pic_disable() */
static int pic_ready = 0;

/* Inline port I/O helpers */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Small delay for PIC I/O (PIC needs time between writes) */
static inline void io_wait(void)
{
    outb(0x80, 0); /* Write to unused port 0x80 */
}

/* ICW1: Initialization Command Word 1 */
#define ICW1_ICW4       0x01    /* ICW4 will be sent */
#define ICW1_INIT       0x10    /* Initialization command */

/* ICW4: Initialization Command Word 4 */
#define ICW4_8086       0x01    /* 8086/88 mode */

/* OCW2: End of Interrupt */
#define PIC_EOI         0x20

void pic_init(void)
{
    uint8_t mask1, mask2;

    /* Save current interrupt masks */
    mask1 = inb(PIC1_DATA);
    mask2 = inb(PIC2_DATA);

    /* ICW1: Start initialization sequence (cascade mode) */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: Set interrupt vector offsets */
    outb(PIC1_DATA, PIC1_OFFSET);   /* Master: IRQ 0-7  -> INT 32-39 */
    io_wait();
    outb(PIC2_DATA, PIC2_OFFSET);   /* Slave:  IRQ 8-15 -> INT 0x70-0x77 */
    io_wait();

    /* ICW3: Configure cascade wiring */
    outb(PIC1_DATA, 0x04);   /* Master: slave PIC on IRQ2 (bit 2) */
    io_wait();
    outb(PIC2_DATA, 0x02);   /* Slave: cascade identity = 2 */
    io_wait();

    /* ICW4: Set 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Restore saved masks (mask all IRQs initially for safety) */
    outb(PIC1_DATA, 0xFF);  /* Mask all master IRQs */
    outb(PIC2_DATA, 0xFF);  /* Mask all slave IRQs */

    /* Unmask cascade IRQ2 so slave PIC can reach the CPU */
    pic_unmask_irq(IRQ_CASCADE);

    pic_ready = 1;

    klog(LOG_INFO, "irq", "PIC remapped (IRQ 0-7 -> INT 0x20-0x27, IRQ 8-15 -> INT 0x70-0x77)");

    (void)mask1;
    (void)mask2;
}

int pic_available(void)
{
    return pic_ready;
}

void pic_send_eoi(uint8_t irq)
{
    if (!pic_ready)
        return;
    /* If the IRQ came from the slave PIC (IRQ 8-15),
     * we must send EOI to both slave AND master */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask_irq(uint8_t irq)
{
    uint16_t port;
    uint8_t val;

    if (!pic_ready)
        return;  /* No PIC -- IOAPIC handles masking */

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    val = inb(port) | (uint8_t)(1 << irq);
    outb(port, val);
}

void pic_unmask_irq(uint8_t irq)
{
    uint16_t port;
    uint8_t val;

    if (!pic_ready)
        return;  /* No PIC -- IOAPIC handles unmasking */

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    val = inb(port) & (uint8_t)~(1 << irq);
    outb(port, val);
}

int pic_irq_masked(uint8_t irq)
{
    uint16_t port;

    if (!pic_ready)
        return -1;  /* No PIC -- nothing to report */

    /* Read-back accessor for save/restore callers. The 8259 IMR is
     * directly readable, so a caller that masks a line temporarily can put
     * it back exactly as it found it instead of unmasking blindly. */
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    return (inb(port) & (uint8_t)(1 << irq)) ? 1 : 0;
}

void pic_disable(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    pic_ready = 0;
}

void irq_eoi(uint8_t irq)
{
    /* Use LAPIC EOI only when the full APIC system is active (IOAPIC routing
     * interrupts AND PIC disabled).  If only the LAPIC is present but the PIC
     * is still routing (no IOAPIC), we MUST send EOI to the PIC -- otherwise
     * the PIC blocks all further interrupts of that priority level.
     * irq > 15 marks a non-ISA vector (LAPIC-delivered even on PIC-routed
     * systems, e.g. the LAPIC timer): never EOI the 8259 for those. */
    if (ioapic_available() || irq > 15) {
        lapic_eoi();
    } else {
        pic_send_eoi(irq);
    }
}

int pic_irq_in_service(uint8_t irq)
{
    uint8_t isr;
    if (irq > 15)
        return 0;
    if (irq < 8) {
        outb(PIC1_CMD, 0x0B);          /* OCW3: read ISR */
        isr = inb(PIC1_CMD);
        return (isr >> irq) & 1;
    }
    outb(PIC2_CMD, 0x0B);
    isr = inb(PIC2_CMD);
    return (isr >> (irq - 8)) & 1;
}
