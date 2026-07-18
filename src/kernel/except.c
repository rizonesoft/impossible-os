/* ============================================================================
 * except.c -- interrupt_frame <-> CONTEXT conversion
 *
 * The frame-backed half of the Windows AMD64 exception ABI. An interrupt_frame
 * carries the CONTROL and INTEGER register groups and nothing else, so these
 * converters serve exactly those groups and report what they actually did --
 * see the contract in kernel/except.h.
 *
 * ARCH: x86-64 -- interrupt_frame and the CONTEXT register file are both the
 * AMD64 ABI.
 * ============================================================================ */

#include "kernel/except.h"
#include "kernel/idt.h"
#include "kernel/vectors.h"     /* VECTOR_* fault vectors */
#include "kernel/cpuid.h"       /* cpu_has, CPU_FEATURE_CET_SS */
#include "kernel/panic.h"       /* panic_screen */
#include "kernel/bugcheck.h"    /* KeBugCheckEx, BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE */
#include "kernel/klog.h"        /* klog */
#include "kernel/smp.h"         /* smp_this_cpu() + MAX_CPUS: per-CPU exception scratch */
#include "kernel/boot_init.h"   /* POST16 (except_init is boot-path phase 1) */
#include "kernel/wer.h"         /* wer_write_crash_report -- user-mode terminal path */

/* Local byte-wise zero: the kernel has no memset declaration in a header, and
 * panic.c zeroes CONTEXT the same way. Keeps this file dependency-free so it
 * is safe to call from fault context. */
static void except_zero(void *dst, uint32_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++)
        p[i] = 0;
}

/* Set only the architectural FPU init FIELDS. The caller MUST have already
 * cleared FltSave -- all-zero would unmask every FP exception and fault the
 * moment the state was restored and used, so these three fields override that.
 * Split out so a caller that just zeroed the whole CONTEXT does not pay a second
 * FltSave clear. */
static void context_set_fpu_init_fields(CONTEXT *ctx)
{
    ctx->FltSave.ControlWord = FPU_FCW_INIT;
    ctx->FltSave.MxCsr       = FPU_MXCSR_INIT;
    ctx->MxCsr               = FPU_MXCSR_INIT;
}

void context_init_fpu_state(CONTEXT *ctx)
{
    if (!ctx)
        return;

    except_zero(&ctx->FltSave, (uint32_t)sizeof(XMM_SAVE_AREA32));
    context_set_fpu_init_fields(ctx);
}

uint32_t context_from_frame(const struct interrupt_frame *frame, CONTEXT *ctx)
{
    uint32_t requested;
    uint32_t captured = CONTEXT_AMD64;

    if (!frame || !ctx)
        return 0;

    /* ContextFlags is the caller's REQUEST on entry; snapshot it before the
     * struct is cleared. */
    requested = ctx->ContextFlags;

    /* Fresh capture: never leave the caller's stale register values in fields
     * this call does not populate. */
    except_zero(ctx, (uint32_t)sizeof(CONTEXT));

    /* The FPU group can never be satisfied from a frame (see header contract):
     * park the area at architectural init state rather than raw zeroes so a
     * caller that ignores the cleared flag cannot unmask every FP exception.
     * The full except_zero above already cleared FltSave, so only set the init
     * fields -- no second FltSave clear. */
    context_set_fpu_init_fields(ctx);

    if (CONTEXT_HAS_GROUP(requested, CONTEXT_CONTROL)) {
        /* In x86-64 LONG MODE the CPU pushes SS:RSP on EVERY interrupt/exception,
         * even without a privilege change (unlike 32-bit protected mode), and
         * IRETQ pops all five -- so the interrupt_frame carries a valid SS:RSP
         * for both ring-0 and ring-3 faults. Read them unconditionally. */
        ctx->SegCs  = (uint16_t)frame->cs;
        ctx->SegSs  = (uint16_t)frame->ss;
        ctx->EFlags = (uint32_t)frame->rflags;
        ctx->Rsp    = frame->rsp;
        ctx->Rip    = frame->rip;
        captured |= CONTEXT_CONTROL;
    }

    if (CONTEXT_HAS_GROUP(requested, CONTEXT_INTEGER)) {
        ctx->Rax = frame->rax;
        ctx->Rcx = frame->rcx;
        ctx->Rdx = frame->rdx;
        ctx->Rbx = frame->rbx;
        ctx->Rbp = frame->rbp;   /* RBP is INTEGER on AMD64, not CONTROL */
        ctx->Rsi = frame->rsi;
        ctx->Rdi = frame->rdi;
        ctx->R8  = frame->r8;
        ctx->R9  = frame->r9;
        ctx->R10 = frame->r10;
        ctx->R11 = frame->r11;
        ctx->R12 = frame->r12;
        ctx->R13 = frame->r13;
        ctx->R14 = frame->r14;
        ctx->R15 = frame->r15;
        captured |= CONTEXT_INTEGER;
    }

    /* Report what was captured, not what was asked for. SEGMENTS /
     * DEBUG_REGISTERS / FLOATING_POINT stay cleared: their fields hold no
     * frame-derived data and must not be advertised as valid. */
    ctx->ContextFlags = captured;
    return captured;
}

