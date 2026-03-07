/* ============================================================================
 * idt.c — Interrupt Descriptor Table (x86-64 Long Mode)
 *
 * Sets up 256 IDT entries for CPU exceptions and hardware IRQs.
 * Provides a C-level interrupt dispatcher called from assembly stubs.
 * ============================================================================ */

#include "kernel/idt.h"
#include "kernel/gdt.h"
#include "kernel/printk.h"
#include "kernel/drivers/framebuffer.h"

/* IDT entry (16 bytes in Long Mode) */
struct idt_entry {
    uint16_t offset_low;    /* offset bits 0-15 */
    uint16_t selector;      /* code segment selector */
    uint8_t  ist;           /* IST index (bits 0-2), rest zero */
    uint8_t  type_attr;     /* type and attributes */
    uint16_t offset_mid;    /* offset bits 16-31 */
    uint32_t offset_high;   /* offset bits 32-63 */
    uint32_t zero;          /* reserved */
} __attribute__((packed));

/* IDT pointer (loaded by lidt) */
struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* The IDT — 256 entries */
static struct idt_entry idt[256];
static struct idt_pointer idtr;

/* Custom handler table (NULL = use default) */
static interrupt_handler_t handlers[256];

/* External assembly: ISR stubs 0-31 */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

/* External assembly: IRQ stubs 32-47 */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

/* External assembly: software interrupt stubs */
extern void isr128(void);  /* syscall — INT 0x80 (user → kernel) */
extern void isr129(void);  /* yield() — cooperative task switch (INT 0x81) */

/* Exception names for debug printing */
static const char *exception_names[32] = {
    "Division By Zero",          /* 0 */
    "Debug",                     /* 1 */
    "Non-Maskable Interrupt",    /* 2 */
    "Breakpoint",                /* 3 */
    "Overflow",                  /* 4 */
    "Bound Range Exceeded",      /* 5 */
    "Invalid Opcode",            /* 6 */
    "Device Not Available",      /* 7 */
    "Double Fault",              /* 8 */
    "Coprocessor Segment Overrun", /* 9 */
    "Invalid TSS",               /* 10 */
    "Segment Not Present",       /* 11 */
    "Stack-Segment Fault",       /* 12 */
    "General Protection Fault",  /* 13 */
    "Page Fault",                /* 14 */
    "Reserved",                  /* 15 */
    "x87 FP Exception",          /* 16 */
    "Alignment Check",           /* 17 */
    "Machine Check",             /* 18 */
    "SIMD FP Exception",         /* 19 */
    "Virtualization Exception",  /* 20 */
    "Control Protection",        /* 21 */
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
    "Hypervisor Injection",      /* 28 */
    "VMM Communication",         /* 29 */
    "Security Exception",        /* 30 */
    "Reserved",                  /* 31 */
};

/* --- Set a single IDT entry --- */
static void idt_set_entry(uint8_t index, uint64_t handler, uint16_t selector,
                           uint8_t ist, uint8_t type_attr)
{
    idt[index].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[index].selector    = selector;
    idt[index].ist         = ist & 0x07;
    idt[index].type_attr   = type_attr;
    idt[index].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[index].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[index].zero        = 0;
}

/* --- C-level interrupt dispatcher (called from assembly) ---
 * Returns the stack frame pointer to restore. Usually the same frame,
 * but the PIT scheduler may return a different task's frame. */
