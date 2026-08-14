/* ============================================================================
 * panic.c -- Styled kernel panic screen
 *
 * Renders a Windows-style blue screen with:
 *   - Sad face emoticon
 *   - Exception name and stop code
 *   - Faulting address and RIP
 *   - Source file + line
 *   - Full register dump
 *   - Stack trace via RBP chain walk
 *   - Crash dump to C:\Impossible\System\crashdump.log
 *   - Auto-restart countdown (configurable via Registry)
 *
 * The blue screen uses direct framebuffer rendering (no compositor)
 * to ensure it works even when the graphics stack is broken.
 * ============================================================================ */

#include "kernel/panic.h"
#include "kernel/bugcheck.h"
#include "kernel/idt.h"
#include "kernel/cpuid.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/version.h"
#include "kernel/fs/vfs.h"
#include "kernel/timer.h"
#include "kernel/boot_splash.h"
#include "registry.h"
#include "bsod_icon.h"
#include "kernel/symtab.h"
#include "kernel/boot_init.h"
#include "kernel/sched/transition_ring.h" /* fast-path transition ring dump */
#include "kernel/smp.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/serial_emergency.h"  /* budget refund on the survivable branch */
#include "kernel/boot_progress.h"   /* boot_stage_history_get (panic evidence) */
#include "kernel/cpu_security.h"    /* __kread_u8 -- guarded caller-string copy */
#include "kernel/vectors.h"         /* VECTOR_NMI -- panic context declaration */
#include "kernel/boot_info.h"       /* boot_history seq + g_boot_info.had_panic */
#include "kernel/mm/pmm.h"          /* pmm_get_free_frames */
#include "kernel/mm/memmap.h"       /* MM_IS_CANONICAL_4LVL -- frame-chain bound */
#include "kernel/quota/quota.h"     /* quota_dump_crash (resource exhaustion) */
#include "kernel/smp.h"
#include "kernel/barrier.h"

/* --- Constants --- */

/* Impossible OS blue (same hue as Windows BSOD but richer) */
#define PANIC_BG_COLOR    0x00003380
#define PANIC_FG_COLOR    0x00FFFFFF
#define PANIC_DIM_COLOR   0x00A0B0D0
#define PANIC_ACCENT      0x004488FF

/* Default auto-restart seconds (0 = disabled) */
#define DEFAULT_RESTART_SECS  30

/* Maximum stack trace depth: PANIC_MAX_STACK_DEPTH lives in panic.h, because it
 * is part of panic_capture_frames' contract (the walk is clamped to it however
 * large a count the caller asks for) rather than a private rendering choice. */

/* Bounded snapshot of the two CALLER-SUPPLIED panic strings. Sized to the
 * evidence record's own fields so the BSOD, the disk dump and the cross-boot
 * record all show the SAME text -- a renderer truncating shorter than the
 * record would make the on-screen reason disagree with the saved one. */
#define PANIC_DESC_SNAP_MAX   256u
#define PANIC_FILE_SNAP_MAX    64u

_Static_assert(PANIC_DESC_SNAP_MAX ==
               sizeof(((struct panic_evidence *)0)->message),
               "panic description snapshot must match the evidence record field");
_Static_assert(PANIC_FILE_SNAP_MAX ==
               sizeof(((struct panic_evidence *)0)->file),
               "panic file snapshot must match the evidence record field");

/* PANIC_STR_NO_GUARD / PANIC_STR_UNREADABLE / PANIC_STR_NONE /
 * PANIC_TRACE_NO_GUARD -- the placeholders emitted INSTEAD of walking a pointer
 * this context may not walk -- live in panic.h so tests assert the exact text.
 *
 * Span searched above the interrupted RSP when walking the frame chain -- the
 * same bound rtl_capture_stack_from_context uses (src/kernel/rtl/unwind.c). A
 * frame pointer outside it is corrupt, not deep. */
#define PANIC_STACK_SPAN      0x10000u

/* Exception names (duplicated here so we don't depend on idt.c internals) */
static const char *panic_exception_names[32] = {
    "DIVISION_BY_ZERO",              /* 0 */
    "DEBUG_EXCEPTION",               /* 1 */
    "NMI_INTERRUPT",                 /* 2 */
    "BREAKPOINT",                    /* 3 */
    "OVERFLOW",                      /* 4 */
    "BOUND_RANGE_EXCEEDED",          /* 5 */
    "INVALID_OPCODE",                /* 6 */
    "DEVICE_NOT_AVAILABLE",          /* 7 */
    "DOUBLE_FAULT",                  /* 8 */
    "COPROCESSOR_SEGMENT_OVERRUN",   /* 9 */
    "INVALID_TSS",                   /* 10 */
    "SEGMENT_NOT_PRESENT",           /* 11 */
    "STACK_SEGMENT_FAULT",           /* 12 */
    "GENERAL_PROTECTION_FAULT",      /* 13 */
    "PAGE_FAULT",                    /* 14 */
    "RESERVED",                      /* 15 */
    "X87_FP_EXCEPTION",              /* 16 */
    "ALIGNMENT_CHECK",               /* 17 */
    "MACHINE_CHECK",                 /* 18 */
    "SIMD_FP_EXCEPTION",             /* 19 */
    "VIRTUALIZATION_EXCEPTION",      /* 20 */
    "CONTROL_PROTECTION",            /* 21 */
    "RESERVED", "RESERVED", "RESERVED", "RESERVED",
    "RESERVED", "RESERVED",
    "HYPERVISOR_INJECTION",          /* 28 */
    "VMM_COMMUNICATION",             /* 29 */
    "SECURITY_EXCEPTION",            /* 30 */
    "RESERVED",                      /* 31 */
};

/* --- Bugcheck (S1 -- TODO-16) ------------------------------------------- */

static BUGCHECK_INFO g_last_bugcheck;

/* STOP code name lookup table */
static const struct { BUGCHECK_CODE code; const char *name; } s_bugcheck_names[] = {
    { 0x0A,       "IRQL_NOT_LESS_OR_EQUAL" },
    { 0x1E,       "KMODE_EXCEPTION_NOT_HANDLED" },
    { 0x50,       "PAGE_FAULT_IN_NONPAGED_AREA" },
    { 0x3B,       "SYSTEM_SERVICE_EXCEPTION" },
    { 0x3E,       "MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED" },
    { 0x77,       "KERNEL_STACK_INPAGE_ERROR" },
    { 0x7A,       "KERNEL_DATA_INPAGE_ERROR" },
    { 0x139,      "KERNEL_SECURITY_CHECK_FAILURE" },
    { 0x133,      "DPC_WATCHDOG_VIOLATION" },
    { 0x109,      "CRITICAL_STRUCTURE_CORRUPTION" },
    { 0xEF,       "CRITICAL_PROCESS_DIED" },
    { 0xC5,       "DRIVER_CORRUPTED_EXPOOL" },
    { 0xD1,       "DRIVER_IRQL_NOT_LESS_OR_EQUAL" },
    { 0xE2,       "MANUALLY_INITIATED_CRASH" },
    { 0xE0000001, "IOS_BOOT_INIT_FAILED" },
    { 0xE0000002, "IOS_HEAP_CORRUPTION" },
    { 0xE0000003, "IOS_GUARD_PAGE_VIOLATION" },
    { 0xE0000004, "IOS_INVARIANT_VIOLATION" },
};

const char *bugcheck_name(BUGCHECK_CODE code)
{
    for (uint32_t i = 0; i < sizeof(s_bugcheck_names) / sizeof(s_bugcheck_names[0]); i++) {
        if (s_bugcheck_names[i].code == code)
            return s_bugcheck_names[i].name;
    }
    return "UNKNOWN";
}

const BUGCHECK_INFO *bugcheck_get_last(void)
{
    return &g_last_bugcheck;
}

/* Shared bugcheck emission core. `frame` is the live trap frame, or NULL for
 * software-initiated (non-fault) callers; passing the real frame preserves the
 * fault-vector + register evidence panic_screen renders and the dump captures.
 * `persist_registry` gates the best-effort cross-boot registry write, which is
 * UNSAFE from an arbitrary fault context (RegSetValueEx takes registry locks and
 * touches the heap, either of which the interrupted thread may already hold or
 * have corrupted): the frame-aware fault terminal skips it and relies on the
 * in-memory g_last_bugcheck + dump pipeline for persistence instead. */
static void panic_screen_impl(struct interrupt_frame *frame, uint64_t error_code,
                              uint32_t bugcheck_code, const uint64_t bugcheck_params[4],
                              const char *description_in, const char *file_in,
                              uint32_t line);

static __attribute__((noreturn)) void ke_bugcheck_emit(
        struct interrupt_frame *frame, BUGCHECK_CODE code,
        uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, int persist_registry)
{
    extern uint64_t KeQueryInterruptTimeCoarse(void);

    /* POST16 renders to the framebuffer (post_display16 -> fb_fill_rect/put_pixel).
     * The frame-aware fault terminal (KeBugCheckExFrame, frame != NULL) runs in an
     * arbitrary kernel-fault context where the framebuffer mapping may be corrupt,
     * so a POST render here could nested-fault and clobber the original crash
     * evidence before panic_collect_evidence claims it. Skip POST for that path
     * (fault-path code uses klog/evidence, not POST16 -- kernel-code-quality Gate 4);
     * the software crash path (NULL frame) keeps its boot-diagnostic POST marker. */
    if (!frame)
        POST16(0xDE40);

    /* NOTE: g_last_bugcheck and the desc buffer are written before
     * panic_screen's owner arbitration, so two SIMULTANEOUS bugchecks on
     * different CPUs can interleave this record (narrow, long-standing
     * window; render/dump stay owner-serialized). Claiming ownership HERE
     * is wrong: the claim must come after the async-worker isolation
     * branch in panic_screen, or an async AP would park holding ownership
     * and silence every later panic. The unified owner-token design is
     * tracked in the crash-dump TODO. */

    /* Store bugcheck params for dump pipeline and BSOD display */
    g_last_bugcheck.code      = code;
    g_last_bugcheck.param1    = p1;
    g_last_bugcheck.param2    = p2;
    g_last_bugcheck.param3    = p3;
    g_last_bugcheck.param4    = p4;
    /* 10 ms units from the lock-free coarse interrupt cache (single __atomic
     * load). NOT system_get_ticks() -- it takes pit_lock on the PIT backend,
     * which can hang here on the fatal/panic path. */
    g_last_bugcheck.timestamp = KeQueryInterruptTimeCoarse() / 100000ULL;

    /* Write last bugcheck to registry for cross-boot persistence.
     * This is best-effort -- registry may not be available during
     * early boot crashes. No lock -- we're about to halt. Gated on
     * persist_registry: the frame-aware fault terminal passes 0 because
     * RegSetValueEx here (locks + heap) can deadlock or refault when the
     * interrupted thread was mid-registry-op or the heap is corrupt. */
    if (persist_registry && kernel_subsystem_ready(SUBSYS_REGISTRY)) {
        HKEY hk = (HKEY)0;
        uint32_t disp = 0;
        if (RegCreateKeyEx((HKEY)(uintptr_t)0x80000002,  /* HKLM */
                           "SYSTEM\\LastBugCheck", 0, (void *)0, 0, 0,
                           (void *)0, &hk, &disp) == 0 && hk) {
            RegSetValueEx(hk, "Code", 0, 4,  /* REG_DWORD */
                          (const uint8_t *)&code, sizeof(code));
            uint32_t p1_32 = (uint32_t)p1, p2_32 = (uint32_t)p2;
            uint32_t p3_32 = (uint32_t)p3, p4_32 = (uint32_t)p4;
            RegSetValueEx(hk, "Param1", 0, 4, (const uint8_t *)&p1_32, 4);
            RegSetValueEx(hk, "Param2", 0, 4, (const uint8_t *)&p2_32, 4);
            RegSetValueEx(hk, "Param3", 0, 4, (const uint8_t *)&p3_32, 4);
            RegSetValueEx(hk, "Param4", 0, 4, (const uint8_t *)&p4_32, 4);
            RegCloseKey(hk);
        }
    }

    /* Build description string for panic_screen. Includes ALL FOUR bugcheck
     * parameters: they are the diagnosis (e.g. for IOS_HEAP_CORRUPTION,
     * P1=faulting pointer, P4=fault class) and previously reached only the
     * registry -- useless when the machine cannot boot back up. */
    static char desc[320];
    {
        const char hex[] = "0123456789ABCDEF";
        const char *name = bugcheck_name(code);
        uint32_t pos = 0;
        const char *p = "STOP 0x";
        while (*p && pos < 300) desc[pos++] = *p++;
        {
            int started = 0;
            for (int shift = 28; shift >= 0; shift -= 4) {
                uint32_t nib = (code >> shift) & 0xF;
                if (nib || started || shift == 0) {
                    desc[pos++] = hex[nib];
                    started = 1;
                }
            }
        }
        p = " (";
        while (*p && pos < 300) desc[pos++] = *p++;
        while (*name && pos < 300) desc[pos++] = *name++;
        desc[pos++] = ')';
        /* Append P1..P4 as 64-bit hex. */
        {
            const uint64_t params[4] = { p1, p2, p3, p4 };
            int pi;
            for (pi = 0; pi < 4 && pos < 300; pi++) {
                const char *lbl = (pi == 0) ? "\n    P1=0x" :
                                  (pi == 1) ? " P2=0x" :
                                  (pi == 2) ? " P3=0x" : " P4=0x";
                int shift;
                int started = 0;
                while (*lbl && pos < 300) desc[pos++] = *lbl++;
                for (shift = 60; shift >= 0; shift -= 4) {
                    uint64_t nib = (params[pi] >> shift) & 0xF;
                    if (nib || started || shift == 0) {
                        if (pos < 316) desc[pos++] = hex[nib];
                        started = 1;
                    }
                }
            }
        }
        desc[pos] = '\0';
    }

    /* Route to the core panic path -- BSOD rendering, klog crash persist, subsystem
     * dump, POST code, halt/restart. error_code drives the VISIBLE serial `ERR=` +
     * BSOD "Error code": for a framed bugcheck it must stay the hardware fault error
     * code (#PF/#GP bits -- the primary evidence when serial is all you have), NOT
     * the STOP code, so pass frame->err_code when a frame exists (else the STOP code
     * for a software crash). bugcheck_code carries the 0x1E/0x3B STOP identity into
     * the cross-boot evidence separately, and the call-local params ride alongside. */
    {
        const uint64_t params[4] = { p1, p2, p3, p4 };
        uint64_t error_code = frame ? frame->err_code : (uint64_t)code;
        panic_screen_impl(frame, error_code, (uint32_t)code, params,
                          desc, __FILE__, __LINE__);
    }

    /* panic_screen should never return, but just in case */
    for (;;) __asm__ volatile("cli; hlt");
}

