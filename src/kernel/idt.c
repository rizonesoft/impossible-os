/* ============================================================================
 * idt.c -- Interrupt Descriptor Table (x86-64 Long Mode)
 *
 * Sets up 256 IDT entries for ALL vectors -- CPU exceptions, hardware IRQs,
 * software interrupts, and dynamic/synthetic vectors.
 *
 * Every vector has a valid ISR stub.  This prevents #GP faults when hardware
 * or a hypervisor delivers an interrupt to an otherwise-empty IDT slot
 * (observed on Hyper-V Gen 2: VMBus SINT on vector 0xF6 → #GP).
 *
 * Provides a C-level interrupt dispatcher called from assembly stubs.
 * ============================================================================ */

#include "kernel/idt.h"
#include "kernel/gdt.h"
#include "kernel/vectors.h"
#include "kernel/klog.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/serial.h"   /* abort-safe emergency serial (fatal paths) */
#include "kernel/panic.h"
#include "kernel/sched/irql.h"
#include "kernel/sched/transition_ring.h" /* fast-path transition ring */
#include "kernel/smp.h"
#include "kernel/irq.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/ioapic.h"

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

/* The IDT -- 256 entries */
static struct idt_entry idt[256];
static struct idt_pointer idtr;

/* Custom handler table (NULL = use default) */
static interrupt_handler_t handlers[256];

/* Set to 1 once lidt has loaded the kernel IDT. Before that the IDTR still
 * points at the UEFI IDT, so installing a handler in handlers[] has no
 * effect -- the kernel ISR stubs that consult it are unreachable. */
static volatile int s_idt_loaded;

/* ---- Per-vector warning rate limiter ----
 * First hit logs full details; subsequent hits are counted silently.
 * Prevents log flooding from repeated synthetic interrupts. */
static uint64_t unhandled_counts[256];

/* Unhandled-vector fire count at which the storm quarantine masks the
 * line at its owning controller (a stuck level line livelocks the CPU) */
#define IDT_STORM_QUARANTINE_LIMIT  10000u

/* ---- External assembly: ISR stubs 0-31 ---- */
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

/* ---- External assembly: IRQ stubs 32-47 ---- */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

/* ---- External assembly: software interrupt stubs ---- */
extern void isr128(void);  /* syscall -- INT 0x80 (user → kernel) */
extern void isr129(void);  /* yield() -- cooperative task switch (INT 0x81) */