uint64_t isr_handler(struct interrupt_frame *frame)
{
    /* If a custom handler is registered, call it */
    if (handlers[frame->int_no]) {
        return handlers[frame->int_no](frame);
    }

    /* Default handler for CPU exceptions (0-31) */
    if (frame->int_no < 32) {
        printk("\n");
        fb_set_color(FB_COLOR_RED, FB_COLOR_BG_DEFAULT);
        printk("=== KERNEL PANIC ===\n");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("Exception %u: %s\n", frame->int_no,
               exception_names[frame->int_no]);
        printk("Error code: %x\n", frame->err_code);
        printk("RIP: %p  CS: %x\n", frame->rip, frame->cs);
        printk("RSP: %p  SS: %x\n", frame->rsp, frame->ss);
        printk("RFLAGS: %x\n", frame->rflags);
        printk("RAX: %p  RBX: %p\n", frame->rax, frame->rbx);
        printk("RCX: %p  RDX: %p\n", frame->rcx, frame->rdx);
        printk("RSI: %p  RDI: %p\n", frame->rsi, frame->rdi);
        printk("RBP: %p\n", frame->rbp);

        /* Halt on unhandled exceptions */
        printk("\nSystem halted.\n");
        for (;;)
            __asm__ volatile ("cli; hlt");
    }

    return (uint64_t)frame;
}

void idt_register_handler(uint8_t n, interrupt_handler_t handler)
{
    handlers[n] = handler;
}

void idt_init(void)
{
    uint32_t i;

    /* ISR stub addresses */
    uint64_t isr_stubs[32] = {
        (uint64_t)isr0,  (uint64_t)isr1,  (uint64_t)isr2,  (uint64_t)isr3,
        (uint64_t)isr4,  (uint64_t)isr5,  (uint64_t)isr6,  (uint64_t)isr7,
        (uint64_t)isr8,  (uint64_t)isr9,  (uint64_t)isr10, (uint64_t)isr11,
        (uint64_t)isr12, (uint64_t)isr13, (uint64_t)isr14, (uint64_t)isr15,
        (uint64_t)isr16, (uint64_t)isr17, (uint64_t)isr18, (uint64_t)isr19,
        (uint64_t)isr20, (uint64_t)isr21, (uint64_t)isr22, (uint64_t)isr23,
        (uint64_t)isr24, (uint64_t)isr25, (uint64_t)isr26, (uint64_t)isr27,
        (uint64_t)isr28, (uint64_t)isr29, (uint64_t)isr30, (uint64_t)isr31,
    };

    /* IRQ stub addresses */
    uint64_t irq_stubs[16] = {
        (uint64_t)irq0,  (uint64_t)irq1,  (uint64_t)irq2,  (uint64_t)irq3,
        (uint64_t)irq4,  (uint64_t)irq5,  (uint64_t)irq6,  (uint64_t)irq7,
        (uint64_t)irq8,  (uint64_t)irq9,  (uint64_t)irq10, (uint64_t)irq11,
        (uint64_t)irq12, (uint64_t)irq13, (uint64_t)irq14, (uint64_t)irq15,
    };

    /* Zero everything */
    for (i = 0; i < 256; i++) {
        idt_set_entry((uint8_t)i, 0, 0, 0, 0);
        handlers[i] = (interrupt_handler_t)0;
    }

    /* Install ISR stubs 0-31 (CPU exceptions)
     * Type: 0x8E = Present, DPL=0, 64-bit Interrupt Gate */
    for (i = 0; i < 32; i++) {
        idt_set_entry((uint8_t)i, isr_stubs[i], GDT_KERNEL_CODE, 0, 0x8E);
    }

    /* Install IRQ stubs 32-47 (hardware interrupts) */
    for (i = 0; i < 16; i++) {
        idt_set_entry((uint8_t)(i + 32), irq_stubs[i], GDT_KERNEL_CODE, 0, 0x8E);
    }

    /* Install software interrupt stubs */
    /* INT 0x80: syscall — DPL=3 (0xEE) so ring 3 can trigger it */
    idt_set_entry(128, (uint64_t)isr128, GDT_KERNEL_CODE, 0, 0xEE);
    /* INT 0x81: yield — DPL=0 (kernel only) */
    idt_set_entry(129, (uint64_t)isr129, GDT_KERNEL_CODE, 0, 0x8E);

    /* Load the IDT */
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base  = (uint64_t)(uintptr_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));

    printk("[OK] IDT loaded (256 entries, ISR 0-31, IRQ 32-47)\n");
}