uint32_t frame_from_context(const CONTEXT *ctx, struct interrupt_frame *frame)
{
    uint32_t flags;
    uint32_t restored = CONTEXT_AMD64;

    if (!ctx || !frame)
        return 0;

    /* TRUSTED INPUT ONLY -- no validation here by design. A ring-3-supplied
     * CONTEXT must be checked by NtContinue first (kernel/except.h contract). */
    flags = ctx->ContextFlags;

    if (CONTEXT_HAS_GROUP(flags, CONTEXT_CONTROL)) {
        frame->cs     = (uint64_t)ctx->SegCs;
        frame->ss     = (uint64_t)ctx->SegSs;
        frame->rflags = (uint64_t)ctx->EFlags;
        frame->rsp    = ctx->Rsp;
        frame->rip    = ctx->Rip;
        restored |= CONTEXT_CONTROL;
    }

    if (CONTEXT_HAS_GROUP(flags, CONTEXT_INTEGER)) {
        frame->rax = ctx->Rax;
        frame->rcx = ctx->Rcx;
        frame->rdx = ctx->Rdx;
        frame->rbx = ctx->Rbx;
        frame->rbp = ctx->Rbp;
        frame->rsi = ctx->Rsi;
        frame->rdi = ctx->Rdi;
        frame->r8  = ctx->R8;
        frame->r9  = ctx->R9;
        frame->r10 = ctx->R10;
        frame->r11 = ctx->R11;
        frame->r12 = ctx->R12;
        frame->r13 = ctx->R13;
        frame->r14 = ctx->R14;
        frame->r15 = ctx->R15;
        restored |= CONTEXT_INTEGER;
    }

    /* int_no / err_code are frame metadata with no CONTEXT counterpart and are
     * deliberately preserved across a restore. */
    return restored;
}

/* --- Dispatch stubs ------------------------------------------------------
 *
 * The #PF triage routes user- and kernel-origin faults through these entry
 * points. The full dispatch pipeline (debugger notification, ring-3 handover,
 * kernel SEH chain walk) lands in later sections; until then the stubs decline
 * every exception (return UNHANDLED), so the caller performs its existing
 * terminal action. Kept dependency-free and lock-free -- they run in fault
 * context where a klog spinlock or an allocation could deadlock or fault. */
KI_EXCEPTION_DISPOSITION ki_dispatch_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                                               struct interrupt_frame *frame,
                                               KPROCESSOR_MODE mode, int first_chance)
{
    (void)rec; (void)ctx; (void)frame; (void)mode; (void)first_chance;
    return KI_EXCEPTION_UNHANDLED;
}

KI_EXCEPTION_DISPOSITION ki_raise_kernel_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                                                   struct interrupt_frame *frame)
{
    (void)rec; (void)ctx; (void)frame;
    return KI_EXCEPTION_UNHANDLED;
}

/* --- General fault-to-exception mapping ----------------------------------
 *
 * ARCH: x86-64 -- these are AMD64 fault vectors and the interrupt_frame is the
 * AMD64 register file. Registered by except_init() (kernel init phase 1, after
 * idt_init()); each handler runs in fault context, so like the #PF triage it is
 * lock-free and allocation-free and builds its EXCEPTION_RECORD in a per-CPU
 * scratch slot rather than on the faulting kernel stack.
 * ------------------------------------------------------------------------- */

