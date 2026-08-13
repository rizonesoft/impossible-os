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
#include "kernel/sched/task.h"  /* thread_current()->in_system_service (bugcheck class) */
#include "kernel/sched/irql.h"  /* KeGetCurrentIrql, APC_LEVEL -- kernel-SEH IRQL gate */
#include "kernel/time/wall_clock.h" /* KeQueryInterruptTimeCoarse -- s16 telemetry rate window */
#include "libc/string.h"        /* snprintf -- s16 telemetry JSON line */

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

/* --- Debugger notification + bugcheck classification -----------------------
 *
 * The master dispatcher orchestrates the NT exception-dispatch sequence:
 *   user-mode:   debugger first-chance -> (ring-3 handover: KiUserException
 *                dispatcher stage) -> second-chance -> terminate
 *   kernel-mode: KiDebugRoutine first-chance -> kernel SEH walk -> KiDebugRoutine
 *                second-chance -> KeBugCheckEx
 * Ring-3 delivery (KiUserExceptionDispatcher) and the kernel SEH chain walk are
 * owned by the ring-3-delivery and kernel-__try/__except stages; both are stubs
 * here (DbgkForwardException -> FALSE, ki_raise_kernel_exception -> UNHANDLED), so
 * an undebugged fault takes the existing terminal. Everything on this path runs
 * in fault context: lock-free, allocation-free. */

/* Kernel-debugger callback. Permanently resident once KD attaches; a single
 * aligned pointer read/write is atomic on x86-64, but use explicit acquire/
 * release so a debug routine published by KD on another CPU is fully visible
 * (no seqlock needed for a resident callback). Default NULL = no debugger. */
static KI_DEBUG_ROUTINE ki_debug_routine;

void ki_set_debug_routine(KI_DEBUG_ROUTINE routine)
{
    __atomic_store_n(&ki_debug_routine, routine, __ATOMIC_RELEASE);
}

/* User-mode debug-port forward. No debug port exists until NtDebugActiveProcess
 * lands, so decline every exception. -> XREF: TODO-29 user-mode debug port. */
int DbgkForwardException(EXCEPTION_RECORD *rec, CONTEXT *ctx, int first_chance)
{
    (void)rec; (void)ctx; (void)first_chance;
    return 0;  /* FALSE -- no debugger attached */
}

uint32_t ki_kernel_bugcheck_code(void)
{
    /* A kernel-mode fault taken while a user-originated system service is on the
     * stack is STOP 0x3B (SYSTEM_SERVICE_EXCEPTION); any other kernel fault is
     * STOP 0x1E. in_system_service (not previous_mode, which zw_dispatch forces
     * to KernelMode for nested Zw calls) is the correct in-service marker.
     * SMP caveat: thread_current() still resolves the GLOBAL current-thread cursor,
     * so a fault on one CPU can read another CPU's thread and mis-pick 0x3B vs 0x1E.
     * This is a forensic STOP-code inaccuracy (not a safety issue) shared with
     * previous_mode and closed by the per-CPU current-thread cursor work
     * (TODO-07 SMP phase-2). */
    struct thread *t = thread_current();
    if (t && t->in_system_service)
        return BUGCHECK_SYSTEM_SERVICE_EXCEPTION;
    return BUGCHECK_KMODE_EXCEPTION_NOT_HANDLED;
}

uint32_t ki_kernel_bugcheck_params(EXCEPTION_RECORD *rec, CONTEXT *ctx, uint64_t params[4])
{
    /* Parameter layout is per-STOP-code, matching NT's KeBugCheckEx contract:
     *   0x1E KMODE_EXCEPTION_NOT_HANDLED: code, fault address, exception param 0,
     *        exception param 1 (for an access violation: access type, fault addr).
     *   0x3B SYSTEM_SERVICE_EXCEPTION:    code, faulting instruction address,
     *        address of the CONTEXT record, 0. Emitting the 0x1E layout for 0x3B
     *        would make a dump consumer read ExceptionInformation[0] as a CONTEXT
     *        pointer and dereference garbage. */
    uint32_t code = ki_kernel_bugcheck_code();

    params[0] = (uint64_t)(uint32_t)rec->ExceptionCode;
    params[1] = (uint64_t)(uintptr_t)rec->ExceptionAddress;
    if (code == BUGCHECK_SYSTEM_SERVICE_EXCEPTION) {
        params[2] = (uint64_t)(uintptr_t)ctx;
        params[3] = 0;
    } else {
        params[2] = rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0;
        params[3] = rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0;
    }
    return code;
}

