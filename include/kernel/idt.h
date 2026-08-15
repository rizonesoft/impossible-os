/* ============================================================================
 * idt.h -- Interrupt Descriptor Table (x86-64 Long Mode)
 *
 * 256 entries: ISRs 0-31 (CPU exceptions), IRQs 32-47 (hardware), rest unused.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Interrupt stack frame pushed by the CPU + our assembly stubs.
 *
 * WARNING: isr_stubs.asm pushes/pops registers in this exact order.
 * schedule() returns a frame pointer. If fields are reordered, EVERY
 * interrupt, context switch, and ring transition breaks silently.
 * Static asserts below enforce the layout at compile time.
 *
 * Stack frame layout (low address = top of stack = offset 0):
 *
 *   Offset  Size  Field      Pushed by          Pop order
 *   ------  ----  ---------  -----------------  ---------
 *     0       8   r15        common stub last   pop first
 *     8       8   r14        common stub        pop
 *    16       8   r13        common stub        pop
 *    24       8   r12        common stub        pop
 *    32       8   r11        common stub        pop
 *    40       8   r10        common stub        pop
 *    48       8   r9         common stub        pop
 *    56       8   r8         common stub        pop
 *    64       8   rbp        common stub        pop
 *    72       8   rdi        common stub        pop
 *    80       8   rsi        common stub        pop
 *    88       8   rdx        common stub        pop
 *    96       8   rcx        common stub        pop
 *   104       8   rbx        common stub        pop
 *   112       8   rax        common stub first  pop last
 *   120       8   int_no     ISR/IRQ stub       add rsp,16
 *   128       8   err_code   ISR/IRQ stub       add rsp,16
 *   136       8   rip        CPU auto           iretq
 *   144       8   cs         CPU auto           iretq
 *   152       8   rflags     CPU auto           iretq
 *   160       8   rsp        CPU auto           iretq
 *   168       8   ss         CPU auto           iretq
 *   ---     ---
 *   176 total (22 x 8 bytes)
 */
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

/* Compile-time enforcement of interrupt frame layout.
 * These offsets are hardcoded in isr_stubs.asm push/pop order and in
 * task.c stack frame construction. Any change = silent register corruption. */
_Static_assert(sizeof(struct interrupt_frame) == 176,
    "interrupt_frame must be 22 x 8 = 176 bytes");

/* General-purpose registers (pushed by common stub) */
_Static_assert(__builtin_offsetof(struct interrupt_frame, r15) ==   0, "r15 at offset 0");
_Static_assert(__builtin_offsetof(struct interrupt_frame, r8)  ==  56, "r8 at offset 56");
_Static_assert(__builtin_offsetof(struct interrupt_frame, rbp) ==  64, "rbp at offset 64");
_Static_assert(__builtin_offsetof(struct interrupt_frame, rdi) ==  72, "rdi at offset 72 (C arg1)");
_Static_assert(__builtin_offsetof(struct interrupt_frame, rax) == 112, "rax at offset 112");

/* Interrupt metadata (pushed by stub) */
_Static_assert(__builtin_offsetof(struct interrupt_frame, int_no)   == 120, "int_no at offset 120");
_Static_assert(__builtin_offsetof(struct interrupt_frame, err_code) == 128, "err_code at offset 128");

/* CPU-pushed iretq frame (must be last 5 fields, contiguous, in order) */
_Static_assert(__builtin_offsetof(struct interrupt_frame, rip)    == 136, "rip at offset 136");
_Static_assert(__builtin_offsetof(struct interrupt_frame, cs)     == 144, "cs at offset 144");
_Static_assert(__builtin_offsetof(struct interrupt_frame, rflags) == 152, "rflags at offset 152");
_Static_assert(__builtin_offsetof(struct interrupt_frame, rsp)    == 160, "rsp at offset 160");
_Static_assert(__builtin_offsetof(struct interrupt_frame, ss)     == 168, "ss at offset 168");

/* Initialize all 256 IDT entries and load with lidt */
void idt_init(void);

/* Non-zero once idt_init() has loaded the kernel IDT via lidt.
 *
 * Before this returns true, the IDTR still points at the UEFI IDT, so the
 * C-level handlers[] table consulted by our ISR stubs is not reachable --
 * any custom #GP handler installed via idt_register_handler() is effectively
 * invisible to the CPU. msr_try_read() gates on this to avoid crashing on
 * KVM, which (unlike WHPX) raises real #GP for unavailable MSRs. */
