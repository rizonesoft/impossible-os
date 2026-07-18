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
#include "kernel/vectors.h"  /* VECTOR_* -- general fault-to-exception mapping */
#include "kernel/mm/vmm.h"   /* pf_build_access_violation -- #PF triage record builder */

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

/* All seven x87/SSE FLT SEH codes must be exposed and wired to the correct
 * NTSTATUS values (a wrong ntstatus.h value would fail these hex checks; the
 * alias mechanism itself is proven by the access-violation test above). */
static void test_exception_flt_codes(void)
{
    TEST_ASSERT_EQ(EXCEPTION_FLT_DENORMAL_OPERAND, 0xC000008Du, "FLT_DENORMAL_OPERAND == 0xC000008D");
    TEST_ASSERT_EQ(EXCEPTION_FLT_DIVIDE_BY_ZERO, 0xC000008Eu, "FLT_DIVIDE_BY_ZERO == 0xC000008E");
    TEST_ASSERT_EQ(EXCEPTION_FLT_INEXACT_RESULT, 0xC000008Fu, "FLT_INEXACT_RESULT == 0xC000008F");
    TEST_ASSERT_EQ(EXCEPTION_FLT_INVALID_OPERATION, 0xC0000090u, "FLT_INVALID_OPERATION == 0xC0000090");
    TEST_ASSERT_EQ(EXCEPTION_FLT_OVERFLOW, 0xC0000091u, "FLT_OVERFLOW == 0xC0000091");
    TEST_ASSERT_EQ(EXCEPTION_FLT_STACK_CHECK, 0xC0000092u, "FLT_STACK_CHECK == 0xC0000092");
    TEST_ASSERT_EQ(EXCEPTION_FLT_UNDERFLOW, 0xC0000093u, "FLT_UNDERFLOW == 0xC0000093");
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

/* ---- #PF triage: pf_build_access_violation + dispatch stubs ---- */

/* A write fault (err_code W bit set, user bit set) builds an access-violation
 * record: code, 2 parameters, WRITE access type, faulting address in [1], and a
 * CONTROL|INTEGER CONTEXT filled from the frame. */
static void test_pf_build_access_violation_write(void)
{
    struct interrupt_frame f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;

    fill_frame(&f);
    f.err_code = 0x07;  /* P | W | U -- user write, protection violation */

    pf_build_access_violation(&rec, &ctx, &f, 0xDEAD000, f.err_code);

    TEST_ASSERT_EQ((uint32_t)rec.ExceptionCode, 0xC0000005,
                   "ExceptionCode == STATUS_ACCESS_VIOLATION");
    TEST_ASSERT_EQ(rec.ExceptionFlags, (uint32_t)EXCEPTION_CONTINUABLE,
                   "access violation is first-chance continuable");
    TEST_ASSERT_EQ(rec.NumberParameters, 2u, "NumberParameters == 2");
    TEST_ASSERT_EQ(rec.ExceptionInformation[EXCEPTION_INFO_ACCESS_TYPE],
                   (uint64_t)EXCEPTION_ACCESS_WRITE, "write fault -> access type 1");
    TEST_ASSERT_EQ(rec.ExceptionInformation[EXCEPTION_INFO_FAULT_ADDR],
                   0xDEAD000ULL, "faulting address in ExceptionInformation[1]");
    TEST_ASSERT_EQ((uintptr_t)rec.ExceptionAddress, 0x0000000000401000ULL,
                   "ExceptionAddress == faulting RIP");
    TEST_ASSERT(CONTEXT_HAS_GROUP(ctx.ContextFlags, CONTEXT_CONTROL),
                "CONTEXT carries CONTROL group");
    TEST_ASSERT(CONTEXT_HAS_GROUP(ctx.ContextFlags, CONTEXT_INTEGER),
                "CONTEXT carries INTEGER group");
    TEST_ASSERT_EQ(ctx.Rip, 0x0000000000401000ULL, "CONTEXT Rip from frame");
    TEST_ASSERT_EQ(ctx.Rax, 0x0202020202020202ULL, "CONTEXT Rax from frame");
}

/* A read fault decodes to READ; setting the instruction-fetch bit decodes to
 * EXECUTE (winnt.h access-type 8) -- proves the access-type decode covers all
 * three cases and is not hard-wired. */
static void test_pf_build_access_violation_read_and_fetch(void)
{
    struct interrupt_frame f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;

    fill_frame(&f);
    f.err_code = 0x04;  /* U only -- user read, not-present */
    pf_build_access_violation(&rec, &ctx, &f, 0x1000, f.err_code);
    TEST_ASSERT_EQ(rec.ExceptionInformation[EXCEPTION_INFO_ACCESS_TYPE],
                   (uint64_t)EXCEPTION_ACCESS_READ, "read fault -> access type 0");

    fill_frame(&f);
    f.err_code = 0x14;  /* U | I(fetch) -- user instruction fetch */
    pf_build_access_violation(&rec, &ctx, &f, 0x2000, f.err_code);
    TEST_ASSERT_EQ(rec.ExceptionInformation[EXCEPTION_INFO_ACCESS_TYPE],
                   (uint64_t)EXCEPTION_ACCESS_EXECUTE, "fetch fault -> access type 8");
}

/* pf_build_access_violation zeroes the CONTEXT first, so an unsatisfiable group
 * (FLOATING_POINT) is never left as caller garbage even from a reused scratch
 * buffer -- the FPU control word is the architectural init value, not stale. */
static void test_pf_build_access_violation_zeroes_context(void)
{
    struct interrupt_frame f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    uint8_t *p = (uint8_t *)&ctx;
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof(CONTEXT); i++)
        p[i] = 0xEE;  /* poison the scratch before the build */

    fill_frame(&f);
    pf_build_access_violation(&rec, &ctx, &f, 0x3000, f.err_code);

    TEST_ASSERT(!CONTEXT_HAS_GROUP(ctx.ContextFlags, CONTEXT_FLOATING_POINT),
                "FLOATING_POINT group not claimed");
    TEST_ASSERT_EQ(ctx.FltSave.ControlWord, FPU_FCW_INIT,
                   "FPU control word is architectural init, not poison");
    TEST_ASSERT_EQ(ctx.Dr0, 0, "Dr0 zeroed, not poison");
}

