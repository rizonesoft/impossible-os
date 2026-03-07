/* ============================================================================
 * pic.c — 8259 Programmable Interrupt Controller driver
 *
 * Initializes the PIC in cascade mode and remaps IRQs:
 *   Master (PIC1): IRQ 0-7  → interrupt vectors 32-39
 *   Slave  (PIC2): IRQ 8-15 → interrupt vectors 40-47
 *
 * This avoids conflicts with CPU exceptions (0-31).
 * ============================================================================ */

#include "kernel/drivers/pic.h"
#include "kernel/printk.h"

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
    outb(PIC1_DATA, PIC1_OFFSET);   /* Master: IRQ 0-7  → INT 32-39 */
    io_wait();
    outb(PIC2_DATA, PIC2_OFFSET);   /* Slave:  IRQ 8-15 → INT 40-47 */
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

    printk("[OK] PIC remapped (IRQ 0-7 → INT 32-39, IRQ 8-15 → INT 40-47)\n");

    (void)mask1;
    (void)mask2;
}

void pic_send_eoi(uint8_t irq)
{
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

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    val = inb(port) & (uint8_t)~(1 << irq);
    outb(port, val);
}

void pic_disable(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
