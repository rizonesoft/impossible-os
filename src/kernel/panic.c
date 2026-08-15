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
#include "kernel/cpu_security.h"    /* __kstr_read_guarded -- guarded string copy */
#include "kernel/kchecksum.h"       /* kcrc32 -- the tree's one table-driven CRC-32 */
#include "kernel/vectors.h"         /* VECTOR_NMI -- panic context declaration */
#include "kernel/boot_info.h"       /* boot_history seq + g_boot_info.had_panic */
#include "kernel/mm/pmm.h"          /* pmm_get_free_frames */
#include "kernel/mm/memmap.h"       /* MM_IS_CANONICAL_4LVL -- frame-chain bound */
#include "kernel/quota/quota.h"     /* quota_dump_crash (resource exhaustion) */
#include "kernel/smp.h"
#include "kernel/barrier.h"
#include "kernel/cache.h"

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

#ifdef KERNEL_TESTS
/* Counts panic_declared_ctx derivations, for the section-24 assertion that a
 * panic pays for the context ONCE. Test-build only: the live panic path must
 * not carry an atomic it never reads. */
static uint32_t s_panic_declared_ctx_calls;
#endif

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
#ifdef KERNEL_TESTS
    /* Derivation counter. The point of passing `ctx` down (section 24) is that
     * a panic derives it ONCE, and "once" is not something a signature can
     * assert -- a later edit could quietly re-derive it inside the collector
     * exactly as this one did, and every test would still pass. Relaxed: the
     * counter is read only by the same CPU that ran the fixture, and making it
     * ordered would put a barrier on the panic path to serve a test. */
    __atomic_fetch_add(&s_panic_declared_ctx_calls, 1u, __ATOMIC_RELAXED);
#endif
    if (frame && frame->int_no == VECTOR_NMI)
        return PANIC_CTX_NMI;
    if (idt_in_nmi())
        return PANIC_CTX_NMI;
    return PANIC_CTX_NORMAL;
}

#ifdef KERNEL_TESTS
uint32_t panic_declared_ctx_calls(void)
{
    return __atomic_load_n(&s_panic_declared_ctx_calls, __ATOMIC_RELAXED);
}

void panic_declared_ctx_calls_reset(void)
{
    __atomic_store_n(&s_panic_declared_ctx_calls, 0u, __ATOMIC_RELAXED);
}
#endif /* KERNEL_TESTS */

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
    {
        uint32_t stop = KSTR_STOP_NUL;
        uint32_t n    = __kstr_read_guarded(buf + *pos, s, cap - *pos, &stop);

        *pos += n;
        /* Same fail-closed test as panic_snapshot_str: anything that is not an
         * ordinary completion means the pointer was part of the corruption.
         * FAULT and NONCANON both used to arrive through the per-byte load's
         * single failure return. */
        if (kstr_stop_is_unreadable(stop))
            panic_append(buf, cap, pos, "<unreadable>");
    }
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
        uint32_t stop = KSTR_STOP_NUL;

        i = __kstr_read_guarded(dst, src, cap, &stop);
        /* A partial read still names the crash; only a pointer that was
         * unreadable from its FIRST byte carries no information, and an empty
         * line would read as "no reason given" rather than "the reason pointer
         * was part of the corruption". NONCANON counts as unreadable exactly as
         * FAULT does -- the per-byte walk rejected a non-canonical address
         * through the same return value, and a source running into the
         * canonical hole must not be rendered as a complete string. */
        if (i == 0u && kstr_stop_is_unreadable(stop))
            fixed = PANIC_STR_UNREADABLE;
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
/* Owner = ONE aligned 64-bit word {cpu:32 low, token:32 high}; 0 = free.
 *
 * The token, not the CPU id, is the OWNER identity. panic_try_claim_owner() is
 * same-CPU re-entrant by design (see its comment), so a nested invocation on the
 * owner CPU is a DIFFERENT panic that can legitimately become terminal while an
 * earlier invocation's record still stands -- an APIC id cannot tell those two
 * apart, and treating them as one is how a nested terminal fault would keep the
 * earlier survivable record instead of the fault that actually killed the box.
 *
 * The CPU id is still carried, because it answers a different question:
 * CONCURRENCY. An invocation on THIS CPU interrupted us and is not running in
 * parallel with us, so taking the page from it needs no quiescence wait; an
 * invocation on ANOTHER CPU may be inside the collector right now, and does. */
#define PANIC_EVIDENCE_NO_OWNER  0ull
static volatile uint64_t s_evidence_owner = PANIC_EVIDENCE_NO_OWNER;

/* Bit 63 marks the owner as ACTIVELY WRITING the page.
 *
 * It lives IN the owner word rather than beside it because ownership and write
 * permission must change in ONE transition. Held apart, there is a window
 * between "I am the owner" and "I am writing" in which a terminal CPU sees a
 * fresh owner that has not yet declared activity, reads that as quiescent, takes
 * the page, and starts populating it while the first owner -- which never
 * revalidates -- populates it too. Two CPUs then zero, fill and CRC the same
 * page and the same klog scratch, which is corruption rather than a merely wrong
 * record. Fused into the CAS the window cannot exist: an invocation acquires
 * ownership and the right to write together, or acquires neither.
 *
 * This also subsumes what a separate activity flag was for. A nested invocation
 * takes the page by CAS, so it can never clear an ACTIVE state belonging to a
 * different writer -- it replaces the whole word, and the invocation it
 * displaced can no longer match on its own value to clear anything. */
#define EV_ACTIVE_BIT  (1ull << 63)
#define EV_TOKEN_MASK  0x7FFFFFFFu        /* bit 63 is the active flag, not token */

static inline uint64_t ev_owner_word(uint32_t cpu, uint32_t token)
{
    return (uint64_t)cpu | ((uint64_t)(token & EV_TOKEN_MASK) << 32);
}
static inline uint32_t ev_owner_cpu(uint64_t w)    { return (uint32_t)w; }
static inline int      ev_owner_active(uint64_t w) { return (w & EV_ACTIVE_BIT) != 0ull; }

/* Per-INVOCATION identity allocator. Never 0 (0 is the free sentinel). */
static volatile uint32_t s_evidence_token_next = 0;

/* Epoch of the record THIS boot published, 0 if none. An epoch-valued completion
 * token rather than a boolean: every revoke keys on it, so a mutation aimed at
 * an older record can never land on a newer one. */
static volatile uint32_t s_evidence_pub_epoch = 0;

/* Next epoch to hand out; 0 = not yet seeded for this boot. */
static volatile uint32_t s_evidence_epoch_next = 0;

/* Non-zero when the standing record was published by a TERMINAL invocation.
 *
 * A terminal invocation may take the page from a SURVIVABLE owner -- that is the
 * whole point of arbitrating at terminal declaration. It must not take it from
 * another TERMINAL record, which would restate a nested fault over the crash
 * that actually killed the machine. The distinction needs its own flag because
 * the terminal path re-enables interrupts for the restart countdown, so an
 * ordinary faulting interrupt can re-enter the panic path AFTER the terminal
 * record is published, arrive with its own token, and be classified terminal in
 * its own right (panic_try_claim_owner is same-CPU re-entrant). Without this the
 * original nested-fault-during-BSOD guarantee -- a complete record is never
 * restated -- would hold for survivable re-entry and silently not for terminal
 * re-entry. */
static volatile uint32_t s_evidence_pub_terminal_epoch = 0;