/* ---- External assembly: dynamic/synthetic stubs 48-255 ---- */
extern void isr48(void);  extern void isr49(void);  extern void isr50(void);
extern void isr51(void);  extern void isr52(void);  extern void isr53(void);
extern void isr54(void);  extern void isr55(void);  extern void isr56(void);
extern void isr57(void);  extern void isr58(void);  extern void isr59(void);
extern void isr60(void);  extern void isr61(void);  extern void isr62(void);
extern void isr63(void);  extern void isr64(void);  extern void isr65(void);
extern void isr66(void);  extern void isr67(void);  extern void isr68(void);
extern void isr69(void);  extern void isr70(void);  extern void isr71(void);
extern void isr72(void);  extern void isr73(void);  extern void isr74(void);
extern void isr75(void);  extern void isr76(void);  extern void isr77(void);
extern void isr78(void);  extern void isr79(void);  extern void isr80(void);
extern void isr81(void);  extern void isr82(void);  extern void isr83(void);
extern void isr84(void);  extern void isr85(void);  extern void isr86(void);
extern void isr87(void);  extern void isr88(void);  extern void isr89(void);
extern void isr90(void);  extern void isr91(void);  extern void isr92(void);
extern void isr93(void);  extern void isr94(void);  extern void isr95(void);
extern void isr96(void);  extern void isr97(void);  extern void isr98(void);
extern void isr99(void);  extern void isr100(void); extern void isr101(void);
extern void isr102(void); extern void isr103(void); extern void isr104(void);
extern void isr105(void); extern void isr106(void); extern void isr107(void);
extern void isr108(void); extern void isr109(void); extern void isr110(void);
extern void isr111(void); extern void isr112(void); extern void isr113(void);
extern void isr114(void); extern void isr115(void); extern void isr116(void);
extern void isr117(void); extern void isr118(void); extern void isr119(void);
extern void isr120(void); extern void isr121(void); extern void isr122(void);
extern void isr123(void); extern void isr124(void); extern void isr125(void);
extern void isr126(void); extern void isr127(void);
/* 128-129 declared above */
extern void isr130(void); extern void isr131(void); extern void isr132(void);
extern void isr133(void); extern void isr134(void); extern void isr135(void);
extern void isr136(void); extern void isr137(void); extern void isr138(void);
extern void isr139(void); extern void isr140(void); extern void isr141(void);
extern void isr142(void); extern void isr143(void); extern void isr144(void);
extern void isr145(void); extern void isr146(void); extern void isr147(void);
extern void isr148(void); extern void isr149(void); extern void isr150(void);
extern void isr151(void); extern void isr152(void); extern void isr153(void);
extern void isr154(void); extern void isr155(void); extern void isr156(void);
extern void isr157(void); extern void isr158(void); extern void isr159(void);
extern void isr160(void); extern void isr161(void); extern void isr162(void);
extern void isr163(void); extern void isr164(void); extern void isr165(void);
extern void isr166(void); extern void isr167(void); extern void isr168(void);
extern void isr169(void); extern void isr170(void); extern void isr171(void);
extern void isr172(void); extern void isr173(void); extern void isr174(void);
extern void isr175(void); extern void isr176(void); extern void isr177(void);
extern void isr178(void); extern void isr179(void); extern void isr180(void);
extern void isr181(void); extern void isr182(void); extern void isr183(void);
extern void isr184(void); extern void isr185(void); extern void isr186(void);
extern void isr187(void); extern void isr188(void); extern void isr189(void);
extern void isr190(void); extern void isr191(void); extern void isr192(void);
extern void isr193(void); extern void isr194(void); extern void isr195(void);
extern void isr196(void); extern void isr197(void); extern void isr198(void);
extern void isr199(void); extern void isr200(void); extern void isr201(void);
extern void isr202(void); extern void isr203(void); extern void isr204(void);
extern void isr205(void); extern void isr206(void); extern void isr207(void);
extern void isr208(void); extern void isr209(void); extern void isr210(void);
extern void isr211(void); extern void isr212(void); extern void isr213(void);
extern void isr214(void); extern void isr215(void); extern void isr216(void);
extern void isr217(void); extern void isr218(void); extern void isr219(void);
extern void isr220(void); extern void isr221(void); extern void isr222(void);
extern void isr223(void); extern void isr224(void); extern void isr225(void);
extern void isr226(void); extern void isr227(void); extern void isr228(void);
extern void isr229(void); extern void isr230(void); extern void isr231(void);
extern void isr232(void); extern void isr233(void); extern void isr234(void);
extern void isr235(void); extern void isr236(void); extern void isr237(void);
extern void isr238(void); extern void isr239(void); extern void isr240(void);
extern void isr241(void); extern void isr242(void); extern void isr243(void);
extern void isr244(void); extern void isr245(void); extern void isr246(void);
extern void isr247(void); extern void isr248(void); extern void isr249(void);
extern void isr250(void); extern void isr251(void); extern void isr252(void);
extern void isr253(void); extern void isr254(void); extern void isr255(void);

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
 *
 * IRQL integration (NT model):
 *   1. On entry: save current IRQL, raise to the mapped DIRQL for this vector
 *   2. Call handler (which manages its own EOI)
 *   3. On exit: restore prior IRQL
 *
 * IRQL tracking is software-only -- we do NOT write LAPIC TPR here.
 * The LAPIC hardware already masks lower-priority vectors via the ISR/PPR
 * mechanism during interrupt delivery.  Explicit TPR writes are reserved
 * for KeRaiseIrql/KeLowerIrql when kernel code intentionally changes level.
 *
 * For nested interrupts, the saved IRQL is per-invocation on the stack,
 * so LIFO unwinding is automatic.
 *
 * CPU exceptions (0-31) do NOT raise IRQL -- they are synchronous faults
 * and execute at the IRQL of the faulting code.
 *
 * Returns the stack frame pointer to restore. Usually the same frame,
 * but the PIT scheduler may return a different task's frame. */
