/* ============================================================================
 * ioapic.h -- I/O APIC driver (system-level interrupt routing)
 *
 * The I/O APIC replaces the legacy 8259 PIC for routing hardware interrupts
 * (IRQs) to Local APICs on individual CPUs. Each I/O APIC has 24 redirection
 * entries, one per IRQ input pin. Each entry specifies:
 *   - Destination LAPIC ID (which CPU receives the interrupt)
 *   - Interrupt vector (IDT entry to invoke)
 *   - Delivery mode (fixed, lowest priority, NMI, etc.)
 *   - Trigger mode (edge, level) and polarity (active high/low)
 *
 * The I/O APIC base address is discovered from the ACPI MADT.
 * Interrupt Source Override entries from the MADT remap ISA IRQs
 * (e.g., PIT IRQ 0 → GSI 2 is extremely common on ACPI systems).
 *
 * MMIO registers:
 *   IOREGSEL (0x00) -- write the register index here
 *   IOWIN    (0x10) -- read/write the selected register value
 *
 * Register indices:
 *   0x00       IOAPICID   -- I/O APIC ID
 *   0x01       IOAPICVER  -- version + max redirection entry count
 *   0x10+2*N   REDTBLn    -- redirection table entry N (low 32 bits)
 *   0x11+2*N   REDTBLn    -- redirection table entry N (high 32 bits)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- API ---- */

/* Initialize the I/O APIC: read max entries, apply MADT interrupt source
 * overrides, and route all ISA IRQs to the BSP's LAPIC. */
void ioapic_init(void);

/* Route a GSI to a specific LAPIC with the given vector. The GSI is
 * translated to this IOAPIC's redirection pin (gsi - gsi_base).
 *   gsi:        absolute Global System Interrupt number
 *   vector:     IDT vector number (32-254)
 *   dest_lapic: target LAPIC ID (use 0 for BSP)
 *   flags:      MADT override flags (polarity + trigger mode)
 *               0 = default (edge-triggered, active-high)
 * Returns 0 on success, -1 when the GSI is outside this IOAPIC's range
 * or the IOAPIC is unavailable. Entry is installed MASKED. */
int ioapic_route_irq(uint32_t gsi, uint8_t vector,
                     uint8_t dest_lapic, uint16_t flags);

/* Mask (disable) a GSI in the I/O APIC redirection table.
 * Returns 0 on success, -1 on invalid GSI / no IOAPIC. */
int ioapic_mask_irq(uint32_t gsi);

/* Unmask (enable) a GSI in the I/O APIC redirection table.
 * Returns 0 on success, -1 on invalid GSI / no IOAPIC. */
int ioapic_unmask_irq(uint32_t gsi);

/* Read a GSI's current mask bit, for callers that mask a line temporarily
 * and must put it back exactly as they found it. Returns 1 (masked),
 * 0 (unmasked), or -1 on invalid GSI / no IOAPIC. Unmasking blindly to
 * "restore" is a bug: the line may have been masked before the caller ran,
 * and enabling it would deliver an interrupt with no registered handler. */
int ioapic_irq_masked(uint32_t gsi);

/* Translate an ISA IRQ number to a GSI using MADT overrides.
 * Returns the GSI (may differ from irq due to overrides). */
uint32_t ioapic_isa_to_gsi(uint8_t isa_irq);

/* Rewrite a routed GSI's destination LAPIC ID (physical mode).
 * Returns 0 on success, -1 on invalid GSI / no IOAPIC. */
int ioapic_set_destination(uint32_t gsi, uint8_t dest_lapic);

/* Returns 1 if the I/O APIC is available and initialized */
int ioapic_available(void);