/* Epoch of the record this boot RESTORED at Phase 0, 0 if none. Seeding reads it
 * so a record published now can never collide with the one still on the page
 * awaiting emission -- consume is keyed by epoch, and a collision would let this
 * boot's fresh crash be erased in place of the older record. Held separately
 * from s_prev_crash because the collector runs long before that buffer is
 * declared, and it is the only field of it the panic path needs. */
static volatile uint32_t s_prev_crash_epoch = 0;

/* TOKEN of the terminal invocation whose takeover could not establish quiescence
 * in its bounded wait; 0 when nothing is owed. The terminal path retries later
 * (the whole BSOD render has run by then) and reports on serial if even that
 * fails.
 *
 * A token rather than a flag, because the obligation belongs to ONE invocation.
 * A bare flag is cleared by whoever next acquires the page, and the acquirer is
 * routinely somebody else: while the terminal CPU renders the BSOD, the
 * survivable writer releases the slot and an unrelated non-terminal panic can
 * take it, clearing the flag before the terminal exit ever checks it. The retry
 * would then skip silently and the boot would keep a non-terminal record, or
 * none, in place of the crash that actually killed the machine. */
static volatile uint32_t s_evidence_takeover_token = 0;

/* Bounds on the quiescence spin.
 *
 * The only writer we can be waiting for is an invocation inside
 * panic_collect_evidence, whose work is FINITE and small: zeroing a ~3 KiB
 * record plus a fixed count of bounded string copies. The per-byte cost rises to
 * a fault fixup when memory is corrupt, which is why the bound is generous
 * relative to that work rather than tight -- but it was originally 2,000,000,
 * orders of magnitude past anything the collector can spend, and every terminal
 * exit then paid the SAME bound again. On an oversubscribed VM, or on a host
 * where a PAUSE loop exits to the hypervisor, that turns the exact
 * double-panic case that most needs a prompt durable record into a long stall
 * before it declines to write one.
 *
 * The retry bound is deliberately much smaller: by the time a terminal exit
 * retries, the entire BSOD render has run, so a writer that is going to finish
 * has finished. A second full-length wait would only delay the honest "not
 * recorded" report. */
/* Candidates the epoch walk tries before giving up. Four is provably enough: it
 * must dodge only the standing epoch, the restored epoch and 0. */
#define PANIC_EVIDENCE_EPOCH_CANDIDATES      4u
/* Times a guarded restore re-copies a record that moved underneath it. Bounded
 * because the competing writer is a panicking CPU -- see the header contract. */
#define PANIC_EVIDENCE_RESTORE_ATTEMPTS      3u
#define PANIC_EVIDENCE_QUIESCE_SPINS        200000u
#define PANIC_EVIDENCE_QUIESCE_SPINS_RETRY   20000u

/* The 64-bit publication word {magic, epoch} living at page offset 0. Publish,
 * revoke and consume are each ONE atomic operation on it. */
static inline volatile uint64_t *ev_pubword(volatile struct panic_evidence *ev)
{
    return (volatile uint64_t *)(void *)ev;
}
static inline uint64_t ev_pubword_val(uint32_t epoch)
{
    return (uint64_t)PANIC_EVIDENCE_MAGIC | ((uint64_t)epoch << 32);
}

/* DURABILITY. Every transition above is atomic in RAM and none of it is
 * durable: the page is write-back memory, so a transition can sit dirty in
 * cache while a reset invalidates it without writeback (kernel/cache.h). The
 * two helpers below are what push a transition to DRAM, and they are called at
 * every lifecycle edge -- publish, un-publish, revoke, consume and drop --
 * because the inverse edges matter as much as publication: a consume that
 * never reaches DRAM lets a reset resurrect the record it retired, and the
 * next boot re-reports a crash the user already saw. */
static void ev_persist_pubword(volatile struct panic_evidence *ev)
{
    cache_writeback_range((const void *)ev, sizeof(uint64_t));
}

/* Push a fully-written but NOT YET PUBLISHED record down. Call this while the
 * publication word still reads zero, and store the word afterwards.
 *
 * The order is enforced by WHEN the publication word is stored, not by the
 * sequence of flushes, and that distinction is the whole design. Flushing a
 * published record body-lines-first and publication-line-last looks equivalent
 * and is not: it holds only where CLFLUSH gives line granularity, and collapses
 * on the WBINVD fallback, which commits many lines with no ordering boundary
 * between them and can therefore land the new magic and CRC while later body
 * lines are still stale. Publishing after the body is durable needs no
 * granularity at all -- there is simply nothing valid in memory to find early.
 * A reset before the publication store leaves no record, which is the honest
 * answer; the alternative is a CRC-valid header over content it never
 * described. */
static void ev_persist_unpublished_body(volatile struct panic_evidence *ev)
{
    cache_writeback_range((const void *)ev, sizeof *ev);
}

uint32_t panic_evidence_next_epoch(uint32_t standing, uint32_t restored)
{
    /* Equality-only generation token, NOT an arithmetic sequence. Numeric
     * monotonicity cannot be promised: `standing` is untrusted cross-boot RAM
     * and may be UINT32_MAX, so a plain max()+1 wraps to 0, and skipping 0 then
     * lands back on a value one of the inputs may already hold (standing
     * 0xFFFFFFFF with restored 1 yields exactly 1). All that is actually
     * required is a value that is nonzero and equal to NEITHER observed epoch,
     * so the walk below advances until it is -- at most three steps. */
    uint32_t e = (standing > restored) ? standing : restored;

    for (uint32_t i = 0u; i < PANIC_EVIDENCE_EPOCH_CANDIDATES; i++) {
        e++;                                   /* wraps to 0, handled below */
        if (e != 0u && e != standing && e != restored)
            return e;
    }
    return 1u;   /* unreachable: 4 distinct candidates cannot all collide with 2 */
}

/* Seed-on-first-use epoch allocator for this boot. */
static uint32_t panic_evidence_alloc_epoch(volatile struct panic_evidence *ev,
                                           uint32_t restored)
{
    uint32_t e = __atomic_load_n(&s_evidence_epoch_next, __ATOMIC_ACQUIRE);

    if (e == 0u)
        e = panic_evidence_next_epoch(ev->epoch, restored);

    __atomic_store_n(&s_evidence_epoch_next, (e + 1u) ? (e + 1u) : 1u,
                     __ATOMIC_RELEASE);
    return e;
}

/* Allocate this invocation's identity: always within EV_TOKEN_MASK and never 0.
 *
 * Both bounds are load-bearing. Bit 63 of the owner word is the ACTIVE flag, so
 * a token wider than the mask would corrupt it; and a token whose masked value
 * is 0 makes ev_owner_word(0, token) equal PANIC_EVIDENCE_NO_OWNER, so an
 * invocation on CPU 0 would publish "the page is free" as its own ownership and
 * any other CPU could take the page out from under it mid-write. */
uint32_t panic_evidence_begin(void)
{
    for (;;) {
        uint32_t t = __sync_add_and_fetch(&s_evidence_token_next, 1u)
                     & EV_TOKEN_MASK;
        if (t != 0u)
            return t;
    }
}

/* Whether the page currently holds a COMPLETE record published by this boot.
 *
 * Both halves, because completion cannot be published in one place: the RAM-side
 * epoch says "this boot published a record" and the page word says "the page
 * still holds it". A record left by a PREVIOUS boot must never suppress this
 * boot's collection, and a record this boot published and a terminal invocation
 * then replaced must not be mistaken for the standing one. */