KI_EXCEPTION_DISPOSITION ki_dispatch_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                                               struct interrupt_frame *frame,
                                               KPROCESSOR_MODE mode, int first_chance)
{
    KI_DEBUG_ROUTINE dbg = __atomic_load_n(&ki_debug_routine, __ATOMIC_ACQUIRE);

    if (mode == KernelMode) {
        /* First-chance kernel debugger. A nonzero return means it resolved the
         * fault and rewrote `frame` to the resume point (frame-ownership
         * contract -- we return HANDLED without touching `ctx`). */
        if (first_chance && dbg &&
            dbg(rec, ctx, frame, KernelMode, 1 /*first_chance*/))
            return KI_EXCEPTION_HANDLED;

        /* Kernel SEH chain walk (owned by the kernel-__try/__except stage; the
         * stub declines today). */
        if (ki_raise_kernel_exception(rec, ctx, frame) == KI_EXCEPTION_HANDLED)
            return KI_EXCEPTION_HANDLED;

        /* Second-chance kernel debugger -- last stop before the bugcheck. */
        if (dbg && dbg(rec, ctx, frame, KernelMode, 0 /*second_chance*/))
            return KI_EXCEPTION_HANDLED;

        /* Terminal. Frame-aware, fault-safe bugcheck (preserves the trap frame's
         * register/vector evidence, skips the fault-unsafe registry write). The
         * STOP code and its per-code parameter layout (0x1E vs 0x3B) come from the
         * pure ki_kernel_bugcheck_params selector. noreturn. */
        {
            uint64_t params[4];
            uint32_t bc = ki_kernel_bugcheck_params(rec, ctx, params);
            KeBugCheckExFrame(frame, bc, params[0], params[1], params[2], params[3]);
        }
        return KI_EXCEPTION_UNHANDLED;  /* unreachable */
    }

    /* UserMode. A ring-3 fault holds no kernel spinlock (the caller enters here
     * from the interrupted user thread), so klog is safe. */
    klog(LOG_INFO, "except", "dispatch user exception code=0x%x, first_chance=%d",
         (uint64_t)(uint32_t)rec->ExceptionCode, first_chance);

    /* Debugger first/second-chance via the user-mode debug port. A nonzero return
     * means the debugger continued execution (frame rewritten to resume). */
    if (DbgkForwardException(rec, ctx, first_chance)) {
        /* s16 telemetry: the debug port resolved the fault (resume). */
        except_log_dispatch(rec, "debugger", EXCEPTION_CONTINUE_EXECUTION);
        return KI_EXCEPTION_HANDLED;
    }

    /* Ring-3 handover (KiUserExceptionDispatcher) is owned by the ring-3-delivery
     * stage. Until it lands, an undebugged user exception has nowhere to go in
     * ring 0: decline so the caller performs the existing terminal (WER report +
     * user-safe panic). When ring-3 delivery lands, the first-chance leg hands
     * `frame` to KiUserExceptionDispatcher here, and the NtRaiseException
     * (first_chance=0) re-entry drives the second-chance debugger + termination. */
    /* s16 telemetry: no ring-0 handler claimed this user fault -- the kernel
     * declines to ring-3. The ring-3 VEH -> SEH -> VCH chain that runs next is
     * ntdll's, and its per-handler telemetry is emitted there (TODO-04 s5). */
    except_log_dispatch(rec, "unhandled", EXCEPTION_CONTINUE_SEARCH);
    return KI_EXCEPTION_UNHANDLED;
}

/* --- Section 16: Exception Dispatch Telemetry -------------------------------
 *
 * A flat JSON structured-log event recording each exception-dispatch decision the
 * kernel can observe at the ring-3 boundary. Emitted ONLY from klog-safe legs (a
 * ring-3 fault holds no kernel spinlock); never from ki_raise_kernel_exception
 * (lock-free). Per-process rate-limited via a packed-atomic CAS and routed through
 * klog_unrated() so one process's flood cannot clip another's events. The full
 * VEH -> SEH -> VCH chain runs in ntdll (no ring-0 walker); its per-handler
 * telemetry is owned by 12-user-platform-sdk/TODO-04 s5, sharing this schema.
 * ------------------------------------------------------------------------- */
#if CONFIG_EXCEPT_TELEMETRY

