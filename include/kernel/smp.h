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
 *
 * WARNING: ap_trampoline.asm uses hardcoded `AP_DATA + 0xNN` offsets.
 * Do NOT change these values without updating the assembly to match.
 * Static asserts below catch C-side drift at compile time, but assembly
 * must be checked manually.
 *
 * Data area offset table (phys base = AP_DATA_BASE = 0x8E00):
 *   Offset  Size  Field        Used by
 *   ------  ----  -----------  -----------------------------------
 *   +0x00     8   CR3          pm_entry: mov eax, [AP_DATA+0x00]
 *   +0x08     8   STACK        lm_entry: mov rsp, [AP_DATA+0x08]
 *   +0x10    10   GDT_PTR      lm_entry: lgdt [AP_DATA+0x10]
 *   +0x20     8   ENTRY        lm_entry: mov rax, [AP_DATA+0x20]
 *   +0x28     4   CPUID        lm_entry: mov edi, [AP_DATA+0x28]
 *   +0x30    10   IDT_PTR      lm_entry: lidt [AP_DATA+0x30]
 *   +0x3C     4   CANARY       smp_init: 0xDEADC0DE magic verify
 */
#define AP_TRAMPOLINE_ADDR     0x8000   /* where trampoline code is loaded */
#define AP_DATA_BASE           0x8E00   /* shared data at trampoline+0xE00 */

#define AP_OFF_CR3             0x00     /* uint64_t: BSP's CR3 */
#define AP_OFF_STACK           0x08     /* uint64_t: per-AP stack top */
#define AP_OFF_GDT_PTR         0x10     /* 10 bytes: GDTR */
#define AP_OFF_ENTRY           0x20     /* uint64_t: C entry point */
#define AP_OFF_CPUID           0x28     /* uint32_t: logical CPU index */
#define AP_OFF_IDT_PTR         0x30     /* 10 bytes: IDTR */
#define AP_OFF_CANARY          0x3C     /* uint32_t: magic 0xDEADC0DE */

#define AP_CANARY_MAGIC        0xDEADC0DE

/* Per-AP kernel stack size (16 KiB, same as BSP) */
#define AP_STACK_SIZE          16384

/* Compile-time enforcement: AP trampoline data area offsets.
 * If these fire, you changed a C #define without updating the assembly. */
_Static_assert(AP_OFF_CR3     == 0x00, "AP trampoline: CR3 must be at +0x00 -- asm uses [AP_DATA+0x00]");
_Static_assert(AP_OFF_STACK   == 0x08, "AP trampoline: STACK must be at +0x08 -- asm uses [AP_DATA+0x08]");
_Static_assert(AP_OFF_GDT_PTR == 0x10, "AP trampoline: GDT_PTR must be at +0x10 -- asm uses [AP_DATA+0x10]");
_Static_assert(AP_OFF_ENTRY   == 0x20, "AP trampoline: ENTRY must be at +0x20 -- asm uses [AP_DATA+0x20]");
_Static_assert(AP_OFF_CPUID   == 0x28, "AP trampoline: CPUID must be at +0x28 -- asm uses [AP_DATA+0x28]");
_Static_assert(AP_OFF_IDT_PTR == 0x30, "AP trampoline: IDT_PTR must be at +0x30 -- asm uses [AP_DATA+0x30]");
_Static_assert(AP_DATA_BASE   == AP_TRAMPOLINE_ADDR + 0xE00,
    "AP data base must be trampoline + 0xE00 (phys 0x8E00)");

/* Non-overlap verification: each field must not stomp its neighbors.
 * IDT_PTR is 10 bytes (IDTR = 2B limit + 8B base), so it spans
 * AP_OFF_IDT_PTR .. AP_OFF_IDT_PTR+9. Canary must start AFTER. */
_Static_assert(AP_OFF_CANARY >= AP_OFF_IDT_PTR + 10,
    "AP canary must not overlap IDT_PTR (10-byte IDTR span)");
_Static_assert(AP_OFF_ENTRY >= AP_OFF_GDT_PTR + 10,
    "AP entry must not overlap GDT_PTR (10-byte GDTR span)");
_Static_assert(AP_OFF_CPUID >= AP_OFF_ENTRY + 8,
    "AP cpuid must not overlap entry (8-byte uint64_t)");
_Static_assert(AP_OFF_IDT_PTR >= AP_OFF_CPUID + 4,
    "AP IDT_PTR must not overlap cpuid (4-byte uint32_t)");

/* ---- Per-CPU data ----
 *
 * WARNING: Assembly code reads hardcoded offsets into this struct via GS.
 * Do NOT insert fields before or between the first 3 entries without
 * updating syscall_entry.asm and the _Static_asserts below.
 *
 * Offset  Field              Used by
 * ------  -----------------  ----------------------------------
 * gs:0    self               smp_this_cpu() inline asm, ISR stubs
 * gs:24   syscall_rsp0       syscall_entry.asm (SYSCALL fast path)
 * gs:32   user_rsp_scratch   syscall_entry.asm (user RSP save)
 */

struct per_cpu_data {
    struct per_cpu_data *self;   /* gs:0  -- self-pointer */
    uint32_t cpu_id;            /* gs:8  -- logical CPU index (0 = BSP) */
    uint32_t lapic_id;          /* gs:12 -- hardware LAPIC ID */
    uint64_t rsp0;              /* gs:16 -- kernel stack top (for TSS) */
    uint64_t syscall_rsp0;      /* gs:24 -- SYSCALL kernel stack */
    uint64_t user_rsp_scratch;  /* gs:32 -- scratch for user RSP */
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

