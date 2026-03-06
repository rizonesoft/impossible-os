/* ============================================================================
 * idt.h — Interrupt Descriptor Table (x86-64 Long Mode)
 *
 * 256 entries: ISRs 0-31 (CPU exceptions), IRQs 32-47 (hardware), rest unused.
 * ============================================================================ */

#pragma once

#include "types.h"

/* Interrupt stack frame pushed by the CPU + our stubs */
struct interrupt_frame {
    /* Pushed by our common stub (in reverse order) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    /* Pushed by our ISR/IRQ stub */
    uint64_t int_no;        /* interrupt number */
    uint64_t err_code;      /* error code (or 0 if none) */

    /* Pushed by the CPU automatically */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

/* Initialize all 256 IDT entries and load with lidt */
void idt_init(void);

/* Register a custom handler for a specific interrupt number */
typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);
void idt_register_handler(uint8_t n, interrupt_handler_t handler);