static uint32_t panic_evidence_standing_epoch(volatile struct panic_evidence *ev)
{
    uint32_t pub = __atomic_load_n(&s_evidence_pub_epoch, __ATOMIC_ACQUIRE);

    if (pub == 0u)
        return 0u;
    return __atomic_load_n(ev_pubword(ev), __ATOMIC_ACQUIRE) == ev_pubword_val(pub)
           ? pub : 0u;
}

/* Whether the record standing at `standing` was published or promoted TERMINAL.
 *
 * Generation-keyed, not a boolean. A bare flag cannot survive the two races it
 * has to: an abandoning predecessor clears it after a terminal successor has
 * already set it, and a record replaced underneath it inherits a classification
 * describing the record that is gone. Comparing a marker epoch against the exact
 * standing epoch makes both impossible to express. */
static int panic_evidence_standing_is_terminal(uint32_t standing)
{
    uint32_t t = __atomic_load_n(&s_evidence_pub_terminal_epoch, __ATOMIC_ACQUIRE);

    return standing != 0u && t == standing;
}

/* Mark the standing record terminal. Used when an invocation that published
 * early (terminal=0, before the panic path knew the machine dies) later reaches
 * terminal arbitration with its OWN record still standing: it does not rewrite
 * the record, so without this the fatal record stays classified survivable and a
 * nested terminal panic during the restart countdown overwrites it -- the exact
 * case the classification exists to prevent. Keyed to the standing epoch, so it
 * can never mark a record that replaced the one we checked. */
static void panic_evidence_promote_terminal(volatile struct panic_evidence *ev,
                                            uint32_t me, uint32_t token)
{
    uint64_t mine = ev_owner_word(me, token);

    for (uint32_t i = 0u; i < PANIC_EVIDENCE_EPOCH_CANDIDATES; i++) {
        uint64_t cur = __atomic_load_n(&s_evidence_owner, __ATOMIC_ACQUIRE);
        uint32_t standing;
        uint32_t t;

        /* ONLY THIS INVOCATION'S OWN RECORD. Promoting whatever happens to be
         * standing is worse than not promoting at all: on the same CPU an async
         * fault that has cleared in_async_work but not yet abandoned still owns a
         * SURVIVABLE record, and a nested abort that becomes terminal would mark
         * that record terminal here -- after which its own terminal take refuses
         * to replace it, and the machine reboots reporting the fault it survived
         * instead of the one that killed it. That is precisely the defect this
         * section exists to remove, so the identity check is the load-bearing
         * part and the early placement is only the optimisation. */
        if ((cur & ~EV_ACTIVE_BIT) != mine)
            return;

        standing = panic_evidence_standing_epoch(ev);
        if (standing == 0u)
            return;                       /* nothing complete to promote */
        t = __atomic_load_n(&s_evidence_pub_terminal_epoch, __ATOMIC_ACQUIRE);
        if (t == standing)
            return;                       /* already terminal */
        if (__sync_bool_compare_and_swap(&s_evidence_pub_terminal_epoch, t,
                                         standing))
            return;
    }
}


/* Become the invocation that owns the page. See the header for the contract.
 *
 * `terminal` is what licenses a takeover: only an invocation panic_try_claim_owner
 * has declared system-terminal may take the page away from a survivable owner,
 * and only one CPU per boot can ever hold that claim -- so there is no
 * terminal-versus-terminal contention across CPUs to arbitrate here. */
