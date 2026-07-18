/* ============================================================================
 * test_except.c -- Exception dispatch / SEH unit tests
 *
 * Section 1 coverage: the AMD64 exception ABI (CONTEXT, EXCEPTION_RECORD,
 * EXCEPTION_POINTERS) and the frame <-> CONTEXT converters.
 *
 * XREF: 02-kernel-core/TODO-23-exception-dispatch-seh.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/except.h"
#include "kernel/idt.h"

/* ---- ABI layout (Layer 3 of the 5-layer defense; the _Static_asserts in
 *      except.h are Layer 1 -- these prove the same contract at runtime so a
 *      failure is reported rather than only breaking the build) ---- */

static void test_context_size(void)
{
    TEST_ASSERT_EQ(sizeof(CONTEXT), 1232, "sizeof(CONTEXT) == 1232 (Windows x64)");
    TEST_ASSERT_EQ(_Alignof(CONTEXT), 16, "CONTEXT 16-byte aligned (FXSAVE / DECLSPEC_ALIGN(16))");
    TEST_ASSERT_EQ(_Alignof(XMM_SAVE_AREA32), 16, "XMM_SAVE_AREA32 16-byte aligned (FXSAVE)");
}

static void test_context_offsets(void)
{
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, ContextFlags), 0x30, "ContextFlags at 0x30");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, FltSave), 0x100, "legacy XMM_SAVE_AREA32 at 0x100");
    TEST_ASSERT_EQ(__builtin_offsetof(CONTEXT, Rip), 0xF8, "Rip at 0xF8");
    TEST_ASSERT_EQ(sizeof(XMM_SAVE_AREA32), 512, "XMM_SAVE_AREA32 is 512 bytes (FXSAVE)");
}

static void test_exception_record_size(void)
{
    TEST_ASSERT_EQ(sizeof(EXCEPTION_RECORD), 152, "sizeof(EXCEPTION_RECORD) == 152 (Windows x64)");
    TEST_ASSERT_EQ(__builtin_offsetof(EXCEPTION_RECORD, ExceptionInformation), 0x20,
                   "ExceptionInformation at 0x20 (past NumberParameters padding)");
    /* The ExceptionInformation[] array must hold EXCEPTION_MAXIMUM_PARAMETERS
     * u64 slots; assert the derived array size rather than the define's literal
     * (a bare `== 15` on the define is tautological). */
    TEST_ASSERT_EQ(sizeof(((EXCEPTION_RECORD *)0)->ExceptionInformation), 15u * sizeof(uint64_t),
                   "ExceptionInformation holds 15 u64 params");
}

static void test_exception_pointers_size(void)
{
    TEST_ASSERT_EQ(sizeof(EXCEPTION_POINTERS), 16, "sizeof(EXCEPTION_POINTERS) == 16");
}

/* ---- ContextFlags group semantics ---- */

/* Every CONTEXT_* group shares the CONTEXT_AMD64 bit, so a naive
 * `flags & GROUP` test is true for ANY group. Prove CONTEXT_HAS_GROUP does not
 * have that defect -- this is the contract every later section relies on when
 * deciding what to restore. */
static void test_context_group_mask_not_confused_by_amd64_bit(void)
{
    uint32_t only_integer = CONTEXT_INTEGER;

    TEST_ASSERT(CONTEXT_HAS_GROUP(only_integer, CONTEXT_INTEGER),
                "CONTEXT_HAS_GROUP finds the group that is present");
    TEST_ASSERT(!CONTEXT_HAS_GROUP(only_integer, CONTEXT_CONTROL),
                "shared CONTEXT_AMD64 bit does not make CONTROL appear present");
    TEST_ASSERT(!CONTEXT_HAS_GROUP(only_integer, CONTEXT_FLOATING_POINT),
                "shared CONTEXT_AMD64 bit does not make FLOATING_POINT appear present");
    /* The naive test the macro exists to prevent: */
    TEST_ASSERT((only_integer & CONTEXT_CONTROL) != 0,
                "bare AND is truthy for the wrong group -- why CONTEXT_HAS_GROUP is required");
}