int idt_is_loaded(void);

/* Interrupt handler type -- returns stack frame pointer (for preemptive switching).
 * Most handlers return the same frame; the scheduler may return a different
 * task's frame pointer to switch contexts. */
typedef uint64_t (*interrupt_handler_t)(struct interrupt_frame *frame);
void idt_register_handler(uint8_t n, interrupt_handler_t handler);
/* Same as idt_register_handler but silent: for EXPECTED temporary swaps (the
 * MSR #GP probe install/restore) where overwriting a resident handler is by
 * design, so the overwrite warning would be pure boot-log noise. Unexpected
 * permanent overwrites must still use idt_register_handler (which warns). */
void idt_register_handler_quiet(uint8_t n, interrupt_handler_t handler);
interrupt_handler_t idt_get_handler(uint8_t n);

/* ---- NMI nesting depth (ARCH: x86-64) ----
 *
 * idt_in_nmi() is nonzero while THIS CPU is inside an NMI handler. The counter
 * is moved by the DEDICATED vector-2 stub in isr_stubs.asm (S22), not by C: the
 * stub raises it ahead of its own error-code push and lowers it after the
 * register pops and the frame pop. That covers the error-code and vector
 * pushes, the shared prologue, the frame load, and the integrity and GS checks
 * -- the entry sequence isr_handler could not reach from inside C. It is not
 * the WHOLE entry: see the exact boundary below.
 *
 * The exact boundary, stated precisely because a looser wording invites a
 * reader to assume more coverage than exists: the raise is PRECEDED by four
 * stack writes (the CPUID clobber set the marker needs to index itself), and
 * the lower is FOLLOWED by one register restore, the CS test, the conditional
 * VERW block, the swapgs and the IRETQ -- all of which run with the depth
 * already down. The lower sits before that return block deliberately, so VERW
 * stays the last instruction to touch memory before returning and no
 * serializing CPUID lands in the post-swapgs user-GS window. Both residuals are
 * argued at their site in isr_stubs.asm.
 *
 * The panic path is the consumer: a fault taken inside the NMI handler re-enters
 * panic with an INNER vector, so a context predicate reading only frame->int_no
 * would call the abort ordinary and re-enable the fault-suppressed kernel read --
 * whose fixup IRETQs and re-arms NMI while the outer NMI still owns IST2.
 * idt_in_nmi() is the depth signal that vector cannot supply.
 *
 * State is keyed by the CPUID-derived APIC id, NOT per_cpu_data, so it is
 * readable from the GS-independent panic emitters. idt_nmi_depth_raw() and the
 * enter/exit pair are the same arithmetic in C, exposed so a unit test can drive
 * the semantics the assembly implements; production code reads idt_in_nmi() and
 * never moves the counter itself. */
int      idt_in_nmi(void);

/* The same question keyed on an ALREADY-DERIVED panic-safe id, so a caller that
 * has one does not pay a second serializing CPUID for it. idt_in_nmi() is this
 * with the derivation folded in.
 *
 * It exists for the panic prefix. cpu_panic_safe_apic_id() executes CPUID,
 * which serializes and exits to the hypervisor under KVM and WHPX, and every
 * panic ingress already derives the id for its own park guard -- so deriving it
 * again inside the context classifier put a second VM exit in front of the
 * evidence capture, on the one path whose whole budget is
 * instructions-before-the-record-is-durable. */
int      idt_in_nmi_for(uint32_t apic_id);
uint32_t idt_nmi_depth_raw(void);
void     idt_nmi_enter(void);
void     idt_nmi_exit(void);

/* The counter itself, one entry per panic-safe CPU id. Declared only because
 * isr_stubs.asm indexes it directly and the stub tests resolve the emitted
 * relocation against it -- C code reads idt_in_nmi() and never touches this. */
extern volatile uint32_t g_nmi_depth[];

/* Promote vector `n`'s gate to DPL=3 so ring-3 code may raise it with `INT n`
 * (e.g. INT3 breakpoint, INTO overflow, INT 0x29 __fastfail). CPU-generated
 * exceptions ignore gate DPL, so this is only needed for software-INT vectors.
 * No-op (with a warning) until the kernel IDT is loaded -- call after idt_init(). */
void idt_set_user_callable(uint8_t n);