/* Until later sections land, the dispatch stubs decline every exception so the
 * #PF handler's terminal path stays correct. */
static void test_pf_dispatch_stubs_return_unhandled(void)
{
    struct interrupt_frame f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;

    fill_frame(&f);
    pf_build_access_violation(&rec, &ctx, &f, 0x4000, f.err_code);

    TEST_ASSERT_EQ(ki_dispatch_exception(&rec, &ctx, &f, UserMode, 1),
                   KI_EXCEPTION_UNHANDLED, "ki_dispatch_exception stub declines");
    TEST_ASSERT_EQ(ki_raise_kernel_exception(&rec, &ctx, &f),
                   KI_EXCEPTION_UNHANDLED, "ki_raise_kernel_exception stub declines");
}

/* A same-CPL (ring-0) frame: in x86-64 LONG MODE the CPU pushes SS:RSP on every
 * exception regardless of CPL (and IRETQ pops all five), so the interrupt_frame
 * carries a valid saved SS:RSP for a kernel fault too. context_from_frame must
 * read them unconditionally -- capturing the actual saved values, never
 * special-casing ring-0. */
static void test_context_from_frame_kernel_cpl(void)
{
    struct interrupt_frame f;
    CONTEXT ctx;
    uint32_t captured;

    fill_frame(&f);
    f.cs  = 0x08;                    /* ring-0 kernel code selector, RPL 0 */
    f.ss  = 0x10;                    /* ring-0 data selector -- the saved SS */
    f.rsp = 0xFFFF800001234000ULL;   /* the CPU-pushed kernel RSP */
    ctx.ContextFlags = CONTEXT_CONTROL;
    captured = context_from_frame(&f, &ctx);

    TEST_ASSERT(CONTEXT_HAS_GROUP(captured, CONTEXT_CONTROL),
                "CONTROL captured for a kernel-CPL frame");
    TEST_ASSERT_EQ(ctx.SegSs, 0x10, "kernel frame SS captured from the saved slot");
    TEST_ASSERT_EQ(ctx.Rsp, 0xFFFF800001234000ULL,
                   "kernel frame Rsp is the saved value, not special-cased");
    TEST_ASSERT_EQ(ctx.Rip, 0x0000000000401000ULL, "kernel frame Rip captured");
}

/* ---- General fault-to-exception mapping (except_vector_to_status,
 *      except_vector_kernel_fatal, except_build_record) ---- */

/* Every general fault vector maps to its Windows NTSTATUS; an unmapped vector
 * (NMI, #BR, #PF -- owned elsewhere) returns STATUS_SUCCESS (0). */