/* ---- Frame -> CONTEXT capture ---- */

static void fill_frame(struct interrupt_frame *f)
{
    f->r15 = 0x1515151515151515ULL;
    f->r14 = 0x1414141414141414ULL;
    f->r13 = 0x1313131313131313ULL;
    f->r12 = 0x1212121212121212ULL;
    f->r11 = 0x1111111111111111ULL;
    f->r10 = 0x1010101010101010ULL;
    f->r9  = 0x0909090909090909ULL;
    f->r8  = 0x0808080808080808ULL;
    f->rbp = 0x00000000BEEF0000ULL;
    f->rdi = 0x0707070707070707ULL;
    f->rsi = 0x0606060606060606ULL;
    f->rdx = 0x0505050505050505ULL;
    f->rcx = 0x0404040404040404ULL;
    f->rbx = 0x0303030303030303ULL;
    f->rax = 0x0202020202020202ULL;
    f->int_no   = 14;
    f->err_code = 0x2;
    f->rip    = 0x0000000000401000ULL;
    f->cs     = 0x20 | 3;   /* user code selector, RPL 3 */
    f->rflags = 0x202;      /* IF | reserved bit 1 */
    f->rsp    = 0x00000000C0000000ULL;
    f->ss     = 0x18 | 3;   /* user data selector, RPL 3 */
}

static void test_context_from_frame_integer(void)
{
    struct interrupt_frame f;
    CONTEXT ctx;
    uint32_t captured;

    fill_frame(&f);
    ctx.ContextFlags = CONTEXT_FULL;
    captured = context_from_frame(&f, &ctx);

    TEST_ASSERT(CONTEXT_HAS_GROUP(captured, CONTEXT_INTEGER), "INTEGER group captured");
    TEST_ASSERT_EQ(ctx.Rax, 0x0202020202020202ULL, "Rax captured from frame");
    TEST_ASSERT_EQ(ctx.Rbx, 0x0303030303030303ULL, "Rbx captured from frame");
    TEST_ASSERT_EQ(ctx.Rcx, 0x0404040404040404ULL, "Rcx captured from frame");
    TEST_ASSERT_EQ(ctx.Rdx, 0x0505050505050505ULL, "Rdx captured from frame");
    TEST_ASSERT_EQ(ctx.Rsi, 0x0606060606060606ULL, "Rsi captured from frame");
    TEST_ASSERT_EQ(ctx.Rdi, 0x0707070707070707ULL, "Rdi captured from frame");
    TEST_ASSERT_EQ(ctx.R8,  0x0808080808080808ULL, "R8 captured from frame");
    TEST_ASSERT_EQ(ctx.R15, 0x1515151515151515ULL, "R15 captured from frame");
    TEST_ASSERT_EQ(ctx.Rbp, 0x00000000BEEF0000ULL, "Rbp captured (INTEGER group on AMD64)");
}

static void test_context_from_frame_control(void)
{
    struct interrupt_frame f;
    CONTEXT ctx;
    uint32_t captured;

    fill_frame(&f);
    ctx.ContextFlags = CONTEXT_FULL;
    captured = context_from_frame(&f, &ctx);

    TEST_ASSERT(CONTEXT_HAS_GROUP(captured, CONTEXT_CONTROL), "CONTROL group captured");
    TEST_ASSERT_EQ(ctx.Rip, 0x0000000000401000ULL, "Rip captured from frame");
    TEST_ASSERT_EQ(ctx.Rsp, 0x00000000C0000000ULL, "Rsp captured from frame");
    TEST_ASSERT_EQ(ctx.EFlags, 0x202, "EFlags captured from frame");
    TEST_ASSERT_EQ(ctx.SegCs, 0x23, "SegCs captured from frame");
    TEST_ASSERT_EQ(ctx.SegSs, 0x1B, "SegSs captured from frame");
}

