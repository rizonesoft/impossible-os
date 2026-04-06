/* ============================================================================
 * panic.h -- Kernel panic screen
 *
 * Provides a styled graphical panic screen (blue screen of death) with:
 *   - Exception name and stop code
 *   - Faulting address and RIP
 *   - Source file + line (via __FILE__, __LINE__)
 *   - Full register dump (RAX–R15, RSP, RFLAGS, CR2, CR3)
 *   - Stack trace (RBP chain walk)
 *   - Auto-restart countdown (configurable via Registry)
 *   - Crash dump to C:\Impossible\System\crashdump.log
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward declaration */
struct interrupt_frame;

/* --- CONTEXT flags (Windows x64) ----------------------------------------- */

#define CONTEXT_AMD64               0x00100000
#define CONTEXT_CONTROL             (CONTEXT_AMD64 | 0x01)
#define CONTEXT_INTEGER             (CONTEXT_AMD64 | 0x02)
#define CONTEXT_SEGMENTS            (CONTEXT_AMD64 | 0x04)
#define CONTEXT_FLOATING_POINT      (CONTEXT_AMD64 | 0x08)
#define CONTEXT_DEBUG_REGISTERS     (CONTEXT_AMD64 | 0x10)
#define CONTEXT_FULL                (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)

/* --- XMM_SAVE_AREA32 (FXSAVE format, 512 bytes) ------------------------- */

typedef struct __attribute__((packed)) _XMM_SAVE_AREA32 {
    uint16_t ControlWord;       /* 0x000 */
    uint16_t StatusWord;        /* 0x002 */
    uint8_t  TagWord;           /* 0x004 */
    uint8_t  _Reserved1;       /* 0x005 */
    uint16_t ErrorOpcode;       /* 0x006 */
    uint32_t ErrorOffset;       /* 0x008 */
    uint16_t ErrorSelector;     /* 0x00C */
    uint16_t _Reserved2;       /* 0x00E */
    uint32_t DataOffset;        /* 0x010 */
    uint16_t DataSelector;      /* 0x014 */
    uint16_t _Reserved3;       /* 0x016 */
    uint32_t MxCsr;             /* 0x018 */
    uint32_t MxCsr_Mask;        /* 0x01C */
    uint8_t  FloatRegisters[128]; /* 0x020: ST0-ST7 (8 x 16) */
    uint8_t  XmmRegisters[256];   /* 0x0A0: XMM0-XMM15 (16 x 16) */
    uint8_t  _Reserved4[96];   /* 0x1A0 */
} XMM_SAVE_AREA32;             /* total: 0x200 = 512 bytes */

/* --- M128A (128-bit value for vector registers) -------------------------- */

typedef struct __attribute__((aligned(16))) _M128A {
    uint64_t Low;
    int64_t  High;
} M128A;

/* --- CONTEXT (Windows x64 layout, 1232 bytes) ----------------------------
 *
 * Matches the Windows 10/11 AMD64 CONTEXT layout byte-for-byte so that
 * MDMP files can be opened in WinDbg. Defined here for crash dump use;
 * TODO-10 (Exception Dispatch) will move this to except.h when SEH lands.
 *
 * struct _CONTEXT is forward-declared in nt_types.h -- this completes it. */

struct _CONTEXT {
    /* Home registers -- offset 0x000 */
    uint64_t P1Home;
    uint64_t P2Home;
    uint64_t P3Home;
    uint64_t P4Home;
    uint64_t P5Home;
    uint64_t P6Home;

    /* Control / MXCSR -- offset 0x030 */
    uint32_t ContextFlags;
    uint32_t MxCsr;

    /* Segment registers -- offset 0x038 */
    uint16_t SegCs;
    uint16_t SegDs;
    uint16_t SegEs;
    uint16_t SegFs;
    uint16_t SegGs;
    uint16_t SegSs;

    /* EFLAGS -- offset 0x044 */
    uint32_t EFlags;

    /* Debug registers -- offset 0x048 */
    uint64_t Dr0;
    uint64_t Dr1;
    uint64_t Dr2;
    uint64_t Dr3;
    uint64_t Dr6;
    uint64_t Dr7;

    /* Integer registers -- offset 0x078 */
    uint64_t Rax;
    uint64_t Rcx;
    uint64_t Rdx;
    uint64_t Rbx;
    uint64_t Rsp;
    uint64_t Rbp;
    uint64_t Rsi;
    uint64_t Rdi;
    uint64_t R8;
    uint64_t R9;
    uint64_t R10;
    uint64_t R11;
    uint64_t R12;
    uint64_t R13;
    uint64_t R14;
    uint64_t R15;

    /* Program counter -- offset 0x0F8 */
    uint64_t Rip;

    /* Floating point state (FXSAVE format) -- offset 0x100 */
    XMM_SAVE_AREA32 FltSave;

    /* YMM high halves + reserved vector slots -- offset 0x300 */
    M128A VectorRegister[26];

    /* Vector control -- offset 0x4A0 */
    uint64_t VectorControl;

    /* Last Branch Record -- offset 0x4A8 */
    uint64_t DebugControl;
    uint64_t LastBranchToRip;
    uint64_t LastBranchFromRip;
    uint64_t LastExceptionToRip;
    uint64_t LastExceptionFromRip;
};

#ifndef _CONTEXT_TYPEDEF_DEFINED
#define _CONTEXT_TYPEDEF_DEFINED
typedef struct _CONTEXT CONTEXT;
#endif

_Static_assert(sizeof(CONTEXT) == 1232, "CONTEXT must be 1232 bytes (Windows x64)");

/* --- XSAVE scratch buffer (exported for tests) --------------------------- */

extern uint8_t g_panic_xsave_buf[4096];

/* --- Crash CONTEXT (exported for dump pipeline) -------------------------- */

extern CONTEXT g_panic_context;

/* Capture FPU/XMM/YMM state into g_panic_xsave_buf.
 * Uses XSAVE if available, FXSAVE fallback otherwise.
 * Must be called as early as possible in the panic path. */
void panic_capture_fpu_state(void);

/* Build a CONTEXT record from interrupt_frame + captured XSAVE state. */
void panic_build_context(struct interrupt_frame *frame, CONTEXT *ctx);

/* Display the styled panic screen and halt.
 * frame: interrupt frame snapshot (NULL if not from an exception)
 * error_code: exception error code or stop code
 * description: human-readable error description
 * file: source file (__FILE__)
 * line: source line (__LINE__) */
void panic_screen(struct interrupt_frame *frame, uint64_t error_code,
                  const char *description, const char *file, uint32_t line);

/* Convenience macro that captures file/line automatically.
 * Routes through KeBugCheckEx for uniform bugcheck handling.
 * Usage: KPANIC("something terrible happened"); */
#define KPANIC(msg) \
    do { \
        extern void KeBugCheckEx(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t); \
        KeBugCheckEx(0xE2, 0, 0, 0, 0); /* MANUALLY_INITIATED_CRASH */ \
    } while (0)

/* Panic with an interrupt frame (called from exception handlers).
 * Still uses panic_screen directly for frame-aware BSOD rendering.
 * Usage: KPANIC_FRAME(frame, "page fault in kernel"); */
#define KPANIC_FRAME(frame, msg) \
    panic_screen((frame), (frame)->err_code, (msg), __FILE__, __LINE__)
