/* ============================================================================
 * smp.h -- Symmetric Multi-Processing (SMP) support
 *
 * Discovers and starts secondary CPUs (Application Processors -- APs) using
 * the ACPI MADT and LAPIC INIT/SIPI IPI sequence.
 *
 * AP startup sequence:
 *   1. BSP copies AP trampoline code to physical 0x8000
 *   2. BSP writes shared data (CR3, stack, GDT/IDT, entry point) to 0x8E00
 *   3. BSP sends INIT IPI → 10ms delay → SIPI (vector 0x08) to each AP
 *   4. AP wakes in 16-bit real mode at 0x8000, transitions to long mode
 *   5. AP calls ap_entry(cpu_index) in C, initializes LAPIC, and parks
 *
 * Per-CPU data is accessed via the GS segment register. Each CPU's GS base
 * points to its own `struct per_cpu_data` block allocated from PMM.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/irql.h"
#include "kernel/acpi.h"    /* MAX_CPUS */

/* ---- AP trampoline data area layout ----
 * Shared data lives INSIDE the trampoline page at offset 0xE00 (phys 0x8E00).
 * Any separate low-memory region (0x6000, 0x7E00, etc.) will be stomped by
 * VBox EFI AP parking firmware, so all data MUST share the trampoline page.
 * Must match the offsets in ap_trampoline.asm. */
#define AP_TRAMPOLINE_ADDR     0x8000   /* where trampoline code is loaded */
#define AP_DATA_BASE           0x8E00   /* shared data at trampoline+0xE00 */

#define AP_OFF_CR3             0x00     /* uint64_t: BSP's CR3 */
#define AP_OFF_STACK           0x08     /* uint64_t: per-AP stack top */
#define AP_OFF_GDT_PTR         0x10     /* 10 bytes: GDTR */
#define AP_OFF_ENTRY           0x20     /* uint64_t: C entry point */
#define AP_OFF_CPUID           0x28     /* uint32_t: logical CPU index */
#define AP_OFF_IDT_PTR         0x30     /* 10 bytes: IDTR */

/* Per-AP kernel stack size (16 KiB, same as BSP) */
#define AP_STACK_SIZE          16384

/* ---- Per-CPU data ---- */

struct per_cpu_data {
    struct per_cpu_data *self;   /* self-pointer (gs:0 reads this) */
    uint32_t cpu_id;            /* logical CPU index (0 = BSP) */
    uint32_t lapic_id;          /* hardware LAPIC ID */
    uint64_t rsp0;              /* kernel stack top (for TSS) */
    uint64_t syscall_rsp0;      /* SYSCALL entry kernel stack (gs:24) */
    uint64_t user_rsp_scratch;  /* scratch for saving user RSP during SYSCALL (gs:32) */
    uint64_t irq_count;         /* total interrupts handled */
    uint32_t preempt_count;     /* preemption nesting counter */
    KIRQL    current_irql;      /* current IRQL (0 = PASSIVE_LEVEL) */
    uint8_t  _irql_pad[3];     /* pad to 4-byte alignment */
    uint32_t is_online;         /* 1 when AP has finished init */
    void    *current_task;      /* pointer to current thread (future) */

    /* Async boot init work dispatch (§13) */
    volatile uint8_t  in_async_work;    /* 1 while AP is executing async init */
    volatile uint8_t  async_done;       /* 1 when async work completed */
    volatile uint8_t  async_result;     /* boot_result_t from async work */
    uint8_t           _async_pad;
    const char       *async_name;       /* step name for logging */
    void             *async_fn;         /* boot_result_t (*fn)(void) */
};

/* ---- API ---- */

/* Initialize SMP: copy trampoline, start all APs discovered in MADT.
 * Must be called after acpi_init() and lapic_init(). */
/* Early BSP per-CPU init -- sets GS_BASE so smp_this_cpu() works.
 * Must be called in Phase 0 before any interrupts fire. */
void smp_early_bsp_init(void);

void smp_init(void);

/* Number of CPUs currently online */
uint32_t smp_cpu_count(void);

/* Current CPU's logical index (0 = BSP). Uses GS-based per-CPU data. */
uint32_t smp_cpu_id(void);

/* Get per-CPU data for current CPU */
struct per_cpu_data *smp_this_cpu(void);

/* Get per-CPU data for a specific CPU */
struct per_cpu_data *smp_get_cpu(uint32_t cpu_id);

/* Convenience macro */
#define this_cpu()    smp_this_cpu()
#define per_cpu(field) (smp_this_cpu()->field)
