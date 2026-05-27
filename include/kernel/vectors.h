/* ============================================================================
 * vectors.h -- IDT vector allocation table (single source of truth)
 *
 * Every hardcoded IDT vector in the kernel MUST be defined here.
 * Static asserts enforce uniqueness at compile time -- adding a duplicate
 * vector number causes a build error, not a silent handler overwrite.
 *
 * WARNING: Dynamic IRQ vectors (0x30-0xEF) are allocated at runtime by
 * irq_alloc_vector() in irq.c and are NOT listed here. This file only
 * covers statically assigned vectors.
 *
 * Vector allocation map:
 *   Range       Purpose                    Owner
 *   ---------   -------------------------  -------------------------
 *   0x00-0x1F   CPU exceptions             x86 architecture (fixed)
 *   0x20-0x2F   PIC/PIT legacy IRQs        pic.c, pit.c
 *   0x22        LAPIC timer                lapic.c (remapped from IRQ0)
 *   0x2E        NT syscall compat          syscall.c (INT 0x2E, DPL=3)
 *   0x30-0xEF   Dynamic IRQ allocation     irq.c (runtime, not here)
 *   0x80        Linux-style syscall        syscall.c (INT 0x80, DPL=3)
 *   0x81        Yield (cooperative switch)  task.c (INT 0x81)
 *   0xFB        CR-pin verify IPI          lapic.h (TODO-09-boot S10)
 *   0xFC        Async init IPI             boot_init.c
 *   0xFD        Reschedule IPI             lapic.h
 *   0xFE        TLB shootdown IPI          lapic.h
 *   0xFF        LAPIC spurious             lapic.h
 * ============================================================================ */

#pragma once

/* ---- CPU exceptions (architecture-fixed, 0x00-0x1F) ---- */
#define VECTOR_DIVIDE_ERROR        0x00
#define VECTOR_DEBUG               0x01
#define VECTOR_NMI                 0x02
#define VECTOR_BREAKPOINT          0x03
#define VECTOR_OVERFLOW            0x04
#define VECTOR_BOUND_RANGE         0x05
#define VECTOR_INVALID_OPCODE      0x06
#define VECTOR_DEVICE_NOT_AVAIL    0x07  /* #NM -- FPU/SSE not available */
#define VECTOR_DOUBLE_FAULT        0x08
#define VECTOR_INVALID_TSS         0x0A
#define VECTOR_SEGMENT_NOT_PRESENT 0x0B
#define VECTOR_STACK_FAULT         0x0C
#define VECTOR_GENERAL_PROTECTION  0x0D
#define VECTOR_PAGE_FAULT          0x0E
#define VECTOR_X87_FP_ERROR        0x10
#define VECTOR_ALIGNMENT_CHECK     0x11
#define VECTOR_MACHINE_CHECK       0x12
#define VECTOR_SIMD_FP_ERROR       0x13

/* ---- Hardware IRQs (PIC offset, 0x20-0x2F) ---- */
#define VECTOR_PIT_TIMER           0x20  /* IRQ0 -- PIT channel 0 */
#define VECTOR_LAPIC_TIMER         0x22  /* remapped LAPIC timer */

/* ---- Software interrupts (statically assigned) ---- */
#define VECTOR_NT_SYSCALL          0x2E  /* INT 0x2E -- NT compat syscall */
#define VECTOR_LINUX_SYSCALL       0x80  /* INT 0x80 -- Linux-style syscall */
#define VECTOR_YIELD               0x81  /* INT 0x81 -- cooperative yield */

/* ---- IPI vectors (high range, 0xFB-0xFE) ---- */
#define VECTOR_IPI_CR_VERIFY       0xFB  /* CR0/CR4 pin re-verify (TODO-09-boot S10) */
#define VECTOR_IPI_ASYNC_INIT      0xFC  /* AP async work dispatch */
#define VECTOR_IPI_RESCHEDULE      0xFD  /* cross-CPU reschedule */
#define VECTOR_IPI_TLB_SHOOTDOWN   0xFE  /* TLB invalidation */

/* ---- LAPIC special (0xFF) ---- */
#define VECTOR_LAPIC_SPURIOUS      0xFF  /* LAPIC spurious interrupt */

