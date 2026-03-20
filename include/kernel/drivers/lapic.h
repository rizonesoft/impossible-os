/* ============================================================================
 * lapic.h — Local APIC driver (per-CPU interrupt controller)
 *
 * Each CPU has its own Local APIC for receiving interrupts. On x86-64, the
 * LAPIC is accessed via MMIO at a physical address discovered from the ACPI
 * MADT (default 0xFEE00000, identity-mapped).
 *
 * The LAPIC replaces the legacy 8259 PIC for interrupt acknowledgement (EOI)
 * and adds support for inter-processor interrupts (IPI) needed by SMP.
 *
 * Key registers (offset from LAPIC base):
 *   0x020  LAPIC ID        — identifies this CPU
 *   0x030  LAPIC Version   — hardware version + max LVT entry
 *   0x080  Task Priority   — interrupt priority threshold
 *   0x0B0  EOI             — write 0 to acknowledge interrupt
 *   0x0F0  Spurious Vector — enable LAPIC + set spurious interrupt vector
 *   0x300  ICR Low         — Interrupt Command Register (IPI destination)
 *   0x310  ICR High        — IPI target APIC ID
 *   0x320  LVT Timer       — local timer configuration
 *   0x380  Timer Initial   — timer countdown start value
 *   0x390  Timer Current   — timer current countdown value
 *   0x3E0  Timer Divide    — clock divider for timer
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* LAPIC register offsets */
#define LAPIC_REG_ID          0x020
#define LAPIC_REG_VERSION     0x030
#define LAPIC_REG_TPR         0x080  /* Task Priority Register */
#define LAPIC_REG_EOI         0x0B0  /* End of Interrupt */
#define LAPIC_REG_SVR         0x0F0  /* Spurious Vector Register */
#define LAPIC_REG_ESR         0x280  /* Error Status Register */
#define LAPIC_REG_ICR_LO      0x300  /* Interrupt Command Register (low) */
#define LAPIC_REG_ICR_HI      0x310  /* Interrupt Command Register (high) */
#define LAPIC_REG_LVT_TIMER   0x320  /* LVT Timer */
#define LAPIC_REG_LVT_LINT0   0x350  /* LVT LINT0 */
#define LAPIC_REG_LVT_LINT1   0x360  /* LVT LINT1 */
#define LAPIC_REG_LVT_ERROR   0x370  /* LVT Error */
#define LAPIC_REG_TIMER_ICR   0x380  /* Timer Initial Count */
#define LAPIC_REG_TIMER_CCR   0x390  /* Timer Current Count */
#define LAPIC_REG_TIMER_DCR   0x3E0  /* Timer Divide Configuration */

/* SVR flags */
#define LAPIC_SVR_ENABLE      0x100  /* bit 8: APIC Software Enable */
#define LAPIC_SPURIOUS_VECTOR 0xFF   /* spurious interrupt vector */

/* ICR delivery modes */
#define ICR_FIXED             0x00000000
#define ICR_INIT              0x00000500
#define ICR_STARTUP           0x00000600

/* ICR destination shorthand */
#define ICR_DEST_FIELD        0x00000000  /* use destination field */
#define ICR_DEST_SELF         0x00040000
#define ICR_DEST_ALL          0x00080000
#define ICR_DEST_ALL_BUT_SELF 0x000C0000

/* ICR level / trigger */
#define ICR_LEVEL_ASSERT      0x00004000
#define ICR_LEVEL_DEASSERT    0x00000000
#define ICR_TRIGGER_EDGE      0x00000000
#define ICR_TRIGGER_LEVEL     0x00008000

/* LVT timer modes */
#define LVT_TIMER_ONESHOT     0x00000000
#define LVT_TIMER_PERIODIC    0x00020000
#define LVT_MASKED            0x00010000

/* Timer divider values (for DCR register) */
#define TIMER_DIV_1           0x0B
#define TIMER_DIV_16          0x03
#define TIMER_DIV_128         0x0A

/* IPI vectors (high end to avoid conflicts with hardware IRQs) */
#define IPI_VECTOR_RESCHEDULE     0xFD
#define IPI_VECTOR_TLB_SHOOTDOWN  0xFE

/* ---- API ---- */

/* Initialize the BSP's Local APIC. Must be called after acpi_init(). */
void lapic_init(void);

/* Initialize an AP's Local APIC (called from ap_main). */
void lapic_init_ap(void);

/* Send End-of-Interrupt to the LAPIC. Replaces pic_send_eoi(). */
void lapic_eoi(void);

/* Read the current CPU's LAPIC ID */
uint32_t lapic_id(void);

/* Read a LAPIC register */
uint32_t lapic_read(uint32_t reg);

/* Write a LAPIC register */
void lapic_write(uint32_t reg, uint32_t val);

/* Send a fixed IPI to a specific CPU (by LAPIC ID) */
void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector);

/* Send a fixed IPI to all CPUs except self */
void lapic_send_ipi_all_but_self(uint8_t vector);

/* Send INIT IPI to target CPU (for AP bringup) */
void lapic_send_init(uint8_t target_apic_id);

/* Send Startup IPI (SIPI) to target CPU with startup vector page */
void lapic_send_sipi(uint8_t target_apic_id, uint8_t vector_page);

/* ---- LAPIC Timer ---- */

/* Dedicated vector for LAPIC timer.
 * Uses vector 34 (IRQ 2 cascade — doesn't exist on APIC systems).
 * Must be within 32-47 (the IDT-stub-covered hardware IRQ range). */
#define LAPIC_TIMER_VECTOR    34

/* Calibrate the LAPIC timer frequency using PIT channel 2.
 * Must be called after lapic_init() and before lapic_timer_init().
 * Uses a 10ms PIT-based measurement window, with fallback to
 * hardcoded frequency if calibration hangs (VBox NEM / Hyper-V). */
void lapic_timer_calibrate(void);

/* Start the LAPIC timer in periodic mode at the given frequency (Hz)
 * on LAPIC_TIMER_VECTOR. Uses the calibrated frequency from
 * lapic_timer_calibrate(), or a hardcoded fallback.
 * Registers a timer IRQ handler that calls the scheduler, and
 * optionally increments tick_count (if LAPIC is tick source). */
void lapic_timer_init(uint32_t hz);

/* Set whether the LAPIC timer drives tick_count.
 *   enable=1: LAPIC timer increments ticks (APIC-only, no PIT)
 *   enable=0: PIT drives ticks, LAPIC only does scheduling
 * Call before lapic_timer_init(). */
void lapic_timer_set_tick_source(int enable);

/* Returns the calibrated LAPIC timer ticks per millisecond (0 if uncalibrated). */
uint32_t lapic_timer_ticks_per_ms(void);

/* Returns 1 if the LAPIC is available and initialized */
int lapic_available(void);