    /* TSC offset for per-CPU correction */
    int64_t           tsc_offset;       /* added to RDTSC on this core to match BSP */

    /* KPTI CR3 pair -- updated on context switch */
    uint64_t          kernel_cr3;       /* full kernel PML4 (all mappings) */
    uint64_t          user_cr3;         /* sparse user PML4 (user + trampoline only) */
    uint64_t          kpti_scratch;     /* scratch for trampoline (save RAX during CR3 swap) */
    uint64_t          kpti_syscall_target; /* jump target after SYSCALL CR3 swap */
    uint64_t          kpti_isr_target;    /* jump target after ISR CR3 swap */

#ifdef KERNEL_TESTS
    /* Per-CPU kmalloc fault-injection countdown. 0 disables the hook.
     * On each kmalloc() call, a non-zero value decrements; when the
     * decrement crosses from 1 to 0, that allocation returns NULL to
     * exercise caller failure-cleanup paths. Released builds compile
     * the field out via KERNEL_TESTS -- zero runtime cost.
     *
     * §6 extensions (ALL per-CPU; no cross-CPU broadcast):
     *   *_task_pid      -- when non-zero, countdown only fires for the
     *                      task whose pid matches on the SAME CPU.
     *                      Foreign tasks on this CPU skip without
     *                      consuming the countdown, so a test can
     *                      isolate the injection to the intended
     *                      consumer when helper kthreads share the
     *                      CPU. SCOPE CAVEAT: a task that migrates
     *                      to a DIFFERENT CPU before calling the
     *                      allocator does NOT trigger, because the
     *                      countdown lives only in the arming CPU's
     *                      per_cpu_data. Test runner is sequential
     *                      single-CPU so this matches usage; cross-
     *                      CPU migration-aware filtering would need
     *                      a future broadcast-to-all-CPUs variant.
     *   *_max_injections-- cap on total fires since last _set. 0 means
     *                      no cap (classic single-shot). With a cap
     *                      set, the hook auto-reloads countdown to 1
     *                      after each fire while fired < max, so
     *                      `max_injections_set(N)` + one `_next()`
     *                      produces exactly N fires without manual
     *                      re-arming.
     *   *_fired_counter -- internal: increments each time a trigger
     *                      fires, reset by _max_injections_set AND
     *                      _task_filter_set so the cap is relative
     *                      to each arm-point.
     *
     * Every subsystem (kmalloc, pmm, vmm_map, copy_user) mirrors the
     * same 4-field layout. See 00-infrastructure/kernel-test-harness
     * specification.
     */

    /* §1 + §6 -- kmalloc fault injection (hook in src/kernel/mm/heap.c). */
    uint32_t          kmalloc_fail_countdown;
    uint32_t          kmalloc_fail_task_pid;
    uint32_t          kmalloc_fail_max_injections;
    uint32_t          kmalloc_fail_fired_counter;

    /* §6 -- pmm fault injection (hook in src/kernel/mm/pmm.c). */
    uint32_t          pmm_alloc_fail_countdown;
    uint32_t          pmm_alloc_fail_task_pid;
    uint32_t          pmm_alloc_fail_max_injections;
    uint32_t          pmm_alloc_fail_fired_counter;

    /* §6 -- vmm_map_page fault injection (hook in src/kernel/mm/vmm.c). */
    uint32_t          vmm_map_fail_countdown;
    uint32_t          vmm_map_fail_task_pid;
    uint32_t          vmm_map_fail_max_injections;
    uint32_t          vmm_map_fail_fired_counter;

    /* §6 -- copy_to_user / copy_from_user fault injection (hook in
     * src/kernel/cpu_security.c). */
    uint32_t          copy_user_fail_countdown;
    uint32_t          copy_user_fail_task_pid;
    uint32_t          copy_user_fail_max_injections;
    uint32_t          copy_user_fail_fired_counter;
#endif
};

/* Compile-time enforcement of assembly-referenced struct offsets */
_Static_assert(__builtin_offsetof(struct per_cpu_data, self) == 0,
    "gs:0 must be self-pointer -- syscall_entry.asm and ISR stubs depend on this");
_Static_assert(__builtin_offsetof(struct per_cpu_data, syscall_rsp0) == 24,
    "gs:24 must be syscall_rsp0 -- syscall_entry.asm depends on this");
_Static_assert(__builtin_offsetof(struct per_cpu_data, user_rsp_scratch) == 32,
    "gs:32 must be user_rsp_scratch -- syscall_entry.asm depends on this");
_Static_assert(__builtin_offsetof(struct per_cpu_data, kernel_cr3) == 104,
    "gs:104 must be kernel_cr3 -- KPTI trampoline depends on this");
_Static_assert(__builtin_offsetof(struct per_cpu_data, user_cr3) == 112,
    "gs:112 must be user_cr3 -- KPTI trampoline depends on this");
_Static_assert(__builtin_offsetof(struct per_cpu_data, kpti_scratch) == 120,
    "gs:120 must be kpti_scratch -- KPTI trampoline depends on this");
_Static_assert(__builtin_offsetof(struct per_cpu_data, kpti_syscall_target) == 128,
    "gs:128 must be kpti_syscall_target -- KPTI trampoline depends on this");
_Static_assert(__builtin_offsetof(struct per_cpu_data, kpti_isr_target) == 136,
    "gs:136 must be kpti_isr_target -- KPTI trampoline depends on this");

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