/* Per-fault behaviour flags. */
#define FAULT_KERNEL_FATAL   0x01u  /* kernel-mode fault is terminal, no dispatch (#DE/#OF/#UD) */
#define FAULT_BP_ADJUST      0x02u  /* rewind RIP by 1 (INT3 is one byte) before capture */
#define FAULT_NONCONTINUABLE 0x04u  /* record is EXCEPTION_NONCONTINUABLE (#CP) */
#define FAULT_CET_BUGCHECK   0x08u  /* kernel-mode -> KeBugCheckEx(KERNEL_SECURITY_CHECK_FAILURE) (#CP) */

struct fault_map {
    uint8_t     vector;
    NTSTATUS    code;      /* user-mode exception code */
    uint32_t    flags;
    const char *name;      /* short label for the terminal panic message */
};

/* The general fault vectors this section owns. #PF (14) is owned by the VMM
 * triage; #NM (7) by the lazy-FPU switch; NMI/#DF/#MC keep their dedicated
 * handlers. __fastfail (INT 0x29) is a dedicated path below, not in this table. */
static const struct fault_map fault_maps[] = {
    { VECTOR_DIVIDE_ERROR,        STATUS_INTEGER_DIVIDE_BY_ZERO, FAULT_KERNEL_FATAL, "#DE" },
    { VECTOR_DEBUG,               STATUS_SINGLE_STEP,            0,                  "#DB" },
    { VECTOR_BREAKPOINT,          STATUS_BREAKPOINT,             FAULT_BP_ADJUST,    "#BP" },
    { VECTOR_OVERFLOW,            STATUS_INTEGER_OVERFLOW,       FAULT_KERNEL_FATAL, "#OF" },
    { VECTOR_INVALID_OPCODE,      STATUS_ILLEGAL_INSTRUCTION,    FAULT_KERNEL_FATAL, "#UD" },
    { VECTOR_SEGMENT_NOT_PRESENT, STATUS_ACCESS_VIOLATION,       0,                  "#NP" },
    { VECTOR_STACK_FAULT,         STATUS_STACK_OVERFLOW,         0,                  "#SS" },
    { VECTOR_GENERAL_PROTECTION,  STATUS_ACCESS_VIOLATION,       0,                  "#GP" },
    { VECTOR_CONTROL_PROTECTION,  STATUS_STACK_BUFFER_OVERRUN,
                                  FAULT_NONCONTINUABLE | FAULT_CET_BUGCHECK,         "#CP" },
};

#define FAULT_MAP_COUNT (sizeof(fault_maps) / sizeof(fault_maps[0]))

/* Layer 1: pin the table shape. #DE/#OF/#UD (kernel-fatal), #DB/#BP/#NP/#SS/#GP
 * (dispatchable), plus the conditional #CP -- nine entries. A dropped row would
 * silently un-map a fault vector back to the generic idt.c panic. */
_Static_assert(FAULT_MAP_COUNT == 9,
               "fault_maps must cover exactly the 9 general fault vectors");

static const struct fault_map *fault_lookup(uint8_t vector)
{
    for (uint32_t i = 0; i < FAULT_MAP_COUNT; i++)
        if (fault_maps[i].vector == vector)
            return &fault_maps[i];
    return (const struct fault_map *)0;
}

NTSTATUS except_vector_to_status(uint8_t vector)
{
    const struct fault_map *m = fault_lookup(vector);
    return m ? m->code : (NTSTATUS)0;
}

int except_vector_kernel_fatal(uint8_t vector)
{
    const struct fault_map *m = fault_lookup(vector);
    return (m && (m->flags & FAULT_KERNEL_FATAL)) ? 1 : 0;
}

void except_build_record(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                         const struct interrupt_frame *frame,
                         NTSTATUS code, uint32_t exception_flags)
{
    if (!rec || !ctx || !frame)
        return;

    /* Seed only the frame-backed groups; context_from_frame() zeroes the rest
     * of the CONTEXT and parks FltSave at architectural init state. */
    ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    context_from_frame(frame, ctx);

    rec->ExceptionCode    = code;
    rec->ExceptionFlags   = exception_flags;
    rec->ExceptionRecord  = (EXCEPTION_RECORD *)0;
    rec->ExceptionAddress = (void *)(uintptr_t)frame->rip;
    rec->NumberParameters = 0;   /* caller sets fault-specific params (e.g. #CP subcode) */
}