/* ---- Per-CPU NMI nesting depth (ARCH: x86-64) ----
 *
 * Nonzero means THIS CPU is executing inside an NMI handler, at any depth. It is
 * the signal the panic path needs and could not previously get: a fault taken
 * INSIDE the NMI handler re-enters panic with frame->int_no naming the inner
 * vector (a #PF, say), so a predicate reading only the vector reclassifies the
 * abort as ordinary and re-enables the fault-suppressed kernel read -- whose
 * fixup returns via IRETQ and re-arms NMI delivery while the outer NMI still
 * owns IST2. A second NMI then resets RSP to the IST2 top and overwrites the
 * outer frames.
 *
 * Indexed by the CPUID-derived 8-bit initial APIC ID rather than held in
 * per_cpu_data, because the consumers are panic-path emitters that are
 * deliberately GS-INDEPENDENT: the pre-arbitration dump must work when gs:0 is
 * exactly what cannot be trusted. 256 entries covers the field width exactly, so
 * an id can never index out of range. Each entry is written only by the CPU that
 * owns it, so plain relaxed atomics suffice and nothing here can block.
 *
 * An NMI handler that never returns (the fatal panic path is the normal case)
 * leaves the depth raised forever. That is correct, not a leak: the CPU is dead,
 * and a raised depth only ever makes the classification MORE conservative. */
#define IDT_NMI_DEPTH_IDS  256u
static volatile uint32_t s_nmi_depth[IDT_NMI_DEPTH_IDS];

static uint32_t idt_self_apic_id(void)
{
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1u), "c"(0u));
    (void)eax; (void)ecx; (void)edx;
    return (ebx >> 24) & 0xFFu;
}

int idt_in_nmi(void)
{
    return __atomic_load_n(&s_nmi_depth[idt_self_apic_id()],
                           __ATOMIC_ACQUIRE) != 0u;
}

uint32_t idt_nmi_depth_raw(void)
{
    return __atomic_load_n(&s_nmi_depth[idt_self_apic_id()], __ATOMIC_ACQUIRE);
}

void idt_nmi_enter(void)
{
    __atomic_fetch_add(&s_nmi_depth[idt_self_apic_id()], 1u, __ATOMIC_ACQ_REL);
}

void idt_nmi_exit(void)
{
    uint32_t id = idt_self_apic_id();

    /* Saturate at zero. An unbalanced exit would wrap to 0xFFFFFFFF and pin this
     * CPU in "inside NMI" for the rest of the boot, permanently disabling the
     * guarded read on a CPU that is not in an NMI at all. */
    if (__atomic_load_n(&s_nmi_depth[id], __ATOMIC_ACQUIRE) != 0u)
        __atomic_fetch_sub(&s_nmi_depth[id], 1u, __ATOMIC_ACQ_REL);
}

