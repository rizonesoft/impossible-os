/* ============================================================================
 * ioapic.c -- I/O APIC driver
 *
 * Routes hardware interrupts through the I/O APIC instead of the legacy PIC.
 * The I/O APIC provides per-IRQ routing to any Local APIC (CPU) with
 * configurable vector, delivery mode, polarity, and trigger mode.
 *
 * Initialization:
 *   1. Read I/O APIC base address from ACPI MADT
 *   2. Read max redirection entries from IOAPICVER register
 *   3. Mask all entries (safe default)
 *   4. Apply MADT Interrupt Source Override entries
 *   5. Route ISA IRQs to BSP's LAPIC with sequential vectors (32+N)
 *
 * ISA IRQ override example (extremely common):
 *   MADT says: IRQ 0 (PIT) → GSI 2
 *   Without this, the PIT interrupt would be lost because the I/O APIC
 *   pin 0 is connected to the PIT on legacy hardware, but on APIC hardware
 *   pin 2 is the PIT. The override table corrects this.
 * ============================================================================ */

#include "kernel/drivers/ioapic.h"
#include "kernel/drivers/lapic.h"
#include "kernel/acpi.h"
#include "kernel/klog.h"
/* ---- State ---- */

static volatile uint32_t *ioapic_base = (volatile uint32_t *)0;
static uint32_t max_redir_entries = 0;
static int ioapic_ready = 0;

/* ---- MMIO register access ---- */

/* I/O APIC uses indirect register access:
 * Write register index to IOREGSEL (offset 0x00),
 * then read/write value at IOWIN (offset 0x10). */

static uint32_t ioapic_read(uint32_t reg)
{
    ioapic_base[0] = reg;          /* IOREGSEL */
    return ioapic_base[4];         /* IOWIN = offset 0x10 / 4 */
}

static void ioapic_write(uint32_t reg, uint32_t val)
{
    ioapic_base[0] = reg;          /* IOREGSEL */
    ioapic_base[4] = val;          /* IOWIN */
}

/* ---- Redirection table access ---- */

/* Each redirection entry is 64 bits, split across two 32-bit registers:
 *   Low  (0x10 + 2*irq): vector, delivery mode, mask, trigger, polarity
 *   High (0x11 + 2*irq): destination APIC ID (bits 24-27) */

static void ioapic_set_entry(uint8_t irq, uint64_t entry)
{
    uint32_t reg_lo = 0x10 + (uint32_t)irq * 2;
    uint32_t reg_hi = reg_lo + 1;

    ioapic_write(reg_hi, (uint32_t)(entry >> 32));
    ioapic_write(reg_lo, (uint32_t)(entry & 0xFFFFFFFF));
}

static uint64_t ioapic_get_entry(uint8_t irq)
{
    uint32_t reg_lo = 0x10 + (uint32_t)irq * 2;
    uint32_t reg_hi = reg_lo + 1;

    uint32_t lo = ioapic_read(reg_lo);
    uint32_t hi = ioapic_read(reg_hi);
    return ((uint64_t)hi << 32) | lo;
}

/* ---- Public API ---- */

uint32_t ioapic_isa_to_gsi(uint8_t isa_irq)
{
    uint32_t i;
    uint32_t count = acpi_get_override_count();

    for (i = 0; i < count; i++) {
        const struct madt_int_override *ovr = acpi_get_override(i);
        if (ovr && ovr->source == isa_irq)
            return ovr->gsi;
    }

    /* No override -- GSI = ISA IRQ number (identity mapping) */
    return (uint32_t)isa_irq;
}