/* Packing for the per-process rate state: window ms in the high bits, event count
 * in the low bits. 20 count bits (max ~1M) dwarf the 100-per-window cap; the ms
 * window uses the remaining bits (a coarse-time ms value is <= 32 bits and wraps
 * only every ~49 days -- an unsigned window-delta absorbs that as one reset). */
#define EXCEPT_TELEM_COUNT_BITS  20u
#define EXCEPT_TELEM_COUNT_MASK  ((1ull << EXCEPT_TELEM_COUNT_BITS) - 1ull)

/* Bounded CAS retry budget: the rate gate runs in fault context with interrupts
 * disabled (CPU exceptions enter through interrupt gates), so a contended global
 * counter under a multi-CPU exception storm must NOT spin unbounded. On budget
 * exhaustion the best-effort telemetry event is simply dropped. */
#define EXCEPT_TELEM_CAS_RETRIES 64u

/* Map a winnt.h filter disposition to its wire string. */
static const char *except_disp_str(int disposition)
{
    switch (disposition) {
    case EXCEPTION_CONTINUE_EXECUTION: return "continue_execution";
    case EXCEPTION_CONTINUE_SEARCH:    return "continue_search";
    case EXCEPTION_EXECUTE_HANDLER:    return "execute_handler";
    default:                           return "unknown";
    }
}

/* Escape a handler-name label into out[] for safe JSON string embedding: quote
 * and backslash are backslash-escaped; control / non-ASCII bytes are dropped
 * (labels never need \uXXXX). Bounded; always NUL-terminates. Handler names are
 * internal literals today -- this is defense-in-depth per the kernel-code-quality
 * "escape structured output" gate against a future untrusted caller. */
static void except_json_escape(char *out, uint32_t outlen, const char *in)
{
    uint32_t o = 0;
    if (outlen == 0)
        return;
    if (in) {
        for (; *in && o + 2u < outlen; in++) {
            unsigned char c = (unsigned char)*in;
            if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
            else if (c >= 0x20 && c < 0x7f) { out[o++] = (char)c; }
            /* else: drop control / non-ASCII byte */
        }
    }
    out[o] = '\0';
}

uint32_t except_format_dispatch_json(char *buf, uint32_t buflen,
                                     uint32_t code, uint64_t fault_addr,
                                     const char *handler_name, int disposition,
                                     uint32_t pid, uint32_t tid)
{
    char esc[48];
    int n;

    if (!buf || buflen == 0)
        return 0;

    except_json_escape(esc, sizeof(esc), handler_name ? handler_name : "unknown");
    n = snprintf(buf, buflen,
        "{\"type\":\"exception_dispatch\",\"code\":\"0x%08x\","
        "\"addr\":\"0x%016llx\",\"handler\":\"%s\",\"disposition\":\"%s\","
        "\"frames_unwound\":null,\"pid\":%u,\"tid\":%u}",
        code, (unsigned long long)fault_addr, esc, except_disp_str(disposition),
        pid, tid);

    /* snprintf returns the would-be length (C99); a truncated event is dropped
     * rather than emitted as malformed JSON. */
    if (n < 0 || (uint32_t)n >= buflen)
        return 0;
    return (uint32_t)n;
}