/* Software/non-fault crash entry -- no trap frame. Registry persistence is safe
 * here: these callers (manual crash, assertion, subsystem failure) are not the
 * arbitrary-fault contexts the frame-aware terminal guards against. */
void KeBugCheckEx(BUGCHECK_CODE code, uint64_t p1, uint64_t p2,
                  uint64_t p3, uint64_t p4)
{
    ke_bugcheck_emit((struct interrupt_frame *)0, code, p1, p2, p3, p4,
                     1 /*persist_registry*/);
}

/* Frame-aware fault terminal for kernel-mode exception dispatch: preserves the
 * trap frame's register/vector evidence and skips the fault-unsafe registry
 * write. -> XREF: TODO-23 s4 (ki_dispatch_exception kernel-mode terminal). */
void KeBugCheckExFrame(struct interrupt_frame *frame, BUGCHECK_CODE code,
                       uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4)
{
    ke_bugcheck_emit(frame, code, p1, p2, p3, p4, 0 /*persist_registry*/);
}

/* --- NMI-triggered crash (S1) ------------------------------------------- */

static uint64_t nmi_crash_handler(struct interrupt_frame *frame)
{
    /* Frame-aware terminal, NOT KeBugCheckEx. An NMI lands at an arbitrary
     * instruction boundary on IST2 -- exactly the "arbitrary fault context"
     * ke_bugcheck_emit guards against -- so the software-crash entry is wrong
     * here twice over: it renders POST16 through a framebuffer that may be
     * mid-update, and it persists to the registry (RegSetValueEx -> registry
     * locks + heap), either of which the interrupted thread may already hold or
     * have corrupted. KeBugCheckExFrame skips both. It also preserves the trap
     * frame this handler previously discarded, so an NMI crash now reports the
     * faulting RIP and registers instead of nothing.
     *
     * The voluntary Ctrl+ScrollLock path (bugcheck_keyboard_check) is NOT
     * affected: it runs in thread context and keeps KeBugCheckEx, so deliberate
     * operator crashes retain cross-boot registry persistence. */
    KeBugCheckExFrame(frame, BUGCHECK_MANUALLY_INITIATED_CRASH,
                      0, 0, 0, 0);
    /* KeBugCheckExFrame never returns */
    return 0;
}

/* --- Keyboard-triggered crash: Ctrl+ScrollLock x2 (S1) ------------------ */

static volatile uint32_t s_ctrl_scroll_count;
static volatile uint64_t s_ctrl_scroll_last_tick;

void bugcheck_keyboard_check(uint8_t scancode, int ctrl_held)
{
    /* ScrollLock make code = 0x46. Must be pressed while Ctrl is held. */
    if (scancode != 0x46 || !ctrl_held)
        return;

    extern uint64_t system_get_ticks(void);
    uint64_t now = system_get_ticks();

    /* Reset if > 2 seconds since last Ctrl+ScrollLock */
    if (now - s_ctrl_scroll_last_tick > 200)
        s_ctrl_scroll_count = 0;

    s_ctrl_scroll_last_tick = now;
    s_ctrl_scroll_count++;

    if (s_ctrl_scroll_count >= 2) {
        /* Check registry: CrashOnCtrlScroll must be enabled */
        uint32_t enabled = 0, vtype = 0, vsize = sizeof(enabled);
        long rc = RegReadKeyValue((HKEY)(uintptr_t)0x80000002,
                                  "SYSTEM\\CrashControl",
                                  "CrashOnCtrlScroll", &vtype,
                                  (uint8_t *)&enabled, &vsize);
        if (rc == 0 && vtype == 4 && vsize == sizeof(enabled) && enabled) {  /* REG_DWORD */
            KeBugCheckEx(BUGCHECK_MANUALLY_INITIATED_CRASH,
                         0, 0, 0, 0);
        }
        s_ctrl_scroll_count = 0;
    }
}

/* --- bugcheck_init -- wire NMI handler ---------------------------------- */

void bugcheck_init(void)
{
    extern void idt_register_handler(uint8_t n,
        uint64_t (*handler)(struct interrupt_frame *));
    idt_register_handler(2, nmi_crash_handler);
}

/* --- FPU/XSAVE state capture (S2 -- TODO-16) ------------------------------ */

/* 4 KiB XSAVE scratch buffer -- 64-byte aligned for XSAVE requirements.
 * Static allocation so it's available even if the heap is corrupt. */
uint8_t g_panic_xsave_buf[4096] __attribute__((aligned(64)));

/* Global CONTEXT record populated during panic for the MDMP pipeline. */
CONTEXT g_panic_context;

/* Atomic panic owner -- first CPU to claim wins; others skip capture.
 * 0xFFFFFFFF = unclaimed. Set via atomic CAS in panic_capture_fpu_state(). */
static volatile uint32_t s_panic_owner = 0xFFFFFFFF;

/* Returns 1 if this CPU is the panic owner (should build context), 0 if not.
 * Re-entrant: if this CPU already owns the panic, returns 1 again so nested
 * faults on the owner CPU don't self-park and deadlock crash handling. */
static int panic_try_claim_owner(void)
{
    struct per_cpu_data *pcpu = smp_this_cpu();
    uint32_t my_id = pcpu ? pcpu->cpu_id : 0;

    /* Try to claim ownership. __sync_val_compare_and_swap returns the old value:
     * - 0xFFFFFFFF: CAS succeeded, we are the new owner
     * - my_id:      CAS failed but we already own it (re-entry)
     * - other:      another CPU owns it */
    uint32_t prev = __sync_val_compare_and_swap(&s_panic_owner, 0xFFFFFFFF, my_id);
    return (prev == 0xFFFFFFFF || prev == my_id);
}

/* ARCH: x86-64 -- will move to arch/ with the rest of the CPU primitives.
 *
 * The declared panic context for this entry: PANIC_CTX_NMI whenever the
 * fault-suppressed kernel read must NOT be used, PANIC_CTX_NORMAL otherwise.
 *
 * DECLARED from hardware state, never probed from memory a panic may have
 * corrupted -- but from TWO signals, not one. The vector alone answers only
 * "was I entered BY an NMI"; it cannot see NESTING, and nesting is the case
 * that defeats the gate. NMI -> nmi_crash_handler -> panic_screen -> a fault
 * re-enters here with int_no naming the INNER vector (a #PF from the frame-chain
 * walk is the live route), so a vector-only predicate returns NORMAL, re-enables
 * __kread_u8, and its fixup IRETQs -- re-arming NMI delivery while the outer
 * NMI's frames are still live under RSP on IST2, because #PF has no IST of its
 * own. The next NMI resets RSP to the IST2 top and overwrites them.
 *
 * idt_in_nmi() supplies the depth the vector cannot. It is keyed by CPUID rather
 * than per-CPU data precisely so this predicate stays usable from the
 * GS-independent pre-arbitration dump.
 *
 * A NULL frame means a software panic, which is never NMI context BY VECTOR --
 * but may still be nested inside one, so the depth test applies there too. */
uint32_t panic_declared_ctx(struct interrupt_frame *frame)
{
    if (frame && frame->int_no == VECTOR_NMI)
        return PANIC_CTX_NMI;
    if (idt_in_nmi())
        return PANIC_CTX_NMI;
    return PANIC_CTX_NORMAL;
}

void panic_capture_fpu_state(void)
{
    POST16(POST16_FPU_CAPTURE);

    /* Zero the buffer so we can detect whether anything was saved */
    for (uint32_t i = 0; i < sizeof(g_panic_xsave_buf); i++)
        g_panic_xsave_buf[i] = 0;

    /* Clear CR0.TS and CR0.EM to prevent #NM during FXSAVE/XSAVE.
     * Lazy FPU scheduling sets CR0.TS; panic path must bypass it. */
    {
        uint64_t cr0;
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((1ULL << 3) | (1ULL << 2));  /* clear TS (bit 3) and EM (bit 2) */
        __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    }

    /* Runtime check: XSAVE requires both CPUID support AND CR4.OSXSAVE
     * actually set on this CPU. cpu_has() checks CPUID; verify CR4 too. */
    {
        int xsave_safe = 0;
        if (cpu_has(CPU_FEATURE_XSAVE)) {
            uint64_t cr4;
            __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
            if (cr4 & (1ULL << 18))  /* CR4.OSXSAVE (bit 18) */
                xsave_safe = 1;
        }

        if (xsave_safe) {
            /* XSAVE: save only the state components actually enabled in XCR0.
             * Using the active mask (not all-ones) avoids #GP on unsupported
             * components. This matches the scheduler's save/restore paths. */
            extern struct cpu_features g_cpu;
            uint32_t lo = (uint32_t)(g_cpu.xcr0_active);
            uint32_t hi = (uint32_t)(g_cpu.xcr0_active >> 32);
            __asm__ volatile (
                "xsave64 (%0)"
                :
                : "r"(g_panic_xsave_buf), "a"(lo), "d"(hi)
                : "memory"
            );
        } else {
            /* FXSAVE fallback -- always available on x86-64. */
            __asm__ volatile (
                "fxsave64 (%0)"
                :
                : "r"(g_panic_xsave_buf)
                : "memory"
            );
        }
    }
}