/* Per-CPU exception scratch: EXCEPTION_RECORD (152 B) + CONTEXT (1232 B) are far
 * too large to build on the arbitrary-depth kernel stack a ring-0 fault runs on.
 * Distinct from the VMM's #PF scratch (different slots -- a #GP that itself #PFs
 * uses the VMM slot, so no aliasing). Cache-line aligned + padded so two CPUs'
 * simultaneous faults never bounce a shared line. `in_use` is a one-shot
 * recursion guard: a nested fault mid-build escalates straight to a panic. */
#define EXCEPT_CACHELINE 64u
struct except_scratch {
    EXCEPTION_RECORD  rec;
    CONTEXT           ctx;
    volatile uint32_t in_use;
} __attribute__((aligned(EXCEPT_CACHELINE)));

_Static_assert(sizeof(struct except_scratch) % EXCEPT_CACHELINE == 0,
               "except_scratch stride must be a cache-line multiple (no false sharing)");

static struct except_scratch except_scratch[MAX_CPUS] __attribute__((aligned(EXCEPT_CACHELINE)));

/* Shared ISR for every general fault vector. isr_handler() dispatches here via
 * handlers[frame->int_no]; the vector is read back from the frame. */
static uint64_t except_common_handler(struct interrupt_frame *frame)
{
    uint8_t vec = (uint8_t)frame->int_no;
    const struct fault_map *m = fault_lookup(vec);
    KPROCESSOR_MODE mode;
    struct except_scratch *s;
    uint32_t cpu;

    if (!m) {
        /* Only mapped vectors register this handler, so an unmapped vector here
         * means frame corruption -- terminate rather than mis-map it. */
        panic_screen(frame, frame->err_code, "except: unmapped vector", "except.c", 0);
        return (uint64_t)frame;  /* unreachable */
    }

    /* INT3 pushes RIP past the 1-byte 0xCC; Windows reports #BP AT the INT3.
     * Rewind the live frame so the record, a HANDLED resume, or a panic all
     * point at the breakpoint instruction. */
    if (m->flags & FAULT_BP_ADJUST)
        frame->rip -= 1;

    mode = (frame->cs & 3) ? UserMode : KernelMode;

    /* Kernel-mode #DE/#OF/#UD are unconditionally terminal -- no SEH recovery is
     * ever attempted for a kernel divide error, overflow, or bad opcode. */
    if (mode == KernelMode && (m->flags & FAULT_KERNEL_FATAL)) {
        panic_screen(frame, frame->err_code, m->name, "except.c", 0);
        return (uint64_t)frame;  /* unreachable */
    }

    /* Kernel-mode #CP -> KERNEL_SECURITY_CHECK_FAILURE bugcheck, the Windows
     * kernel shadow-stack-violation STOP (Parameter 1 = the fast-fail subcode).
     * Lock-/alloc-/VFS-free: KeBugCheckEx is the fault-safe terminal. */
    if (mode == KernelMode && (m->flags & FAULT_CET_BUGCHECK)) {
        KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE,
                     FAST_FAIL_CONTROL_INVALID_RETURN_ADDRESS,
                     frame->rip, 0, 0);
        return (uint64_t)frame;  /* unreachable */
    }

    /* Build the record in per-CPU scratch under the one-shot recursion guard. */
    cpu = smp_this_cpu()->cpu_id;
    if (cpu >= MAX_CPUS || except_scratch[cpu].in_use) {
        panic_screen(frame, frame->err_code, "except (nested)", "except.c", 0);
        return (uint64_t)frame;  /* unreachable */
    }
    s = &except_scratch[cpu];
    s->in_use = 1;

    {
        uint32_t exc_flags = (m->flags & FAULT_NONCONTINUABLE)
                                 ? EXCEPTION_NONCONTINUABLE : EXCEPTION_CONTINUABLE;
        KI_EXCEPTION_DISPOSITION disp;

        except_build_record(&s->rec, &s->ctx, frame, m->code, exc_flags);

        /* User-mode #CP carries the fast-fail subcode so a consumer can tell a
         * shadow-stack violation from other STATUS_STACK_BUFFER_OVERRUN causes. */
        if (m->flags & FAULT_CET_BUGCHECK) {
            s->rec.NumberParameters = 1;
            s->rec.ExceptionInformation[0] = FAST_FAIL_CONTROL_INVALID_RETURN_ADDRESS;
        }

        /* Both modes route through the master dispatcher (kernel-mode is handled
         * internally via ki_raise_kernel_exception). The stub declines until the
         * debugger / ring-3 delivery / kernel-SEH stages land. */
        disp = ki_dispatch_exception(&s->rec, &s->ctx, frame, mode, 1 /*first_chance*/);
        if (disp == KI_EXCEPTION_HANDLED) {
            s->in_use = 0;           /* resolved -- release the slot */
            return (uint64_t)frame;  /* frame carries the resume state, IRET */
        }

        /* Unhandled: terminal. Leave in_use SET so a fault DURING the terminal
         * path hits the nested guard. A user fault entered from ring 3 holds no
         * kernel spinlock, so klog + a user WER crash report are safe here (the
         * VFS write runs at the interrupted thread's IRQL); a kernel fault may
         * hold locks, so it takes the fault-safe panic directly (no WER/klog).
         * Ring-3 delivery + per-process termination land with the later stages. */
        if (mode == UserMode) {
            wer_write_crash_report(frame, vec);
            klog(LOG_ERROR, "except",
                 "%s at %p err=0x%x -> status 0x%x (user, unhandled)",
                 m->name, (void *)(uintptr_t)frame->rip,
                 (unsigned int)frame->err_code, (unsigned int)m->code);
            panic_screen(frame, frame->err_code, m->name, "except.c", 0);
        } else {
            panic_screen(frame, frame->err_code, m->name, "except.c", 0);
        }
        return (uint64_t)frame;  /* unreachable */
    }
}