int except_telem_rate_gate(volatile uint64_t *state, uint32_t now_ms, uint32_t max)
{
    unsigned retry;

    if (!state)
        return 1;   /* no state = no limiting (fail open) */

    /* Bounded retry (fault context, IF cleared): never spin unbounded on a
     * contended global counter -- drop the best-effort event on exhaustion. */
    for (retry = 0; retry < EXCEPT_TELEM_CAS_RETRIES; retry++) {
        uint64_t old = __atomic_load_n(state, __ATOMIC_RELAXED);
        uint64_t win = old >> EXCEPT_TELEM_COUNT_BITS;
        uint64_t cnt = old & EXCEPT_TELEM_COUNT_MASK;
        uint64_t nwin, ncnt, nv;

        /* Fresh window when the state is UNSET (whole packed word == 0, i.e. the
         * zero-initialized state, count==0) or the coarse-ms delta rolled past the
         * window. `old == 0` (not `win == 0`) is the unset test: a live window may
         * legitimately have win==0 (now_ms is 0 before the first tick and again at
         * the ~49.7-day ms wrap), and the stored count>=1 keeps the word nonzero,
         * so a real now_ms==0 window is not mistaken for "unset" and cannot bypass
         * the cap. Unsigned subtraction makes a coarse-time wrap self-correct. */
        if (old == 0 || ((uint32_t)now_ms - (uint32_t)win) >= EXCEPT_TELEM_WINDOW_MS) {
            nwin = now_ms;   /* store as-is; 0 is a valid window (count>=1 marks set) */
            ncnt = 1;
        } else if (cnt >= max) {
            return 0;   /* saturated -- drop WITHOUT mutating (bounded, no CAS churn) */
        } else {
            nwin = win;
            ncnt = cnt + 1;
        }

        nv = (nwin << EXCEPT_TELEM_COUNT_BITS) | (ncnt & EXCEPT_TELEM_COUNT_MASK);
        if (__atomic_compare_exchange_n(state, &old, nv, 0 /*strong*/,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;   /* within budget -- emit */
        /* CAS lost to a concurrent fault on another CPU: back off, then retry. */
        __asm__ volatile("pause" ::: "memory");
    }
    return 0;   /* CAS contention budget exhausted -- drop this best-effort event */
}

/* System-wide aggregate telemetry window (packed like the per-task state). A
 * farm of processes each within its own per-process budget still cannot exceed
 * EXCEPT_TELEM_GLOBAL_MAX events/window in total. Lock-free CAS -- no lock. */
static volatile uint64_t s_except_telem_global;

void except_log_dispatch(EXCEPTION_RECORD *rec, const char *handler_name, int disposition)
{
    struct task   *t;
    struct thread *th;
    uint32_t now_ms, pid, tid, code;
    uint64_t fault_addr;
    char line[256];   /* max event ~189B; headroom so a valid event never truncates */

    if (!rec)
        return;

    /* Per-process budget FIRST (packed-atomic CAS), then the system-wide aggregate
     * ceiling -- so one process cannot starve another's per-process reservation and
     * the total is still bounded. Attribution uses task_current(), a process-global
     * cursor today; a ring-3 fault is serviced on the CPU that was running the
     * faulting thread, so this is accurate in the common case and best-effort under
     * a concurrent cross-CPU reschedule -- the same caveat klog documents (klog.c),
     * exact once per-CPU current-task lands
     * (-> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md). */
    t = task_current();
    now_ms = (uint32_t)(KeQueryInterruptTimeCoarse() / 10000ull);   /* 100ns -> ms */
    if (t && !except_telem_rate_gate(&t->except_telem_rate, now_ms,
                                     EXCEPT_TELEM_MAX_PER_WINDOW))
        return;   /* this process is over budget this window -- drop */
    if (!except_telem_rate_gate(&s_except_telem_global, now_ms,
                                EXCEPT_TELEM_GLOBAL_MAX))
        return;   /* system-wide aggregate ceiling reached this window -- drop */

    th = thread_current();
    pid = t  ? (uint32_t)t->pid : 0;
    tid = th ? (uint32_t)th->id : 0;
    code = (uint32_t)rec->ExceptionCode;
    fault_addr = (rec->NumberParameters >= 2)
        ? rec->ExceptionInformation[EXCEPTION_INFO_FAULT_ADDR]
        : (uint64_t)(uintptr_t)rec->ExceptionAddress;

    if (except_format_dispatch_json(line, (uint32_t)sizeof(line), code, fault_addr,
                                    handler_name, disposition, pid, tid) == 0)
        return;   /* truncated/failed -- drop rather than emit partial JSON */

    /* klog_unrated: keeps the "except.telem" tag + unified ring/disk/serial sink,
     * but bypasses the shared per-subsystem cap (this event is already
     * per-process rate-limited above). Safe here: caller guarantees a ring-3-
     * originated fault leg, which holds no kernel spinlock. */
    klog_unrated(LOG_INFO, "except.telem", "%s", line);
}

#endif /* CONFIG_EXCEPT_TELEMETRY */

/* --- Kernel-mode structured exception handling (KI_TRY / KI_EXCEPT) ---------
 *
 * A registration-list __try/__except: KI_TRY pushes a stack-local node onto the
 * current thread's chain (kernel_exception_list) and captures a setjmp-style
 * landing pad via ki_seh_setjmp; a kernel fault walks the chain here and, on a
 * matching handler, rewrites the trap frame to the landing pad and returns
 * HANDLED (the IRETQ then "returns 1" from ki_seh_setjmp into the __except body).
 * See include/kernel/except.h for the full contract; this file owns the raise +
 * chain-management side. All of it runs in fault context: klog-free, alloc-free.
 * -------------------------------------------------------------------------- */

/* Per-CPU "inside kernel-SEH dispatch" flag. A fault that RE-ENTERS the walk or
 * a filter on this CPU is a collided kernel exception -- possibly on a different
 * vector than the original (so the per-vector in_use scratch guards do not catch
 * it) -- and terminates. File-static per-CPU array (not the gs:-based per-CPU
 * struct, so the ABI/offsets are untouched); cache-line padded like the scratch
 * to avoid false sharing between CPUs. */
#define EXCEPT_CACHELINE 64u   /* shared with the per-CPU fault scratch below */
struct ki_seh_disp {
    volatile uint32_t active;
} __attribute__((aligned(EXCEPT_CACHELINE)));
static struct ki_seh_disp ki_seh_dispatch[MAX_CPUS] __attribute__((aligned(EXCEPT_CACHELINE)));

/* RFLAGS bits normalized on a handler landing (or checked at raise time). */
#define KI_RFLAGS_TF  0x00000100u
#define KI_RFLAGS_IF  0x00000200u
#define KI_RFLAGS_DF  0x00000400u
#define KI_RFLAGS_NT  0x00004000u
#define KI_RFLAGS_RF  0x00010000u
#define KI_RFLAGS_AC  0x00040000u

/* Bound on the chain walk so a corrupt/cyclic chain cannot spin forever. A real
 * kernel call chain nests far below this. */
#define KI_SEH_MAX_WALK  512u

/* True if [lo, hi) is a valid window and `p` lies within it. */
static int ki_seh_in_range(uint64_t p, uint64_t lo, uint64_t hi)
{
    return (lo != 0) && (hi > lo) && (p >= lo) && (p < hi);
}

/* Does `addr` fall within one of THIS thread's kernel-mode stacks? A kernel
 * thread executes on stack_base..+stack_size (kernel_rsp == 0); a ring-3 thread
 * that trapped into the kernel executes on the per-thread kernel stack
 * (kernel_stack_base..kernel_rsp, kernel_rsp being the TOP). Best-effort: a
 * kmalloc'd kernel stack has no kernel_stack_base and is only covered by the
 * primary window, which is acceptable -- a miss declines (safe), never corrupts. */
static int ki_seh_addr_on_kstack(const struct thread *t, uint64_t addr)
{
    uint64_t base = (uint64_t)(uintptr_t)t->stack_base;
    uint64_t top  = base + (uint64_t)t->stack_size;
    if (ki_seh_in_range(addr, base, top))
        return 1;
    if (t->kernel_stack_base && t->kernel_rsp) {
        base = (uint64_t)(uintptr_t)t->kernel_stack_base;
        top  = t->kernel_rsp;
        if (ki_seh_in_range(addr, base, top))
            return 1;
    }
    return 0;
}

/* Push a registration onto the current thread's chain. Normal (non-fault)
 * context only (KI_TRY lowering). Publishes ONLY when the node lives on the
 * resolved thread's kernel stack: because thread_current() reads a process-global
 * cursor (the per-CPU cursor is TODO-07 SMP phase-2), an SMP mismatch could
 * otherwise link this thread's stack node onto ANOTHER thread's chain. The node
 * is a local on the CALLING thread's stack, so a mismatch (or no current thread,
 * or a thread with untracked stack bounds like the boot stack) fails the check
 * and we skip publication -- KI_TRY then provides no protection (the fault stays
 * terminal), never cross-thread corruption. */
void ki_seh_register(KI_EXCEPTION_REGISTRATION *reg)
{
    struct thread *t = thread_current();
    reg->prev = (KI_EXCEPTION_REGISTRATION *)0;
    reg->owner = (struct thread *)0;
    reg->linked = 0;
    if (!t || !ki_seh_addr_on_kstack(t, (uint64_t)(uintptr_t)reg))
        return;
    /* Record the VALIDATED owner so deregistration is owner-stable: a later
     * cursor change on another CPU can no longer make cleanup resolve a foreign
     * chain and orphan this node into a returned frame. */
    reg->owner = t;
    reg->prev = t->kernel_exception_list;
    reg->linked = 1;
    t->kernel_exception_list = reg;
}

/* Pop a registration off its OWNER's chain (KI_END_TRY / KI_EXCEPT / cleanup-guard
 * lowering). Idempotent via `linked`. Unlinks through the owner recorded at
 * publication -- NOT thread_current() -- so a global-cursor change between
 * register and cleanup cannot leave the node dangling on the real owner's chain
 * as the frame ends. The normal case is a LIFO pop of the head; the scan branch
 * defensively unlinks a node a leaked inner frame left ahead of it. */
void ki_seh_deregister(KI_EXCEPTION_REGISTRATION *reg)
{
    struct thread *t;
    int removed = 0;
    if (!reg->linked)
        return;
    t = reg->owner;
    if (t) {
        if (t->kernel_exception_list == reg) {
            t->kernel_exception_list = reg->prev;
            removed = 1;
        } else {
            KI_EXCEPTION_REGISTRATION *p = t->kernel_exception_list;
            uint32_t guard = 0;
            while (p && p->prev != reg && guard < KI_SEH_MAX_WALK) {
                p = p->prev;
                guard++;
            }
            if (p && p->prev == reg) {
                p->prev = reg->prev;
                removed = 1;
            }
        }
    }
    /* Clear linked once removed. If the owner pointer was somehow stale (never
     * happens on the publication path above) removed stays 0 and linked persists,
     * but no foreign chain was touched. */
    if (removed)
        reg->linked = 0;
}

/* clang `cleanup` handler for KI_EXCEPTION_FRAME: pops the node on ANY scope
 * exit (including a forbidden non-local exit). Idempotent -- a no-op once the
 * normal path (KI_EXCEPT) or the fault path (ki_raise) already cleared `linked`. */
void ki_seh_auto_pop(KI_EXCEPTION_REGISTRATION *reg)
{
    if (reg->linked)
        ki_seh_deregister(reg);
}

/* Pointer form of ki_seh_auto_pop for the KI_TRY do-block cleanup guard: clang
 * `cleanup` passes the ADDRESS of the guarded variable (a
 * KI_EXCEPTION_REGISTRATION *), so this pops through one level of indirection.
 * The do-block guard fires on EVERY exit from the KI_TRY construct -- normal
 * fall-through, a fault-resume, AND a non-local break/continue/goto/return that
 * the lexically-scoped KI_EXCEPTION_FRAME cleanup alone would miss. */
void ki_seh_auto_pop_ptr(KI_EXCEPTION_REGISTRATION *const *reg)
{
    if (reg && *reg)
        ki_seh_auto_pop(*reg);
}

/* Kernel-mode SEH chain walk. Called by ki_dispatch_exception (KernelMode leg)
 * after the first-chance debugger declines. Fault-safe: no klog, no allocation,
 * no locks. Returns HANDLED (frame rewritten to the landing pad, or left as-is
 * for CONTINUE_EXECUTION) or UNHANDLED (fault stays terminal). */
KI_EXCEPTION_DISPOSITION ki_raise_kernel_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                                                   struct interrupt_frame *frame)
{
    struct thread *t;
    KI_EXCEPTION_REGISTRATION *reg;
    KI_EXCEPTION_DISPOSITION result = KI_EXCEPTION_UNHANDLED;
    uint32_t cpu;
    uint32_t walk = 0;

    (void)ctx;

    /* Fault-safe IRQL gate: kernel SEH is only legal at PASSIVE/APC. A fault at
     * elevated IRQL may be inside klog holding s_klog_lock, so this MUST be a
     * non-logging check -- KeGetCurrentIrql is a bare per-CPU read (no klog, no
     * lock). Above APC_LEVEL the fault stays terminal. */
    if (KeGetCurrentIrql() > APC_LEVEL)
        return KI_EXCEPTION_UNHANDLED;

    /* Never resume a handler with interrupts disabled: the faulting code was in
     * a cli / raw critical section, and landing in the __except body would leave
     * IF clear after abandoning whatever would have re-enabled it. Stay terminal. */
    if (!(frame->rflags & KI_RFLAGS_IF))
        return KI_EXCEPTION_UNHANDLED;

    cpu = smp_this_cpu()->cpu_id;
    if (cpu >= MAX_CPUS)
        return KI_EXCEPTION_UNHANDLED;

    /* Collided-exception guard: a fault raised while this CPU is mid-walk or in
     * a filter is a kernel bug -- terminate with KMODE_EXCEPTION_NOT_HANDLED. */
    if (ki_seh_dispatch[cpu].active) {
        KeBugCheckExFrame(frame, BUGCHECK_KMODE_EXCEPTION_NOT_HANDLED,
                          (uint64_t)(uint32_t)rec->ExceptionCode,
                          (uint64_t)(uintptr_t)rec->ExceptionAddress, 0, 0);
        return KI_EXCEPTION_UNHANDLED;  /* unreachable */
    }

    /* Snapshot the current thread EXACTLY ONCE. Every check below uses `t`, so a
     * concurrent global-cursor change on another CPU cannot make us push onto one
     * thread and walk another (the reviewed SMP hazard). */
    t = thread_current();
    if (!t || !t->kernel_exception_list)
        return KI_EXCEPTION_UNHANDLED;

    /* SMP cursor validation: the trap RSP must belong to THIS thread's kernel
     * stack. If the global cursor points at a thread that did NOT fault here, the
     * stacks will not match and we decline (terminal) rather than IRET into a
     * foreign thread. */
    if (!ki_seh_addr_on_kstack(t, frame->rsp))
        return KI_EXCEPTION_UNHANDLED;

    ki_seh_dispatch[cpu].active = 1;

    for (reg = t->kernel_exception_list; reg && walk < KI_SEH_MAX_WALK; reg = reg->prev, walk++) {
        int disp;
        uint64_t node = (uint64_t)(uintptr_t)reg;

        /* Node lifetime + bounds validation (defense-in-depth vs a leaked node):
         * the node must be owned by THIS thread, the node and its saved landing
         * RSP must be on this stack, and the landing must be at or above the fault
         * point (you resume UP the stack, never into an already-passed deeper
         * frame). A stale/corrupt/foreign node fails here and stops the walk. */
        if (reg->owner != t ||
            !ki_seh_addr_on_kstack(t, node) ||
            !ki_seh_addr_on_kstack(t, reg->jmp.Rsp) ||
            reg->jmp.Rsp < frame->rsp) {
            result = KI_EXCEPTION_UNHANDLED;
            break;
        }

        disp = reg->filter
                   ? reg->filter(rec->ExceptionCode, rec->ExceptionAddress, reg->filter_ctx)
                   : EXCEPTION_EXECUTE_HANDLER;

        if (disp == EXCEPTION_CONTINUE_EXECUTION) {
            /* Resume at the faulting instruction, frame unchanged. */
            result = KI_EXCEPTION_HANDLED;
            break;
        }

        if (disp == EXCEPTION_EXECUTE_HANDLER) {
            /* Deliver the code, unlink through the matched node (pop this frame
             * and every inner one that declined via CONTINUE_SEARCH), then
             * rewrite the trap frame to the landing pad. */
            KI_EXCEPTION_REGISTRATION *p;
            uint32_t g = 0;
            reg->code = rec->ExceptionCode;
            reg->fault_addr = rec->ExceptionAddress;
            for (p = t->kernel_exception_list; p && g < KI_SEH_MAX_WALK; p = p->prev, g++) {
                p->linked = 0;
                if (p == reg)
                    break;
            }
            t->kernel_exception_list = reg->prev;

            /* Frame rewrite == the synthesized "ki_seh_setjmp returned 1". */
            frame->rbx = reg->jmp.Rbx;
            frame->rbp = reg->jmp.Rbp;
            frame->r12 = reg->jmp.R12;
            frame->r13 = reg->jmp.R13;
            frame->r14 = reg->jmp.R14;
            frame->r15 = reg->jmp.R15;
            frame->rsp = reg->jmp.Rsp;
            frame->rip = reg->jmp.Rip;
            frame->rax = 1;  /* setjmp second-return value */

            /* Normalize landing RFLAGS: clear DF/AC/TF/NT/RF so the handler runs
             * with sane SysV ABI state (the ISR stub does not CLD, and a stale AC
             * would weaken SMAP). IF is guaranteed set (checked above). */
            frame->rflags &= ~(uint64_t)(KI_RFLAGS_DF | KI_RFLAGS_AC | KI_RFLAGS_TF |
                                         KI_RFLAGS_NT | KI_RFLAGS_RF);
            result = KI_EXCEPTION_HANDLED;
            break;
        }
        /* EXCEPTION_CONTINUE_SEARCH: fall through to the next-older frame. */
    }

    ki_seh_dispatch[cpu].active = 0;
    return result;
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

        /* STATUS_ACCESS_VIOLATION (#GP/#NP here) is an ABI record with exactly
         * two parameters: [0] = access type, [1] = faulting linear address (the
         * winnt.h contract, same as the #PF triage). Unlike #PF these vectors
         * carry no CR2, so the address is not known -- report a read at address
         * 0 so the record is WELL-FORMED (NumberParameters must be 2) rather
         * than left at 0 which a consumer would read as a malformed AV. A finer
         * cause decode (#GP privileged-instruction / invalid-LOCK) is the filed
         * cause-aware refinement. */
        if (m->code == STATUS_ACCESS_VIOLATION) {
            s->rec.NumberParameters = 2;
            s->rec.ExceptionInformation[EXCEPTION_INFO_ACCESS_TYPE] = EXCEPTION_ACCESS_READ;
            s->rec.ExceptionInformation[EXCEPTION_INFO_FAULT_ADDR]  = 0;
        }

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
         * VFS write runs at the interrupted thread's IRQL). A kernel fault does
         * NOT reach the else branch anymore: ki_dispatch_exception owns the
         * kernel terminal (KeBugCheckExFrame, noreturn) and never returns
         * UNHANDLED for KernelMode -- the else is defense-in-depth if that
         * contract ever changes. Ring-3 delivery + per-process termination land
         * with the ring-3-delivery stage. */
        if (mode == UserMode) {
            /* Faulting linear address if the record carries one (AV records set
             * ExceptionInformation[1]); 0 otherwise. */
            uint64_t fault_addr = (s->rec.NumberParameters >= 2)
                ? s->rec.ExceptionInformation[EXCEPTION_INFO_FAULT_ADDR] : 0;
            /* Serial-safe hook FIRST, so the lightweight crash evidence is on the
             * wire before the fallible VFS report (which can block/fault on
             * degraded media -- the filed TODO-24 reentrancy risk, the likelier
             * failure). The inverse risk (a stuck UART stalling the serial write
             * before the VFS report runs) is CLOSED: WerpReportFault emits via
             * serial_write_recoverable, which try-locks g_serial_lock and bounds
             * every UART wait with a SHORT per-byte cap and a call-local budget --
             * so a wedged transmitter costs the report its bytes rather than
             * stalling this survivable path, and it never spends the shared
             * terminal allowance a later panic depends on. */
            WerpReportFault((uint32_t)m->code, fault_addr);
            wer_write_crash_report(frame, vec, fault_addr);
            klog(LOG_ERROR, "except",
                 "%s at %p err=0x%x -> status 0x%x (user, unhandled)",
                 m->name, (void *)(uintptr_t)frame->rip,
                 (uint64_t)frame->err_code, (uint64_t)(uint32_t)m->code);
            /* Terminal action is panic_screen for now: per-process termination
             * (mark the faulting task DEAD + hand to a guaranteed idle frame)
             * needs the per-CPU current-thread cursor + KI_EXCEPTION_TERMINATE
             * primitive owned by TODO-23 s5 -> XREF: 03-memory-concurrency/
             * TODO-07-smp-phase2.md. */
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

    /* #CP (vector 21) is raised by CET shadow-stack (SHSTK) RET mismatches AND by
     * CET IBT missing-ENDBR64 violations -- either capability can issue it, and
     * the two are independently enumerated. Register when EITHER is present; a
     * CPU with neither never issues #CP, so claiming the vector there is wasteful.
     * (The current delivery uses the SHSTK fast-fail subcode; decoding the #CP
     * error-code bits to distinguish SHSTK vs IBT is the filed refinement.) */
    int cp_capable = cpu_has(CPU_FEATURE_CET_SS) || cpu_has(CPU_FEATURE_CET_IBT);
    for (uint32_t i = 0; i < FAULT_MAP_COUNT; i++) {
        uint8_t vec = fault_maps[i].vector;
        if (vec == VECTOR_CONTROL_PROTECTION && !cp_capable)
            continue;
        idt_register_handler(vec, except_common_handler);
    }

    /* __fastfail takes the dedicated (dispatch-bypassing) handler. */
    idt_register_handler(VECTOR_FASTFAIL, except_fastfail_handler);

    /* Ring-3 must be able to raise the software-INT vectors directly, so promote
     * their gates to DPL=3: INT3 (breakpoint), INT 4 (#OF -- note INTO is #UD in
     * 64-bit mode, so vector 4 is reachable from ring 3 only via an explicit
     * `int 4`), and INT 0x29 (__fastfail). CPU-generated faults
     * (#DE/#UD/#NP/#SS/#GP/#CP) ignore gate DPL and need no promotion. */
    idt_set_user_callable(VECTOR_BREAKPOINT);
    idt_set_user_callable(VECTOR_OVERFLOW);
    idt_set_user_callable(VECTOR_FASTFAIL);

    klog(LOG_INFO, "except",
         "fault-to-exception handlers registered (#DE/#DB/#BP/#OF/#UD/#NP/#SS/#GP + __fastfail, #CP %s)",
         cp_capable ? "on" : "off");

    POST16(POST16_EXCEPT_OK);
}