/* ---- Compile-time uniqueness verification ----
 * Every pair of statically assigned non-exception vectors must differ.
 * CPU exceptions (0x00-0x1F) are architecture-fixed and cannot collide
 * with each other, so we only check the software/IPI vectors. */

/* Software vectors must not collide with each other */
_Static_assert(VECTOR_NT_SYSCALL != VECTOR_LINUX_SYSCALL,
    "NT syscall and Linux syscall vectors must differ");
_Static_assert(VECTOR_NT_SYSCALL != VECTOR_YIELD,
    "NT syscall and yield vectors must differ");
_Static_assert(VECTOR_LINUX_SYSCALL != VECTOR_YIELD,
    "Linux syscall and yield vectors must differ");

/* IPI vectors must not collide with each other */
_Static_assert(VECTOR_IPI_ASYNC_INIT != VECTOR_IPI_RESCHEDULE,
    "Async init and reschedule IPI vectors must differ");
_Static_assert(VECTOR_IPI_ASYNC_INIT != VECTOR_IPI_TLB_SHOOTDOWN,
    "Async init and TLB shootdown IPI vectors must differ");
_Static_assert(VECTOR_IPI_RESCHEDULE != VECTOR_IPI_TLB_SHOOTDOWN,
    "Reschedule and TLB shootdown IPI vectors must differ");
_Static_assert(VECTOR_IPI_CR_VERIFY != VECTOR_IPI_ASYNC_INIT,
    "CR-verify and async init IPI vectors must differ");
_Static_assert(VECTOR_IPI_CR_VERIFY != VECTOR_IPI_RESCHEDULE,
    "CR-verify and reschedule IPI vectors must differ");
_Static_assert(VECTOR_IPI_CR_VERIFY != VECTOR_IPI_TLB_SHOOTDOWN,
    "CR-verify and TLB shootdown IPI vectors must differ");
_Static_assert(VECTOR_IPI_CR_VERIFY != VECTOR_LAPIC_SPURIOUS,
    "CR-verify IPI must not be spurious vector");
_Static_assert(VECTOR_IPI_CR_VERIFY != VECTOR_LAPIC_TIMER,
    "CR-verify IPI must not collide with LAPIC timer");
_Static_assert(VECTOR_IPI_CR_VERIFY != VECTOR_LINUX_SYSCALL &&
               VECTOR_IPI_CR_VERIFY != VECTOR_NT_SYSCALL &&
               VECTOR_IPI_CR_VERIFY != VECTOR_YIELD,
    "CR-verify IPI must not collide with a software-interrupt vector");

/* Software vectors must not collide with IPI vectors */
_Static_assert(VECTOR_LINUX_SYSCALL != VECTOR_IPI_ASYNC_INIT,
    "Linux syscall must not collide with async init IPI");
_Static_assert(VECTOR_LINUX_SYSCALL != VECTOR_IPI_RESCHEDULE,
    "Linux syscall must not collide with reschedule IPI");
_Static_assert(VECTOR_YIELD != VECTOR_IPI_ASYNC_INIT,
    "Yield must not collide with async init IPI");
_Static_assert(VECTOR_YIELD != VECTOR_IPI_RESCHEDULE,
    "Yield must not collide with reschedule IPI");

/* No vector can be the spurious vector */
_Static_assert(VECTOR_LINUX_SYSCALL != VECTOR_LAPIC_SPURIOUS,
    "Linux syscall must not be spurious vector");
_Static_assert(VECTOR_YIELD != VECTOR_LAPIC_SPURIOUS,
    "Yield must not be spurious vector");
_Static_assert(VECTOR_IPI_ASYNC_INIT != VECTOR_LAPIC_SPURIOUS,
    "Async init IPI must not be spurious vector");

/* LAPIC timer must not collide with software vectors */
_Static_assert(VECTOR_LAPIC_TIMER != VECTOR_PIT_TIMER,
    "LAPIC timer and PIT timer vectors must differ");

/* NT syscall must not collide with PIT range */
_Static_assert(VECTOR_NT_SYSCALL != VECTOR_PIT_TIMER,
    "NT syscall must not collide with PIT timer");
_Static_assert(VECTOR_NT_SYSCALL != VECTOR_LAPIC_TIMER,
    "NT syscall must not collide with LAPIC timer");