/* __fastfail (INT 0x29): an immediate, noncontinuable termination that by
 * contract bypasses VEH/SEH/VCH and the top-level filter. The fast-fail CODE is
 * in ECX (low 32 bits of RCX). We build a record for a future crash dump but do
 * NOT route it through the dispatcher, and keep the terminal path VFS-/lock-free
 * (no WER) since a fastfail can be raised from kernel mode too. */
static uint64_t except_fastfail_handler(struct interrupt_frame *frame)
{
    uint32_t code = (uint32_t)frame->rcx;
    uint32_t cpu  = smp_this_cpu()->cpu_id;

    if (cpu < MAX_CPUS && !except_scratch[cpu].in_use) {
        struct except_scratch *s = &except_scratch[cpu];
        s->in_use = 1;
        except_build_record(&s->rec, &s->ctx, frame,
                            STATUS_STACK_BUFFER_OVERRUN, EXCEPTION_NONCONTINUABLE);
        s->rec.NumberParameters = 1;
        s->rec.ExceptionInformation[0] = code;
    }

    panic_screen(frame, (uint64_t)code, "__fastfail", "except.c", 0);
    return (uint64_t)frame;  /* unreachable */
}

void except_init(void)
{
    POST16(POST16_EXCEPT);

    for (uint32_t i = 0; i < FAULT_MAP_COUNT; i++) {
        uint8_t vec = fault_maps[i].vector;
        /* #CP is raised only by CET shadow-stack hardware; claiming the vector
         * on a CPU without CET would intercept a vector the CPU never issues. */
        if (vec == VECTOR_CONTROL_PROTECTION && !cpu_has(CPU_FEATURE_CET_SS))
            continue;
        idt_register_handler(vec, except_common_handler);
    }

    /* __fastfail takes the dedicated (dispatch-bypassing) handler. */
    idt_register_handler(VECTOR_FASTFAIL, except_fastfail_handler);

    /* Ring-3 must raise INT3 (breakpoint), INTO (#OF) and INT 0x29 (__fastfail)
     * directly, so promote those gates to DPL=3. CPU-generated faults
     * (#DE/#UD/#NP/#SS/#GP/#CP) ignore gate DPL and need no promotion. */
    idt_set_user_callable(VECTOR_BREAKPOINT);
    idt_set_user_callable(VECTOR_OVERFLOW);
    idt_set_user_callable(VECTOR_FASTFAIL);

    klog(LOG_INFO, "except",
         "fault-to-exception handlers registered (#DE/#DB/#BP/#OF/#UD/#NP/#SS/#GP + __fastfail, #CP %s)",
         cpu_has(CPU_FEATURE_CET_SS) ? "on" : "off");

    POST16(POST16_EXCEPT_OK);
}
