/* ============================================================================
 * except.h -- Windows AMD64 exception ABI (EXCEPTION_RECORD, CONTEXT,
 *             EXCEPTION_POINTERS)
 *
 * Canonical owner of the exception-dispatch ABI. A real ntdll must be able to
 * consume these types unpatched, so every load-bearing offset is pinned with a
 * _Static_assert below (Layer 1 of the 5-layer defense).
 *
 * CONTEXT, XMM_SAVE_AREA32 and M128A previously lived in panic.h for crash-dump
 * use; this header is now their home and panic.h includes it. The layout is
 * unchanged, so MDMP files still open in WinDbg.
 *
 * ARCH: x86-64 -- the CONTEXT register file is the AMD64 ABI by definition.
 *
 * Reference: Windows SDK winnt.h (AMD64), Windows Internals 7e ch. 8.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

/* Full definition in kernel/idt.h. Forward-declared so this header stays free
 * of an arch include; except.c includes idt.h for the field access. */
struct interrupt_frame;

/* --- CONTEXT flags (Windows x64) -----------------------------------------
 *
 * Every group carries the CONTEXT_AMD64 architecture bit. A naive
 * `flags & CONTEXT_CONTROL` test is therefore TRUE for any AMD64 group and
 * silently defeats the mask -- use CONTEXT_HAS_GROUP(), never a bare AND. */

#define CONTEXT_AMD64               0x00100000
#define CONTEXT_CONTROL             (CONTEXT_AMD64 | 0x01)
#define CONTEXT_INTEGER             (CONTEXT_AMD64 | 0x02)
#define CONTEXT_SEGMENTS            (CONTEXT_AMD64 | 0x04)
#define CONTEXT_FLOATING_POINT      (CONTEXT_AMD64 | 0x08)
#define CONTEXT_DEBUG_REGISTERS     (CONTEXT_AMD64 | 0x10)
#define CONTEXT_FULL                (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)
#define CONTEXT_ALL                 (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS | \
                                     CONTEXT_FLOATING_POINT | CONTEXT_DEBUG_REGISTERS)

/* Groups an interrupt_frame physically carries (see converter contract below). */
#define CONTEXT_FRAME_BACKED        (CONTEXT_CONTROL | CONTEXT_INTEGER)

/* Exact-group test: every bit of `group` must be present in `flags`.
 * Required because all groups share CONTEXT_AMD64 (see comment above). */
#define CONTEXT_HAS_GROUP(flags, group) \
    (((uint32_t)(flags) & (uint32_t)(group)) == (uint32_t)(group))

/* Architectural initial FPU state (Intel SDM Vol.1 "Initialization").
 * A zeroed FCW/MXCSR unmasks every FP exception, so a CONTEXT whose FPU group
 * was never captured is filled with these rather than left as raw zeroes. */
#define FPU_FCW_INIT                0x037F
#define FPU_MXCSR_INIT              0x1F80

/* --- XMM_SAVE_AREA32 (FXSAVE format, 512 bytes) -------------------------
 *
 * `aligned(16)` matches the Windows XSAVE_FORMAT ABI (winnt.h) and FXSAVE's
 * hardware requirement that the 512-byte save area be 16-byte aligned. `packed`
 * keeps every field at its exact byte offset; the two compose (offsets fixed,
 * minimum alignment raised to 16). */