void ioapic_init(void)
{
    uint32_t base_addr;
    uint32_t ver;
    uint32_t i;
    uint32_t bsp_lapic_id;

    base_addr = acpi_get_ioapic_base();
    if (base_addr == 0) {
        klog(LOG_WARN, "ioapic", "No I/O APIC found -- staying with PIC");
        return;
    }

    ioapic_base = (volatile uint32_t *)(uintptr_t)base_addr;

    /* Read version register: bits 16-23 = max redirection entry */
    ver = ioapic_read(0x01);
    max_redir_entries = ((ver >> 16) & 0xFF) + 1;

    /* Get BSP LAPIC ID for routing */
    bsp_lapic_id = lapic_id();

    /* Mask ALL entries first (safe default) */
    for (i = 0; i < max_redir_entries; i++) {
        /* Set mask bit (bit 16), vector 0, destination 0 */
        ioapic_set_entry((uint8_t)i, (uint64_t)1 << 16);
    }

    /* Route standard ISA IRQs to BSP with vectors 32-47.
     * Apply MADT interrupt source overrides for correct pin mapping.
     *
     * CRITICAL: Skip IRQ 2 (8259 cascade) -- it doesn't exist on APIC
     * systems. With the standard IRQ 0→GSI 2 override, IRQ 2 would also
     * map to GSI 2 and overwrite the PIT routing, killing the timer. */
    {
        uint8_t gsi_routed[24];   /* track which GSIs are already routed */
        for (i = 0; i < 24; i++)
            gsi_routed[i] = 0;

        for (i = 0; i < 16; i++) {
            uint32_t gsi;
            uint16_t flags = 0;
            uint32_t j;

            /* Skip cascade IRQ -- doesn't exist on APIC */
            if (i == 2)
                continue;

            gsi = ioapic_isa_to_gsi((uint8_t)i);

            /* Skip if this GSI was already routed (prevents collision) */
            if (gsi < 24 && gsi_routed[gsi])
                continue;

            /* Check for override flags */
            for (j = 0; j < acpi_get_override_count(); j++) {
                const struct madt_int_override *ovr = acpi_get_override(j);
                if (ovr && ovr->source == (uint8_t)i) {
                    flags = ovr->flags;
                    break;
                }
            }

            if (gsi < max_redir_entries) {
                klog(LOG_DEBUG, "ioapic",
                     "  Route: ISA IRQ %u -> GSI %u, vec %u, dest LAPIC %u, flags=0x%x",
                     (uint64_t)i, (uint64_t)gsi, (uint64_t)(32 + i),
                     (uint64_t)bsp_lapic_id, (uint64_t)flags);
                ioapic_route_irq((uint8_t)gsi, (uint8_t)(32 + i),
                                (uint8_t)bsp_lapic_id, flags);
                if (gsi < 24)
                    gsi_routed[gsi] = 1;
            }
        }
    }

    ioapic_ready = 1;

    klog(LOG_INFO, "ioapic",
         "I/O APIC at 0x%x: %u entries, ISA IRQs routed to BSP (LAPIC %u)",
         (uint64_t)base_addr, (uint64_t)max_redir_entries,
         (uint64_t)bsp_lapic_id);
}

void ioapic_route_irq(uint8_t irq, uint8_t vector,
                      uint8_t dest_lapic, uint16_t flags)
{
    uint64_t entry = 0;

    if (!ioapic_base || irq >= max_redir_entries)
        return;

    /* Vector (bits 0-7) */
    entry = (uint64_t)vector;

    /* Delivery mode: fixed (000) -- bits 8-10 */
    /* entry |= 0; */

    /* Polarity (bit 13): from MADT flags bits 0-1
     *   00 = bus default (active high for ISA)
     *   01 = active high
     *   11 = active low */
    if ((flags & 0x03) == 0x03) {
        entry |= (1ULL << 13);  /* active low */
    }

    /* Trigger mode (bit 15): from MADT flags bits 2-3
     *   00 = bus default (edge for ISA)
     *   01 = edge
     *   11 = level */
    if (((flags >> 2) & 0x03) == 0x03) {
        entry |= (1ULL << 15);  /* level-triggered */
    }

    /* Destination LAPIC ID (bits 56-59 in the high dword) */
    entry |= ((uint64_t)dest_lapic << 56);

    /* NOT masked (bit 16 = 0) */

    ioapic_set_entry(irq, entry);
}

void ioapic_mask_irq(uint8_t irq)
{
    uint64_t entry;

    if (!ioapic_base || irq >= max_redir_entries)
        return;

    entry = ioapic_get_entry(irq);
    entry |= (1ULL << 16);  /* set mask bit */
    ioapic_set_entry(irq, entry);
}

void ioapic_unmask_irq(uint8_t irq)
{
    uint64_t entry;

    if (!ioapic_base || irq >= max_redir_entries)
        return;

    entry = ioapic_get_entry(irq);
    entry &= ~(1ULL << 16);  /* clear mask bit */
    ioapic_set_entry(irq, entry);
}

int ioapic_available(void)
{
    return ioapic_ready;
}
