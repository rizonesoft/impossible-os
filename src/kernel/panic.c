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
#include "kernel/boot_progress.h"   /* boot_stage_history_get (panic evidence) */
#include "kernel/boot_info.h"       /* boot_history seq + g_boot_info.had_panic */
#include "kernel/mm/pmm.h"          /* pmm_get_free_frames */
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

/* Maximum stack trace depth */
#define MAX_STACK_DEPTH   16

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

void KeBugCheckEx(BUGCHECK_CODE code, uint64_t p1, uint64_t p2,
                  uint64_t p3, uint64_t p4)
{
    extern uint64_t system_get_ticks(void);

    POST16(0xDE40);

    /* Store bugcheck params for dump pipeline and BSOD display */
    g_last_bugcheck.code      = code;
    g_last_bugcheck.param1    = p1;
    g_last_bugcheck.param2    = p2;
    g_last_bugcheck.param3    = p3;
    g_last_bugcheck.param4    = p4;
    g_last_bugcheck.timestamp = system_get_ticks();

    /* Write last bugcheck to registry for cross-boot persistence.
     * This is best-effort -- registry may not be available during
     * early boot crashes. No lock -- we're about to halt. */
    if (kernel_subsystem_ready(SUBSYS_REGISTRY)) {
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

    /* Build description string for panic_screen */
    static char desc[128];
    {
        const char *name = bugcheck_name(code);
        uint32_t pos = 0;
        const char *p = "STOP 0x";
        while (*p && pos < 120) desc[pos++] = *p++;
        /* hex code */
        {
            const char hex[] = "0123456789ABCDEF";
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
        while (*p && pos < 120) desc[pos++] = *p++;
        while (*name && pos < 120) desc[pos++] = *name++;
        desc[pos++] = ')';
        desc[pos] = '\0';
    }

    /* Route to panic_screen -- it handles BSOD rendering, klog crash persist,
     * subsystem dump, POST code, and halt/restart. */
    panic_screen((void *)0, (uint64_t)code, desc, __FILE__, __LINE__);

    /* panic_screen should never return, but just in case */
    for (;;) __asm__ volatile("cli; hlt");
}

/* --- NMI-triggered crash (S1) ------------------------------------------- */

static uint64_t nmi_crash_handler(struct interrupt_frame *frame)
{
    (void)frame;
    KeBugCheckEx(BUGCHECK_MANUALLY_INITIATED_CRASH,
                 0, 0, 0, 0);
    /* KeBugCheckEx never returns */
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
        if (rc == 0 && vtype == 4 && enabled) {
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
    /* Zero the entire CONTEXT first */
    {
        uint8_t *p = (uint8_t *)ctx;
        for (uint32_t i = 0; i < sizeof(CONTEXT); i++)
            p[i] = 0;
    }

    ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;

    if (frame) {
        /* --- Control registers (CONTEXT_CONTROL) --- */
        ctx->Rip    = frame->rip;
        ctx->SegCs  = (uint16_t)frame->cs;
        ctx->SegSs  = (uint16_t)frame->ss;
        ctx->Rsp    = frame->rsp;
        ctx->EFlags = (uint32_t)frame->rflags;
        ctx->Rbp    = frame->rbp;

        /* --- Integer registers (CONTEXT_INTEGER) --- */
        ctx->Rax = frame->rax;
        ctx->Rcx = frame->rcx;
        ctx->Rdx = frame->rdx;
        ctx->Rbx = frame->rbx;
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
    }

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

/* --- Hang-proof hex writer for the unconditional panic-reason dump ---
 *
 * Writes a 64-bit value as "0x<hex>" directly to serial via
 * serial_putchar(). No locks, no buffers, no formatting library --
 * so even if klog, printk, the heap, or the compositor lock are
 * corrupted, this still produces readable output. Used only by the
 * top-of-panic dump (before async isolation, ownership, readiness,
 * or framebuffer); elsewhere printk("%x") is fine. */
static void serial_write_hex(uint64_t v)
{
    extern void serial_putchar(char c);
    static const char d[] = "0123456789ABCDEF";
    serial_putchar('0');
    serial_putchar('x');
    int leading = 1;
    int i;
    for (i = 60; i >= 0; i -= 4) {
        uint32_t nib = (uint32_t)((v >> i) & 0xFu);
        if (leading && nib == 0 && i > 0)
            continue;
        leading = 0;
        serial_putchar(d[nib]);
    }
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

/* --- Crash dump to file --- */

static void write_crash_dump(struct interrupt_frame *frame,
                             const char *description, const char *file,
                             uint32_t line)
{
    char buf[2048];
    int pos = 0;
    uint64_t cr2_val;
    uint64_t cr3_val;
    uint64_t rbp;
    uint32_t depth;
    char num[12];

    if (!vfs_is_mounted('C'))
        return;

    /* Build crash dump text manually (no snprintf in freestanding) */
    /* Header */
    {
        const char *hdr = "=== IMPOSSIBLE OS CRASH DUMP ===\n";
        const char *s = hdr;
        while (*s && pos < 2040) buf[pos++] = *s++;
    }

    /* Description */
    {
        const char *s;
        const char *lbl = "Description: ";
        s = lbl;
        while (*s && pos < 2040) buf[pos++] = *s++;
        s = description;
        while (*s && pos < 2040) buf[pos++] = *s++;
        buf[pos++] = '\n';
    }

    /* File:line */
    if (file) {
        const char *lbl = "Source: ";
        const char *s = lbl;
        while (*s && pos < 2040) buf[pos++] = *s++;
        s = file;
        while (*s && pos < 2040) buf[pos++] = *s++;
        buf[pos++] = ':';
        itoa_simple(line, num);
        s = num;
        while (*s && pos < 2040) buf[pos++] = *s++;
        buf[pos++] = '\n';
    }

    /* Registers from frame */
    if (frame) {
        const char *rlbl = "\nRegisters:\n";
        const char *s = rlbl;
        while (*s && pos < 2040) buf[pos++] = *s++;

        /* We'll just note that registers are available in the frame */
        {
            const char *note = "  (See serial output for full register dump)\n";
            s = note;
            while (*s && pos < 2040) buf[pos++] = *s++;
        }
    }

    /* Stack trace */
    cr2_val = read_cr2();
    cr3_val = read_cr3();
    (void)cr2_val;
    (void)cr3_val;

    {
        const char *stlbl = "\nStack Trace:\n";
        const char *s = stlbl;
        while (*s && pos < 2040) buf[pos++] = *s++;
    }

    rbp = frame ? frame->rbp : 0;
    if (!rbp) {
        __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
    }

    for (depth = 0; depth < MAX_STACK_DEPTH && rbp != 0; depth++) {
        uint64_t *frame_ptr = (uint64_t *)rbp;
        uint64_t ret_addr;

        /* Safety check: ensure RBP points to a reasonable address */
        if (rbp < 0x100000 || rbp > 0x200000)
            break;

        ret_addr = frame_ptr[1];  /* return address is at [rbp+8] */
        if (ret_addr == 0)
            break;

        {
            const char *fr = "  #";
            const char *s = fr;
            while (*s && pos < 2040) buf[pos++] = *s++;
            itoa_simple(depth, num);
            s = num;
            while (*s && pos < 2040) buf[pos++] = *s++;
            {
                const char *at = " at 0x";
                s = at;
                while (*s && pos < 2040) buf[pos++] = *s++;
            }
            /* Hex format for return address */
            {
                uint64_t v = ret_addr;
                char hex[17];
                int hi;
                for (hi = 15; hi >= 0; hi--) {
                    uint32_t nib = (uint32_t)(v & 0xF);
                    hex[hi] = "0123456789ABCDEF"[nib];
                    v >>= 4;
                }
                hex[16] = '\0';
                s = hex;
                while (*s && pos < 2040) buf[pos++] = *s++;
            }
            /* Resolve symbol name */
            {
                uint64_t sym_off = 0;
                const char *sym_name = symtab_resolve(ret_addr, &sym_off);
                if (sym_name) {
                    const char *sp = "  ";
                    s = sp;
                    while (*s && pos < 2040) buf[pos++] = *s++;
                    s = sym_name;
                    while (*s && pos < 2040) buf[pos++] = *s++;
                    const char *plus = "+0x";
                    s = plus;
                    while (*s && pos < 2040) buf[pos++] = *s++;
                    /* Hex offset (compact) */
                    char ohex[17];
                    int oi = 16;
                    ohex[oi] = '\0';
                    uint64_t ov = sym_off;
                    if (ov == 0) { ohex[--oi] = '0'; }
                    else { while (ov && oi > 0) { ohex[--oi] = "0123456789abcdef"[ov & 0xF]; ov >>= 4; } }
                    s = &ohex[oi];
                    while (*s && pos < 2040) buf[pos++] = *s++;
                }
            }
            buf[pos++] = '\n';
        }

        rbp = frame_ptr[0];  /* previous RBP */
    }

    buf[pos] = '\0';

    /* Ensure the directory exists and write the crash dump */
    {
        struct vfs_node *sys_dir = vfs_open("C:\\Impossible\\System", VFS_O_READ);
        if (sys_dir) {
            vfs_close(sys_dir);
        }
    }

    vfs_create("C:\\Impossible\\System\\crashdump.log", 1);
    {
        struct vfs_node *dump_file = vfs_open(
            "C:\\Impossible\\System\\crashdump.log", VFS_O_WRITE);
        if (dump_file) {
            vfs_write(dump_file, 0, (uint32_t)pos, (const uint8_t *)buf);
            vfs_close(dump_file);
        }
    }
}

/* --- Main panic screen --- */

/* ============================================================================
 * Panic forensic evidence -- cross-boot crash record
 * ========================================================================== */

/* First-caller-wins guard: a nested fault during BSOD render must not overwrite
 * the original crash record. Set on the first collect of this boot. */
static volatile int s_evidence_collected = 0;

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

static void pe_copy(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0u;
    if (src)
        for (; i + 1u < cap && src[i]; i++)
            dst[i] = src[i];
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
                            const char *message, const char *file, uint32_t line)
{
    /* Atomic claim: on an SMP double-panic two CPUs must not both write the
     * fixed 0x80000 page / shared scratch. The first to swap 1 in wins; the
     * loser returns without touching any shared evidence state. */
    if (__atomic_exchange_n(&s_evidence_collected, 1, __ATOMIC_ACQ_REL))
        return;

    /* The page at PANIC_EVIDENCE_ADDR is identity-mapped and reserved by PMM.
     * Raw physical writes only -- no kmalloc / VFS / printk / spinlock here. */
    struct panic_evidence *ev = (struct panic_evidence *)(uintptr_t)PANIC_EVIDENCE_ADDR;

    /* Zero the record (8 bytes at a time; sizeof is a multiple of 8). */
    for (uint32_t i = 0u; i < sizeof *ev / 8u; i++)
        ((volatile uint64_t *)ev)[i] = 0u;

    ev->version  = PANIC_EVIDENCE_VERSION;
    ev->size     = (uint32_t)sizeof *ev;
    ev->boot_seq = boot_history_kernel_phase3_committed_seq();

    ev->bugcheck_code = bugcheck_code;
    /* g_last_bugcheck params are only trustworthy when this panic actually came
     * through KeBugCheckEx, i.e. its recorded code matches the code we are
     * collecting. A raw exception (frame != NULL) or a direct panic_screen()
     * caller (crash-test) leaves g_last_bugcheck stale, so the params stay zero
     * and the fault_vector / bugcheck_code fields carry the identity. */
    if (!frame && g_last_bugcheck.code == bugcheck_code) {
        ev->bugcheck_params[0] = g_last_bugcheck.param1;
        ev->bugcheck_params[1] = g_last_bugcheck.param2;
        ev->bugcheck_params[2] = g_last_bugcheck.param3;
        ev->bugcheck_params[3] = g_last_bugcheck.param4;
    }
    ev->fault_vector = frame ? frame->int_no  : 0u;
    ev->err_code     = frame ? frame->err_code : 0u;

    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    ev->cr0 = cr0; ev->cr2 = cr2; ev->cr3 = cr3; ev->cr4 = cr4;

    /* CPUID leaf 1 -> initial APIC id in EBX[31:24]; pure CPUID, fault-safe. */
    {
        uint32_t a, b, c, d;
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u));
        ev->cpu_id = (b >> 24) & 0xFFu;
    }
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
    pe_copy(ev->file, sizeof ev->file, file);
    pe_copy(ev->message, sizeof ev->message, message);

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
            pe_copy(ev->stages[i].msg, sizeof ev->stages[i].msg, e->msg);
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
            pe_copy(ev->klogs[i].subsystem, sizeof ev->klogs[i].subsystem,
                    s_panic_klog_scratch[i].subsystem);
            pe_copy(ev->klogs[i].message, sizeof ev->klogs[i].message,
                    s_panic_klog_scratch[i].message);
        }
        ev->klog_count = n;
    }

    /* crc32 over everything AFTER the crc32 field, then publish the magic last
     * so a reader never sees a valid magic over a half-written record. */
    {
        uint32_t off = (uint32_t)__builtin_offsetof(struct panic_evidence, boot_seq);
        ev->crc32 = panic_crc32((const uint8_t *)ev + off, ev->size - off);
        ev->magic = PANIC_EVIDENCE_MAGIC;
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
    ev->magic = 0u;
    return 1;
}

/* Phase-0 restored record + flag. Kept kernel-side (not in g_boot_info) so the
 * boot_info ABI is untouched -- the desktop reads panic_had_previous_crash().
 * The restore runs pre-heap, so the record lands in this BSS buffer; the
 * X:\Crash\ emission is deferred to panic_evidence_write_blackbox() post-VFS. */
static struct panic_evidence s_prev_crash;
static int s_had_prev_crash = 0;

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
        vfs_close(f);
        if (wrote >= 0 && (uint32_t)wrote == pos)
            klog(LOG_INFO, "panic", "previous crash written to %s (%u bytes)",
                 (uint64_t)(uintptr_t)path, (uint64_t)pos);
        else
            klog(LOG_WARN, "panic", "last-panic: short write (%d of %u)",
                 (uint64_t)wrote, (uint64_t)pos);
    } else {
        klog(LOG_WARN, "panic", "last-panic: vfs_open failed for %s",
             (uint64_t)(uintptr_t)path);
    }
    for (uint32_t p = 0u; p < pages; p++)
        pmm_free_frame(phys + p * 4096u);
}

