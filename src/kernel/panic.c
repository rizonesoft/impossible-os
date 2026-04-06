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
#include "kernel/drivers/serial.h"
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
    { 0x77,       "KERNEL_STACK_INPAGE_ERROR" },
    { 0x7A,       "KERNEL_DATA_INPAGE_ERROR" },
    { 0x139,      "KERNEL_SECURITY_CHECK_FAILURE" },
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

void panic_screen(struct interrupt_frame *frame, uint64_t error_code,
                  const char *description, const char *file, uint32_t line)
{
    uint32_t screen_w;
    uint32_t screen_h;
    uint64_t cr2_val;
    uint64_t cr3_val;
    uint64_t rbp;
    uint32_t depth;
    int32_t restart_secs = 0;
    uint32_t reg_restart;

    /* Disable interrupts to prevent further exceptions */
    __asm__ volatile ("cli");

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