/* A frame carries no FPU state. Requesting FLOATING_POINT must NOT set the bit
 * (fabricated state must never be advertised as valid) and must leave the save
 * area at architectural init, because all-zero FCW/MXCSR unmasks every FP
 * exception on a later restore. */
static void test_context_from_frame_clears_unsupported_groups(void)
{
    struct interrupt_frame f;
    CONTEXT ctx;
    uint32_t captured;

    fill_frame(&f);
    ctx.ContextFlags = CONTEXT_ALL;
    captured = context_from_frame(&f, &ctx);

    TEST_ASSERT(!CONTEXT_HAS_GROUP(captured, CONTEXT_FLOATING_POINT),
                "FLOATING_POINT cleared -- a frame carries no FPU state");
    TEST_ASSERT(!CONTEXT_HAS_GROUP(captured, CONTEXT_SEGMENTS),
                "SEGMENTS cleared -- a frame carries no DS/ES/FS/GS");
    TEST_ASSERT(!CONTEXT_HAS_GROUP(captured, CONTEXT_DEBUG_REGISTERS),
                "DEBUG_REGISTERS cleared -- a frame carries no DR0-DR7");
    TEST_ASSERT_EQ(captured, ctx.ContextFlags,
                   "returned mask equals the resulting ContextFlags");
    TEST_ASSERT_EQ(ctx.FltSave.ControlWord, FPU_FCW_INIT,
                   "FltSave FCW at architectural init (0x037F), not zero");
    TEST_ASSERT_EQ(ctx.FltSave.MxCsr, FPU_MXCSR_INIT,
                   "FltSave MXCSR at architectural init (0x1F80), not zero");
    TEST_ASSERT_EQ(ctx.Dr0, 0, "Dr0 zeroed, not fabricated from the live cpu");
    TEST_ASSERT_EQ(ctx.SegDs, 0, "SegDs zeroed, not fabricated from the live cpu");
}

/* Requesting CONTROL alone must not leak INTEGER values into the CONTEXT. */
static void test_context_from_frame_honours_partial_request(void)
{
    struct interrupt_frame f;
    CONTEXT ctx;
    uint32_t captured;

    fill_frame(&f);
    ctx.ContextFlags = CONTEXT_CONTROL;
    captured = context_from_frame(&f, &ctx);

    TEST_ASSERT(CONTEXT_HAS_GROUP(captured, CONTEXT_CONTROL), "CONTROL captured as requested");
    TEST_ASSERT(!CONTEXT_HAS_GROUP(captured, CONTEXT_INTEGER), "INTEGER not captured (not requested)");
    TEST_ASSERT_EQ(ctx.Rip, 0x0000000000401000ULL, "Rip captured under CONTROL");
    TEST_ASSERT_EQ(ctx.Rax, 0, "Rax left zero when INTEGER not requested");
    TEST_ASSERT_EQ(ctx.R15, 0, "R15 left zero when INTEGER not requested");
}

static void test_context_from_frame_null(void)
{
    struct interrupt_frame f;
    CONTEXT ctx;

    fill_frame(&f);
    ctx.ContextFlags = CONTEXT_FULL;

    TEST_ASSERT_EQ(context_from_frame(NULL, &ctx), 0, "NULL frame returns 0");
    TEST_ASSERT_EQ(context_from_frame(&f, NULL), 0, "NULL ctx returns 0");
    TEST_ASSERT_EQ(ctx.ContextFlags, CONTEXT_FULL, "NULL frame leaves ctx untouched");
}

/* ---- CONTEXT -> frame restore ---- */