typedef struct __attribute__((packed, aligned(16))) _XMM_SAVE_AREA32 {
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
 * Matches the Windows 10/11 AMD64 CONTEXT layout byte-for-byte so that a real
 * ntdll consumes it unpatched and MDMP files open in WinDbg.
 *
 * struct _CONTEXT is forward-declared in nt_types.h -- this completes it.
 * aligned(16) mirrors Windows DECLSPEC_ALIGN(16): the embedded FltSave area
 * must be 16-byte aligned for FXSAVE/FXRSTOR, which only holds if CONTEXT
 * itself is 16-aligned (sizeof 1232 is a multiple of 16, so no tail padding
 * is added). */

struct __attribute__((aligned(16))) _CONTEXT {
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

/* Layer 1 -- pin the AMD64 CONTEXT ABI. A real ntdll indexes these offsets. */
_Static_assert(sizeof(CONTEXT) == 1232, "CONTEXT must be 1232 bytes (Windows x64)");
_Static_assert(sizeof(XMM_SAVE_AREA32) == 512, "XMM_SAVE_AREA32 must be 512 bytes (FXSAVE)");
_Static_assert(_Alignof(CONTEXT) == 16, "CONTEXT must be 16-byte aligned (Windows DECLSPEC_ALIGN(16))");
_Static_assert(_Alignof(XMM_SAVE_AREA32) == 16, "XMM_SAVE_AREA32 must be 16-byte aligned (FXSAVE)");
/* Pin the FXSAVE-format field offsets. `packed` + a size assert alone would let
 * a field reorder relocate MxCsr/ControlWord while sizeof stays 512 --
 * context_init_fpu_state() writes those two by name and panic.c reads MXCSR at
 * the hardcoded FXSAVE byte offset 0x18, so a silent desync would break both. */
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, ControlWord)    == 0x00, "FXSAVE ControlWord at 0x00");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, StatusWord)     == 0x02, "FXSAVE StatusWord at 0x02");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, MxCsr)          == 0x18, "FXSAVE MxCsr at 0x18");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, MxCsr_Mask)     == 0x1C, "FXSAVE MxCsr_Mask at 0x1C");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, FloatRegisters) == 0x20, "FXSAVE FloatRegisters at 0x20");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, XmmRegisters)   == 0xA0, "FXSAVE XmmRegisters at 0xA0");
_Static_assert(__builtin_offsetof(CONTEXT, ContextFlags) == 0x30, "ContextFlags at 0x30");
_Static_assert(__builtin_offsetof(CONTEXT, MxCsr)   == 0x34, "MxCsr at 0x34");
_Static_assert(__builtin_offsetof(CONTEXT, SegCs)   == 0x38, "SegCs at 0x38");
_Static_assert(__builtin_offsetof(CONTEXT, SegSs)   == 0x42, "SegSs at 0x42");
_Static_assert(__builtin_offsetof(CONTEXT, EFlags)  == 0x44, "EFlags at 0x44");
_Static_assert(__builtin_offsetof(CONTEXT, Dr0)     == 0x48, "Dr0 at 0x48");
_Static_assert(__builtin_offsetof(CONTEXT, Rax)     == 0x78, "Rax at 0x78");
_Static_assert(__builtin_offsetof(CONTEXT, Rsp)     == 0x98, "Rsp at 0x98");
_Static_assert(__builtin_offsetof(CONTEXT, R15)     == 0xF0, "R15 at 0xF0");
_Static_assert(__builtin_offsetof(CONTEXT, Rip)     == 0xF8, "Rip at 0xF8");
_Static_assert(__builtin_offsetof(CONTEXT, FltSave) == 0x100, "legacy XMM_SAVE_AREA32 at 0x100");
_Static_assert(__builtin_offsetof(CONTEXT, VectorRegister) == 0x300, "VectorRegister at 0x300");
_Static_assert(__builtin_offsetof(CONTEXT, VectorControl)  == 0x4A0, "VectorControl at 0x4A0");

/* --- EXCEPTION_RECORD ----------------------------------------------------- */

#define EXCEPTION_MAXIMUM_PARAMETERS 15

/* ExceptionFlags bits. EXCEPTION_CONTINUABLE is the ABSENCE of
 * EXCEPTION_NONCONTINUABLE (value 0), matching winnt.h -- test it with
 * EXCEPTION_IS_CONTINUABLE(), never a bitwise AND against zero. */
#define EXCEPTION_CONTINUABLE       0x00000000
#define EXCEPTION_NONCONTINUABLE    0x00000001
#define EXCEPTION_UNWINDING         0x00000002
#define EXCEPTION_EXIT_UNWIND       0x00000004
#define EXCEPTION_STACK_INVALID     0x00000008
#define EXCEPTION_NESTED_CALL       0x00000010
#define EXCEPTION_TARGET_UNWIND     0x00000020
#define EXCEPTION_COLLIDED_UNWIND   0x00000040

