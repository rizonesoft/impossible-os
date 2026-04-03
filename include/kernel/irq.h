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

/* Initialize the IRQ subsystem. Called once during boot. */
void irq_init(void);