void panic_build_context(struct interrupt_frame *frame, CONTEXT *ctx)
{
    /* Frame-backed groups come from the shared converter. The panic path then
     * APPENDS live segment/debug/FPU state below -- sound here (and only here)
     * because a panic has stopped the world and deliberately wants this cpu's
     * current state, which is exactly what context_from_frame must never
     * fabricate for a general caller. */
    ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (frame) {
        context_from_frame(frame, ctx);
    } else {
        uint8_t *p = (uint8_t *)ctx;
        for (uint32_t i = 0; i < sizeof(CONTEXT); i++)
            p[i] = 0;
    }

    /* Segments, debug registers and FPU state are captured live below, so
     * advertise them as valid on top of what the converter reported. */
    ctx->ContextFlags |= CONTEXT_SEGMENTS | CONTEXT_DEBUG_REGISTERS |
                         CONTEXT_FLOATING_POINT;

    /* --- Segment registers --- */
    /* DS/ES/FS/GS aren't in interrupt_frame; read them live.
     * In a panic context these are still valid. */
    {
        uint16_t ds, es, fs, gs;
        __asm__ volatile ("mov %%ds, %0" : "=r"(ds));
        __asm__ volatile ("mov %%es, %0" : "=r"(es));
        __asm__ volatile ("mov %%fs, %0" : "=r"(fs));
        __asm__ volatile ("mov %%gs, %0" : "=r"(gs));
        ctx->SegDs = ds;
        ctx->SegEs = es;
        ctx->SegFs = fs;
        ctx->SegGs = gs;
    }

    /* --- Debug registers --- */
    {
        uint64_t dr0, dr1, dr2, dr3, dr6, dr7;
        __asm__ volatile ("mov %%dr0, %0" : "=r"(dr0));
        __asm__ volatile ("mov %%dr1, %0" : "=r"(dr1));
        __asm__ volatile ("mov %%dr2, %0" : "=r"(dr2));
        __asm__ volatile ("mov %%dr3, %0" : "=r"(dr3));
        __asm__ volatile ("mov %%dr6, %0" : "=r"(dr6));
        __asm__ volatile ("mov %%dr7, %0" : "=r"(dr7));
        ctx->Dr0 = dr0;
        ctx->Dr1 = dr1;
        ctx->Dr2 = dr2;
        ctx->Dr3 = dr3;
        ctx->Dr6 = dr6;
        ctx->Dr7 = dr7;
    }

    /* --- Floating point state (CONTEXT_FLOATING_POINT) --- */
    /* Copy the FXSAVE region (first 512 bytes of XSAVE area) into FltSave */
    {
        const uint8_t *src = g_panic_xsave_buf;
        uint8_t *dst = (uint8_t *)&ctx->FltSave;
        for (uint32_t i = 0; i < 512; i++)
            dst[i] = src[i];
    }

    /* Copy MXCSR from the FXSAVE area into the top-level CONTEXT.MxCsr */
    {
        uint32_t mxcsr;
        const uint8_t *p = &g_panic_xsave_buf[24]; /* MXCSR at FXSAVE offset 24 */
        mxcsr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        ctx->MxCsr = mxcsr;
    }

    /* --- YMM high halves (AVX state) --- */
    if (cpu_has(CPU_FEATURE_XSAVE) && cpu_has(CPU_FEATURE_AVX)) {
        /* XSAVE header is at offset 512 (64 bytes).
         * AVX (component 2) YMM high halves start at offset 576.
         * 16 registers x 16 bytes = 256 bytes -> VectorRegister[0..15]. */
        const uint8_t *ymm_hi = &g_panic_xsave_buf[576];
        uint8_t *vr = (uint8_t *)ctx->VectorRegister;
        for (uint32_t i = 0; i < 256; i++)
            vr[i] = ymm_hi[i];
    }
}

/* --- Helpers --- */