int panic_evidence_take(uint32_t me, uint32_t token, int terminal, uint32_t spins_max)
{
    volatile struct panic_evidence *ev =
        (volatile struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR;
    uint64_t mine = ev_owner_word(me, token);

    /* THE PREVIOUS BOOT'S RECORD IS NOT FREE REAL ESTATE.
     *
     * Phase 0 restores the prior crash and deliberately RETAINS the page, so the
     * record survives for a next-boot retry if this boot's last-panic.txt write
     * fails. But a reboot zeroes the RAM-side ownership words, so that retained
     * record looks unowned to the first panic of this boot -- which would zero
     * and replace it. A fault the machine SURVIVES could therefore destroy the
     * only retry copy of the fault that killed the previous one.
     *
     * A terminal invocation may still take it: this boot's fatal crash is the
     * one the operator needs, and the prior record has already had its emission
     * attempt. A survivable one may not. */
    if (!terminal) {
        uint32_t restored = __atomic_load_n(&s_prev_crash_epoch, __ATOMIC_ACQUIRE);

        if (restored != 0u &&
            __atomic_load_n(ev_pubword(ev), __ATOMIC_ACQUIRE)
                == ev_pubword_val(restored))
            return 0;
    }

    for (;;) {
        uint64_t cur = __atomic_load_n(&s_evidence_owner, __ATOMIC_ACQUIRE);

        if (cur == mine) {
            /* Ours already. If the standing record is complete, THIS invocation
             * published it and it therefore already describes this exact fault
             * -- re-collecting would repeat the guarded string walks on the most
             * latency-sensitive path in the kernel to produce the same bytes.
             * The nested-terminal case is NOT this case: a nested invocation
             * carries a different token, so it falls through to the same-CPU
             * branch below and correctly replaces its predecessor's record. */
            {
                uint32_t standing = panic_evidence_standing_epoch(ev);

                if (standing != 0u) {
                    /* Our own complete record already describes this fault. If
                     * we have SINCE become terminal, say so on the record before
                     * leaving -- it was published by the early capture with
                     * terminal=0, and leaving it classified survivable is what
                     * lets a nested terminal panic replace the crash that
                     * actually killed the machine. */
                    if (terminal)
                        panic_evidence_promote_terminal(ev, me, token);
                    return 0;
                }
            }
            /* Ours but unfinished. Re-assert ACTIVE rather than falling through
             * to `took`: returning write permission over a word that advertises
             * quiescence is the exact window this design fused the bit into the
             * CAS to remove, and a terminal invocation on another CPU reading
             * ev_owner_active(cur)==0 would skip its quiescence wait and take a
             * page we are about to write. */
            if (ev_owner_active(cur) ||
                __sync_bool_compare_and_swap(&s_evidence_owner, cur,
                                             cur | EV_ACTIVE_BIT))
                goto took;
            continue;
        }

        if (cur == PANIC_EVIDENCE_NO_OWNER) {
            if (__sync_bool_compare_and_swap(&s_evidence_owner,
                                             PANIC_EVIDENCE_NO_OWNER,
                                             mine | EV_ACTIVE_BIT))
                goto took;
            continue;
        }

        if (ev_owner_cpu(cur) == me) {
            /* A different invocation on THIS CPU. It cannot be running in
             * parallel with us -- we interrupted it and panic never returns --
             * so no quiescence wait is needed. A COMPLETE record still stands
             * unless we are the terminal invocation: an ordinary nested fault
             * during BSOD render must not restate its own crash over the
             * original, while a nested invocation that has become terminal is
             * exactly the fault that killed the machine and must replace a
             * merely-survivable predecessor. */
            /* A COMPLETE record stands. Never restate it over itself -- that is
             * the nested-fault-during-BSOD guarantee -- with ONE exception: a
             * terminal invocation may replace a merely SURVIVABLE predecessor,
             * because that predecessor's machine kept running and this one's did
             * not. Terminal over terminal is refused like any other nested
             * re-entry; the first fatal record is the one that matters, and the
             * restart countdown re-enables interrupts, so a faulting IRQ can
             * arrive here with its own token and claim terminal status of its
             * own. */
            {
            uint32_t standing = panic_evidence_standing_epoch(ev);
            if (standing != 0u &&
                (!terminal || panic_evidence_standing_is_terminal(standing))) {
                /* RETIRE THE PREDECESSOR'S ACTIVE BIT BEFORE LEAVING.
                 *
                 * We are a nested invocation that has decided not to write, and
                 * the invocation we interrupted may have published its record
                 * and been interrupted before it could clear ACTIVE. It is never
                 * coming back, and we are about to park, so nobody else would
                 * ever clear it -- leaving a terminal CPU on another core to time
                 * out on every takeover attempt and lose its evidence to a record
                 * that merely got there first. Clearing it is sound precisely
                 * because the record is COMPLETE: the predecessor had finished
                 * writing the page and only its bookkeeping store was
                 * interrupted. Conditional on the exact word, and re-read on
                 * failure, so we can never retire a DIFFERENT writer's
                 * activity. */
                if (ev_owner_active(cur) &&
                    !__sync_bool_compare_and_swap(&s_evidence_owner, cur,
                                                  cur & ~EV_ACTIVE_BIT))
                    continue;
                return 0;
            }
            }
            if (__sync_bool_compare_and_swap(&s_evidence_owner, cur,
                                             mine | EV_ACTIVE_BIT))
                goto took;
            continue;
        }

        /* Another CPU owns the page. */
        if (!terminal)
            return 0;

        if (ev_owner_active(cur)) {
            /* That CPU is inside the collector RIGHT NOW. Wait for it to publish
             * -- taking the page from a live writer is the corruption case. */
            uint32_t spins = 0u;

            while (ev_owner_active(
                       __atomic_load_n(&s_evidence_owner, __ATOMIC_ACQUIRE))) {
                if (++spins >= spins_max) {
                    /* FAIL CLOSED. A bounded wait cannot PROVE another live CPU
                     * is quiescent, and writing anyway would put two CPUs into
                     * the same page and the same klog scratch -- corruption in
                     * place of a wrong record. The obligation is recorded
                     * against THIS invocation's token and retried at the
                     * terminal exits, so the loss is bounded and, if it still
                     * happens, stated on serial rather than silent. */
                    __atomic_store_n(&s_evidence_takeover_token,
                                     token & EV_TOKEN_MASK, __ATOMIC_RELEASE);
                    return 0;
                }
                /* ARCH: x86-64 -- will move to arch/ with the rest of the CPU
                 * primitives. PAUSE is the spin-wait hint; an ARM64 port wants
                 * YIELD/WFE here. */
                __asm__ volatile ("pause");
            }
            continue;                        /* re-read: ownership may have moved */
        }

        if (__sync_bool_compare_and_swap(&s_evidence_owner, cur,
                                         mine | EV_ACTIVE_BIT))
            goto took;
        continue;
    }

took:
    /* Ownership and write permission were acquired in the SAME compare-and-swap,
     * so there is nothing left to assert here and no window in which a superseded
     * claimant could still believe it may write: any other invocation that took
     * the page replaced this whole word, and this invocation's later
     * publish/release CASes are keyed on its own value and simply fail. */
    return 1;
}

/* Release the ACTIVE bit, keeping ownership, once this invocation has published.
 * Conditional on our exact word: an invocation that was superseded mid-write
 * must not clear the successor's activity. */
static void panic_evidence_write_done(uint32_t me, uint32_t token)
{
    uint64_t mine = ev_owner_word(me, token);

    if (__sync_bool_compare_and_swap(&s_evidence_owner, mine | EV_ACTIVE_BIT,
                                     mine)) {
        /* Our own takeover obligation, if any, is now discharged. Keyed by
         * token, so a different invocation's pending retry is never cancelled. */
        (void)__sync_bool_compare_and_swap(&s_evidence_takeover_token,
                                           token & EV_TOKEN_MASK, 0u);
    }
}

int panic_evidence_takeover_pending_for(uint32_t token)
{
    return __atomic_load_n(&s_evidence_takeover_token, __ATOMIC_ACQUIRE)
           == (token & EV_TOKEN_MASK)
           && (token & EV_TOKEN_MASK) != 0u;
}

void panic_evidence_abandon(uint32_t me)
{
    volatile struct panic_evidence *ev =
        (volatile struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR;
    uint64_t cur = __atomic_load_n(&s_evidence_owner, __ATOMIC_ACQUIRE);
    uint32_t pub;

    /* Keyed on the CPU, not on an invocation token, and NOT on an epoch this
     * frame remembers. The invocation that publishes a record is not always the
     * one that reaches the park: a nested fault can arrive after the record is
     * published and before the park, and that nested invocation is the one that
     * parks. Keying on a remembered epoch would leave the published record
     * standing, which is precisely the misleading-record defect this section
     * exists to remove. Every mutation below is still generation-conditional, so
     * a terminal invocation that already took the page is never disturbed. */
    if (cur == PANIC_EVIDENCE_NO_OWNER || ev_owner_cpu(cur) != me)
        return;

    /* ORDER IS LOAD-BEARING: capture the epoch, RELINQUISH, then revoke exactly
     * the captured epoch.
     *
     * Selecting the epoch after ownership may have transferred is the bug this
     * ordering exists to prevent. Reading the live publication epoch late lets
     * this sequence through: we observe ourselves as owner, a terminal
     * invocation then takes the page and publishes ITS record, we read that
     * newer epoch, and our generation-conditional CAS -- perfectly valid against
     * the value we just read -- erases the terminal record. The takeover had
     * already succeeded, so nothing retries, and the boot loses the crash that
     * killed it. Capturing first makes the CAS name OUR record and no other; if
     * the page moved on, the CAS simply fails.
     *
     * The owner CAS goes BEFORE the revoke so a successful relinquish is the
     * proof that no takeover happened in between: if a terminal invocation did
     * take the page, this CAS fails against its word and we touch nothing at
     * all. */
    pub = __atomic_load_n(&s_evidence_pub_epoch, __ATOMIC_ACQUIRE);

    if (!__sync_bool_compare_and_swap(&s_evidence_owner, cur,
                                      PANIC_EVIDENCE_NO_OWNER))
        return;                              /* superseded -- the page is theirs */

    if (pub != 0u) {
        /* Revocation is only real once in memory. The owner word was already
         * relinquished above, so a successor can be mid-publication while this
         * flush lands -- which is bounded, not unbounded: every operation here
         * is keyed to the epoch captured before the relinquish, so none of them
         * can touch a successor's record. What the window DOES leave is our own
         * complete, CRC-valid record standing in memory if a reset arrives
         * between the CAS and the flush, so the next boot reports a fault the
         * machine survived. That is a wrong report, not a torn one, and the CRC
         * cannot catch it -- the repair is a retiring state in the ownership
         * machinery, parked in TODO-10 with an owner. */
        if (__sync_bool_compare_and_swap(ev_pubword(ev),
                                         ev_pubword_val(pub), 0ull))
            ev_persist_pubword(ev);
        (void)__sync_bool_compare_and_swap(&s_evidence_pub_epoch, pub, 0u);
        /* Keyed to OUR epoch. An unconditional clear here would strip the
         * classification off a terminal successor's record: it can take the page
         * and mark itself terminal in the window between our relinquish and this
         * line, and the record would then advertise as survivable and be
         * overwritten by the next nested terminal panic. */
        (void)__sync_bool_compare_and_swap(&s_evidence_pub_terminal_epoch, pub, 0u);
    }
}

/* Second attempt at a terminal takeover that could not establish quiescence the
 * first time, run at the terminal path's exits.
 *
 * A bounded wait cannot PROVE another live CPU is quiescent, so the first
 * attempt fails closed rather than risk two writers on one page. That alone
 * would lose the record silently, which is the failure this section exists to
 * remove -- so the attempt is repeated here, by which point the whole BSOD
 * render has run and any competing collector has had orders of magnitude more
 * time to finish. If even this fails, the loss is stated on serial: an operator
 * reading the log learns the next boot has no record, instead of finding one
 * missing with no explanation. No-op unless a takeover is actually pending. */
static void panic_evidence_terminal_retry(struct interrupt_frame *frame,
                                          uint32_t bugcheck_code,
                                          const uint64_t bugcheck_params[4],
                                          const char *desc, const char *file,
                                          uint32_t line, uint32_t me,
                                          uint32_t token, uint32_t ctx)
{
    if (!panic_evidence_takeover_pending_for(token))
        return;

    /* The SHORT bound: the whole BSOD render has run since the first attempt,
     * so a writer that was going to finish has finished. Paying the full wait
     * again would only delay the honest "not recorded" report below. */
    panic_collect_evidence(frame, bugcheck_code, bugcheck_params, desc, file,
                           line, me, token, 1,
                           PANIC_EVIDENCE_QUIESCE_SPINS_RETRY, ctx);

    if (panic_evidence_takeover_pending_for(token))
        serial_write_recoverable(
            "\n[PANIC] cross-boot evidence NOT recorded: evidence page stayed busy\n");
}

/* Off-stack klog scratch: the collector may run on a small IST stack (#DF), so
 * the snapshot lands in BSS, not on the panic stack.
 *
 * What makes ONE static buffer safe is NOT that panic is cli'd -- `cli` masks
 * neither NMI nor #MC, and panic_evidence_take is deliberately same-CPU
 * re-entrant, so the collector can be re-entered mid-write on one CPU. It is
 * safe because (a) across CPUs the owner word admits exactly one writer at a
 * time, and (b) a nested invocation on this CPU has interrupted an outer one
 * that will never resume, so the two never interleave their use of it.
 *
 * That argument is about the LIVE path, and panic_evidence_populate is now
 * callable WITHOUT taking the page -- the test fixtures do exactly that. It
 * still holds for them by a different route: a test fixture is single-threaded
 * by construction and no live panic is in flight beside it, so the buffer has
 * one writer there too. A future caller that is neither the collector nor a
 * fixture would need its own argument, or a caller-supplied scratch. */
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

    if (!cap)
        return;

    if (src) {
        if (serial_emerg_ctx_allows_guarded_read(ctx)) {
            /* ONE protected loop for the whole field. The record has 26 of
             * these (file, message, 16 stage messages, 8 klog subsystem +
             * message pairs), and a per-byte call paid a canonical test and a
             * fixup epilogue on each of ~2,900 bytes -- all of it inside the
             * window between the fault and a durable record. The stop reason is
             * discarded here on purpose: a truncated field in a forensic record
             * IS the report, and the record has no room for a marker the reader
             * would have to distinguish from real text. */
            return (void)__kstr_read_guarded(dst, src, cap, (uint32_t *)0);
        }
        for (; i + 1u < cap; i++) {
            char c = src[i];
            if (!c)
                break;
            dst[i] = c;
        }
    }
    dst[i] = '\0';
}

/* Populate a crash record IN PLACE, from `version` through `crc32`.
 *
 * Split out of panic_collect_evidence so the record's CONTENT is testable
 * against a fixture page: the collector's own body could only ever be exercised
 * on the live 0x80000 page, through panic_evidence_take, which mutates
 * boot-global ownership and would lock a later real panic out of the record.
 * Everything that makes the page a SHARED, PUBLISHED object -- arbitration, the
 * epoch, the un-publish, the zeroing, and the final publication word -- stays in
 * the collector, so this function writes no publication state and establishes no
 * ordering that a caller could get wrong.
 *
 * `ev` is a plain pointer, not volatile: every byte here is written exactly once
 * and read back only after the collector's release store, so there is nothing
 * for a volatile qualifier to protect. The live page is identity-mapped normal
 * WB memory, so it takes ordinary stores exactly as a BSS fixture does.
 *
 * `ctx` is the panic context DERIVED ONCE at entry and passed down -- see the
 * panic_collect_evidence contract in panic.h.
 *
 * ARCH: x86-64 -- will move to arch/ with the rest of the CPU primitives. The
 * control-register reads and the legacy-PIC port I/O below are the x86 parts;
 * everything else is portable record population. */
void panic_evidence_populate(struct panic_evidence *ev,
                             struct interrupt_frame *frame,
                             uint32_t bugcheck_code,
                             const uint64_t bugcheck_params[4],
                             const char *message, const char *file,
                             uint32_t line, uint32_t me, uint32_t ctx)
{
    if (!ev)
        return;

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
    /* `ctx` is the panic context the ENTRY declared (panic_declared_ctx, from
     * the vector OR the NMI nesting depth) and handed down. Re-deriving it here
     * cost a second CPUID -- serializing, and a hypervisor exit under KVM/WHPX
     * -- for a value that cannot legitimately differ: the invocation whose IST
     * frames the guarded read must not trample is the one already on this
     * stack, and its context was fixed the moment it was entered. This is the
     * CONTEXT half of the rule the IDENTITY half (`me`) already follows. */
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
        ev->crc32 = kcrc32((const uint8_t *)ev + off, ev->size - off);
    }
}