static void test_frame_from_context_round_trip(void)
{
    struct interrupt_frame f, out;
    CONTEXT ctx;
    uint32_t restored;

    fill_frame(&f);
    ctx.ContextFlags = CONTEXT_FULL;
    context_from_frame(&f, &ctx);

    /* Distinct starting state so a missed field is visible, and so the
     * preserved-metadata claim is actually tested. */
    for (uint32_t i = 0; i < sizeof(out); i++)
        ((uint8_t *)&out)[i] = 0;
    out.int_no   = 0xAB;
    out.err_code = 0xCD;

    restored = frame_from_context(&ctx, &out);

    TEST_ASSERT(CONTEXT_HAS_GROUP(restored, CONTEXT_CONTROL), "CONTROL restored");
    TEST_ASSERT(CONTEXT_HAS_GROUP(restored, CONTEXT_INTEGER), "INTEGER restored");
    TEST_ASSERT_EQ(out.rip, f.rip, "round-trip preserves RIP");
    TEST_ASSERT_EQ(out.rsp, f.rsp, "round-trip preserves RSP");
    TEST_ASSERT_EQ(out.rflags, f.rflags, "round-trip preserves RFLAGS");
    TEST_ASSERT_EQ(out.cs, f.cs, "round-trip preserves CS");
    TEST_ASSERT_EQ(out.ss, f.ss, "round-trip preserves SS");
    TEST_ASSERT_EQ(out.rax, f.rax, "round-trip preserves RAX");
    TEST_ASSERT_EQ(out.rbx, f.rbx, "round-trip preserves RBX");
    TEST_ASSERT_EQ(out.rbp, f.rbp, "round-trip preserves RBP");
    TEST_ASSERT_EQ(out.r15, f.r15, "round-trip preserves R15");
    TEST_ASSERT_EQ(out.int_no, 0xAB, "int_no preserved -- no CONTEXT counterpart");
    TEST_ASSERT_EQ(out.err_code, 0xCD, "err_code preserved -- no CONTEXT counterpart");
}

/* A CONTEXT whose FLOATING_POINT bit is clear must leave the frame's other
 * groups alone -- the mask gates restoration, never a blind copy. */
static void test_frame_from_context_honours_flags(void)
{
    struct interrupt_frame out;
    CONTEXT ctx;
    uint32_t restored;

    for (uint32_t i = 0; i < sizeof(out); i++)
        ((uint8_t *)&out)[i] = 0;
    for (uint32_t i = 0; i < sizeof(ctx); i++)
        ((uint8_t *)&ctx)[i] = 0;

    ctx.Rip = 0xDEAD0000ULL;
    ctx.Rax = 0xFEED0000ULL;
    ctx.ContextFlags = CONTEXT_CONTROL;   /* INTEGER deliberately absent */

    restored = frame_from_context(&ctx, &out);

    TEST_ASSERT(CONTEXT_HAS_GROUP(restored, CONTEXT_CONTROL), "CONTROL restored as flagged");
    TEST_ASSERT(!CONTEXT_HAS_GROUP(restored, CONTEXT_INTEGER), "INTEGER not restored (bit clear)");
    TEST_ASSERT_EQ(out.rip, 0xDEAD0000ULL, "flagged CONTROL field restored");
    TEST_ASSERT_EQ(out.rax, 0, "unflagged INTEGER field left untouched");
}

static void test_frame_from_context_null(void)
{
    struct interrupt_frame f;
    CONTEXT ctx;

    fill_frame(&f);
    ctx.ContextFlags = CONTEXT_FULL;

    TEST_ASSERT_EQ(frame_from_context(NULL, &f), 0, "NULL ctx returns 0");
    TEST_ASSERT_EQ(frame_from_context(&ctx, NULL), 0, "NULL frame returns 0");
}

/* ---- EXCEPTION_RECORD construction ---- */

