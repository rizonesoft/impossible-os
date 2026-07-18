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

/* Local byte-wise zero: the kernel has no memset declaration in a header, and
 * panic.c zeroes CONTEXT the same way. Keeps this file dependency-free so it
 * is safe to call from fault context. */
static void except_zero(void *dst, uint32_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++)
        p[i] = 0;
}

void context_init_fpu_state(CONTEXT *ctx)
{
    if (!ctx)
        return;

    except_zero(&ctx->FltSave, (uint32_t)sizeof(XMM_SAVE_AREA32));

    /* Architectural reset state. All-zero would unmask every FP exception and
     * fault the moment the state was restored and used. */
    ctx->FltSave.ControlWord = FPU_FCW_INIT;
    ctx->FltSave.MxCsr       = FPU_MXCSR_INIT;
    ctx->MxCsr               = FPU_MXCSR_INIT;
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
     * caller that ignores the cleared flag cannot unmask every FP exception. */
    context_init_fpu_state(ctx);

    if (CONTEXT_HAS_GROUP(requested, CONTEXT_CONTROL)) {
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