#define EXCEPTION_IS_CONTINUABLE(flags) \
    (((uint32_t)(flags) & (uint32_t)EXCEPTION_NONCONTINUABLE) == 0)

/* Exception codes. In the Windows ABI these are aliases of the matching
 * NTSTATUS values -- canonical definitions live in kernel/nt/ntstatus.h. */
#define EXCEPTION_ACCESS_VIOLATION      ((uint32_t)STATUS_ACCESS_VIOLATION)
#define EXCEPTION_ILLEGAL_INSTRUCTION   ((uint32_t)STATUS_ILLEGAL_INSTRUCTION)
#define EXCEPTION_INT_DIVIDE_BY_ZERO    ((uint32_t)STATUS_INTEGER_DIVIDE_BY_ZERO)
#define EXCEPTION_INT_OVERFLOW          ((uint32_t)STATUS_INTEGER_OVERFLOW)
#define EXCEPTION_BREAKPOINT            ((uint32_t)STATUS_BREAKPOINT)
#define EXCEPTION_SINGLE_STEP           ((uint32_t)STATUS_SINGLE_STEP)
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED ((uint32_t)STATUS_ARRAY_BOUNDS_EXCEEDED)
#define EXCEPTION_DATATYPE_MISALIGNMENT ((uint32_t)STATUS_DATATYPE_MISALIGNMENT)
#define EXCEPTION_PRIV_INSTRUCTION      ((uint32_t)STATUS_PRIVILEGED_INSTRUCTION)
#define EXCEPTION_STACK_OVERFLOW        ((uint32_t)STATUS_STACK_OVERFLOW)
#define EXCEPTION_IN_PAGE_ERROR         ((uint32_t)STATUS_IN_PAGE_ERROR)
#define EXCEPTION_GUARD_PAGE            ((uint32_t)STATUS_GUARD_PAGE_VIOLATION)
#define EXCEPTION_INVALID_DISPOSITION   ((uint32_t)STATUS_INVALID_DISPOSITION)
#define EXCEPTION_FLT_DENORMAL_OPERAND  ((uint32_t)STATUS_FLOAT_DENORMAL_OPERAND)
#define EXCEPTION_FLT_DIVIDE_BY_ZERO    ((uint32_t)STATUS_FLOAT_DIVIDE_BY_ZERO)
#define EXCEPTION_FLT_INEXACT_RESULT    ((uint32_t)STATUS_FLOAT_INEXACT_RESULT)
#define EXCEPTION_FLT_INVALID_OPERATION ((uint32_t)STATUS_FLOAT_INVALID_OPERATION)
#define EXCEPTION_FLT_OVERFLOW          ((uint32_t)STATUS_FLOAT_OVERFLOW)
#define EXCEPTION_FLT_UNDERFLOW         ((uint32_t)STATUS_FLOAT_UNDERFLOW)
#define EXCEPTION_FLT_STACK_CHECK       ((uint32_t)STATUS_FLOAT_STACK_CHECK)
/* CET #CP (STATUS_CONTROL_STACK_VIOLATION) is defined by the fault-to-exception
 * mapping that owns the vector-21 (#CP) handler in the exception-dispatch/SEH
 * work. CET is not probed yet (CPU-hardening / shadow-stack work), so pinning
 * the code here would ship an unverified ABI constant. */

/* ExceptionInformation[] indices for STATUS_ACCESS_VIOLATION / IN_PAGE_ERROR,
 * per the winnt.h contract: [0] = access type, [1] = faulting address. */
#define EXCEPTION_INFO_ACCESS_TYPE  0
#define EXCEPTION_INFO_FAULT_ADDR   1