uint64_t isr_handler(struct interrupt_frame *frame)
{
    uint8_t vec = (uint8_t)frame->int_no;
    uint64_t result;

    /* Raise the NMI depth BEFORE the frame-integrity and GS checks below.
     * Those checks dereference the frame and gs:0 and can themselves fault, and
     * a fault there while the depth was still clear is exactly the nested abort
     * this counter exists to catch. The counter is indexed by CPUID, not GS, so
     * it is safe to touch before GS has been proven sane. */
    if (vec == VECTOR_NMI)
        idt_nmi_enter();

    /* ---- Interrupt frame integrity check ----
     * CS must be kernel (0x08) or user (0x23 = GDT_USER_CODE|RPL3).
     * Any other value means the frame is corrupt -- push/pop order
     * mismatch, stack corruption, or struct layout drift. */
    {
        uint64_t cs_val = frame->cs;
        if (cs_val != 0x08 && cs_val != 0x23) {
            /* Emit through the bounded try-lock writer. Plain serial_write
             * would block on g_serial_lock if the interrupted code held it, and
             * spin forever on a wedged UART -- losing the only evidence this
             * branch ever produces.
             * It does NOT arm the global emergency latch, and it uses the
             * RECOVERABLE budget: this halt stops only the CURRENT CPU, so the
             * rest of the system keeps running -- and must keep both its
             * ordinary locked serial path and an unspent terminal allowance for
             * a later real panic. */
            serial_write_recoverable(
                "[FATAL] isr_handler: corrupt CS in interrupt frame\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
    }

    /* ---- swapgs symmetry verify ----
     * If we came from ring 3 (CS & 3 != 0), entry swapgs swapped GS
     * from TEB to per-CPU. Verify GS:0 self-pointer is valid -- if
     * swapgs was missed or doubled, gs:0 reads TEB or garbage. */
    {
        struct per_cpu_data *gs_self;
        __asm__ volatile("mov %%gs:0, %0" : "=r"(gs_self));
        if (!gs_self || gs_self->self != gs_self) {
            /* serial_write is UNUSABLE here and always was: it takes
             * g_serial_lock via spin_lock_irqsave, which reads
             * smp_this_cpu()->current_irql -- i.e. gs:0, the exact pointer this
             * branch just proved invalid. The diagnostic for "GS is broken"
             * cannot itself depend on GS. serial_write_emergency touches no
             * per-CPU state: local_irq_save is pushfq/cli and spin_trylock is a
             * plain CAS, so it is the only writer that can report this.
             * Not armed, and recoverable accounting, for the same reason as the
             * branch above: this halts one CPU, not the machine. */
            serial_write_recoverable(
                "[FATAL] isr_handler: GS self-pointer invalid -- swapgs symmetry broken\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
    }

    /* ---- CR0/CR4 safety-bit verify on #GP (TODO-09-boot S7) ----
     * A #GP is the classic symptom of a kernel exploit having just cleared a
     * protection bit (CR4.SMEP to run user pages, CR0.WP to patch RO text).
     * Run BEFORE the handler dispatch / default exception panic so it covers
     * BOTH a handled #GP (msr probe) AND an unhandled #GP (which would
     * otherwise panic_screen() and never reach the return path). A cleared pin
     * bug-checks CRITICAL_STRUCTURE_CORRUPTION (BSP) or records + halts (AP);
     * no-op until pinning is active / this CPU has pinned. Runs after the GS
     * self-pointer check above so smp_this_cpu() is valid. */
    if (vec == 13) {
        extern void cr0_verify_pinned(void);
        extern void cr4_verify_pinned(void);
        cr0_verify_pinned();
        cr4_verify_pinned();
    }

    /* ---- transition ring: record ring-3 -> ring-0 entry ----
     * Only record when coming from ring 3 (user mode). Covers INT
     * 0x80, INT 0x2E, and hardware IRQs that preempted user code.
     * Kernel-internal exceptions and timer ticks taken in kernel
     * mode do not count as fast-path transitions and would bloat
     * the ring with noise. */
    if ((frame->cs & 3) == 3)
        transition_ring_record(TRANSITION_DIR_TO_KERNEL,
                               frame->rip, frame->rsp);

    /* ---- IRQL raise for hardware interrupts (vectors 32+) ----
     * CPU exceptions (0-31) are synchronous faults and run at the
     * IRQL of the faulting code -- do not raise. */
    struct per_cpu_data *pcpu = smp_this_cpu();
    KIRQL prev_irql = pcpu->current_irql;

    /* VECTOR_FASTFAIL (INT 0x29) is a synchronous SOFTWARE exception, not a
     * hardware IRQ -- it just happens to land in the 0x20-0x2F numeric window.
     * Raising IRQL to a device DIRQL for it would run the fast-fail terminal
     * path at DISPATCH_LEVEL, where its panic/termination work must not be
     * gated. Keep it at the interrupted thread's IRQL like the vec<32 faults. */
    if (vec >= 32 && vec != VECTOR_FASTFAIL) {
        KIRQL isr_irql = vector_to_irql(vec);
        if (isr_irql > prev_irql)
            pcpu->current_irql = isr_irql;
    }

    /* ---- Dispatch to registered handler ---- */
    if (handlers[vec]) {
        result = handlers[vec](frame);
        goto irql_restore;
    }

    /* ---- Default: CPU exceptions (0-31) -- panic ---- */
    if (vec < 32) {
        /* Truly-unhandled-vector fallback -- terminal panic path, no WER hook. The
         * exception vectors are 0x8E interrupt gates (IF cleared) and this reaches
         * mainly the #DF(8)/#MC(18) abort vectors on an IST stack. The VFS writer
         * (FAT32/AHCI can wait on an IRQ that cannot fire with IF=0, and the abort
         * context has no #PF recovery) is still unsafe here, so this path keeps no
         * WER hook. The serial half is now safe: panic_screen emits its reason and
         * register dump through the bounded try-lock emergency writers, and arms
         * the emergency latch once it claims panic ownership, so the diagnostics
         * on this path cannot self-deadlock on a lock the interrupted code held
         * nor spin forever on a wedged UART. panic_screen dumps the
         * full trap frame to serial, which IS the crash evidence for this path. The
         * lock-free WER hook lives on the recoverable ring-3 user terminals
         * (except.c general faults, vmm.c #PF); unregistered ring-3 FP/SIMD faults
         * (#MF/#XM) reach WER once mapped into that terminal by the TODO-23 fault-
         * to-exception mapping follow-up. */
        panic_screen(frame, frame->err_code, exception_names[vec],
                     "idt.c", 0);
        /* panic_screen never returns */
    }

    /* ---- Default: unclaimed hardware/dynamic vectors ---- */
    {
        uint64_t count;

        /* LAPIC spurious (0xFF): NO EOI -- spurious interrupts do not set
         * an ISR bit (Intel SDM 11.9); an EOI here would acknowledge the
         * highest real in-service interrupt instead. Silent return. */
        if (vec == 0xFF) {
            result = (uint64_t)frame;
            goto irql_restore;
        }

        /* Hyper-V synthetic (0x90-0x9F): silent EOI */
        if (vec >= 0x90 && vec <= 0x9F) {
            lapic_eoi();
            result = (uint64_t)frame;
            goto irql_restore;
        }

        /* PIC spurious IRQ7/IRQ15: the 8259 delivers these with no ISR
         * bit set. No EOI for spurious IRQ7; spurious IRQ15 needs an EOI
         * to the MASTER only (the cascade IS in service there). */
        if (!ioapic_available() && pic_available()) {
            uint8_t isa = irq_vector_to_isa(vec);
            if ((isa == 7 || isa == 15) && !pic_irq_in_service(isa)) {
                if (isa == 15)
                    pic_send_eoi(2);  /* cascade line on the master */
                result = (uint64_t)frame;
                goto irql_restore;
            }
        }

        count = ++unhandled_counts[vec];

        if (count == 1) {
            klog(LOG_WARN, "idt",
                 "Unhandled interrupt vec=%u (0x%x), RIP=0x%x",
                 (uint64_t)vec, (uint64_t)vec, frame->rip);
        } else if (count == 10 || count == 100 || count == 1000) {
            klog(LOG_WARN, "idt",
                 "Unhandled interrupt vec=%u (0x%x): %u total hits",
                 (uint64_t)vec, (uint64_t)vec, count);
        } else if (count == IDT_STORM_QUARANTINE_LIMIT) {
            /* A no-handler vector storming at this rate livelocks the
             * CPU (stuck level line). Mask it at the owning controller
             * when one is known; otherwise it stays log-only. */
            uint8_t  isa = irq_vector_to_isa((uint8_t)vec);
            uint32_t gsi = irq_gsi_for_vector((uint8_t)vec);
            if (gsi != 0xFFFFFFFFu && ioapic_available()) {
                ioapic_mask_irq(gsi);
                klog(LOG_ERROR, "idt",
                     "vec 0x%x QUARANTINED (GSI %u masked, %u unhandled hits)",
                     (uint64_t)vec, (uint64_t)gsi, count);
            } else if (isa != 0xFF) {
                if (ioapic_available())
                    ioapic_mask_irq(ioapic_isa_to_gsi(isa));
                else
                    pic_mask_irq(isa);
                klog(LOG_ERROR, "idt",
                     "vec 0x%x QUARANTINED (ISA IRQ %u masked, %u unhandled hits)",
                     (uint64_t)vec, (uint64_t)isa, count);
            } else {
                klog(LOG_ERROR, "idt",
                     "vec 0x%x storming (%u unhandled hits) -- no maskable controller",
                     (uint64_t)vec, count);
            }
        }

        /* EOI -- controller-aware: PIC-delivered ISA vectors must EOI the
         * 8259 (a stuck in-service bit blocks that priority level), all
         * other vectors EOI the LAPIC. Idempotent on the LAPIC side. */
        if (vec >= 32)
            irq_eoi(irq_vector_to_isa((uint8_t)vec));
        else
            lapic_eoi();
    }

    result = (uint64_t)frame;

irql_restore:
    /* ---- IRQL restore on interrupt exit ---- */
    if (vec >= 32 && pcpu->current_irql != prev_irql)
        pcpu->current_irql = prev_irql;

    /* ---- -18 iretq invariants ----
     * Before the asm stub's swapgs + iretq returns to ring 3, verify
     * the two selector invariants that are cheap to check from C (the
     * frame pointer the stub will consume is whatever we return in
     * `result`). Full ring-3 invariant suite (MSR_KERNEL_GS_BASE, CR3,
     * RSP-in-user-stack) requires per-CPU cached task info that does
     * not exist yet; filing those three as a -18 follow-up so the
     * two-that-can-ship today are not gated on the three-that-need-
     * infra. A failure here LOG_FATALs with a named panic; silent
     * "return to ring 3 with wrong DPL" is exactly the class of bug
     * this section was filed to eliminate. */
    {
        struct interrupt_frame *final_frame =
            (struct interrupt_frame *)(uintptr_t)result;
        uint64_t cs_rpl = final_frame->cs & 3;
        uint64_t ss_rpl = final_frame->ss & 3;
        if (cs_rpl == 3) {
            if (ss_rpl != 3)
                klog(LOG_FATAL, "idt",
                     "iretq: CS.DPL=3 but SS.DPL=%u (frame CS=0x%X SS=0x%X)",
                     (uint64_t)ss_rpl,
                     final_frame->cs, final_frame->ss);
            /* transition ring: record ring-0 -> ring-3 exit.
             * Pairs with the TO_KERNEL record at function entry so
             * each user-mode crossing leaves a matched entry/exit
             * pair in the ring, letting a dump show exactly which
             * transition sequence preceded the panic. */
            transition_ring_record(TRANSITION_DIR_TO_USER,
                                   final_frame->rip, final_frame->rsp);
        } else if (cs_rpl != 0) {
            klog(LOG_FATAL, "idt",
                 "iretq: CS.DPL=%u (neither ring 0 nor ring 3) -- frame corrupt",
                 (uint64_t)cs_rpl);
        }
    }

    /* Lower the NMI depth at the LAST point in C, not at irql_restore: the block
     * above still dereferences the outgoing frame, can klog, and records a
     * transition -- all of it still inside the NMI, and all of it able to fault.
     * Dropping the depth at the label would reopen the exact window this counter
     * closes, one layer lower down.
     *
     * RESIDUAL: the stub's swapgs + iretq epilogue (isr_stubs.asm) runs after
     * this returns and is not covered. Closing it would mean a per-vector test in
     * the common stub, paid by every interrupt on the machine for a counter only
     * the NMI path reads, and the epilogue performs no guarded reads -- it pops
     * registers off the IST stack it is already using. */
    if (vec == VECTOR_NMI)
        idt_nmi_exit();

    return result;
}

void idt_register_handler(uint8_t n, interrupt_handler_t handler)
{
    /* Guard: warn if overwriting an existing non-NULL handler -- catches an
     * UNEXPECTED permanent replacement (two subsystems claiming one vector).
     * Expected temporary swaps (the MSR #GP probe) use the _quiet variant so
     * they do not flood the boot log (84 lines/boot before that split).
     * NULL handler = deregistration. */
    if (handler && handlers[n]) {
        klog(LOG_WARN, "idt",
             "vector 0x%02X: overwriting existing handler %p with %p",
             (uint64_t)n, (uint64_t)(uintptr_t)handlers[n],
             (uint64_t)(uintptr_t)handler);
    }
    handlers[n] = handler;
}

void idt_register_handler_quiet(uint8_t n, interrupt_handler_t handler)
{
    handlers[n] = handler;
}

interrupt_handler_t idt_get_handler(uint8_t n)
{
    return handlers[n];
}

void idt_set_user_callable(uint8_t n)
{
    if (!s_idt_loaded) {
        /* Before lidt the IDTR still points at the UEFI IDT; poking our idt[]
         * gate has no effect. Callers must run after idt_init(). */
        klog(LOG_WARN, "idt",
             "idt_set_user_callable(0x%02X) before kernel IDT loaded -- ignored",
             (uint64_t)n);
        return;
    }
    /* Present, DPL=3, 64-bit interrupt gate (0xEE) -- same shape idt_init()
     * uses for the INT 0x2E / INT 0x80 syscall gates. Preserves the IST index
     * already programmed for this vector (only the type_attr byte changes). */
    idt[n].type_attr = 0xEE;
}

void idt_init(void)
{
    uint32_t i;

    /* ISR stub addresses for vectors 0-31 (CPU exceptions) */
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

    /* IRQ stub addresses for vectors 32-47 (hardware interrupts) */
    uint64_t irq_stubs[16] = {
        (uint64_t)irq0,  (uint64_t)irq1,  (uint64_t)irq2,  (uint64_t)irq3,
        (uint64_t)irq4,  (uint64_t)irq5,  (uint64_t)irq6,  (uint64_t)irq7,
        (uint64_t)irq8,  (uint64_t)irq9,  (uint64_t)irq10, (uint64_t)irq11,
        (uint64_t)irq12, (uint64_t)irq13, (uint64_t)irq14, (uint64_t)irq15,
    };

    /* Dynamic/synthetic stub addresses for vectors 48-255 */
    uint64_t dyn_stubs[208] = {
        (uint64_t)isr48,  (uint64_t)isr49,  (uint64_t)isr50,  (uint64_t)isr51,
        (uint64_t)isr52,  (uint64_t)isr53,  (uint64_t)isr54,  (uint64_t)isr55,
        (uint64_t)isr56,  (uint64_t)isr57,  (uint64_t)isr58,  (uint64_t)isr59,
        (uint64_t)isr60,  (uint64_t)isr61,  (uint64_t)isr62,  (uint64_t)isr63,
        (uint64_t)isr64,  (uint64_t)isr65,  (uint64_t)isr66,  (uint64_t)isr67,
        (uint64_t)isr68,  (uint64_t)isr69,  (uint64_t)isr70,  (uint64_t)isr71,
        (uint64_t)isr72,  (uint64_t)isr73,  (uint64_t)isr74,  (uint64_t)isr75,
        (uint64_t)isr76,  (uint64_t)isr77,  (uint64_t)isr78,  (uint64_t)isr79,
        (uint64_t)isr80,  (uint64_t)isr81,  (uint64_t)isr82,  (uint64_t)isr83,
        (uint64_t)isr84,  (uint64_t)isr85,  (uint64_t)isr86,  (uint64_t)isr87,
        (uint64_t)isr88,  (uint64_t)isr89,  (uint64_t)isr90,  (uint64_t)isr91,
        (uint64_t)isr92,  (uint64_t)isr93,  (uint64_t)isr94,  (uint64_t)isr95,
        (uint64_t)isr96,  (uint64_t)isr97,  (uint64_t)isr98,  (uint64_t)isr99,
        (uint64_t)isr100, (uint64_t)isr101, (uint64_t)isr102, (uint64_t)isr103,
        (uint64_t)isr104, (uint64_t)isr105, (uint64_t)isr106, (uint64_t)isr107,
        (uint64_t)isr108, (uint64_t)isr109, (uint64_t)isr110, (uint64_t)isr111,
        (uint64_t)isr112, (uint64_t)isr113, (uint64_t)isr114, (uint64_t)isr115,
        (uint64_t)isr116, (uint64_t)isr117, (uint64_t)isr118, (uint64_t)isr119,
        (uint64_t)isr120, (uint64_t)isr121, (uint64_t)isr122, (uint64_t)isr123,
        (uint64_t)isr124, (uint64_t)isr125, (uint64_t)isr126, (uint64_t)isr127,
        (uint64_t)isr128, (uint64_t)isr129, (uint64_t)isr130, (uint64_t)isr131,
        (uint64_t)isr132, (uint64_t)isr133, (uint64_t)isr134, (uint64_t)isr135,
        (uint64_t)isr136, (uint64_t)isr137, (uint64_t)isr138, (uint64_t)isr139,
        (uint64_t)isr140, (uint64_t)isr141, (uint64_t)isr142, (uint64_t)isr143,
        (uint64_t)isr144, (uint64_t)isr145, (uint64_t)isr146, (uint64_t)isr147,
        (uint64_t)isr148, (uint64_t)isr149, (uint64_t)isr150, (uint64_t)isr151,
        (uint64_t)isr152, (uint64_t)isr153, (uint64_t)isr154, (uint64_t)isr155,
        (uint64_t)isr156, (uint64_t)isr157, (uint64_t)isr158, (uint64_t)isr159,
        (uint64_t)isr160, (uint64_t)isr161, (uint64_t)isr162, (uint64_t)isr163,
        (uint64_t)isr164, (uint64_t)isr165, (uint64_t)isr166, (uint64_t)isr167,
        (uint64_t)isr168, (uint64_t)isr169, (uint64_t)isr170, (uint64_t)isr171,
        (uint64_t)isr172, (uint64_t)isr173, (uint64_t)isr174, (uint64_t)isr175,
        (uint64_t)isr176, (uint64_t)isr177, (uint64_t)isr178, (uint64_t)isr179,
        (uint64_t)isr180, (uint64_t)isr181, (uint64_t)isr182, (uint64_t)isr183,
        (uint64_t)isr184, (uint64_t)isr185, (uint64_t)isr186, (uint64_t)isr187,
        (uint64_t)isr188, (uint64_t)isr189, (uint64_t)isr190, (uint64_t)isr191,
        (uint64_t)isr192, (uint64_t)isr193, (uint64_t)isr194, (uint64_t)isr195,
        (uint64_t)isr196, (uint64_t)isr197, (uint64_t)isr198, (uint64_t)isr199,
        (uint64_t)isr200, (uint64_t)isr201, (uint64_t)isr202, (uint64_t)isr203,
        (uint64_t)isr204, (uint64_t)isr205, (uint64_t)isr206, (uint64_t)isr207,
        (uint64_t)isr208, (uint64_t)isr209, (uint64_t)isr210, (uint64_t)isr211,
        (uint64_t)isr212, (uint64_t)isr213, (uint64_t)isr214, (uint64_t)isr215,
        (uint64_t)isr216, (uint64_t)isr217, (uint64_t)isr218, (uint64_t)isr219,
        (uint64_t)isr220, (uint64_t)isr221, (uint64_t)isr222, (uint64_t)isr223,
        (uint64_t)isr224, (uint64_t)isr225, (uint64_t)isr226, (uint64_t)isr227,
        (uint64_t)isr228, (uint64_t)isr229, (uint64_t)isr230, (uint64_t)isr231,
        (uint64_t)isr232, (uint64_t)isr233, (uint64_t)isr234, (uint64_t)isr235,
        (uint64_t)isr236, (uint64_t)isr237, (uint64_t)isr238, (uint64_t)isr239,
        (uint64_t)isr240, (uint64_t)isr241, (uint64_t)isr242, (uint64_t)isr243,
        (uint64_t)isr244, (uint64_t)isr245, (uint64_t)isr246, (uint64_t)isr247,
        (uint64_t)isr248, (uint64_t)isr249, (uint64_t)isr250, (uint64_t)isr251,
        (uint64_t)isr252, (uint64_t)isr253, (uint64_t)isr254, (uint64_t)isr255,
    };

    /* Zero everything */
    for (i = 0; i < 256; i++) {
        idt_set_entry((uint8_t)i, 0, 0, 0, 0);
        handlers[i] = (interrupt_handler_t)0;
        unhandled_counts[i] = 0;
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

    /* Install dynamic/synthetic stubs 48-255.
     * dyn_stubs[0] = isr48, dyn_stubs[80] = isr128, etc. */
    for (i = 0; i < 208; i++) {
        idt_set_entry((uint8_t)(i + 48), dyn_stubs[i], GDT_KERNEL_CODE,
                      0, 0x8E);
    }

    /* IST overrides for critical exceptions -- use dedicated stacks
     * so these handlers work even when the kernel stack is corrupted.
     * IST indices match TSS ist1-ist3 set in gdt_init(). */
    idt[2].ist  = 2;   /* NMI → IST2 */
    idt[8].ist  = 1;   /* #DF → IST1 */
    idt[18].ist = 3;   /* MCE → IST3 */

    /* Override DPL for software interrupts callable from ring 3 */
    /* INT 0x2E: NT syscall compatibility -- DPL=3 */
    idt[0x2E].type_attr = 0xEE;
    /* INT 0x80: Linux-style syscall -- DPL=3 */
    idt[128].type_attr = 0xEE;

    /* Load the IDT */
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base  = (uint64_t)(uintptr_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));
    s_idt_loaded = 1;

    klog(LOG_INFO, "cpu",
         "IDT loaded (256 entries, ISR 0-31, IRQ 32-47, dynamic 48-255)");
}

int idt_is_loaded(void)
{
    return s_idt_loaded;
}

void idt_get_idtr(void *out_idtr)
{
    uint8_t *dst = (uint8_t *)out_idtr;
    const uint8_t *src = (const uint8_t *)&idtr;
    uint32_t i;
    for (i = 0; i < 10; i++)
        dst[i] = src[i];
}
