/* ============================================================================
 * irq.h -- Dynamic IRQ Registration API
 *
 * Allows drivers to register/unregister interrupt handlers at runtime.
 * Built on top of the IDT dispatch table (idt_register_handler).
 *
 * Two handler types:
 *   - irq_handler_t:  simple callback for device IRQs (keyboard, mouse, NIC)
 *   - interrupt_handler_t:  frame-aware handler for PIT/scheduler, exceptions
 *
 * Use irq_register() for device IRQs, idt_register_handler() for exceptions.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Handler callback type ----
 * Receives the vector number and a user-provided context pointer.
 * Called with interrupts disabled. Must not block. */
typedef void (*irq_handler_t)(uint8_t vector, void *ctx);

/* ---- Error codes ---- */
#define IRQ_OK         0
#define IRQ_ERR_BUSY  -1   /* Vector already claimed */
#define IRQ_ERR_RANGE -2   /* Vector out of valid range */
#define IRQ_ERR_NONE  -3   /* No free vectors available */

/* ---- Dynamic vector allocation range ----
 * Vectors 0x30–0xEF are available for MSI/MSI-X and VMBus.
 * Vectors 0x20–0x2F (32–47) are ISA IRQs (PIT, kbd, mouse).
 * Vectors 0x00–0x1F are CPU exceptions (non-registrable).
 * Vectors 0xF0–0xFF are reserved (LAPIC timer, spurious, IPI). */
#define IRQ_DYNAMIC_BASE  0x30
#define IRQ_DYNAMIC_END   0xEF

/* ---- Registration API ---- */

/* Register a handler for a specific vector.
 * Returns IRQ_OK on success, IRQ_ERR_BUSY if already claimed.
 * name: short label for debugging (e.g., "pit", "ps2_kbd", "vmbus"). */
int  irq_register(uint8_t vector, irq_handler_t handler, void *ctx,
                  const char *name);

/* Unregister a handler, releasing the vector. */
void irq_unregister(uint8_t vector);

/* ---- Dynamic vector allocation ---- */

/* Find and claim an unused vector in the dynamic range [0x30, 0xEF].
 * Returns the vector number, or 0 on failure (no free vectors). */
uint8_t irq_alloc_vector(void);

/* Release a dynamically allocated vector back to the pool. */
void    irq_free_vector(uint8_t vector);

/* ---- Query API ---- */

/* Get the name of the handler registered on a vector (or NULL). */
const char *irq_get_name(uint8_t vector);

/* Get the interrupt count for a vector. */
uint64_t irq_get_count(uint8_t vector);

/* Get the interrupt count table pointer (256 entries).
 * Used by Task Manager for per-vector statistics. */
const uint64_t *irq_get_counts(void);

/* Translate a vector back to its ISA irq number (0-15) for PIC EOI
 * routing. Returns 0xFF for non-ISA vectors (LAPIC EOI only). */
uint8_t irq_vector_to_isa(uint8_t vec);

/* ---- High-level GSI-based API ---- */

/* Request an IRQ by GSI number. Allocates a vector, programs the IOAPIC
 * redirection entry (using MADT override flags if applicable), and
 * registers the handler. Returns the allocated vector, or 0 on failure. */
uint8_t irq_request_gsi(uint32_t gsi, irq_handler_t handler, void *ctx,
                         const char *name);

/* Release a GSI: mask IOAPIC entry, unregister handler, free vector. */
void irq_free_gsi(uint32_t gsi);

/* Get interrupt fire count for a GSI (looks up the mapped vector). */
uint64_t irq_gsi_count(uint32_t gsi);

/* ---- Shared GSI API (PCI INTx lines can be shared by several devices) ----
 * Shared handlers return a claim status so the dispatcher can tell a
 * serviced level interrupt from line noise / another device's interrupt. */
#define IRQ_NONE     0   /* not my interrupt */
#define IRQ_HANDLED  1   /* device serviced */
typedef int (*irq_shared_handler_t)(uint8_t vector, void *ctx);

/* Request a GSI with explicit MADT-style flags (PCI INTx is level-low =
 * 0x0F; a MADT override naming the GSI is authoritative over the caller
 * flags, including override flags 0 = conforms-to-bus) and an optional
 * shared mode. Shared registrants chain on one vector; every handler in
 * the chain runs per interrupt and reports IRQ_HANDLED / IRQ_NONE.
 * Returns the vector, or 0 on failure. Non-shared requests on a vector
 * that already has a chain fail (and vice versa). PASSIVE_LEVEL-only
 * (current_irql < DISPATCH_LEVEL): callers at DISPATCH_LEVEL or above
 * are refused with 0 (joining drains in-flight dispatches). */
uint8_t irq_request_gsi_ex(uint32_t gsi, irq_shared_handler_t handler,
                           void *ctx, const char *name, uint16_t flags,
                           int shared);

/* Remove ONE shared registrant from a GSI chain (matched by handler+ctx).
 * Masks the line during chain mutation and drains in-flight dispatches
 * before the node is reusable. Parks the vector when the chain empties.
 * Returns 0 on success, -1 when not found. PASSIVE_LEVEL-only
 * (current_irql < DISPATCH_LEVEL, broader than just ISR context):
 * refused with -1 -- a DISPATCH_LEVEL caller would deadlock the drain. */
int irq_release_gsi_shared(uint32_t gsi, irq_shared_handler_t handler,
                           void *ctx);

/* Route a GSI's interrupt delivery to the first set bit of cpu_mask
 * (logical CPU index). The CPU must be online. Returns 0 on success,
 * -1 on unrouted GSI / offline CPU / empty mask / no IOAPIC.
 * PASSIVE_LEVEL-only (current_irql < DISPATCH_LEVEL): refused with -1. */
int irq_set_affinity(uint32_t gsi, uint64_t cpu_mask);

/* Reverse map: the GSI a vector was requested for, or 0xFFFFFFFF. */
uint32_t irq_gsi_for_vector(uint8_t vec);

/* 1 when the vector was auto-masked by the storm quarantine. */
int irq_vector_quarantined(uint8_t vec);

/* Statically reserve a dynamic-range vector for a handler that installs
 * straight into the IDT (no irq_table footprint), e.g. a firmware-derived
 * ACPI SCI vector. The allocator will never return it. Returns 0 on
 * success or when the vector is outside the dynamic range (nothing to
 * reserve), -1 when a dynamic registration already owns the vector. */
int irq_reserve_vector(uint8_t vector, const char *name);

/* Initialize the IRQ subsystem. Called once during boot. */
void irq_init(void);