/* Access-type values stored in ExceptionInformation[0]. */
#define EXCEPTION_ACCESS_READ       0
#define EXCEPTION_ACCESS_WRITE      1
#define EXCEPTION_ACCESS_EXECUTE    8

typedef struct _EXCEPTION_RECORD {
    NTSTATUS  ExceptionCode;                   /* 0x00 */
    uint32_t  ExceptionFlags;                  /* 0x04 */
    struct _EXCEPTION_RECORD *ExceptionRecord; /* 0x08: nested/chained record */
    void     *ExceptionAddress;                /* 0x10 */
    uint32_t  NumberParameters;                /* 0x18 */
    /* 4 bytes implicit padding at 0x1C -- ExceptionInformation needs 8-align */
    uint64_t  ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS]; /* 0x20 */
} EXCEPTION_RECORD;                            /* total: 0x98 = 152 bytes */

/* Layer 1 -- pin the EXCEPTION_RECORD ABI. Size alone would not catch a field
 * reorder across the implicit padding, so every offset is asserted. */
_Static_assert(sizeof(EXCEPTION_RECORD) == 152, "EXCEPTION_RECORD must be 152 bytes (Windows x64)");
_Static_assert(_Alignof(EXCEPTION_RECORD) == 8, "EXCEPTION_RECORD must be 8-byte aligned");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionCode)    == 0x00, "ExceptionCode at 0x00");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionFlags)   == 0x04, "ExceptionFlags at 0x04");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionRecord)  == 0x08, "ExceptionRecord at 0x08");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionAddress) == 0x10, "ExceptionAddress at 0x10");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, NumberParameters) == 0x18, "NumberParameters at 0x18");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionInformation) == 0x20,
               "ExceptionInformation at 0x20");

/* --- EXCEPTION_POINTERS --------------------------------------------------- */

typedef struct _EXCEPTION_POINTERS {
    EXCEPTION_RECORD *ExceptionRecord;  /* 0x00 */
    CONTEXT          *ContextRecord;    /* 0x08 */
} EXCEPTION_POINTERS;                   /* total: 16 bytes */

_Static_assert(sizeof(EXCEPTION_POINTERS) == 16, "EXCEPTION_POINTERS must be 16 bytes (Windows x64)");
_Static_assert(__builtin_offsetof(EXCEPTION_POINTERS, ExceptionRecord) == 0x00, "ExceptionRecord at 0x00");
_Static_assert(__builtin_offsetof(EXCEPTION_POINTERS, ContextRecord)   == 0x08, "ContextRecord at 0x08");

/* --- Frame <-> CONTEXT conversion ----------------------------------------
 *
 * An interrupt_frame physically carries only the CONTROL and INTEGER register
 * groups. It has no DS/ES/FS/GS, no debug registers and no FPU state, so those
 * groups can never be honoured from a frame alone:
 *
 *   - Reading them live would capture the CURRENT cpu, not the frame's thread.
 *   - Executing FXSAVE would, under the lazy-FPU scheme (sched/task.c
 *     nm_handler), save whichever task last touched the FPU -- a cross-task
 *     data leak.
 *
 * Both converters therefore serve the frame-backed groups only, and report what
 * they actually did instead of echoing the caller's requested mask.
 * Per-thread FPU/segment/debug capture belongs to NtGetContextThread
 * (the process/thread native-API work). The panic path keeps its own live-capture
 * route (panic_build_context), which is sound because a panic has stopped the
 * world and deliberately wants the current cpu state. */

/* Populate `ctx` from `frame`. `ctx->ContextFlags` is the REQUESTED mask on
 * entry. Only CONTEXT_CONTROL and CONTEXT_INTEGER can be satisfied; any other
 * requested group is zeroed (FltSave to architectural init state) and its bit
 * is CLEARED from ctx->ContextFlags, so a caller can never mistake fabricated
 * values for captured ones.
 * Returns the mask actually captured (equal to the resulting ctx->ContextFlags).
 * Returns 0 and touches nothing if either pointer is NULL. */
uint32_t context_from_frame(const struct interrupt_frame *frame, CONTEXT *ctx);