void panic_screen(struct interrupt_frame *frame, uint64_t error_code,
                  const char *description, const char *file, uint32_t line)
{
    /* Mask interrupts FIRST -- nothing may re-enter the panic path while the
     * collector touches the fixed 0x80000 evidence page and shared log state.
     * The faulting interrupt state is preserved in frame->rflags for forensics. */
    __asm__ volatile ("cli");

    /* Capture cross-boot forensic evidence before any other panic work (serial,
     * async isolation, framebuffer, VFS) that could itself fault. The explicit
     * error_code is the authoritative no-frame stop code (KeBugCheckEx passes
     * its bugcheck code; direct callers like the crash-test pass theirs). For a
     * raw exception (frame set) the fault_vector field carries identity. */
    panic_collect_evidence(frame, frame ? 0u : (uint32_t)error_code,
                           description, file, line);

    uint32_t screen_w;
    uint32_t screen_h;
    uint64_t cr2_val;
    uint64_t cr3_val;
    uint64_t rbp;
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
     * Uses serial_write() directly -- no printk, no klog, no
     * framebuffer, no locks. If this itself hangs the panic is SO
     * broken that serial itself is dead, in which case no output
     * could have been captured anyway.
     *
     * Why this landed 2026-04-22: operators kept seeing boot logs
     * end at the readiness-dump `[OK] TPM` line with no panic header
     * after -- because the readiness dump fires BEFORE any "[PANIC]"
     * marker, and any hang in the subsequent fb/printk/compositor
     * path swallowed the actual reason. Panic MUST be observable
     * even under the most hostile downstream state. */
    {
        serial_write("\n\n[PANIC] ");
        serial_write(description ? description : "(no description)");
        if (file) {
            serial_write("\n  at ");
            serial_write(file);
        }
        serial_write("\n");

        if (frame) {
            serial_write("  RIP=");
            serial_write_hex(frame->rip);
            serial_write(" CS=");
            serial_write_hex(frame->cs);
            serial_write(" ERR=");
            serial_write_hex(error_code);
            serial_write("\n  CR2=");
            serial_write_hex(read_cr2());
            serial_write(" CR3=");
            serial_write_hex(read_cr3());
            serial_write("\n");
            serial_write("  RAX=");     serial_write_hex(frame->rax);
            serial_write(" RBX=");      serial_write_hex(frame->rbx);
            serial_write(" RCX=");      serial_write_hex(frame->rcx);
            serial_write(" RDX=");      serial_write_hex(frame->rdx);
            serial_write("\n  RSI=");   serial_write_hex(frame->rsi);
            serial_write(" RDI=");      serial_write_hex(frame->rdi);
            serial_write(" RBP=");      serial_write_hex(frame->rbp);
            serial_write(" RSP=");      serial_write_hex(frame->rsp);
            serial_write("\n");
        }
    }

    /* --- Async init fault isolation ---
     * If this CPU is executing an async boot init step, don't crash the
     * whole system. Record BOOT_FATAL for this step and park the AP.
     * The BSP's barrier will detect the failure via async_done/async_result. */
    {
        struct per_cpu_data *pcpu = smp_this_cpu();
        if (pcpu && pcpu->in_async_work) {
            klog(LOG_ERROR, "ASYNC",
                 "[ASYNC] FAULT on CPU%u during '%s': %s (err=0x%x, RIP=0x%x)",
                 pcpu->cpu_id,
                 pcpu->async_name ? pcpu->async_name : "?",
                 description ? description : "unknown",
                 error_code, frame ? frame->rip : 0);
            pcpu->async_result = (uint8_t)2;  /* BOOT_FATAL */
            pcpu->in_async_work = 0;
            smp_mb();
            pcpu->async_done = 1;
            smp_mb();
            /* Park this AP permanently -- BSP will handle the failure */
            for (;;) __asm__ volatile("hlt");
        }
    }

    /* Atomic panic ownership: only the first CPU to panic captures FPU state
     * and builds the CONTEXT record. Secondary CPUs that panic simultaneously
     * park immediately to avoid clobbering the owner's crash data.
     * Placed after async isolation so APs doing async work park even earlier. */
    if (panic_try_claim_owner()) {
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

    /* If framebuffer is not yet initialized (Phase 0 panic), fall back to
     * serial-only output via boot_halt(). No BSOD drawing is possible. */
    if (!kernel_subsystem_ready(SUBSYS_FB)) {
        serial_write("\n[PANIC] ");
        serial_write(description ? description : "(unknown)");
        serial_write("\n");
        if (file) {
            serial_write("  at ");
            serial_write(file);
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
    printk("    Description: %s\n", (uint64_t)(uintptr_t)description);

    /* Source location */
    if (file) {
        fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
        printk("    Source:  %s:%u\n", (uint64_t)(uintptr_t)file, (uint64_t)line);
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

    /* Walk the RBP chain */
    rbp = frame ? frame->rbp : 0;
    if (!rbp) {
        __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
    }

    for (depth = 0; depth < MAX_STACK_DEPTH && rbp != 0; depth++) {
        uint64_t *frame_ptr = (uint64_t *)rbp;
        uint64_t ret_addr;

        /* Safety: RBP should be in kernel memory range */
        if (rbp < 0x100000 || rbp > 0x200000)
            break;

        ret_addr = frame_ptr[1];
        if (ret_addr == 0)
            break;

        {
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
        rbp = frame_ptr[0];
    }

    if (depth == 0) {
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
    write_crash_dump(frame, description, file, line);

    /* === Persist klog ring buffer to reserved physical memory === */
    klog_crash_persist();

    {
        fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
        if (vfs_is_mounted('C'))
            printk("\n    Crash dump saved to C:\\Impossible\\System\\crashdump.log\n");
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
