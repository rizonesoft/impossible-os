/******************************************************************************
 *
 * Module Name: acimpossible.h - OS specific defines for Impossible OS
 *
 * LOCAL ADDITION -- not part of upstream ACPICA. See src/libs/PROVENANCE.md.
 *
 * Selected by -D__IMPOSSIBLE_OS__ from the platform dispatch in acenv.h.
 *
 * Design note: this header deliberately pulls in NO kernel headers. ACPICA's
 * handle types are declared as void pointers and the OS Services Layer in
 * src/kernel/acpi_osl.c is the single place that knows what they really are.
 * Including kernel/sched/spinlock.h here would put kernel types into all 164
 * ACPICA translation units and invite collisions with ACPICA's own names.
 *
 *****************************************************************************/

#ifndef __ACIMPOSSIBLE_H__
#define __ACIMPOSSIBLE_H__

/* x86-64 long mode, always. */
#define ACPI_MACHINE_WIDTH          64

#define COMPILER_DEPENDENT_INT64    long long
#define COMPILER_DEPENDENT_UINT64   unsigned long long

/*
 * Use the KERNEL's mem/str functions rather than ACPICA's bundled copies in
 * utclib.c (that whole module is wrapped in #ifndef ACPI_USE_SYSTEM_CLIBRARY).
 * Two reasons: shipping a second memcpy/strlen would collide at link time, and
 * the kernel's live in mm/memops.c with SSE/AVX-512 variants, so ACPICA gets
 * the tuned implementations for free.
 *
 * ACPI_USE_STANDARD_HEADERS must accompany it: accommon.h includes acclib.h
 * ONLY when the system clib is off, so without the standard headers ACPICA
 * would lose every mem/str prototype AND the is* macros (whose AcpiGbl_Ctypes
 * table lives in the now-compiled-out utclib.c). The pair is all-or-nothing.
 *
 * This pulls <stdlib.h>, <string.h> and <ctype.h> only. The heavy set
 * (stdio/fcntl/errno/time/signal) sits behind ACPI_APPLICATION/ACPI_LIBRARY
 * in acenv.h and neither is defined here, so a freestanding build stays
 * freestanding. All three resolve to the shims in include/freestanding/.
 */
#define ACPI_USE_SYSTEM_CLIBRARY
#define ACPI_USE_STANDARD_HEADERS

/* ACPICA's internal object cache, since the kernel exposes no slab API. */
#define ACPI_USE_LOCAL_CACHE

/*
 * SMP-safe by default (CLAUDE.md): ACPI_SINGLE_THREADED must never be defined.
 * It compiles out every AcpiOs*Lock and semaphore call, which would leave the
 * AML interpreter unserialised across CPUs.
 */
#undef ACPI_SINGLE_THREADED

/* Keep error text (diagnosable failures on real firmware); drop debug tracing. */
#undef ACPI_DEBUG_OUTPUT
#undef ACPI_DBG_TRACK_ALLOCATIONS

/*
 * Opaque handle types. The OSL casts these to spinlock_t* / semaphore_t*.
 * ACPI_CPU_FLAGS carries the saved RFLAGS from spin_lock_irqsave().
 *
 * ACPI_MUTEX is deliberately NOT defined here: actypes.h derives it from
 * ACPI_SEMAPHORE for the default ACPI_BINARY_SEMAPHORE mutex model, and
 * defining it again collides with that.
 */
#define ACPI_SPINLOCK               void *
#define ACPI_SEMAPHORE              void *
#define ACPI_CPU_FLAGS              unsigned long

/* ARCH: x86-64. Cache coherence for the sleep path; a RESET invalidates
 * without writeback, so the ACPI sleep sequence needs a real flush. */
#define ACPI_FLUSH_CPU_CACHE()      __asm__ __volatile__ ("wbinvd" ::: "memory")

/*
 * ACPI 6.5 section 5.2.10.1 global lock, shared with SMM firmware.
 *
 * acenv.h's fallback is `Acquired = 1`, which claims the lock unconditionally
 * and silently races the firmware on any box that actually arbitrates it.
 * Both helpers live in acpi_osl.c so the algorithm is one testable C function
 * rather than a macro-expanded asm block repeated across translation units.
 */
int  AcpiOsImpossibleAcquireGlobalLock (volatile unsigned int *Lock);
int  AcpiOsImpossibleReleaseGlobalLock (volatile unsigned int *Lock);

#define ACPI_ACQUIRE_GLOBAL_LOCK(GLptr, Acq) \
    do { (Acq) = AcpiOsImpossibleAcquireGlobalLock ((volatile unsigned int *)(GLptr)); } while (0)

#define ACPI_RELEASE_GLOBAL_LOCK(GLptr, Pnd) \
    do { (Pnd) = AcpiOsImpossibleReleaseGlobalLock ((volatile unsigned int *)(GLptr)); } while (0)

#endif /* __ACIMPOSSIBLE_H__ */