static inline uint64_t read_cr2(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

/* Pre-arbitration emit. TERMINAL accounting, unconditionally, and deliberately
 * GS-INDEPENDENT: this is the panic reason and register dump, the evidence that
 * matters most, and it must not depend on per-CPU state being intact.
 *
 * An earlier revision chose the budget here by asking whether this CPU was an
 * async worker (so a survivable park would not spend the terminal allowance).
 * That was wrong in kind: the check calls smp_this_cpu(), which reads gs:0 and
 * accepts any non-NULL value without validating it, so a panic caused by a
 * corrupt or unmapped GS base took a nested fault BEFORE any output -- and on a
 * #DF that triple-faults and erases everything. It also contradicted the
 * routing design in serial.c, which uses CPUID precisely because GS cannot be
 * trusted here. The budget question is not worth a faultable read in front of
 * the crash evidence; the async branch below still uses recoverable accounting
 * for its OWN diagnostic, where per-CPU state has already been dereferenced. */
static void panic_emit(const char *s)
{
    serial_write_emergency(s);
}


/* --- Hang-proof hex writer for the unconditional panic-reason dump ---
 *
 * Formats a 64-bit value as "0x<hex>" into a stack buffer and emits it with ONE
 * serial_write_emergency call. No heap, no formatting library, and no blocking
 * lock acquisition -- so even if klog, printk, the heap, or the compositor lock
 * are corrupted, and even if the interrupted code holds g_serial_lock, this
 * still produces readable output.
 *
 * The buffer matters because emitting per character (serial_putchar_emergency)
 * drops and retakes the lock between every hex digit, and the register dump
 * below deliberately runs on EVERY panicking CPU before panic_try_claim_owner()
 * parks the losers -- so a concurrent panic would interleave into the middle of
 * a register VALUE at every digit boundary. One emit per value removes those
 * voluntary boundaries.
 *
 * It is NOT an atomicity guarantee, and must not be read as one: the emergency
 * writer proceeds UNLOCKED when its try-lock fails, so a second panicking CPU
 * can still write into the UART mid-value. Interleaving is merely made rare
 * rather than structural; preventing it outright would need arbitration between
 * concurrent direct panic writers, which the pre-arbitration dump deliberately
 * does not have.
 *
 * The emergency writer is named EXPLICITLY rather than relying on the
 * serial_write/serial_putchar re-routing latch, because this dump runs before
 * the panic is known to be system-terminal (the async-isolation branch can park
 * one AP and let the system continue), so the latch is deliberately not armed
 * yet. */
static void serial_write_hex(uint64_t v)
{
    static const char d[] = "0123456789ABCDEF";
    char buf[19];                 /* "0x" + 16 nibbles + NUL */
    uint32_t pos = 0;
    int leading = 1;
    int i;

    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (i = 60; i >= 0; i -= 4) {
        uint32_t nib = (uint32_t)((v >> i) & 0xFu);
        if (leading && nib == 0 && i > 0)
            continue;
        leading = 0;
        buf[pos++] = d[nib];
    }
    buf[pos] = '\0';
    panic_emit(buf);
}

/* Bounded appenders so a whole diagnostic record can be built in one stack
 * buffer and emitted with ONE call. Multiple fragments would each start a fresh
 * call-local budget, so a wedged UART could pay a full-length wait per fragment. */
static void panic_append(char *buf, uint32_t cap, uint32_t *pos, const char *s)
{
    while (*s && *pos + 1u < cap)
        buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

/* panic_append for a CALLER-SUPPLIED string, i.e. one that may itself be the
 * corruption being reported. Same bounds, but each byte is read through the
 * guarded load, and an unreadable byte ends the append with a marker instead of
 * faulting.
 *
 * This exists because guarding pe_copy and the emergency serial walk was NOT
 * sufficient: the async-isolation branch builds its diagnostic with
 * panic_append, so the ORIGINAL description and step name were still read raw
 * there -- on a survivable AP fault, after `async_done` was already published,
 * which is precisely where a nested fault turns an isolated failure into a
 * system-terminal panic. A guarantee about a pointer holds only when every site
 * that dereferences it honours it. */
static void panic_append_guarded(char *buf, uint32_t cap, uint32_t *pos,
                                 const char *s, uint32_t ctx)
{
    if (!s) {
        panic_append(buf, cap, pos, "(null)");
        return;
    }
    if (!serial_emerg_ctx_allows_guarded_read(ctx)) {
        panic_append(buf, cap, pos, s);
        return;
    }
    while (*pos + 1u < cap) {
        uint8_t b;
        if (__kread_u8(&b, s)) {
            panic_append(buf, cap, pos, "<unreadable>");
            return;
        }
        if (!b)
            break;
        buf[(*pos)++] = (char)b;
        s++;
    }
    buf[*pos] = '\0';
}

static void panic_append_hex(char *buf, uint32_t cap, uint32_t *pos, uint64_t v)
{
    static const char d[] = "0123456789ABCDEF";
    int leading = 1;
    int i;

    panic_append(buf, cap, pos, "0x");
    for (i = 60; i >= 0; i -= 4) {
        uint32_t nib = (uint32_t)((v >> i) & 0xFu);
        if (leading && nib == 0 && i > 0)
            continue;
        leading = 0;
        if (*pos + 1u < cap)
            buf[(*pos)++] = d[nib];
    }
    buf[*pos] = '\0';
}


/* Simple integer to decimal string (for countdown display) */
static void itoa_simple(uint32_t val, char *buf)
{
    char tmp[12];
    int i = 0;
    int j;

    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (val > 0) {
        tmp[i++] = '0' + (char)(val % 10);
        val /= 10;
    }
    for (j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

/* Draw the embedded BSOD icon (alpha-blended white over blue background) */
static void draw_bsod_icon(uint32_t x, uint32_t y)
{
    uint32_t ix, iy;
    for (iy = 0; iy < BSOD_ICON_H; iy++) {
        for (ix = 0; ix < BSOD_ICON_W; ix++) {
            uint32_t px = bsod_icon_pixels[iy * BSOD_ICON_W + ix];
            uint32_t a = (px >> 24) & 0xFF;
            if (a == 0) continue;  /* fully transparent */
            if (a == 255) {
                fb_put_pixel(x + ix, y + iy, 0x00FFFFFF);
            } else {
                /* Alpha blend: white * a + bg * (255-a) */
                uint32_t bg_r = (PANIC_BG_COLOR >> 16) & 0xFF;
                uint32_t bg_g = (PANIC_BG_COLOR >> 8) & 0xFF;
                uint32_t bg_b = PANIC_BG_COLOR & 0xFF;
                uint32_t r = (0xFF * a + bg_r * (255 - a)) / 255;
                uint32_t g = (0xFF * a + bg_g * (255 - a)) / 255;
                uint32_t b = (0xFF * a + bg_b * (255 - a)) / 255;
                fb_put_pixel(x + ix, y + iy, (r << 16) | (g << 8) | b);
            }
        }
    }
}

/* --- Panic-entry snapshot of the caller-supplied strings --- */

/* The BSOD, the disk crash dump and the cross-boot evidence record all name the
 * crash using the SAME two pointers the crash may have corrupted. Guarding each
 * consumer separately does not make the pointer safe -- it only moves which
 * renderer dies on it, which is exactly how a machine ended up reporting its
 * reason on serial and then painting no bugcheck and writing no dump. So the
 * pointers are read ONCE, at panic entry, into kernel-owned storage, and every
 * later consumer reads that copy.
 *
 * The copy is bounded, so a description missing its NUL costs a truncation
 * rather than a walk off the end of the mapping.
 *
 * ARCH: x86-64 -- the guarded read is the fault-suppressed kernel load. */
void panic_snapshot_str(char *dst, uint32_t cap, const char *src,
                        uint32_t ctx, const char *if_null)
{
    const char *fixed = (const char *)0;
    uint32_t i = 0u;

    if (!cap)
        return;

    if (!src) {
        /* Decided entirely from `src`, never by falling through to the reader:
         * a NULL `if_null` must still mean "render nothing", not "walk address
         * zero" -- which is what a shared `fixed == NULL` test would do, in
         * every context including the ones that forbid the read. */
        if (!if_null) {
            dst[0] = '\0';
            return;
        }
        fixed = if_null;
    } else if (!serial_emerg_ctx_allows_guarded_read(ctx))
        /* No fixup available here: a raw walk of a corrupt pointer would fault,
         * and in NMI context the fixup's IRETQ would re-arm NMI delivery over
         * the live IST frames (see panic_declared_ctx). A fixed string names
         * less than the description would have, and costs nothing. */
        fixed = PANIC_STR_NO_GUARD;

    if (!fixed) {
        for (; i + 1u < cap; i++) {
            uint8_t b;
            if (__kread_u8(&b, &src[i])) {
                /* A partial read still names the crash; only a pointer that was
                 * unreadable from its FIRST byte carries no information, and an
                 * empty line would read as "no reason given" rather than "the
                 * reason pointer was part of the corruption". */
                if (i == 0u)
                    fixed = PANIC_STR_UNREADABLE;
                break;
            }
            if (!b)
                break;
            dst[i] = (char)b;
        }
    }

    if (fixed)
        for (i = 0u; i + 1u < cap && fixed[i]; i++)
            dst[i] = fixed[i];

    dst[i] = '\0';
}

/* --- Fault-safe frame-chain walk --- */

/* The kernel image window, same source of truth the RTL walker uses. */
extern char __text_start[];
extern char __text_end[];

static int panic_is_kernel_text(uint64_t pc)
{
    return pc >= (uint64_t)(uintptr_t)__text_start &&
           pc <  (uint64_t)(uintptr_t)__text_end;
}


/* Both terminal renderers (BSOD and disk dump) used to walk the RBP chain by
 * raw dereference, gated only by a hardcoded address window -- so a corrupt but
 * in-window RBP took a terminal fault in the very abort context the caller
 * strings are now protected from. One walker, fault-suppressed reads, and the
 * same frame-validity rules the RtlCaptureStackBackTrace walker applies.
 *
 * Returns the number of return addresses written to out[]. Zero in a context
 * where the guarded read is unavailable: the walk is the documented route by
 * which a nested #PF re-arms NMI delivery, so it is not attempted there.
 *
 * ARCH: x86-64 -- frame-pointer chain layout and canonical-address form. */
uint32_t panic_capture_frames(struct interrupt_frame *frame, uint32_t ctx,
                              uint64_t *out, uint32_t count)
{
    uint64_t rbp;
    uint64_t lo;
    uint64_t hi;
    uint64_t prev = 0u;
    uint32_t n = 0u;

    if (!out || !count || !serial_emerg_ctx_allows_guarded_read(ctx))
        return 0u;

    /* A RING-3 frame carries the USER stack pointers, and this renders a KERNEL
     * stack trace onto the BSOD and into crashdump.log. vmm.c calls panic_screen
     * with a user-mode frame, so those values are not merely corrupt but
     * attacker-INFLUENCED; walking them would print user memory as kernel
     * frames. Use this CPU's live kernel registers instead -- the kernel stack
     * is what the trace is supposed to describe. */
    if (frame && (frame->cs & 0x3u) == 0u) {
        rbp = frame->rbp;
        lo  = frame->rsp;
    } else {
        __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
        __asm__ volatile ("mov %%rsp, %0" : "=r"(lo));
    }
    if (!rbp || !lo)
        return 0u;

    /* An IST entry (#DF/#MC/NMI) switched stacks, so the frame's RSP and RBP
     * describe the INTERRUPTED stack and stay a coherent pair; the live-RBP
     * fallback above pairs with the live RSP for the same reason.
     *
     * The wrap guard is the only bound check needed before computing `hi`: it
     * proves hi == lo + PANIC_STACK_SPAN >= lo + 16, so a further `hi < lo + 16`
     * test could never fire and is deliberately not written. */
    if (lo > ~(uint64_t)0 - PANIC_STACK_SPAN)
        return 0u;                       /* span would wrap */
    hi = lo + PANIC_STACK_SPAN;

    while (n < count && n < PANIC_MAX_STACK_DEPTH) {
        uint64_t saved_rbp, ret;

        /* 8-aligned, at/above SP with the whole slot pair in range, strictly
         * climbing (no cycle), and canonical -- a non-canonical slot would #GP
         * rather than #PF and escape the read's fixup, so reject it first. */
        if (rbp & 0x7u)                             break;
        if (rbp < lo || rbp > hi - 16u)             break;
        if (rbp <= prev)                            break;
        if (!MM_IS_CANONICAL_4LVL(rbp) ||
            !MM_IS_CANONICAL_4LVL(rbp + 15u))       break;

        if (__kstack_read_u64(&ret, (const void *)(uintptr_t)(rbp + 8u)) != 0)
            break;
        if (__kstack_read_u64(&saved_rbp, (const void *)(uintptr_t)rbp) != 0)
            break;
        /* Same acceptance test the RTL walker applies (rtlp_is_code_pc): a
         * return address outside the kernel text is not a kernel frame. Without
         * it the two walkers were only NEARLY equivalent, and this is the check
         * that keeps a ring-3 or wild chain from rendering data as a trace. */
        if (ret == 0u || !panic_is_kernel_text(ret))
            break;

        out[n++] = ret;
        prev = rbp;
        rbp  = saved_rbp;
    }

    return n;
}

/* --- Crash-dump append helpers: capacity-aware, always NUL-terminated ---
 *
 * `cap` is the FULL buffer size and one byte is always reserved for the
 * terminator, so *pos never reaches cap and buf[*pos] is always writable.
 * `s` may be NULL (renders nothing) so a caller need not special-case it. */
static void cd_put(char *buf, uint32_t cap, uint32_t *pos, const char *s)
{
    if (!s || cap == 0u)
        return;
    while (*s && *pos + 1u < cap)
        buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

/* Hex, `digits` wide zero-padded, or compact when digits == 0. */
static void cd_put_hex(char *buf, uint32_t cap, uint32_t *pos, uint64_t v,
                       int digits)
{
    char tmp[17];
    int i;

    if (digits > 0) {
        for (i = digits - 1; i >= 0; i--) {
            tmp[i] = "0123456789ABCDEF"[v & 0xFu];
            v >>= 4;
        }
        tmp[digits] = '\0';
        cd_put(buf, cap, pos, tmp);
        return;
    }

    i = 16;
    tmp[i] = '\0';
    if (v == 0u) {
        tmp[--i] = '0';
    } else {
        while (v && i > 0) {
            tmp[--i] = "0123456789abcdef"[v & 0xFu];
            v >>= 4;
        }
    }
    cd_put(buf, cap, pos, &tmp[i]);
}

/* --- Crash dump to file --- */

/* Returns 1 only when the FULL dump was confirmed written (vfs_write
 * accepted every byte); the caller's "saved" message must not lie about a
 * dump that never landed (previously it printed whenever C: was mounted).
 *
 * `description` and `file` are the panic-entry SNAPSHOTS, not the caller's
 * pointers -- this runs last of the three renderers, so it is the one that
 * never ran at all when an earlier consumer faulted on the original. */
static int write_crash_dump(struct interrupt_frame *frame, uint32_t ctx,
                            const char *description, const char *file,
                            uint32_t line, const uint64_t *frames,
                            uint32_t nframes)
{
    char buf[2048];
    uint32_t pos = 0u;
    uint32_t depth;
    char num[12];

    if (!vfs_is_mounted('C'))
        return 0;

    /* Build crash dump text manually (no snprintf in freestanding).
     *
     * EVERY append goes through cd_put, including the single-character
     * delimiters and the final terminator. The previous shape bounded only the
     * string copies (`while (*s && pos < 2040)`) and then wrote `buf[pos++] =
     * '\n'` unchecked at five sites, one of them once per frame -- so a full
     * dump could reach 2040 and then walk to 2059, past the end of a 2 KiB
     * buffer sitting on an IST stack, corrupting the very frame it was writing
     * a crash report from. A capacity-aware helper is the only shape where that
     * cannot come back: there is no unbounded write left to overlook. */
    {
        cd_put(buf, sizeof buf, &pos, "=== IMPOSSIBLE OS CRASH DUMP ===\n");

        cd_put(buf, sizeof buf, &pos, "Description: ");
        cd_put(buf, sizeof buf, &pos, description);
        cd_put(buf, sizeof buf, &pos, "\n");

        if (file) {
            cd_put(buf, sizeof buf, &pos, "Source: ");
            cd_put(buf, sizeof buf, &pos, file);
            cd_put(buf, sizeof buf, &pos, ":");
            itoa_simple(line, num);
            cd_put(buf, sizeof buf, &pos, num);
            cd_put(buf, sizeof buf, &pos, "\n");
        }

        if (frame) {
            cd_put(buf, sizeof buf, &pos, "\nRegisters:\n");
            cd_put(buf, sizeof buf, &pos,
                   "  (See serial output for full register dump)\n");
        }

        cd_put(buf, sizeof buf, &pos, "\nStack Trace:\n");
    }

    /* The trace was captured ONCE by the caller, before the BSOD rendered it.
     * Walking it again here would repeat up to 32 guarded reads over the same
     * stack and -- on a software panic, where the walk starts from live
     * registers -- describe a DIFFERENT stack than the one the user just read
     * off the screen. */
    if (nframes == 0u && !serial_emerg_ctx_allows_guarded_read(ctx))
        cd_put(buf, sizeof buf, &pos, "  " PANIC_TRACE_NO_GUARD "\n");

    for (depth = 0u; depth < nframes; depth++) {
        uint64_t ret_addr = frames[depth];
        uint64_t sym_off = 0;
        const char *sym_name = symtab_resolve(ret_addr, &sym_off);

        cd_put(buf, sizeof buf, &pos, "  #");
        itoa_simple(depth, num);
        cd_put(buf, sizeof buf, &pos, num);
        cd_put(buf, sizeof buf, &pos, " at 0x");
        cd_put_hex(buf, sizeof buf, &pos, ret_addr, 16);
        if (sym_name) {
            cd_put(buf, sizeof buf, &pos, "  ");
            cd_put(buf, sizeof buf, &pos, sym_name);
            cd_put(buf, sizeof buf, &pos, "+0x");
            cd_put_hex(buf, sizeof buf, &pos, sym_off, 0);
        }
        cd_put(buf, sizeof buf, &pos, "\n");
    }

    /* Ensure the directory exists and write the crash dump */
    {
        struct vfs_node *sys_dir = vfs_open("C:\\Impossible\\System", VFS_O_READ);
        if (sys_dir) {
            vfs_close(sys_dir);
        }
    }

    vfs_create("C:\\Impossible\\System\\crashdump.log", 1);
    {
        /* TRUNC: a shorter record must not leave the previous dump's tail
         * spliced onto this one (the klog.c 2026-06 stale-bytes class).
         * Success requires the full write AND a durable flush -- the BSOD
         * "saved" line must not vouch for bytes still in a cache. */
        struct vfs_node *dump_file = vfs_open(
            "C:\\Impossible\\System\\crashdump.log",
            VFS_O_WRITE | VFS_O_TRUNC);
        if (dump_file) {
            int wrote = vfs_write(dump_file, 0, pos,
                                  (const uint8_t *)buf);
            int flushed = vfs_flush(dump_file);
            vfs_close(dump_file);
            return wrote == (int)pos && flushed == 0;
        }
    }
    return 0;
}

/* ============================================================================
 * Panic forensic evidence -- cross-boot crash record
 * ========================================================================== */

/* First-caller-wins, in TWO parts, because the two things it guards are not the
 * same thing and no longer happen at the same moment.
 *
 * RESERVATION decides WHICH CPU's crash the record describes. It is taken at
 * panic entry, before the caller-string snapshot, because the snapshot performs
 * up to PANIC_DESC_SNAP_MAX fault-suppressed reads -- and a CPU whose
 * description pointer is corrupt (the case this whole path exists for) pays a
 * fixup per faulting byte. If the claim came after that work, the originating
 * CPU could lose the record to a CPU that panicked LATER with a short, valid
 * string, and the cross-boot evidence would then describe the cascade instead
 * of the failure that started it.
 *
 * POPULATION stays a separate one-shot so a nested fault during BSOD render
 * still cannot overwrite the original record. Reservation alone cannot carry
 * that: the re-entering CPU is the SAME CPU, so it matches its own claim.
 *
 * Owner identity is the panic-safe APIC id -- the same identity the serial
 * owner word and the evidence record's own cpu_id field use, and readable
 * without GS. */
#define PANIC_EVIDENCE_NO_OWNER  0xFFFFFFFFu
_Static_assert(PANIC_EVIDENCE_NO_OWNER > CPU_PANIC_SAFE_ID_MASK,
               "the no-owner sentinel must not alias a real CPU identity");
static volatile uint32_t s_evidence_owner    = PANIC_EVIDENCE_NO_OWNER;

/* Set only AFTER a COMPLETE record has been published, never on entry.
 *
 * The difference is the whole guarantee. A one-shot claimed on entry makes
 * population irrevocable before anything durable exists, so an abort between
 * the claim and the magic store -- and `cli` masks neither NMI nor #MC -- locks
 * every later invocation out of a page that was never finished, and the boot
 * ends with no record at all. Claimed on completion instead, a nested abort
 * that interrupted an unfinished record simply writes its own complete one:
 * the outer invocation is never coming back, so the nested crash is the best
 * evidence still obtainable. A record that DID complete is never overwritten,
 * which is the original nested-fault-during-BSOD guarantee, unchanged.
 *
 * Kept in RAM rather than read back from the page's magic so a record left by a
 * PREVIOUS boot can never suppress this boot's collection -- the flag says
 * "THIS boot published one" and the magic says "the page holds a restorable
 * record"; completion needs both, and neither alone is the question. */
static volatile int      s_evidence_published = 0;

/* Claim the single evidence record. Returns 1 to the FIRST panic invocation of
 * this boot and 0 to every other one -- another CPU's, and this CPU's own
 * nested re-entry alike, which is what keeps the record describing the crash
 * that started the cascade.
 *
 * ONE compare-and-swap, not a flag plus an owner store: `cli` does not mask NMI
 * or #MC, so any gap between "reserved" and "owner published" is a window an
 * abort can land in -- and a nested abort that lands there would find a
 * reservation belonging to nobody, decline to write, and (being terminal) never
 * let the outer invocation resume, leaving the boot with no record at all. */
static int panic_evidence_reserve(void)
{
    uint32_t me = cpu_panic_safe_apic_id();
    uint32_t prev = __sync_val_compare_and_swap(&s_evidence_owner,
                                                PANIC_EVIDENCE_NO_OWNER, me);
    return prev == PANIC_EVIDENCE_NO_OWNER;
}

/* Whether the record belongs to `me` -- the backstop for a caller that reaches
 * panic_collect_evidence without having reserved. The identity is passed IN
 * rather than re-derived: cpu_panic_safe_apic_id executes CPUID, which
 * serializes and exits to the hypervisor under KVM/WHPX, and the collector would
 * otherwise pay for it four times on a path whose whole cost function is
 * instructions-between-fault-and-durable-record. */
static int panic_evidence_owned_by(uint32_t me)
{
    return __atomic_load_n(&s_evidence_owner, __ATOMIC_ACQUIRE) == me;
}

/* Off-stack klog scratch: the collector may run on a small IST stack (#DF), so
 * the snapshot lands in BSS, not on the panic stack. Panic is cli'd and
 * first-caller-wins, so a single static buffer is not a reentrancy hazard. */
static klog_entry_t s_panic_klog_scratch[PANIC_EVIDENCE_KLOGS];

/* Fault-safe port read (no shared io.h in this tree; pic.c uses the same). */
static inline uint8_t pe_inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Copy a caller-supplied C string into the evidence record, surviving a source
 * pointer that is itself part of the corruption being reported.
 *
 * THIS IS THE FIRST DEREFERENCE OF THE PANIC DESCRIPTION -- earlier than the
 * serial writer, because panic_collect_evidence runs before any output (it
 * captures forensics before anything that could itself fault). Guarding only the
 * serial walk would therefore protect nothing: a corrupt description would fault
 * here, re-entering the panic path and losing the original crash evidence, long
 * before the emergency writer ever saw the pointer.
 *
 * `ctx` is the panic context and selects whether the guarded read is available;
 * see serial.h PANIC_CTX_*. On a fault the copy stops and the record keeps what
 * was readable, NUL-terminated -- a truncated description still names the crash,
 * whereas a triple fault names nothing. */
static void pe_copy(char *dst, uint32_t cap, const char *src, uint32_t ctx)
{
    uint32_t i = 0u;

    if (src) {
        for (; i + 1u < cap; i++) {
            char c;

            if (serial_emerg_ctx_allows_guarded_read(ctx)) {
                uint8_t b;
                if (__kread_u8(&b, &src[i]))
                    break;               /* unreadable -- keep what we have */
                c = (char)b;
            } else {
                c = src[i];
            }
            if (!c)
                break;
            dst[i] = c;
        }
    }
    dst[i] = '\0';
}

uint32_t panic_crc32(const void *data, uint32_t len)
{
    /* Standard reflected IEEE CRC-32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF);
     * crc32("123456789") == 0xCBF43926. Same constants as klog.c crash_crc32. */
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= (uint32_t)p[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1u)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc = (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void panic_collect_evidence(struct interrupt_frame *frame, uint32_t bugcheck_code,
                            const uint64_t bugcheck_params[4],
                            const char *message, const char *file, uint32_t line)
{
    /* The page at PANIC_EVIDENCE_ADDR is identity-mapped and reserved by PMM.
     * Raw physical writes only -- no kmalloc / VFS / printk / spinlock here. */
    struct panic_evidence *ev = (struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR;
    uint32_t ctx;
    uint32_t me;

    /* On an SMP double-panic two CPUs must not both write the fixed 0x80000
     * page / shared scratch. Ownership is normally decided at panic entry by
     * panic_evidence_reserve, which admits exactly ONE invocation per boot; a
     * caller that skipped that step claims here instead, so this function is
     * still correct standing alone. Non-owners return untouched. */
    me = cpu_panic_safe_apic_id();
    if (!panic_evidence_owned_by(me))
        (void)panic_evidence_reserve();
    if (!panic_evidence_owned_by(me))
        return;

    /* A COMPLETE record already stands -- a nested fault during BSOD render must
     * not restate its own crash over the original. An INCOMPLETE one does not
     * stop us: see s_evidence_published.
     *
     * BOTH conditions, because completion cannot be published in one store. The
     * flag is written FIRST and the magic LAST, and the magic is zeroed at the
     * start of population below, so the pair is true only over a record that
     * reached its final instruction: re-entry between the two stores sees the
     * flag set with the magic still zero, correctly reads that as unfinished,
     * and rewrites the record rather than leaving the boot with an unrestorable
     * page. Ordering the stores the other way round would leave exactly one
     * instruction at which a nested abort erases a valid record. */
    if (__atomic_load_n(&s_evidence_published, __ATOMIC_ACQUIRE) &&
        ev->magic == PANIC_EVIDENCE_MAGIC)
        return;

    /* Zero the record (8 bytes at a time; sizeof is a multiple of 8). This is
     * also what clears the magic for the completion gate above: from here until
     * the final store the page reads as "no record", which is exactly what a
     * re-entering invocation must conclude. */
    for (uint32_t i = 0u; i < sizeof *ev / 8u; i++)
        ((volatile uint64_t *)ev)[i] = 0u;

    ev->version  = PANIC_EVIDENCE_VERSION;
    ev->size     = (uint32_t)sizeof *ev;
    ev->boot_seq = boot_history_kernel_phase3_committed_seq();

    ev->bugcheck_code = bugcheck_code;
    /* Parameters come from the emitting call's own snapshot, not the global
     * g_last_bugcheck: recovering them by code-equality would let a concurrent
     * same-code bugcheck on another CPU overwrite g_last_bugcheck between the store
     * and this read, combining this frame with torn/foreign parameters. A raw
     * exception passes NULL (no STOP parameters -- identity is the fault_vector). */
    if (bugcheck_params) {
        ev->bugcheck_params[0] = bugcheck_params[0];
        ev->bugcheck_params[1] = bugcheck_params[1];
        ev->bugcheck_params[2] = bugcheck_params[2];
        ev->bugcheck_params[3] = bugcheck_params[3];
    }
    ev->fault_vector = frame ? frame->int_no  : 0u;
    ev->err_code     = frame ? frame->err_code : 0u;

    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    ev->cr0 = cr0; ev->cr2 = cr2; ev->cr3 = cr3; ev->cr4 = cr4;

    /* Same panic-safe identity the serial owner word and the NMI depth use, so a
     * crash record names the CPU those two are keyed by. */
    ev->cpu_id = me;
    ev->line           = line;
    ev->pmm_free_pages = pmm_get_free_frames();
    /* Legacy PIC mask via port I/O (fault-safe); best-effort IRQ-state proxy. */
    ev->irq_mask       = (uint64_t)pe_inb(0x21) | ((uint64_t)pe_inb(0xA1) << 8);

    if (frame) {
        ev->rip = frame->rip; ev->rsp = frame->rsp; ev->rflags = frame->rflags;
        ev->cs  = frame->cs;  ev->ss  = frame->ss;
        ev->rax = frame->rax; ev->rbx = frame->rbx; ev->rcx = frame->rcx;
        ev->rdx = frame->rdx; ev->rsi = frame->rsi; ev->rdi = frame->rdi;
        ev->rbp = frame->rbp;
        ev->r8  = frame->r8;  ev->r9  = frame->r9;  ev->r10 = frame->r10;
        ev->r11 = frame->r11; ev->r12 = frame->r12; ev->r13 = frame->r13;
        ev->r14 = frame->r14; ev->r15 = frame->r15;
    }
    /* Panic context, DECLARED from hardware state rather than probed -- vector
     * OR NMI nesting depth; see panic_declared_ctx. */
    ctx = panic_declared_ctx(frame);

    pe_copy(ev->file, sizeof ev->file, file, ctx);
    pe_copy(ev->message, sizeof ev->message, message, ctx);

    ev->post_code = (uint32_t)boot_post_last_shadow();

    /* Boot-stage tail (inline strings copied here, while pointers are valid). */
    {
        uint32_t cnt = 0u;
        const boot_stage_entry_t *h = boot_stage_history_get(&cnt);
        uint32_t n = (cnt < PANIC_EVIDENCE_STAGES) ? cnt : PANIC_EVIDENCE_STAGES;
        for (uint32_t i = 0u; i < n && h; i++) {
            const boot_stage_entry_t *e = &h[cnt - n + i];
            ev->stages[i].stage      = (uint32_t)e->stage;
            ev->stages[i].elapsed_ms = e->elapsed_ms;
            pe_copy(ev->stages[i].msg, sizeof ev->stages[i].msg, e->msg, ctx);
        }
        ev->stage_count = h ? n : 0u;
    }

    /* klog tail (lock-free snapshot into BSS scratch, then serialize inline). */
    {
        uint32_t n = klog_panic_snapshot(s_panic_klog_scratch, PANIC_EVIDENCE_KLOGS);
        for (uint32_t i = 0u; i < n; i++) {
            ev->klogs[i].level     = (uint32_t)s_panic_klog_scratch[i].level;
            ev->klogs[i].timestamp = s_panic_klog_scratch[i].timestamp;
            ev->klogs[i].cpu_id    = s_panic_klog_scratch[i].cpu_id;
            ev->klogs[i].pid       = s_panic_klog_scratch[i].pid;
            ev->klogs[i].tid       = s_panic_klog_scratch[i].tid;
            pe_copy(ev->klogs[i].subsystem, sizeof ev->klogs[i].subsystem,
                    s_panic_klog_scratch[i].subsystem, ctx);
            pe_copy(ev->klogs[i].message, sizeof ev->klogs[i].message,
                    s_panic_klog_scratch[i].message, ctx);
        }
        ev->klog_count = n;
    }

    /* crc32 over everything AFTER the crc32 field, then publish the magic last
     * so a reader never sees a valid magic over a half-written record -- and
     * only THEN mark the record complete, so an abort anywhere above leaves the
     * next invocation free to write a whole one rather than locked out of a
     * half-written page. Every CRC-covered byte is written exactly once, before
     * the CRC: a record is never amended in place, because an abort between an
     * amendment and its recomputed CRC would leave a magic-valid record that
     * panic_evidence_restore then discards for failing its checksum. */
    {
        uint32_t off = (uint32_t)__builtin_offsetof(struct panic_evidence, boot_seq);
        ev->crc32 = panic_crc32((const uint8_t *)ev + off, ev->size - off);
        /* Flag first, magic last -- see the completion gate at the top. BOTH
         * stores are atomic, because the ordering is the guarantee: a RELEASE
         * store is a one-way barrier that stops earlier accesses sinking below
         * it and says nothing about a LATER plain store being hoisted above it.
         * Written that way the compiler could legally emit magic-then-flag,
         * which is precisely the arrangement the gate's comment says leaves one
         * instruction where a nested abort erases a valid record. */
        __atomic_store_n(&s_evidence_published, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&ev->magic, PANIC_EVIDENCE_MAGIC, __ATOMIC_RELEASE);
    }
}

int panic_evidence_restore(struct panic_evidence *out)
{
    struct panic_evidence *ev = (struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR;
    if (!out)
        return 0;
    if (ev->magic != PANIC_EVIDENCE_MAGIC)
        return 0;                                  /* no record */
    if (ev->version != PANIC_EVIDENCE_VERSION ||
        ev->size != (uint32_t)sizeof *ev) {
        ev->magic = 0u;                            /* stale/incompatible -> drop */
        return 0;
    }
    {
        uint32_t off = (uint32_t)__builtin_offsetof(struct panic_evidence, boot_seq);
        if (panic_crc32((const uint8_t *)ev + off, ev->size - off) != ev->crc32) {
            ev->magic = 0u;                        /* corrupt -> drop */
            return 0;
        }
    }
    /* The page is cross-boot UNTRUSTED RAM and crc32 only proves byte integrity,
     * not semantic validity. Reject out-of-range counts so a crc-consistent but
     * bogus page cannot drive an out-of-bounds read in the artifact writer. */
    if (ev->stage_count > PANIC_EVIDENCE_STAGES ||
        ev->klog_count  > PANIC_EVIDENCE_KLOGS) {
        ev->magic = 0u;
        return 0;
    }
    /* Copy out, then clear the magic so the same crash is not re-reported. */
    for (uint32_t i = 0u; i < sizeof *out / 8u; i++)
        ((uint64_t *)out)[i] = ((volatile uint64_t *)ev)[i];
    /* Force-terminate every fixed-size string from the untrusted page so a
     * forged/corrupt-but-crc-consistent record whose strings lack a NUL cannot
     * make a downstream reader (the artifact writer's NUL-seeking pe_put) read
     * past the field. */
    out->file[sizeof out->file - 1u] = '\0';
    out->message[sizeof out->message - 1u] = '\0';
    for (uint32_t i = 0u; i < PANIC_EVIDENCE_STAGES; i++)
        out->stages[i].msg[sizeof out->stages[i].msg - 1u] = '\0';
    for (uint32_t i = 0u; i < PANIC_EVIDENCE_KLOGS; i++) {
        out->klogs[i].subsystem[sizeof out->klogs[i].subsystem - 1u] = '\0';
        out->klogs[i].message[sizeof out->klogs[i].message - 1u] = '\0';
    }
    /* Do NOT clear the page magic here: the evidence is only durably persisted
     * once last-panic.txt is written. If this boot dies before that write (or
     * the write fails), the record must survive on 0x80000 for a next-boot
     * retry. panic_evidence_consume() clears it after a successful write. */
    return 1;
}

/* Phase-0 restored record + flag. Kept kernel-side (not in g_boot_info) so the
 * boot_info ABI is untouched -- the desktop reads panic_had_previous_crash().
 * The restore runs pre-heap, so the record lands in this BSS buffer; the
 * X:\Crash\ emission is deferred to panic_evidence_write_blackbox() post-VFS. */
static struct panic_evidence s_prev_crash;
static int s_had_prev_crash = 0;

/* Clear the evidence page once the record has been durably emitted, so the same
 * crash is not re-reported next boot. Conditional: only clear if the page STILL
 * holds the record we restored (boot_seq + crc identity) -- by Phase 3 the APs
 * are up, and a new panic on another CPU may have overwritten 0x80000 with a
 * fresh crash; that one must survive, not be erased here. */
void panic_evidence_consume(void)
{
    volatile struct panic_evidence *ev =
        (volatile struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR;
    if (ev->magic == PANIC_EVIDENCE_MAGIC &&
        ev->boot_seq == s_prev_crash.boot_seq &&
        ev->crc32 == s_prev_crash.crc32)
        ev->magic = 0u;
}

void panic_evidence_restore_early(void)
{
    if (panic_evidence_restore(&s_prev_crash)) {
        s_had_prev_crash = 1;
        klog(LOG_ERROR, "panic",
             "[PANIC] Previous crash evidence found (STOP 0x%x rip=0x%lx)",
             (uint64_t)s_prev_crash.bugcheck_code, s_prev_crash.rip);
    }
}

int panic_had_previous_crash(void)
{
    return s_had_prev_crash;
}

/* --- last-panic.txt text emission (post-VFS) ----------------------------- */

static uint32_t pe_put(char *b, uint32_t pos, uint32_t cap, const char *s)
{
    if (s)
        while (*s && pos + 1u < cap)
            b[pos++] = *s++;
    return pos;
}

static uint32_t pe_hex(char *b, uint32_t pos, uint32_t cap, uint64_t v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    for (int sh = (digits - 1) * 4; sh >= 0; sh -= 4)
        pos = (pos + 1u < cap) ? (b[pos] = hx[(v >> sh) & 0xFu], pos + 1u) : pos;
    return pos;
}

static uint32_t pe_dec(char *b, uint32_t pos, uint32_t cap, uint64_t v)
{
    char t[20];
    int n = 0;
    if (v == 0u)
        return (pos + 1u < cap) ? (b[pos] = '0', pos + 1u) : pos;
    while (v && n < 20) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n-- > 0)
        pos = (pos + 1u < cap) ? (b[pos] = t[n], pos + 1u) : pos;
    return pos;
}

static uint32_t pe_line(char *b, uint32_t pos, uint32_t cap, const char *k, uint64_t v)
{
    pos = pe_put(b, pos, cap, k);
    pos = pe_put(b, pos, cap, "0x");
    pos = pe_hex(b, pos, cap, v, 16);
    pos = pe_put(b, pos, cap, "\n");
    return pos;
}

/* Render the restored crash record to X:\Crash\last-panic.txt (BlackBox) or the
 * C:\ fallback. Best-effort; no-op if no prior crash was restored. */
void panic_evidence_write_blackbox(void)
{
    if (!s_had_prev_crash)
        return;

    const struct panic_evidence *e = &s_prev_crash;
    const uint32_t pages = 2u;
    uintptr_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        klog(LOG_WARN, "panic", "last-panic: cannot alloc %u pages", (uint64_t)pages);
        return;
    }
    char *b = (char *)phys;
    const uint32_t cap = pages * 4096u;
    uint32_t pos = 0u;

    pos = pe_put(b, pos, cap, "Impossible OS -- Previous Crash Evidence\n");
    pos = pe_put(b, pos, cap, "========================================\n");
    pos = pe_put(b, pos, cap, "stop:    0x");
    pos = pe_hex(b, pos, cap, e->bugcheck_code, 8);
    pos = pe_put(b, pos, cap, "  ");
    pos = pe_put(b, pos, cap, bugcheck_name(e->bugcheck_code));
    pos = pe_put(b, pos, cap, "\nmessage: ");
    pos = pe_put(b, pos, cap, e->message);
    pos = pe_put(b, pos, cap, "\nsource:  ");
    pos = pe_put(b, pos, cap, e->file);
    pos = pe_put(b, pos, cap, ":");
    pos = pe_dec(b, pos, cap, e->line);
    pos = pe_put(b, pos, cap, "\nboot_seq=");
    pos = pe_dec(b, pos, cap, e->boot_seq);
    pos = pe_put(b, pos, cap, " cpu=");
    pos = pe_dec(b, pos, cap, e->cpu_id);
    pos = pe_put(b, pos, cap, " vector=");
    pos = pe_dec(b, pos, cap, e->fault_vector);
    pos = pe_put(b, pos, cap, " post=0x");
    pos = pe_hex(b, pos, cap, e->post_code, 4);
    pos = pe_put(b, pos, cap, "\n");
    pos = pe_line(b, pos, cap, "rip:     ", e->rip);
    pos = pe_line(b, pos, cap, "rsp:     ", e->rsp);
    pos = pe_line(b, pos, cap, "rflags:  ", e->rflags);
    pos = pe_line(b, pos, cap, "err:     ", e->err_code);
    pos = pe_line(b, pos, cap, "cr2:     ", e->cr2);
    pos = pe_line(b, pos, cap, "cr3:     ", e->cr3);
    pos = pe_put(b, pos, cap, "free_pages=");
    pos = pe_dec(b, pos, cap, e->pmm_free_pages);
    /* Clamp the counts here too (defense in depth -- restore already rejects
     * over-cap records, but never iterate an untrusted count past the arrays). */
    uint32_t sc = e->stage_count > PANIC_EVIDENCE_STAGES ? PANIC_EVIDENCE_STAGES : e->stage_count;
    uint32_t kc = e->klog_count  > PANIC_EVIDENCE_KLOGS  ? PANIC_EVIDENCE_KLOGS  : e->klog_count;
    pos = pe_put(b, pos, cap, "\n\nboot stages:\n");
    for (uint32_t i = 0u; i < sc; i++) {
        pos = pe_put(b, pos, cap, "  +");
        pos = pe_dec(b, pos, cap, e->stages[i].elapsed_ms);
        pos = pe_put(b, pos, cap, "ms ");
        pos = pe_put(b, pos, cap, e->stages[i].msg);
        pos = pe_put(b, pos, cap, "\n");
    }
    pos = pe_put(b, pos, cap, "\nlast klog:\n");
    for (uint32_t i = 0u; i < kc; i++) {
        pos = pe_put(b, pos, cap, "  [");
        pos = pe_put(b, pos, cap, e->klogs[i].subsystem);
        pos = pe_put(b, pos, cap, " p");
        pos = pe_dec(b, pos, cap, e->klogs[i].pid);
        pos = pe_put(b, pos, cap, "/t");
        pos = pe_dec(b, pos, cap, e->klogs[i].tid);
        pos = pe_put(b, pos, cap, "] ");
        pos = pe_put(b, pos, cap, e->klogs[i].message);
        pos = pe_put(b, pos, cap, "\n");
    }
    if (pos >= cap)
        pos = cap - 1u;
    b[pos] = '\0';

    extern int klog_using_blackbox;
    const char *path = klog_using_blackbox ? "X:\\Crash\\last-panic.txt"
                                           : "C:\\Impossible\\System\\Logs\\last-panic.txt";
    struct vfs_node *f = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (f) {
        int wrote = vfs_write(f, 0, pos, (const uint8_t *)b);
        int full  = (wrote >= 0 && (uint32_t)wrote == pos);
        /* Flush to stable storage BEFORE consuming the page -- vfs_close does
         * NOT flush, so consuming after the write alone could lose the file on a
         * reset. Only clear the evidence page once the bytes are durable. */
        int flushed = full ? vfs_flush(f) : -1;
        vfs_close(f);
        if (full && flushed == 0) {
            panic_evidence_consume();   /* durable -> safe to clear the page */
            klog(LOG_INFO, "panic", "previous crash written to %s (%u bytes)",
                 (uint64_t)(uintptr_t)path, (uint64_t)pos);
        } else if (full) {
            klog(LOG_WARN, "panic",
                 "last-panic: written but flush failed; retaining 0x80000 for retry");
        } else {
            klog(LOG_WARN, "panic", "last-panic: short write (%d of %u)",
                 (uint64_t)wrote, (uint64_t)pos);
        }
    } else {
        klog(LOG_WARN, "panic", "last-panic: vfs_open failed for %s",
             (uint64_t)(uintptr_t)path);
    }
    for (uint32_t p = 0u; p < pages; p++)
        pmm_free_frame(phys + p * 4096u);
}

/* Core panic path. `bugcheck_code` is the authoritative STOP code recorded in the
 * cross-boot evidence, decoupled from frame presence: a raw exception passes 0
 * (identity is the fault vector), while a framed bugcheck (KeBugCheckExFrame)
 * passes its real 0x1E/0x3B code AND the trap frame, so the black-box record keeps
 * both the STOP identity+params and the register/vector evidence. */
static void panic_screen_impl(struct interrupt_frame *frame, uint64_t error_code,
                              uint32_t bugcheck_code, const uint64_t bugcheck_params[4],
                              const char *description_in, const char *file_in,
                              uint32_t line)
{
    /* Mask interrupts FIRST -- nothing may re-enter the panic path while the
     * collector touches the fixed 0x80000 evidence page and shared log state.
     * The faulting interrupt state is preserved in frame->rflags for forensics. */
    __asm__ volatile ("cli");

    /* Decide who owns the cross-boot record BEFORE doing any variable-latency
     * work, so the CPU that panicked FIRST is the one the record describes even
     * when its own description pointer is the corruption being reported and
     * every byte of the snapshot below costs a fault fixup. Exactly one
     * invocation of this boot gets a 1 here, which is also what keeps another
     * CPU's later panic from becoming the recorded one. It is deliberately the
     * FIRST thing that happens: everything below, starting with the snapshot,
     * takes time proportional to how corrupt memory is. */
    (void)panic_evidence_reserve();

    /* THE panic-string snapshot. Every consumer below -- the evidence record,
     * the emergency serial dump, the async diagnostic, the no-framebuffer
     * fallback, the BSOD and the disk crash dump -- reads desc_snap/file_snap.
     * `description_in` and `file_in` are the caller's pointers and appear
     * NOWHERE else in this function: read exactly once, here, under the guard.
     * That is what makes the property greppable rather than a promise.
     *
     * Stack-local, not static: on an SMP double panic the CPU that loses the
     * record still has to render its own bugcheck, and a shared buffer would
     * make it paint the winner's reason. Per-invocation storage also needs no
     * GS and no lock, which the pre-arbitration dump below requires. */
    const uint32_t snap_ctx = panic_declared_ctx(frame);
    const int have_file = (file_in != (const char *)0);
    char desc_snap[PANIC_DESC_SNAP_MAX];
    char file_snap[PANIC_FILE_SNAP_MAX];

    panic_snapshot_str(desc_snap, sizeof desc_snap, description_in, snap_ctx,
                       PANIC_STR_NONE);
    panic_snapshot_str(file_snap, sizeof file_snap, file_in, snap_ctx, "");

    /* Capture cross-boot forensic evidence before any other panic work (serial,
     * async isolation, framebuffer, VFS) that could itself fault -- and now with
     * the reason already in kernel-owned memory, so the collector's own copy of
     * it cannot fault either. bugcheck_code is the authoritative STOP code (0
     * for a raw exception -- fault_vector carries identity there).
     *
     * Unconditional: the ownership and completion gates live inside the
     * collector, and this is the ONE call that must reach them -- a caller-side
     * `if (owns_record)` would stop a nested abort from finishing a record its
     * outer invocation never got to write. */
    panic_collect_evidence(frame, bugcheck_code, bugcheck_params, desc_snap,
                           have_file ? file_snap : (const char *)0, line);

    /* NOTE: emergency serial is NOT armed here. Arming is a SYSTEM-TERMINAL
     * declaration and this function is not yet committed to one -- the async
     * isolation branch below parks only the faulting AP and reports BOOT_FATAL,
     * after which boot_storage.c falls back to sequential init and the system
     * keeps running. Arming here would leave that surviving system permanently
     * routed through the lossy try-lock path. The pre-arbitration dump below is
     * made abort-safe by calling the emergency writers DIRECTLY instead, which
     * needs no global state; the latch is armed once ownership is claimed. */

    /* Charges THIS CPU already holds before the unconditional dump below.
     * The dump must be GS-independent, so it cannot ask whether this CPU is
     * survivable before spending; it spends first, and the async-isolation
     * branch refunds what it spent if the machine turns out to keep running.
     *
     * Per-CPU, not the global charge: both reads must describe the SAME writer
     * or the difference between them is not this CPU's spending. The global
     * counter cannot provide that -- a concurrent panic charging between the two
     * reads is indistinguishable from this dump charging. */
    uint32_t charges_before = serial_emerg_charges_self();
    uint32_t screen_w;
    uint32_t screen_h;
    uint64_t cr2_val;
    uint64_t cr3_val;
    uint64_t frames[PANIC_MAX_STACK_DEPTH];
    uint32_t nframes;
    uint32_t depth;
    int32_t restart_secs = 0;
    uint32_t reg_restart;

    /* --- UNCONDITIONAL serial dump of the panic reason, FIRST ---
     *
     * Every panic MUST print its reason to serial BEFORE any other
     * work, so a reader of the serial log can always see WHY the
     * system panicked, even if downstream panic steps (async
     * isolation, ownership claim, readiness dump, VPD, NVRAM,
     * framebuffer BSOD, compositor unlock) hang or deadlock.
     *
     * NOTE the accounting: this dump runs BEFORE we know whether the machine
     * dies, and the async-isolation branch below parks one AP and lets it keep
     * booting. A survivable dump must therefore not spend the shared terminal
     * allowance -- if it did, a later REAL panic would emit its reason and
     * registers on a saturated budget, before arming clears it, and lose exactly
     * the evidence that matters. panic_emit/panic_emit_hex below pick
     * recoverable accounting when this CPU is an async worker.
     *
     * Uses the emergency writers DIRECTLY -- no printk, no klog, no
     * framebuffer, and NOT the re-routing latch, which is not armed yet
     * (see the NOTE at the top of this function). They try-lock
     * g_serial_lock and proceed unlocked on failure, with every UART wait
     * bounded, so this block cannot self-deadlock on a lock the panicking
     * code already held nor spin forever on a wedged transmitter. Before
     * they existed this block CLAIMED to take no locks while serial_write
     * took g_serial_lock unconditionally, which is exactly how a
     * #DF/#MC/NMI mid-write silenced the panic it caused.
     *
     * Why this landed 2026-04-22: operators kept seeing boot logs
     * end at the readiness-dump `[OK] TPM` line with no panic header
     * after -- because the readiness dump fires BEFORE any "[PANIC]"
     * marker, and any hang in the subsequent fb/printk/compositor
     * path swallowed the actual reason. Panic MUST be observable
     * even under the most hostile downstream state. */
    {
        /* Every emit here is now a string literal, a stack hex buffer, or the
         * panic-entry snapshot -- all memory this kernel owns and cannot fault
         * on, so the context-aware writer has nothing left to guard against and
         * the plain emitter (which keeps the recoverable-vs-terminal accounting
         * this branch depends on) is the right writer. */
        panic_emit("\n\n[PANIC] ");
        panic_emit(desc_snap);
        if (have_file) {
            panic_emit("\n  at ");
            panic_emit(file_snap);
        }
        panic_emit("\n");

        if (frame) {
            panic_emit("  RIP=");
            serial_write_hex(frame->rip);
            panic_emit(" CS=");
            serial_write_hex(frame->cs);
            panic_emit(" ERR=");
            serial_write_hex(error_code);
            panic_emit("\n  CR2=");
            serial_write_hex(read_cr2());
            panic_emit(" CR3=");
            serial_write_hex(read_cr3());
            panic_emit("\n");
            panic_emit("  RAX=");     serial_write_hex(frame->rax);
            panic_emit(" RBX=");      serial_write_hex(frame->rbx);
            panic_emit(" RCX=");      serial_write_hex(frame->rcx);
            panic_emit(" RDX=");      serial_write_hex(frame->rdx);
            panic_emit("\n  RSI=");   serial_write_hex(frame->rsi);
            panic_emit(" RDI=");      serial_write_hex(frame->rdi);
            panic_emit(" RBP=");      serial_write_hex(frame->rbp);
            panic_emit(" RSP=");      serial_write_hex(frame->rsp);
            panic_emit("\n");
        }
    }

    /* --- Async init fault isolation ---
     * If this CPU is executing an async boot init step, don't crash the
     * whole system. Record BOOT_FATAL for this step and park the AP.
     * The BSP's barrier will detect the failure via async_done/async_result. */
    {
        struct per_cpu_data *pcpu = smp_this_cpu();
        if (pcpu && pcpu->in_async_work) {
            /* Snapshot the identifying fields BEFORE publishing below. Once
             * async_done is observed the BSP's barrier exits, and the next async
             * group clears async_name and re-arms the slot for a DIFFERENT step
             * -- so reading it afterwards can misattribute the crash, or print
             * "?", which destroys the value of this diagnostic. */
            const char *step_name = pcpu->async_name ? pcpu->async_name : "?";
            /* Same declared-context rule as the pre-arbitration dump. */
            uint32_t    async_ctx = panic_declared_ctx(frame);
            uint32_t    step_cpu  = pcpu->cpu_id;

            /* REFUND FIRST, then publish, then diagnose.
             *
             * The refund gives back what the GS-independent pre-arbitration
             * dump above charged: this CPU turns out to be survivable, so it
             * must not leave the shared terminal allowance spent, or a later
             * REAL panic would emit its reason and registers on a saturated
             * budget. It goes BEFORE the publication because everything after
             * that point is observable: once async_done is visible the BSP
             * proceeds, and a terminal NMI/#MC landing on this AP in the
             * meantime would no longer be classified survivable -- and would
             * find the allowance still spent. The refund touches only the
             * packed serial word, so it is safe this early.
             *
             * ATTRIBUTABLE. Both readings come from this CPU's own charge slot,
             * which no other CPU writes, so the difference is exactly what this
             * dump spent -- a concurrent panic charging in between changes the
             * global counter but not this slot. `serial_emerg_refund_self` then
             * bounds the refund by what this CPU is still recorded as holding in
             * the LIVE epoch, so it can neither absorb another CPU's charge nor
             * credit an epoch that has since been published. */
            {
                uint32_t now   = serial_emerg_charges_self();
                uint32_t spent = (now > charges_before) ? (now - charges_before) : 0u;
                serial_emerg_refund_self(spent);
            }

            /* PUBLISH. The BSP barrier waits on async_done to run the sequential
             * fallback (the loop lives in boot_init.c), so the completion signal
             * must not depend on the diagnostic below surviving. This ordering
             * used to be reversed, and the diagnostic was a klog() -- which
             * takes s_klog_lock and sinks to the ordinary locked serial_write.
             * An async worker that faulted while holding either lock therefore
             * self-deadlocked HERE, never published async_done, and hung the BSP
             * forever on a failure it was designed to recover from. */
            /* GO OFFLINE BEFORE PUBLISHING. boot_async_group selects its workers
             * on live online membership plus a lifecycle-claim CAS
             * (boot_init.c, TODO-10 S21), and this CPU is about to park with
             * interrupts masked, so it can never take the wake IPI again.
             * Leaving the flag set means every LATER async group assigns it a
             * step and then eats the group's whole 10-second barrier deadline
             * waiting for a CPU that will never answer -- turning one recovered
             * async fault into a visibly stalled boot.
             *
             * ORDERING, attributed precisely: what stops a BSP observing
             * async_done=1 while still seeing this CPU online is the smp_mb()
             * below plus x86 TSO -- NOT the RELEASE annotation on this store. A
             * release store orders EARLIER accesses before ITSELF; it says
             * nothing about the later async_done store. The annotation is kept
             * because it correctly publishes this write to the ACQUIRE loads in
             * boot_init.c and smp.c, but an ARM64 port must keep the fence.
             *
             * SCOPE, closed by TODO-10 S21: this CPU is now retired from the
             * SYSTEM's view of itself, not just from async dispatch. The
             * retract clears the live online-mask bit (which is what
             * smp_cpu_count(), NT processor reporting and IRQ affinity read),
             * parks the async claim word so no later group can dispatch to this
             * CPU even if it already selected it, and clears is_online last.
             * All three are atomics -- no lock, no allocation, panic-path
             * safe. */
            smp_retract_cpu_online(pcpu);

            pcpu->async_result = (uint8_t)BOOT_FATAL;
            pcpu->in_async_work = 0;
            smp_mb();
            pcpu->async_done = 1;
            smp_mb();

            /* DIAGNOSE, as ONE record. Built in a stack buffer and emitted with
             * a single recoverable call: separate fragments would each start a
             * fresh call-local budget, so a wedged UART could pay a full-length
             * wait per fragment. Not klog -- this runs before the emergency
             * latch is armed (the system survives this branch), and no
             * formatting machinery is trustworthy in a fault context. */
            {
                char     rec[224];
                uint32_t rp = 0;

                panic_append(rec, sizeof rec, &rp, "\n[ASYNC] FAULT on CPU");
                panic_append_hex(rec, sizeof rec, &rp, step_cpu);
                panic_append(rec, sizeof rec, &rp, " during '");
                /* step_name is still CALLER-SUPPLIED and may be the corruption
                 * being reported, so it keeps the guarded append. The panic
                 * description does not: it was snapshotted at entry, so it is
                 * kernel-owned memory here like every literal beside it. */
                panic_append_guarded(rec, sizeof rec, &rp, step_name, async_ctx);
                panic_append(rec, sizeof rec, &rp, "': ");
                panic_append(rec, sizeof rec, &rp, desc_snap);
                panic_append(rec, sizeof rec, &rp, " (err=");
                panic_append_hex(rec, sizeof rec, &rp, error_code);
                panic_append(rec, sizeof rec, &rp, " RIP=");
                panic_append_hex(rec, sizeof rec, &rp, frame ? frame->rip : 0);
                panic_append(rec, sizeof rec, &rp, ")\n");
                serial_write_recoverable(rec);
            }

            /* HAND BACK THE UART BEFORE PARKING.
             *
             * The step that faulted may have been holding g_serial_lock -- an
             * ordinary klog from inside an async init step is enough. This CPU
             * never runs again, so nothing else can ever release it, and the
             * surviving BSP would block forever on its next ordinary serial
             * write: a silent hang instead of the recovered boot this branch
             * exists to deliver. Owner-scoped, so a CPU that does not hold the
             * lock changes nothing.
             *
             * After the diagnostic above, not before: that record is emitted
             * through the bounded try-lock writer, which may itself be the holder,
             * and releasing first would let another CPU interleave into it. */
            serial_lock_release_if_owner();

            /* Park this AP permanently -- BSP will handle the failure */
            for (;;) __asm__ volatile("hlt");
        }
    }

    /* Atomic panic ownership: only the first CPU to panic captures FPU state
     * and builds the CONTEXT record. Secondary CPUs that panic simultaneously
     * park immediately to avoid clobbering the owner's crash data.
     * Placed after async isolation so APs doing async work park even earlier. */
    if (panic_try_claim_owner()) {
        /* SYSTEM-TERMINAL from here: async isolation declined to park this CPU
         * and we own the panic, so the machine is going to the BSOD and halt.
         * Arm emergency serial NOW -- this is the first point where a global,
         * never-cleared latch is honest. The transition-ring dump,
         * kernel_subsystem_dump, quota_dump_crash and klog's serial sink all
         * reach serial_write, which from here routes to the bounded try-lock
         * path instead of blocking on g_serial_lock.
         *
         * This does NOT make those dumpers fully abort-safe: klog_emit takes
         * s_klog_lock BEFORE reaching serial, so a panic that interrupted
         * logging still stalls there. That residual is pre-existing and
         * repo-wide on this path, and is owned by the panic-safe dump_emit_raw
         * emitter in the crash-dump-generation roadmap. The reason and
         * register dump above are emitted before this point precisely so they
         * survive regardless. */
        serial_enter_emergency();

        panic_capture_fpu_state();
        panic_build_context(frame, &g_panic_context);

        /* fast-path transition ring: dump the last 64 ring-3
         * transitions on THIS CPU (the crashing one) BEFORE any
         * other panic output. If the crash root cause is "user
         * task returned with bad state after transition N", that
         * transition is visible in the ring. Only the owner CPU
         * runs this -- the secondary-panic park above ensures no
         * concurrent writer on another CPU could race the dump. */
        transition_ring_dump_to_serial(smp_this_cpu());
    } else {
        /* Secondary panic CPU -- park without touching global crash state.
         * The owner CPU will handle BSOD rendering and dump writing. */
        for (;;) __asm__ volatile("hlt");
    }

    /* Mark current VPD stage as failed (red) before BSOD overwrites screen */
    {
        extern void vpd_stage_fail(void);
        vpd_stage_fail();
    }

    /* Write failure POST code to UEFI NVRAM for post-mortem diagnosis */
    boot_post_nvram_write16(POST16_BOOT_FAILED);

    /* Dump subsystem readiness to serial for post-mortem analysis */
    kernel_subsystem_dump();

    /* Outstanding per-principal quota, for the crash class this cannot
     * otherwise distinguish: a bugcheck that followed a resource exhaustion
     * looks identical to an unrelated one until you can see which principal
     * was at its cap. Non-blocking and allocation-free by contract -- see
     * quota_dump_crash; the plain quota_dump() must NEVER be called here. */
    quota_dump_crash();

    /* If framebuffer is not yet initialized (Phase 0 panic), fall back to
     * serial-only output via boot_halt(). No BSOD drawing is possible. */
    if (!kernel_subsystem_ready(SUBSYS_FB)) {
        serial_write("\n[PANIC] ");
        serial_write(desc_snap);
        serial_write("\n");
        if (have_file) {
            serial_write("  at ");
            serial_write(file_snap);
            serial_write("\n");
        }
        serial_write("System halted (no framebuffer for BSOD).\n");
        for (;;) __asm__ volatile ("hlt");
    }

    /* Stop boot animation dots if still running */
    if (boot_splash_active())
        boot_splash_finish();

    /* Unlock the compositor so we can draw directly */
    fb_unlock_compositor();

    screen_w = fb_get_width();
    screen_h = fb_get_height();

    /* === Draw the blue background === */
    fb_fill_rect(0, 0, screen_w, screen_h, PANIC_BG_COLOR);
    fb_swap();

    /* Read CR2/CR3 before we lose context */
    cr2_val = read_cr2();
    cr3_val = read_cr3();

    /* === Draw BSOD icon === */
    draw_bsod_icon(80, 60);
    fb_swap();

    /* Text position is determined by printk cursor */

    /* Move cursor down to below the icon
     * (each char row is ~16px, icon is 128px + 60px offset + gap = ~14 rows) */
    {
        uint32_t i;
        fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);
        for (i = 0; i < 14; i++)
            printk("\n");
    }

    /* Title */
    printk("    Your Impossible OS ran into a problem and needs to restart.\n");
    printk("    We're just collecting some error info, and then we'll restart for you.\n\n");

    /* Stop code */
    fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
    if (frame && frame->int_no < 32) {
        printk("    Stop code:  %s\n", (uint64_t)(uintptr_t)panic_exception_names[frame->int_no]);
    } else {
        printk("    Stop code:  KERNEL_PANIC\n");
    }

    /* Description */
    fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);
    printk("    Description: %s\n", (uint64_t)(uintptr_t)desc_snap);

    /* Source location */
    if (have_file) {
        fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
        printk("    Source:  %s:%u\n", (uint64_t)(uintptr_t)file_snap,
               (uint64_t)line);
    }

    printk("\n");

    /* Error code and addresses */
    fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);
    if (frame) {
        printk("    Error code: %p\n", error_code);
        printk("    RIP:        %p\n", frame->rip);
        printk("    CR2:        %p\n", cr2_val);
    }

    printk("\n");

    /* === Register dump === */
    if (frame) {
        fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
        printk("    --- Register Dump ---\n");
        fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);
        printk("    RAX=%p  RBX=%p  RCX=%p\n", frame->rax, frame->rbx, frame->rcx);
        printk("    RDX=%p  RSI=%p  RDI=%p\n", frame->rdx, frame->rsi, frame->rdi);
        printk("    R8 =%p  R9 =%p  R10=%p\n", frame->r8, frame->r9, frame->r10);
        printk("    R11=%p  R12=%p  R13=%p\n", frame->r11, frame->r12, frame->r13);
        printk("    R14=%p  R15=%p  RBP=%p\n", frame->r14, frame->r15, frame->rbp);
        printk("    RSP=%p  RFLAGS=%p\n", frame->rsp, frame->rflags);
        printk("    CR2=%p  CR3=%p\n", cr2_val, cr3_val);
        printk("    CS=0x%x  SS=0x%x\n", frame->cs, frame->ss);
    }

    printk("\n");

    /* === Stack Trace === */
    fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
    printk("    --- Stack Trace ---\n");
    fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);

    /* Walk the RBP chain through the shared fault-safe walker. */
    nframes = panic_capture_frames(frame, snap_ctx, frames, PANIC_MAX_STACK_DEPTH);

    for (depth = 0; depth < nframes; depth++) {
        uint64_t ret_addr = frames[depth];
        uint64_t sym_off = 0;
        const char *sym = symtab_resolve(ret_addr, &sym_off);

        if (sym) {
            printk("    #%u  %p  %s+0x%x\n",
                   (uint64_t)depth, ret_addr,
                   (uint64_t)(uintptr_t)sym, sym_off);
        } else {
            printk("    #%u  %p\n", (uint64_t)depth, ret_addr);
        }
    }

    if (nframes == 0u) {
        /* Distinguish "walked and found nothing" from "did not walk": the
         * second is a context decision, not an empty stack, and reading it as
         * an empty stack would send a reader looking for a corrupt chain. */
        if (!serial_emerg_ctx_allows_guarded_read(snap_ctx))
            printk("    %s\n", (uint64_t)(uintptr_t)PANIC_TRACE_NO_GUARD);
        else
            printk("    (no stack frames available)\n");
    }

    /* === Version footer === */
    printk("\n");
    fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
    printk("    Impossible OS v%s (build %u, %s@%s)\n",
           (uint64_t)(uintptr_t)version_short(),
           (uint64_t)version_build_number(),
           (uint64_t)(uintptr_t)version_branch(),
           (uint64_t)(uintptr_t)version_git_hash());

    /* === Write crash dump to disk === */
    /* The SAME capture the BSOD rendered above, not a second walk: sharing it
     * halves the guarded reads on the terminal path and guarantees the screen
     * and the file describe one stack. */
    int dump_written = write_crash_dump(frame, snap_ctx, desc_snap,
                                        have_file ? file_snap : (const char *)0,
                                        line, frames, nframes);

    /* === Persist klog ring buffer to reserved physical memory === */
    klog_crash_persist();

    {
        fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
        if (dump_written)
            printk("\n    Crash dump saved to C:\\Impossible\\System\\crashdump.log\n");
        else if (vfs_is_mounted('C'))
            printk("\n    Crash dump write FAILED (see serial/klog persist for the record)\n");
    }

    /* Swap to show everything */
    fb_swap();

    /* === Auto-restart countdown === */
    /* Check Registry for auto-restart preference */
    {
        HKEY hRecovery = (HKEY)0;
        long rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Recovery",
                               0, KEY_READ, &hRecovery);
        if (rc == ERROR_SUCCESS) {
            if (RegGetDword(hRecovery, "AutoRestart", &reg_restart)
                == ERROR_SUCCESS) {
                restart_secs = (int32_t)reg_restart;
            } else {
                restart_secs = DEFAULT_RESTART_SECS;
                RegSetDword(hRecovery, "AutoRestart",
                            (uint32_t)DEFAULT_RESTART_SECS);
            }
            RegCloseKey(hRecovery);
        } else {
            /* Create the key with defaults */
            uint32_t disp;
            rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Recovery",
                                0, (const char *)0, 0, KEY_ALL_ACCESS,
                                (void *)0, &hRecovery, &disp);
            if (rc == ERROR_SUCCESS) {
                RegSetDword(hRecovery, "AutoRestart",
                            (uint32_t)DEFAULT_RESTART_SECS);
                restart_secs = DEFAULT_RESTART_SECS;
                RegCloseKey(hRecovery);
            }
        }
    }

    /* Runtime tunable override (panic.timeout). Read locklessly -- a plain
     * aligned 32-bit load is panic-path safe (no spinlock acquire). When the
     * operator set panic.timeout at runtime it wins over the registry value. */
    {
        extern volatile int32_t g_panic_tunable_restart_secs;
        int32_t t = g_panic_tunable_restart_secs;
        if (t >= 0) restart_secs = t;
    }

    if (restart_secs > 0) {
        /* Countdown loop using PIT ticks */
        int32_t secs_left = restart_secs;
        uint64_t last_tick = uptime();

        /* Re-enable interrupts just for the PIT timer */
        __asm__ volatile ("sti");

        while (secs_left > 0) {
            uint64_t now = uptime();
            if (now != last_tick) {
                last_tick = now;
                secs_left--;

                /* Redraw countdown line at bottom */
                fb_set_color(PANIC_ACCENT, PANIC_BG_COLOR);
                /* Clear the countdown line area */
                fb_fill_rect(0, screen_h - 40, screen_w, 40, PANIC_BG_COLOR);

                /* Draw a progress bar */
                {
                    uint32_t bar_w = (screen_w - 160) *
                        (uint32_t)(restart_secs - secs_left) / (uint32_t)restart_secs;
                    fb_fill_rect(80, screen_h - 30, bar_w, 8, PANIC_ACCENT);
                    fb_fill_rect(80, screen_h - 30,
                                 screen_w - 160, 8, PANIC_DIM_COLOR);
                    fb_fill_rect(80, screen_h - 30, bar_w, 8, PANIC_ACCENT);
                }

                fb_swap();
            }

            /* Busy-wait (HLT until next interrupt) */
            __asm__ volatile ("hlt");
        }

        /* Restart via ACPI reset or triple fault */
        {
            /* Try ACPI reset first (port 0xCF9) */
            __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x06), "Nd"((uint16_t)0xCF9));
            /* If that didn't work, triple fault by loading invalid IDT */
            {
                struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
                __asm__ volatile ("lidt %0" : : "m"(null_idt));
                __asm__ volatile ("int $0");  /* triple fault → reboot */
            }
        }
    }

    /* No auto-restart -- halt permanently */
    fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
    printk("\n    System halted. Press reset to restart.\n");
    fb_swap();

    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Public terminal. A present frame means "raw exception" -- no structured STOP
 * code, identity is the fault vector. (KeBugCheckEx / KeBugCheckExFrame go through
 * panic_screen_impl directly with an explicit bugcheck code.) */
void panic_screen(struct interrupt_frame *frame, uint64_t error_code,
                  const char *description, const char *file, uint32_t line)
{
    /* Raw fault / non-bugcheck caller: no STOP parameters (NULL), identity via the
     * fault vector. (KeBugCheckEx / KeBugCheckExFrame call panic_screen_impl
     * directly with an explicit bugcheck code + call-local params.) */
    panic_screen_impl(frame, error_code, frame ? 0u : (uint32_t)error_code,
                      (const uint64_t *)0, description, file, line);
}