/* Restore `frame` from `ctx`, honouring ctx->ContextFlags -- never blindly.
 * Only CONTEXT_CONTROL and CONTEXT_INTEGER are restorable into a frame; other
 * groups are ignored. Returns the mask actually restored, or 0 on NULL.
 *
 * TRUSTED INPUT ONLY. This copier performs NO validation: it will place any
 * RIP/RSP/CS/SS/RFLAGS it is given into an IRETQ frame. It is safe only for
 * ring-0 callers supplying a kernel-produced CONTEXT. A ring-3-supplied CONTEXT
 * MUST go through the checked path -- previous mode, canonical RIP/RSP,
 * user-mode CS/SS selectors with RPL 3, and sanitised RFLAGS (IOPL/NT/VM
 * cleared, reserved bit 1 forced) -- which is owned by NtContinue (the ring-3
 * exception-delivery path). Do not call this directly from a syscall path. */
uint32_t frame_from_context(const CONTEXT *ctx, struct interrupt_frame *frame);

/* Fill `ctx->FltSave` with the architectural initial FPU state (FCW=0x037F,
 * MXCSR=0x1F80). Used for a CONTEXT whose FPU group was never captured: raw
 * zeroes would unmask every FP exception on a restore. */
void context_init_fpu_state(CONTEXT *ctx);

/* --- Exception dispatch ABI --------------------------------------------------
 *
 * Declared by the #PF triage; implemented by the exception-dispatcher, ring-3
 * delivery, and kernel-SEH stages that follow. KPROCESSOR_MODE is the NT
 * previous-mode enum; the canonical definition moves to a shared NT types
 * header when the native-API surface matures. Keep the values pinned (0/1) --
 * task.previous_mode already uses this encoding (0 = KernelMode, 1 = UserMode). */
typedef enum { KernelMode = 0, UserMode = 1 } KPROCESSOR_MODE;

/* Dispatch disposition. The #PF triage and every later dispatch stage agree on
 * this contract so the seam is stable across the dispatcher / ring-3 delivery /
 * kernel-SEH stages:
 *   UNHANDLED -- nobody resolved the exception; the CALLER performs the
 *                terminal action (deliver to ring-3 later, or panic now).
 *   HANDLED   -- resolved; the live `frame` MUST carry the final resume/delivery
 *                state before returning (the caller returns it through IRET and
 *                never re-applies `ctx`). A dispatcher that works in `ctx` must
 *                write it back via frame_from_context first -- returning HANDLED
 *                with only `ctx` updated would IRET the unchanged frame and
 *                refault in a loop.
 * The triage stub returns UNHANDLED, so today every fault the pager chain
 * declined terminates -- exactly the pre-triage behavior, but now routed
 * through the durable dispatch contract instead of an inline panic. */
typedef enum {
    KI_EXCEPTION_UNHANDLED = 0,
    KI_EXCEPTION_HANDLED   = 1,
} KI_EXCEPTION_DISPOSITION;

/* Master user-mode exception dispatcher. The full sequence (debugger
 * first-chance -> ring-3 handover via KiUserExceptionDispatcher -> second-chance
 * -> terminate) is owned by the dispatcher / ring-3 delivery stages; the #PF
 * triage ships a stub that returns UNHANDLED. `frame` is the live interrupt
 * frame: a HANDLED return means it was rewritten to the resume/delivery point.
 * `first_chance` is 1 on the initial raise. */
KI_EXCEPTION_DISPOSITION ki_dispatch_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                                               struct interrupt_frame *frame,
                                               KPROCESSOR_MODE mode, int first_chance);

/* Kernel-mode exception raise -- walks the per-thread kernel exception chain and
 * runs SEH filters. Owned by the kernel-mode __try/__except stage; the #PF
 * triage ships a stub that returns UNHANDLED so an unresolved kernel fault stays
 * terminal (panic) as it is today. */
KI_EXCEPTION_DISPOSITION ki_raise_kernel_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                                                   struct interrupt_frame *frame);