static void test_exception_record_access_violation(void)
{
    EXCEPTION_RECORD rec;

    for (uint32_t i = 0; i < sizeof(rec); i++)
        ((uint8_t *)&rec)[i] = 0;

    rec.ExceptionCode    = STATUS_ACCESS_VIOLATION;
    rec.ExceptionFlags   = EXCEPTION_CONTINUABLE;
    rec.ExceptionAddress = (void *)0x401000ULL;
    rec.NumberParameters = 2;
    rec.ExceptionInformation[EXCEPTION_INFO_ACCESS_TYPE] = EXCEPTION_ACCESS_WRITE;
    rec.ExceptionInformation[EXCEPTION_INFO_FAULT_ADDR]  = 0xCAFE0000ULL;

    TEST_ASSERT_EQ((uint32_t)rec.ExceptionCode, EXCEPTION_ACCESS_VIOLATION,
                   "EXCEPTION_ACCESS_VIOLATION aliases STATUS_ACCESS_VIOLATION");
    TEST_ASSERT_EQ((uint32_t)rec.ExceptionCode, 0xC0000005,
                   "STATUS_ACCESS_VIOLATION == 0xC0000005");
    TEST_ASSERT_EQ(rec.ExceptionInformation[EXCEPTION_INFO_FAULT_ADDR], 0xCAFE0000ULL,
                   "fault address in ExceptionInformation[1] per the winnt.h contract");
    TEST_ASSERT(EXCEPTION_IS_CONTINUABLE(rec.ExceptionFlags),
                "access violation is continuable");
}

static void test_exception_record_divide_by_zero(void)
{
    EXCEPTION_RECORD rec;

    for (uint32_t i = 0; i < sizeof(rec); i++)
        ((uint8_t *)&rec)[i] = 0;

    rec.ExceptionCode  = STATUS_INTEGER_DIVIDE_BY_ZERO;
    rec.ExceptionFlags = EXCEPTION_CONTINUABLE;

    TEST_ASSERT_EQ((uint32_t)rec.ExceptionCode, 0xC0000094,
                   "STATUS_INTEGER_DIVIDE_BY_ZERO == 0xC0000094");
    TEST_ASSERT(EXCEPTION_IS_CONTINUABLE(rec.ExceptionFlags),
                "divide-by-zero carries EXCEPTION_CONTINUABLE");

    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    TEST_ASSERT(!EXCEPTION_IS_CONTINUABLE(rec.ExceptionFlags),
                "EXCEPTION_NONCONTINUABLE is detected (CONTINUABLE is absence, value 0)");
}

static void test_exception_pointers_pairing(void)
{
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    EXCEPTION_POINTERS ptrs;

    rec.ExceptionCode = STATUS_ILLEGAL_INSTRUCTION;
    ctx.ContextFlags  = CONTEXT_FULL;
    ptrs.ExceptionRecord = &rec;
    ptrs.ContextRecord   = &ctx;

    TEST_ASSERT_EQ((uint32_t)ptrs.ExceptionRecord->ExceptionCode, 0xC000001D,
                   "STATUS_ILLEGAL_INSTRUCTION == 0xC000001D via EXCEPTION_POINTERS");
    TEST_ASSERT_EQ(ptrs.ContextRecord->ContextFlags, (uint32_t)CONTEXT_FULL,
                   "EXCEPTION_POINTERS pairs the CONTEXT record");
}

/* ---- Registration ---- */

void test_register_except(void)
{
    test_suite_register_cat("Except: CONTEXT size",
                            test_context_size, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: CONTEXT field offsets",
                            test_context_offsets, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: EXCEPTION_RECORD layout",
                            test_exception_record_size, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: EXCEPTION_POINTERS size",
                            test_exception_pointers_size, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: ContextFlags group mask",
                            test_context_group_mask_not_confused_by_amd64_bit, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: context_from_frame INTEGER",
                            test_context_from_frame_integer, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: context_from_frame CONTROL",
                            test_context_from_frame_control, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: unsupported groups cleared",
                            test_context_from_frame_clears_unsupported_groups, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: partial group request",
                            test_context_from_frame_honours_partial_request, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: context_from_frame NULL",
                            test_context_from_frame_null, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: frame/CONTEXT round trip",
                            test_frame_from_context_round_trip, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: frame_from_context honours flags",
                            test_frame_from_context_honours_flags, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: frame_from_context NULL",
                            test_frame_from_context_null, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: EXCEPTION_RECORD access violation",
                            test_exception_record_access_violation, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: EXCEPTION_RECORD divide by zero",
                            test_exception_record_divide_by_zero, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: EXCEPTION_POINTERS pairing",
                            test_exception_pointers_pairing, TEST_CAT_EXCEPT);
}

#endif /* KERNEL_TESTS */