void panic_collect_evidence(struct interrupt_frame *frame, uint32_t bugcheck_code,
                            const uint64_t bugcheck_params[4],
                            const char *message, const char *file, uint32_t line,
                            uint32_t me, uint32_t token, int terminal,
                            uint32_t spins_max, uint32_t ctx)
{
    /* The page at PANIC_EVIDENCE_ADDR is identity-mapped and reserved by PMM.
     * Raw physical writes only -- no kmalloc / VFS / printk / spinlock here. */
    struct panic_evidence *ev = (struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR;
    uint32_t epoch;

    /* On an SMP double-panic two CPUs must not both write the fixed 0x80000
     * page / shared scratch. panic_evidence_take arbitrates that, and also
     * enforces the completion rule (a COMPLETE record is never overwritten
     * except by a terminal invocation taking it from a survivable one). A caller
     * that never took the page writes nothing, so this function is still correct
     * standing alone.
     *
     * `terminal` is 0 for the EARLY capture, which runs before the panic path
     * knows whether the machine dies, and 1 when the terminal invocation calls
     * back in at arbitration to take the page from a survivable owner. */
    if (!panic_evidence_take(me, token, terminal, spins_max))
        return;

    epoch = panic_evidence_alloc_epoch(
                (volatile struct panic_evidence *)ev,
                __atomic_load_n(&s_prev_crash_epoch, __ATOMIC_ACQUIRE));

    /* Zero the record (8 bytes at a time; sizeof is a multiple of 8). This also
     * clears the publication word: from here until the final store the page
     * reads as "no record", which is exactly what a re-entering invocation must
     * conclude. */
    /* UN-PUBLISH ATOMICALLY, then zero the body.
     *
     * The zero loop below starts at index 1 because index 0 IS the publication
     * word, and clearing it with a plain store would make the un-publish the one
     * transition on that location that is not atomic -- mixed atomic and
     * non-atomic access to a single object, against the contract in panic.h that
     * every lifecycle transition on the word is a single atomic store or CAS.
     * It happens to work on x86-64, where the compiler emits one aligned mov and
     * TSO orders it, and would not survive the planned ARM64 port. */
    __atomic_store_n(ev_pubword((volatile struct panic_evidence *)ev), 0ull,
                     __ATOMIC_RELEASE);
    /* Make the un-publish DURABLE before a single body byte changes. Skipping
     * this is the mirror of publishing in the wrong order: DRAM would still
     * hold the previous record's valid publication word while the body under
     * it is being overwritten, so a reset landing inside the zero loop leaves
     * the next boot a valid header over a half-erased record. Ordered by the
     * MFENCE that closes the range call, so no body store can pass it.
     *
     * Durably clearing is safe HERE, unlike the klog crash region, because
     * panic_evidence_take has already arbitrated ownership of this page: a
     * writer that dies mid-record was entitled to replace what it erased. The
     * klog region has no such arbitration and therefore does not do this. */
    ev_persist_pubword((volatile struct panic_evidence *)ev);
    for (uint32_t i = 1u; i < sizeof *ev / 8u; i++)
        ((volatile uint64_t *)ev)[i] = 0u;

    /* CONTENT, including the CRC over it. Every CRC-covered byte is written
     * exactly once, before the CRC: a record is never amended in place, because
     * an abort between an amendment and its recomputed CRC would leave a
     * magic-valid record that panic_evidence_restore then discards for failing
     * its checksum. */
    panic_evidence_populate(ev, frame, bugcheck_code, bugcheck_params,
                            message, file, line, me, ctx);

    /* DURABLE BEFORE PUBLISHED. The record is complete and the publication
     * word still reads zero, so this is the only point at which the body can
     * be committed to memory with nothing valid standing over it. Doing it
     * here rather than after the publication store is what makes the ordering
     * hold without depending on cache-line granularity. */
    ev_persist_unpublished_body(ev);

    {
        /* PUBLISH: the completion epoch first, then magic+epoch as ONE aligned
         * 64-bit release store. A single store is what removes the window the
         * old flag-then-magic pair had to reason so carefully about -- there is
         * no longer an instruction at which the page reads as half-published,
         * because {magic, epoch} become visible together or not at all.
         *
         * The RAM-side epoch is written FIRST for the same reason the flag used
         * to be: it says "this boot has a record", the page word says "the page
         * still holds it", and a re-entering invocation that sees the first
         * without the second correctly reads the record as unfinished and writes
         * a whole one rather than leaving the boot unrestorable. */
        /* Terminality BEFORE the epoch, for the same reason the epoch precedes
         * the page word: a re-entering invocation that sees the record as
         * complete must already be able to see whether a terminal invocation
         * wrote it, or it would read a terminal record as survivable and
         * overwrite the crash that killed the machine. */
        if (terminal)
            __atomic_store_n(&s_evidence_pub_terminal_epoch, epoch,
                             __ATOMIC_RELEASE);
        __atomic_store_n(&s_evidence_pub_epoch, epoch, __ATOMIC_RELEASE);
        __atomic_store_n(ev_pubword((volatile struct panic_evidence *)ev),
                         ev_pubword_val(epoch), __ATOMIC_RELEASE);

        /* The body is already in memory; this commits the word that makes it
         * findable, and only that word's line is dirty. It happens HERE rather
         * than at the reset tail because the machine may never reach that tail
         * -- a spontaneous triple fault, a watchdog, or a second fault
         * mid-BSOD all reset without running another instruction of ours. A
         * record is restorable from the instant it is published, and this is
         * what makes that true of memory and not just of cache. */
        ev_persist_pubword((volatile struct panic_evidence *)ev);

        /* Quiescent again, and any takeover this invocation owed is discharged.
         * Both conditional on our own owner word, so an invocation that was
         * superseded mid-write clears neither. */
        panic_evidence_write_done(me, token);
    }
}

static int panic_evidence_restore_body(volatile struct panic_evidence *ev,
                                       struct panic_evidence *out,
                                       uint64_t observed);

/* Drop exactly the record the caller validated, never whatever stands now. */
static void ev_drop(volatile struct panic_evidence *ev, uint64_t observed)
{
    if (__sync_bool_compare_and_swap(ev_pubword(ev), observed, 0ull))
        ev_persist_pubword(ev);
}

int panic_evidence_restore_at(volatile struct panic_evidence *page,
                              struct panic_evidence *out)
{
    /* Epoch-guarded read. The page can be rewritten by a panic on another CPU
     * while we copy it, so the publication word is sampled before and after and
     * the copy is discarded if it moved -- a torn record is reported as NO
     * record, never as a valid one. Bounded, because the competing writer is a
     * panicking CPU: retrying forever would hang the survivor. */
    for (uint32_t attempt = 0u; attempt < PANIC_EVIDENCE_RESTORE_ATTEMPTS; attempt++) {
        uint64_t before = __atomic_load_n(ev_pubword(page), __ATOMIC_ACQUIRE);

        if ((uint32_t)before != PANIC_EVIDENCE_MAGIC)
            return 0;                              /* no record */

        /* Epoch 0 is the no-record sentinel EVERYWHERE else in the lifecycle:
         * publish refuses it and consume is inert for it. Accepting it here
         * would restore a record that can never be consumed, so the same crash
         * would be re-reported on every subsequent boot forever. The page is
         * untrusted cross-boot RAM, so this is reachable without any bug on the
         * writing side -- stale contents with a plausible magic are exactly what
         * the header validation exists to reject. */
        if ((uint32_t)(before >> 32) == 0u) {
            /* Routed through ev_drop rather than an inline CAS so this
             * rejection is persisted like every other one: an epoch-0 record
             * that is cleared in cache only comes back after a reset and is
             * rejected again on every subsequent boot, forever. */
            ev_drop(page, before);
            return 0;
        }

        if (!panic_evidence_restore_body(page, out, before))
            return 0;
        if (__atomic_load_n(ev_pubword(page), __ATOMIC_ACQUIRE) == before)
            return 1;                              /* stable across the copy */
    }
    return 0;
}

int panic_evidence_restore(struct panic_evidence *out)
{
    return panic_evidence_restore_at(
        (volatile struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR, out);
}

/* Validate + copy one record. Split out so the live page and a test fixture
 * share exactly one implementation of the trust rules.
 *
 * `observed` is the publication word the caller sampled before validation. Every
 * rejection below DROPS the record, and that drop is generation-conditional
 * against `observed` rather than a plain store to the magic: the page can be
 * republished by a panicking CPU while we validate, and a blind `magic = 0`
 * would then erase a brand-new terminal record on the strength of the OLD one
 * having failed its checks. */
static int panic_evidence_restore_body(volatile struct panic_evidence *ev,
                                       struct panic_evidence *out,
                                       uint64_t observed)
{
    if (!out)
        return 0;
    if (ev->version != PANIC_EVIDENCE_VERSION ||
        ev->size != (uint32_t)sizeof *ev) {
        ev_drop(ev, observed);                     /* stale/incompatible -> drop */
        return 0;
    }
    {
        uint32_t off = (uint32_t)__builtin_offsetof(struct panic_evidence, boot_seq);
        const uint8_t *body = (const uint8_t *)(uintptr_t)ev + off;
        /* Length from the CONSTANT, never a second read of the volatile size
         * field. The size was validated equal to sizeof *ev above, but a
         * concurrent writer zeroing the page between the check and the use would
         * make the reload 0, and the unsigned `0 - off` underflows into a ~4 GiB
         * read walking far past the evidence page -- before the publication-word
         * recheck downstream ever gets to reject the copy. Check-then-use on
         * untrusted shared memory has to use the checked value, not re-read it. */
        if (kcrc32(body, sizeof *ev - off) != ev->crc32) {
            ev_drop(ev, observed);                 /* corrupt -> drop */
            return 0;
        }
    }
    /* The page is cross-boot UNTRUSTED RAM and crc32 only proves byte integrity,
     * not semantic validity. Reject out-of-range counts so a crc-consistent but
     * bogus page cannot drive an out-of-bounds read in the artifact writer. */
    if (ev->stage_count > PANIC_EVIDENCE_STAGES ||
        ev->klog_count  > PANIC_EVIDENCE_KLOGS) {
        ev_drop(ev, observed);
        return 0;
    }
    /* Copy out. The page is RETAINED -- see the header contract. */
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
 * crash is not re-reported next boot. Conditional on the EPOCH we restored -- by
 * Phase 3 the APs are up and a new panic on another CPU may have replaced
 * 0x80000 with a fresh crash; that one must survive, not be erased here.
 *
 * ONE compare-and-swap. The old form compared boot_seq + crc and then cleared
 * the magic as a separate store, so a panic publishing between the two lost its
 * record -- the check knew about the race and the implementation could not
 * express it. A CAS keyed by the publication word says exactly the intended
 * thing: clear this record, or nothing. */
void panic_evidence_consume_at(volatile struct panic_evidence *page, uint32_t epoch)
{
    if (epoch == 0u)
        return;
    /* NOT persisted, unlike every other edge in this lifecycle, and the
     * asymmetry is the point.
     *
     * Consume retires a crash on the strength of last-panic.txt having been
     * written. Making the retirement durable is only safe if the FILE is
     * durable, and it is not yet: the caller treats vfs_flush() == 0 as proof,
     * while the fallback filesystem's flush writes its own caches without a
     * device-level sync. A durable clear on top of that turns a reset in the
     * window into deterministic loss of BOTH copies -- the record retired for
     * good, the file never on the platter.
     *
     * The cost of leaving it in cache is that a reset can restore the still
     * valid word and the next boot re-reports a crash the user has already
     * seen. That is an annoyance; losing the evidence is the failure this
     * whole subsystem exists to prevent, so the annoyance wins. Persist this
     * edge once the backend can prove a device-level sync -- tracked as a
     * parked item in TODO-10 with the filesystem-side owner named. */
    (void)__sync_bool_compare_and_swap(ev_pubword(page),
                                       ev_pubword_val(epoch), 0ull);
}

void panic_evidence_consume(void)
{
    panic_evidence_consume_at(
        (volatile struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR,
        s_prev_crash.epoch);
}

/* Fixture-page publish/revoke, so the lifecycle transitions can be asserted in a
 * unit test without the live page or the boot-global ownership words. They carry
 * the same atomicity as the live path because they ARE the live path's
 * operations on a caller-supplied page. */
int panic_evidence_publish_at(volatile struct panic_evidence *page, uint32_t epoch)
{
    uint32_t off = (uint32_t)__builtin_offsetof(struct panic_evidence, boot_seq);
    const uint8_t *body = (const uint8_t *)(uintptr_t)page + off;

    if (epoch == 0u)
        return 0;

    /* UN-PUBLISH FIRST, exactly as the live collector does. Republishing a
     * page that still carries a valid word would otherwise flush the new body
     * out from under the OLD word: the full-writeback fallback commits both at
     * once, and even the targeted path commits the old word alongside the new
     * CRC, which share a line. Omitting this made the helper diverge from the
     * live path it claims to be, so a fixture could pass a sequence the real
     * collector would never perform. */
    __atomic_store_n(ev_pubword(page), 0ull, __ATOMIC_RELEASE);
    ev_persist_pubword(page);

    page->version = PANIC_EVIDENCE_VERSION;
    page->size    = (uint32_t)sizeof *page;
    page->crc32   = kcrc32(body, sizeof *page - off);
    /* Same durable-before-published order as the live collector, because this
     * IS the live path's publish operation on a caller-supplied page. */
    ev_persist_unpublished_body(page);
    __atomic_store_n(ev_pubword(page), ev_pubword_val(epoch), __ATOMIC_RELEASE);
    ev_persist_pubword(page);
    return 1;
}

int panic_evidence_revoke_at(volatile struct panic_evidence *page, uint32_t epoch)
{
    if (epoch == 0u)
        return 0;
    if (!__sync_bool_compare_and_swap(ev_pubword(page),
                                      ev_pubword_val(epoch), 0ull))
        return 0;
    ev_persist_pubword(page);
    return 1;
}

void panic_evidence_restore_early(void)
{
    if (panic_evidence_restore(&s_prev_crash)) {
        s_had_prev_crash = 1;
        /* RELEASE, and published AFTER the record is fully copied out: the
         * panic path reads this to refuse overwriting a restored-but-unemitted
         * record, so it must never become visible ahead of the copy it
         * describes. */
        __atomic_store_n(&s_prev_crash_epoch, s_prev_crash.epoch,
                         __ATOMIC_RELEASE);
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
    /* Derive the panic-safe CPU identity ONCE, here, and pass it down.
     *
     * cpu_panic_safe_apic_id executes CPUID, which serializes and exits to the
     * hypervisor under KVM/WHPX. The evidence lifecycle needs the identity in
     * four places (take, record population, write completion, terminal take),
     * and re-deriving it in each added two exits to every ordinary panic --
     * pure latency on the one path whose entire cost function is
     * instructions-between-fault-and-durable-record. */
    const uint32_t ev_cpu   = cpu_panic_safe_apic_id();
    const uint32_t ev_token = panic_evidence_begin();

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
                           have_file ? file_snap : (const char *)0, line,
                           ev_cpu, ev_token, 0, PANIC_EVIDENCE_QUIESCE_SPINS,
                           snap_ctx);

    /* NOTE: emergency serial is NOT armed here. Arming is a SYSTEM-TERMINAL
     * declaration and this function is not yet committed to one -- the async
     * isolation branch below parks only the faulting AP and reports BOOT_FATAL,
     * after which boot_storage.c falls back to sequential init and the system
     * keeps running. Arming here would leave that surviving system permanently
     * routed through the lossy try-lock path. The pre-arbitration dump below is
     * made abort-safe by calling the emergency writers DIRECTLY instead, which
     * needs no global state; the latch is armed once ownership is claimed. */

    /* NOTE the wedged-UART accounting: the dump below must be GS-independent, so
     * it cannot ask whether this CPU is survivable before spending; it spends
     * first, and the async-isolation branch below hands back what is owed if the
     * machine turns out to keep running. Nothing is sampled here -- what is owed
     * is read from this CPU's own claims at the point of parking, not inferred
     * from a delta against an entry snapshot (TODO-10 S29). */
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
            /* THE ENTRY context, not a fresh derivation. Same declared-context
             * rule as the pre-arbitration dump, and the same reason the
             * collector stopped re-deriving it: panic_declared_ctx reads the
             * NMI depth through cpu_panic_safe_apic_id(), i.e. a CPUID, which
             * serializes and exits to the hypervisor under KVM/WHPX. This
             * branch runs inside the SAME panic entry that already computed
             * snap_ctx, so a second derivation could not legitimately differ
             * from it -- it would only cost another exit on the path whose
             * whole budget is instructions-before-the-record-is-durable. */
            uint32_t    async_ctx = snap_ctx;
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
             * find the allowance still spent. The reclamation reads the packed
             * serial word only for the live generation and writes nothing but
             * the separate, cacheline-isolated claim array, so it is safe this
             * early.
             *
             * ATTRIBUTABLE, AND IT COVERS EVERY INVOCATION ON THIS CPU. The
             * reclamation reads this CPU's own claims -- which no other CPU
             * writes -- and hands back each one that was taken but never charged
             * by a timeout, so it can neither absorb another CPU's charge nor
             * credit an epoch that has since been published.
             *
             * A DELTA CANNOT DO THIS, which is why it no longer is one. The
             * previous shape refunded `charges_self() - charges_before` against
             * a count sampled at panic entry, so a nested abort inside an outer
             * dump on this CPU carried the OUTER dump's reservations inside its
             * own baseline and refunded none of them -- leaking up to the whole
             * pre-arm budget on the CPU that is about to park as its sole
             * recorded owner (TODO-10 S29).
             *
             * SAFE ONLY BECAUSE THIS CPU NEVER RETURNS TO THE FRAME IT
             * INTERRUPTED. An outer writer frame stopped mid-wait still holds a
             * token whose slot this frees, and freeing it lets the next reserve
             * hand the same slot out; if that frame ever resumed, its return
             * would clear the new holder's charge.
             *
             * It never resumes, and the reason is NOT that every later abort
             * re-enters this branch -- it does not. There are THREE intervals,
             * and the boundary is the `in_async_work` clear rather than the
             * `async_done` publication that follows it two stores later:
             *   - up to the clear, a nested abort passes the async test at the
             *     top of this branch, re-enters it, and parks;
             *   - between the clear and the publication, it already FAILS that
             *     test and takes the terminal path, while the BSP has not yet
             *     been told anything;
             *   - after the publication, it takes the terminal path while the
             *     BSP is proceeding with sequential init.
             * The invariant that actually holds is weaker than "it re-enters
             * this branch" and is sufficient for the reclaim: every one of
             * those paths is non-returning, so none unwinds to the interrupted
             * writer. Which of them is the CORRECT disposition is a separate
             * question, filed as TODO-10 S30. */
            (void)serial_emerg_reclaim_self();

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

            /* REVOKE THE CROSS-BOOT RECORD. This branch is the one place in the
             * panic path that KNOWS the machine survives: the BSP falls back to
             * sequential init and keeps booting. A record left standing here is
             * restored by the next boot and written to last-panic.txt as an
             * unexpected shutdown that never happened -- and, holding the only
             * slot, it refuses that slot to the crash that eventually does kill
             * the machine. Both halves of that were the section-23 defect.
             *
             * AFTER the diagnostic and the UART hand-back, not before: those two
             * are what make the surviving system usable, and the revoke must not
             * be able to leave the page owned by a CPU that then faults on its
             * way to the park. Generation-conditional throughout, so a terminal
             * invocation that already took the page is untouched. */
            panic_evidence_abandon(ev_cpu);

            /* Park this AP permanently -- BSP will handle the failure */
            for (;;) __asm__ volatile("hlt");
        }
    }

    /* Atomic panic ownership: only the first CPU to panic captures FPU state
     * and builds the CONTEXT record. Secondary CPUs that panic simultaneously
     * park immediately to avoid clobbering the owner's crash data.
     * Placed after async isolation so APs doing async work park even earlier. */
    if (panic_try_claim_owner()) {
        /* CLASSIFY THE STANDING RECORD TERMINAL IMMEDIATELY, before anything
         * else in this branch.
         *
         * This invocation published its record early, with terminal=0, because
         * the panic path did not yet know the machine dies. It knows NOW. Until
         * the marker says so that already-fatal record advertises as survivable
         * -- and `cli` masks neither NMI nor #MC while panic_try_claim_owner is
         * same-CPU re-entrant, so a nested abort landing in the gap wins terminal
         * arbitration of its own, reads the original record as survivable, and
         * replaces the crash that first made the machine terminal with a fault
         * in the panic handler.
         *
         * Promoting here rather than at the later collect call is what removes
         * serial_enter_emergency and the whole call transition from that gap.
         * What remains is the distance between the claim CAS above and this one:
         * the two live in different ownership words keyed by different CPU-id
         * spaces (logical id there, panic-safe APIC id in the evidence state),
         * so fusing them into one transition is a design change this section
         * does not own. That residual is parked with the other
         * live-double-panic cases. */
        panic_evidence_promote_terminal(
            (volatile struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR,
            ev_cpu, ev_token);

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

        /* TERMINAL ARBITRATION OF THE CROSS-BOOT RECORD.
         *
         * This is the first point at which the panic path knows the machine
         * dies, and therefore the only honest point at which to decide who owns
         * the one evidence slot. The early capture above ran before that was
         * known, so the standing record may belong to a survivable fault that
         * has since parked its AP -- which used to hold the slot forever and
         * leave the fatal crash unrecorded.
         *
         * Ownership is TAKEN rather than waited for. Waiting on the survivable
         * path to release was tried and races the other way: the terminal CPU
         * checks ownership before the release lands, declines, and never
         * retries, so the boot ends with neither record. Taking is safe because
         * exactly one CPU per boot holds the panic-owner claim, so no second
         * terminal invocation can contend for the page, and because
         * panic_evidence_take establishes writer quiescence before it writes. */
        panic_collect_evidence(frame, bugcheck_code, bugcheck_params, desc_snap,
                               have_file ? file_snap : (const char *)0, line,
                               ev_cpu, ev_token, 1,
                               PANIC_EVIDENCE_QUIESCE_SPINS, snap_ctx);

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
        panic_evidence_terminal_retry(frame, bugcheck_code, bugcheck_params,
                                      desc_snap,
                                      have_file ? file_snap : (const char *)0,
                                      line, ev_cpu, ev_token, snap_ctx);
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

        /* MASK INTERRUPTS BEFORE COLLECTING. The countdown above re-enabled them
         * (`sti`, so the timer can tick the seconds down), and the collector
         * zeroes and rewrites the fixed 0x80000 page plus the shared klog
         * scratch -- exactly the state the invariant at the top of this function
         * says nothing may re-enter while it is being touched. Running it
         * preemptibly lets a timer ISR schedule a thread that panics, whose
         * same-CPU take would hand a second writer the page. The other two
         * terminal retries are already masked; this one had drifted out from
         * under that guarantee. Nothing below re-enables: the next step resets
         * the machine. */
        __asm__ volatile ("cli");
        panic_evidence_terminal_retry(frame, bugcheck_code, bugcheck_params,
                                      desc_snap,
                                      have_file ? file_snap : (const char *)0,
                                      line, ev_cpu, ev_token, snap_ctx);

        /* Restart via ACPI reset or triple fault */
        {
            /* DEFENCE IN DEPTH, and nothing more. Each evidence writer has
             * already persisted its own record at publication time, which is
             * the mechanism that actually carries the guarantee -- this call
             * cannot, because it runs on THIS CPU only, does not quiesce the
             * others, and does not wait for external caches to finish. What
             * it does buy is everything no writer knew to flush: the klog
             * crash region if a later store dirtied it again, and any other
             * state a post-mortem might want. Free at this point, since the
             * next instruction resets the machine. */
            cache_writeback_all();

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

    panic_evidence_terminal_retry(frame, bugcheck_code, bugcheck_params,
                                  desc_snap,
                                  have_file ? file_snap : (const char *)0,
                                  line, ev_cpu, ev_token, snap_ctx);

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