static void test_except_vector_status_map(void)
{
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_DIVIDE_ERROR),
                   (uint32_t)STATUS_INTEGER_DIVIDE_BY_ZERO, "#DE -> INTEGER_DIVIDE_BY_ZERO");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_DEBUG),
                   (uint32_t)STATUS_SINGLE_STEP, "#DB -> SINGLE_STEP");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_BREAKPOINT),
                   (uint32_t)STATUS_BREAKPOINT, "#BP -> BREAKPOINT");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_OVERFLOW),
                   (uint32_t)STATUS_INTEGER_OVERFLOW, "#OF -> INTEGER_OVERFLOW");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_INVALID_OPCODE),
                   (uint32_t)STATUS_ILLEGAL_INSTRUCTION, "#UD -> ILLEGAL_INSTRUCTION");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_SEGMENT_NOT_PRESENT),
                   (uint32_t)STATUS_ACCESS_VIOLATION, "#NP -> ACCESS_VIOLATION");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_STACK_FAULT),
                   (uint32_t)STATUS_STACK_OVERFLOW, "#SS -> STACK_OVERFLOW");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_GENERAL_PROTECTION),
                   (uint32_t)STATUS_ACCESS_VIOLATION, "#GP -> ACCESS_VIOLATION");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_CONTROL_PROTECTION),
                   (uint32_t)STATUS_STACK_BUFFER_OVERRUN, "#CP -> STACK_BUFFER_OVERRUN");
    /* Vectors this mapping does NOT own return 0. */
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_NMI), 0u, "NMI unmapped");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_PAGE_FAULT), 0u, "#PF owned by VMM");
    TEST_ASSERT_EQ((uint32_t)except_vector_to_status(VECTOR_BOUND_RANGE), 0u, "#BR unmapped");
}

/* Only #DE/#OF/#UD are unconditionally terminal in kernel mode; the rest are
 * dispatchable (kernel SEH may one day resolve them). */
static void test_except_vector_kernel_fatal(void)
{
    TEST_ASSERT_EQ(except_vector_kernel_fatal(VECTOR_DIVIDE_ERROR), 1, "#DE kernel-fatal");
    TEST_ASSERT_EQ(except_vector_kernel_fatal(VECTOR_OVERFLOW), 1, "#OF kernel-fatal");
    TEST_ASSERT_EQ(except_vector_kernel_fatal(VECTOR_INVALID_OPCODE), 1, "#UD kernel-fatal");
    TEST_ASSERT_EQ(except_vector_kernel_fatal(VECTOR_DEBUG), 0, "#DB dispatchable");
    TEST_ASSERT_EQ(except_vector_kernel_fatal(VECTOR_BREAKPOINT), 0, "#BP dispatchable");
    TEST_ASSERT_EQ(except_vector_kernel_fatal(VECTOR_GENERAL_PROTECTION), 0, "#GP dispatchable");
    TEST_ASSERT_EQ(except_vector_kernel_fatal(VECTOR_CONTROL_PROTECTION), 0, "#CP not fatal-panic");
    TEST_ASSERT_EQ(except_vector_kernel_fatal(VECTOR_PAGE_FAULT), 0, "unmapped vector not fatal");
}

/* #DE record: correct code, EXCEPTION_CONTINUABLE, address = faulting RIP,
 * no parameters, no chained record. */
static void test_except_build_record_divide_by_zero(void)
{
    struct interrupt_frame f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;

    fill_frame(&f);
    f.int_no = VECTOR_DIVIDE_ERROR;
    f.err_code = 0;
    except_build_record(&rec, &ctx, &f, STATUS_INTEGER_DIVIDE_BY_ZERO, EXCEPTION_CONTINUABLE);

    TEST_ASSERT_EQ((uint32_t)rec.ExceptionCode, (uint32_t)STATUS_INTEGER_DIVIDE_BY_ZERO,
                   "#DE record code");
    TEST_ASSERT(EXCEPTION_IS_CONTINUABLE(rec.ExceptionFlags), "#DE record is continuable");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)rec.ExceptionAddress, 0x0000000000401000ULL,
                   "#DE address is faulting RIP");
    TEST_ASSERT_EQ(rec.NumberParameters, 0u, "#DE record has no parameters");
    TEST_ASSERT(rec.ExceptionRecord == (EXCEPTION_RECORD *)0, "#DE record has no chained record");
    TEST_ASSERT(CONTEXT_HAS_GROUP(ctx.ContextFlags, CONTEXT_CONTROL) &&
                CONTEXT_HAS_GROUP(ctx.ContextFlags, CONTEXT_INTEGER),
                "#DE CONTEXT captures CONTROL + INTEGER");
    TEST_ASSERT_EQ(ctx.Rip, 0x0000000000401000ULL, "#DE CONTEXT Rip captured");
}

/* #BP: the handler rewinds the live RIP by 1 (past the INT3 byte) BEFORE building
 * the record, so the record's ExceptionAddress lands on the INT3 instruction. */
static void test_except_build_record_breakpoint_address_adjust(void)
{
    struct interrupt_frame f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;

    fill_frame(&f);
    f.int_no = VECTOR_BREAKPOINT;
    f.rip = 0x0000000000401234ULL;   /* CPU-pushed RIP -- one past the 0xCC */
    f.rip -= 1;                       /* the adjustment except_common_handler applies */
    except_build_record(&rec, &ctx, &f, except_vector_to_status(VECTOR_BREAKPOINT),
                        EXCEPTION_CONTINUABLE);

    TEST_ASSERT_EQ((uint32_t)rec.ExceptionCode, (uint32_t)STATUS_BREAKPOINT, "#BP record code");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)rec.ExceptionAddress, 0x0000000000401233ULL,
                   "#BP address is the INT3 byte (RIP - 1)");
    TEST_ASSERT_EQ(ctx.Rip, 0x0000000000401233ULL, "#BP CONTEXT Rip is the adjusted RIP");
}

/* #CP / __fastfail: noncontinuable STATUS_STACK_BUFFER_OVERRUN carrying the
 * fast-fail subcode in ExceptionInformation[0]. */
static void test_except_build_record_control_protection(void)
{
    struct interrupt_frame f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;

    fill_frame(&f);
    f.int_no = VECTOR_CONTROL_PROTECTION;
    except_build_record(&rec, &ctx, &f, STATUS_STACK_BUFFER_OVERRUN, EXCEPTION_NONCONTINUABLE);
    /* Handler stamps the subcode after the pure build. */
    rec.NumberParameters = 1;
    rec.ExceptionInformation[0] = FAST_FAIL_CONTROL_INVALID_RETURN_ADDRESS;

    TEST_ASSERT_EQ((uint32_t)rec.ExceptionCode, (uint32_t)STATUS_STACK_BUFFER_OVERRUN,
                   "#CP record code is STACK_BUFFER_OVERRUN");
    TEST_ASSERT(!EXCEPTION_IS_CONTINUABLE(rec.ExceptionFlags), "#CP record is noncontinuable");
    TEST_ASSERT_EQ(rec.NumberParameters, 1u, "#CP record carries one parameter");
    TEST_ASSERT_EQ(rec.ExceptionInformation[0], (uint64_t)FAST_FAIL_CONTROL_INVALID_RETURN_ADDRESS,
                   "#CP subcode is FAST_FAIL_CONTROL_INVALID_RETURN_ADDRESS");
}

/* NULL arguments are ignored without touching memory. */
static void test_except_build_record_null(void)
{
    struct interrupt_frame f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;

    fill_frame(&f);
    rec.ExceptionCode = 0x5A5A5A5A;
    /* No crash and no write when any pointer is NULL. */
    except_build_record(NULL, &ctx, &f, STATUS_BREAKPOINT, EXCEPTION_CONTINUABLE);
    except_build_record(&rec, NULL, &f, STATUS_BREAKPOINT, EXCEPTION_CONTINUABLE);
    except_build_record(&rec, &ctx, NULL, STATUS_BREAKPOINT, EXCEPTION_CONTINUABLE);
    TEST_ASSERT_EQ((uint32_t)rec.ExceptionCode, 0x5A5A5A5Au,
                   "record untouched when frame is NULL");
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
    test_suite_register_cat("Except: FLT exception codes",
                            test_exception_flt_codes, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: EXCEPTION_POINTERS pairing",
                            test_exception_pointers_pairing, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: pf access violation (write)",
                            test_pf_build_access_violation_write, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: pf access violation (read/fetch)",
                            test_pf_build_access_violation_read_and_fetch, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: pf builder zeroes CONTEXT",
                            test_pf_build_access_violation_zeroes_context, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: pf dispatch stubs decline",
                            test_pf_dispatch_stubs_return_unhandled, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: context_from_frame kernel CPL",
                            test_context_from_frame_kernel_cpl, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: fault vector -> NTSTATUS map",
                            test_except_vector_status_map, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: kernel-fatal vector flags",
                            test_except_vector_kernel_fatal, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: build record #DE (continuable)",
                            test_except_build_record_divide_by_zero, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: build record #BP address adjust",
                            test_except_build_record_breakpoint_address_adjust, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: build record #CP fastfail subcode",
                            test_except_build_record_control_protection, TEST_CAT_EXCEPT);
    test_suite_register_cat("Except: build record NULL args",
                            test_except_build_record_null, TEST_CAT_EXCEPT);
}

#endif /* KERNEL_TESTS */
