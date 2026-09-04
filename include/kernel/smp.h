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
#include "kernel/mm/memmap.h"   /* MM_AP_ENVELOPE_* -- canonical AP low window */
#include "kernel/sched/irql.h"
#include "kernel/acpi.h"    /* MAX_CPUS */
#include "kernel/cpuid.h"   /* cpu_feature_mask_t */

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

/* Total bytes the AP data area occupies, derived from its last field (CANARY
 * at +0x3C, 4 bytes) rather than hardcoded -- add a field past the canary and
 * this grows with it, and the envelope-containment assert below re-checks. */
#define AP_DATA_FOOTPRINT      (AP_OFF_CANARY + 4)

/* AP bringup handshake states, in per_cpu_data.ap_bringup_state.
 *
 * TERMINAL BY CONSTRUCTION (TODO-10 S27). Each transition has exactly one
 * writer and the live-set publication is BSP-OWNED, so by the time smp_init()
 * returns every discovered slot is already ONLINE or ABANDONED and no AP can
 * join the live set afterwards:
 *
 *   STARTING   the BSP arms this before the SIPI. Sole writer: the BSP, once.
 *   READY      the AP has finished ALL of its local bringup and is parked with
 *              interrupts still masked, awaiting the verdict. Sole writer: the
 *              AP, by CAS from STARTING. It publishes NOTHING of its own.
 *   ONLINE     the BSP accepted this AP: it published membership on the AP's
 *              behalf (async claim IDLE, is_online, online-mask bit) and only
 *              THEN released the AP. Sole writer: the BSP.
 *   ABANDONED  the BSP rejected this AP -- it did not reach READY inside the
 *              bringup budget. Sole writer: the BSP, by CAS from STARTING.
 *
 * The AP no longer publishes its own membership, and that is the whole point.
 * Under the previous protocol the AP CASed STARTING->ONLINE and then wrote the
 * mask bit, so an AP stalled between the two was COMMITTED to going live with
 * the BSP unable to stop it: the BSP waited a bounded 100 ms, gave up, and ran
 * cpu_features_finalize_global(), cpu_audit_consistency_check() and (after
 * smp_init returned) topology_init() over a set the AP was not in -- after
 * which the AP published its bit and started taking IPIs. Splitting the AP's
 * READINESS from the system's ACCEPTANCE makes the decision the BSP's alone,
 * and an AP that stalls simply never reaches READY in time. A late AP then
 * observes ABANDONED and parks dark instead of joining.
 *
 * ONLINE therefore now IMPLIES is_online: membership is published BEFORE the
 * state store that reveals it. That is strictly stronger than the old
 * "committed, publication still pending" meaning that
 * cpu_slot_committed_online() was written to tolerate. READY is deliberately
 * NOT committed -- the BSP may still abandon it. */
#define AP_BRINGUP_STARTING    0u
#define AP_BRINGUP_ONLINE      1u
#define AP_BRINGUP_ABANDONED   2u
#define AP_BRINGUP_READY       3u

/* Layer 1 for the handshake. STARTING must stay 0 because the BSP arms a
 * zero-initialised slot, and READY must NOT be 0 or a slot nobody armed would
 * read as an AP awaiting a verdict. */
_Static_assert(AP_BRINGUP_STARTING == 0u,
               "AP_BRINGUP_STARTING must be the zero-initialised slot value");
_Static_assert(AP_BRINGUP_READY != 0u,
               "AP_BRINGUP_READY must not alias an unarmed slot");
_Static_assert(AP_BRINGUP_STARTING  != AP_BRINGUP_ONLINE    &&
               AP_BRINGUP_STARTING  != AP_BRINGUP_ABANDONED &&
               AP_BRINGUP_STARTING  != AP_BRINGUP_READY     &&
               AP_BRINGUP_ONLINE    != AP_BRINGUP_ABANDONED &&
               AP_BRINGUP_ONLINE    != AP_BRINGUP_READY     &&
               AP_BRINGUP_ABANDONED != AP_BRINGUP_READY,
               "AP bringup states must be mutually distinct");

/* ---- Async worker lifecycle claim (TODO-10 S21) ----
 *
 * ONE word per CPU holding both the slot's dispatchability and the identity of
 * the dispatch that owns it, so no instruction boundary exists at which a slot
 * reads "free" while a worker is still running on it. Layout:
 *
 *   bits [1:0]  state -- OFFLINE (never online, or parked), IDLE, RESERVED,
 *                        BUSY
 *   bits [31:2] generation -- incremented by each successful dispatch
 *
 * Dispatch is TWO-PHASE, and that is correctness rather than bookkeeping. A
 * single IDLE -> BUSY transition would expose a RUNNABLE slot before its
 * payload was written: a delayed or misdelivered async IPI landing in that
 * window would find BUSY, execute the PREVIOUS group's function under the new
 * generation, retire the new group's claim, and leave the new group reported
 * complete having never run -- with a non-idempotent boot step run twice. So
 * the BSP reserves (IDLE -> RESERVED), writes the payload, fences, and only
 * then arms (RESERVED -> BUSY). RESERVED is not runnable; the AP handler
 * accepts BUSY alone.
 *
 * The generation is what makes a completion attributable: a worker the BSP
 * already timed out cannot retire a LATER dispatch's claim, because its
 * exact-value compare-exchange names the generation it was dispatched under.
 *
 * OFFLINE at generation 0 encodes to 0, which is the value a zero-initialised
 * per_cpu_data slot already holds -- a CPU that never came online is therefore
 * undispatchable by construction rather than by an initialiser someone has to
 * remember to run.
 *
 * DISTINCT FROM in_async_work, deliberately. The claim is BSP-owned dispatch
 * state and reads BUSY before the target AP has taken the wake IPI, whereas
 * in_async_work is CPU-local and means "this CPU is actually executing an
 * async step". Only the latter may gate the survivable async park in panic.c:
 * treating BUSY as equivalent would classify an unrelated NMI or machine check
 * landing in the claim-to-handler window as a recovered async failure and let
 * the kernel continue past an arbitrary fault. */
#define SMP_ASYNC_STATE_MASK   0x3u
#define SMP_ASYNC_OFFLINE      0x0u
#define SMP_ASYNC_IDLE         0x1u
#define SMP_ASYNC_BUSY         0x2u
#define SMP_ASYNC_RESERVED     0x3u
#define SMP_ASYNC_GEN_SHIFT    2u
#define SMP_ASYNC_GEN_MAX      (0xFFFFFFFFu >> SMP_ASYNC_GEN_SHIFT)

#define SMP_ASYNC_CLAIM(state, gen)                                     \
    (((((uint32_t)(gen)) & SMP_ASYNC_GEN_MAX) << SMP_ASYNC_GEN_SHIFT) | \
     (((uint32_t)(state)) & SMP_ASYNC_STATE_MASK))
#define SMP_ASYNC_STATE_OF(w)  (((uint32_t)(w)) & SMP_ASYNC_STATE_MASK)
#define SMP_ASYNC_GEN_OF(w)    (((uint32_t)(w)) >> SMP_ASYNC_GEN_SHIFT)

_Static_assert(SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 0) == 0u,
    "a zero-initialised claim word must read OFFLINE -- a slot that never came "
    "online must not be dispatchable");
_Static_assert(SMP_ASYNC_OFFLINE != SMP_ASYNC_IDLE &&
               SMP_ASYNC_IDLE    != SMP_ASYNC_BUSY &&
               SMP_ASYNC_OFFLINE != SMP_ASYNC_BUSY &&
               SMP_ASYNC_RESERVED != SMP_ASYNC_OFFLINE &&
               SMP_ASYNC_RESERVED != SMP_ASYNC_IDLE &&
               SMP_ASYNC_RESERVED != SMP_ASYNC_BUSY,
    "claim states must be distinct");
/* Assert over the ENCODED values, not the raw constants. Asserting raw
 * distinctness alone is satisfied by an out-of-range constant that
 * SMP_ASYNC_CLAIM then truncates: renumber IDLE to 4 and it stays "distinct"
 * while every idle slot encodes and decodes as OFFLINE, so no CPU is ever
 * dispatchable -- with a green build and no failing test. */
_Static_assert(SMP_ASYNC_OFFLINE  <= SMP_ASYNC_STATE_MASK &&
               SMP_ASYNC_IDLE     <= SMP_ASYNC_STATE_MASK &&
               SMP_ASYNC_RESERVED <= SMP_ASYNC_STATE_MASK &&
               SMP_ASYNC_BUSY     <= SMP_ASYNC_STATE_MASK,
    "every claim state must fit the state field");
_Static_assert(SMP_ASYNC_STATE_OF(SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 1)) == SMP_ASYNC_OFFLINE &&
               SMP_ASYNC_STATE_OF(SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 1))    == SMP_ASYNC_IDLE &&
               SMP_ASYNC_STATE_OF(SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 1)) == SMP_ASYNC_RESERVED &&
               SMP_ASYNC_STATE_OF(SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 1))     == SMP_ASYNC_BUSY,
    "every claim state must survive encode-then-decode");
_Static_assert(SMP_ASYNC_GEN_OF(SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, SMP_ASYNC_GEN_MAX)) == SMP_ASYNC_GEN_MAX,
    "the maximum generation must survive encode-then-decode");
_Static_assert((SMP_ASYNC_GEN_MAX << SMP_ASYNC_GEN_SHIFT) ==
               (0xFFFFFFFFu & ~SMP_ASYNC_STATE_MASK),
    "the generation field must occupy every bit the state field does not");

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

/* Bind the AP low envelope to the canonical memory map. These addresses are
 * the ONLY low-memory region the higher-half kernel must keep identity-mapped
 * (the AP stub loads CR3 with a 32-bit `mov` and cannot reach a high VA), so
 * memmap.h reserves an envelope for them. Without these asserts the two
 * headers can drift apart silently: the retained identity mapping would cover
 * one interval while SIPI and smp.c use another, and AP startup would fail at
 * runtime with no compile-time warning. */
_Static_assert(AP_TRAMPOLINE_ADDR == MM_AP_ENVELOPE_BASE,
    "AP trampoline must start at the canonical AP envelope base (memmap.h)");
_Static_assert(AP_DATA_BASE >= MM_AP_ENVELOPE_BASE,
    "AP data area must lie inside the canonical AP envelope (memmap.h)");
_Static_assert(AP_DATA_BASE + AP_DATA_FOOTPRINT <= MM_AP_ENVELOPE_END,
    "AP data area must END inside the canonical AP envelope (memmap.h)");

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
    /* AP bringup handshake state (TODO-09-boot S10). A CAS state machine that
     * closes the live-but-uncounted race: the AP transitions STARTING->ONLINE
     * (and only then publishes is_online + sti) while the BSP transitions
     * STARTING->ABANDONED on bringup timeout. Exactly one transition wins, so a
     * slow AP that the BSP gave up on parks instead of going live uncounted. */
    uint32_t ap_bringup_state;  /* AP_BRINGUP_* (atomic, CAS-transitioned) */
    void    *current_task;      /* pointer to current thread (future) */

    /* Async boot init work dispatch */
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

    /* Boot-time control/MSR snapshot. The BSP fills cpu_data[0] in
     * cpu_record_bsp_profile(); each AP fills its own block in
     * ap_cpu_harden() (TODO-09 AP CPU hardening). Consumed by the AP-vs-BSP
     * consistency warning here and the per-CPU register audit trail. All
     * fields are per-CPU (only the owning CPU writes them) so no lock is
     * required. Placed after the asm-pinned KPTI region; never read by
     * assembly, so no offset _Static_assert is needed. */
    uint64_t          efer_at_boot;        /* IA32_EFER after hardening */
    uint64_t          cr4_at_boot;         /* CR4 after hardening */
    uint64_t          pat_at_boot;         /* IA32_PAT after profile replay */
    uint64_t          xcr0_at_boot;        /* XCR0 (0 if XSAVE absent) */
    uint64_t          spec_ctrl_at_boot;   /* IA32_SPEC_CTRL (0 until SPEC_CTRL setter ships) */
    uint64_t          tsc_aux;             /* IA32_TSC_AUX = logical CPU id */
    uint32_t          msr_profile_applied; /* count of BSP profile MSRs replayed */
    uint8_t           umwait_unbounded;    /* 1 = WAITPKG present but UMWAIT_CONTROL write rejected (S19) */
    uint8_t           eibrs_unset;         /* 1 = eIBRS present but SPEC_CTRL write rejected on this AP (S8) */
    uint8_t           _harden_pad[2];      /* alignment */

    /* CPU register audit trail (TODO-09-boot S9). Captured by cpu_audit_registers()
     * on the owning CPU (BSP in Phase 2, each AP at the ap_cpu_harden() tail);
     * emitted as a single `[CPU%u AUDIT]` line BSP-side. The EFER/CR4/PAT/XCR0/
     * SPEC_CTRL fields above are reused; these add the remaining audit state. */
    uint64_t          cr0_at_boot;         /* CR0 (WP/PG/etc.) on this CPU */
    uint64_t          misc_enable;         /* IA32_MISC_ENABLE (0 if unreadable) */
    uint64_t          arch_caps;           /* IA32_ARCH_CAPABILITIES (0 if absent) */
    uint32_t          ucode_rev;           /* microcode revision (vendor-decoded) */
    uint32_t          audit_captured;      /* 1 once cpu_audit_registers() ran */

    /* AP feature consistency (TODO-09-boot S6). Published by the AP in
     * cpu_validate_ap_features() (top of ap_cpu_harden), read by the BSP after
     * the is_online acquire pass. Per-CPU (only the owning CPU writes), so no
     * lock; visibility rides the same is_online release/acquire edge as the
     * boot snapshot above. */
    cpu_feature_mask_t features;           /* security-critical CPUID subset (CPU_FEATURES_AP_PROBE_MASK layout) */
    uint8_t           core_type;           /* CORE_TYPE_P / _E / _GENERIC from CPUID 0x1A */
    uint8_t           feature_mismatch;    /* 1 = BSP has an optional probed feature this AP lacks */
    uint8_t           _feat_pad[2];        /* alignment */

    /* CR0/CR4 safety-bit pinning (TODO-09-boot S7). Per-CPU: each CPU pins the
     * bits IT actually has set (skew-safe -- a feature-skewed AP pins fewer
     * bits, no false bug-check). 0 = not yet pinned (no enforcement). Written
     * once by the owning CPU at pin time; read by that CPU's verify path. */
    uint64_t          cr0_pinned;          /* CR0 bits to keep set on this CPU (CR0_WP) */
    uint64_t          cr4_pinned;          /* CR4 security bits to keep set on this CPU */

    /* MTRR parity snapshot (TODO-09-boot S8). Compact, comparable view of this
     * CPU's MTRR state (mirrors struct mtrr_snapshot scalars). BSP fills
     * cpu_data[0] in cpu_record_bsp_profile(); each AP fills its own in
     * ap_cpu_harden(); the BSP compares AP-vs-BSP in ap_cpu_harden_log() and
     * WARNs on divergence (audit only -- no reprogramming). Per-CPU, no lock. */
    uint64_t          mtrr_cap;            /* IA32_MTRRCAP (0 if MTRR absent) */
    uint64_t          mtrr_def_type;       /* IA32_MTRR_DEF_TYPE */
    uint64_t          mtrr_checksum;       /* FNV-1a over variable + fixed MTRRs */
    uint32_t          mtrr_var_count;      /* MTRRCAP.VCNT captured */
    uint32_t          mtrr_supported;      /* 1 = MTRRs present + snapshotted */

    /* Async worker lifecycle claim (TODO-10 S21). SMP_ASYNC_* encoding above;
     * every transition is a single atomic on this word. Placed at the tail of
     * the struct, NOT beside the async block, because the KPTI trampoline pins
     * gs:128 and gs:136 by _Static_assert -- inserting a field earlier would
     * shift them. */
    uint32_t          async_claim;         /* SMP_ASYNC_CLAIM(state, generation) */

    /* Terminal-cause witness for the claim (TODO-10 S27). The worker writes the
     * exact word it is about to retire its slot to -- SMP_ASYNC_CLAIM(IDLE, gen)
     * -- immediately BEFORE completing; the BSP clears it to 0 when it reserves
     * the slot.
     *
     * It exists because the claim word alone cannot say WHY a slot reads OFFLINE
     * at a generation. A worker that completed normally and whose CPU parked
     * afterwards, and a worker that faulted mid-step and was parked by panic.c,
     * both leave OFFLINE(gen) -- yet the first ran to completion and the second
     * abandoned a half-programmed controller. Re-running is correct for one and
     * forbidden for the other. 0 is a safe "no retirement": a real retirement
     * always encodes IDLE, which is never 0. */
    uint32_t          async_retired;       /* SMP_ASYNC_CLAIM(IDLE, gen), or 0 */

    /* This CPU's cpu_panic_safe_apic_id(), PLUS ONE, or 0 when never published.
     * The +1 is the encoding the serial lock owner word uses, for the same
     * reason: APIC id 0 is legitimate, so a bare field could not distinguish
     * "the BSP" from "this slot was never brought up", and a lookup scanning for
     * id 0 would match the first unused slot. Written once by the CPU it
     * describes (smp_publish_panic_safe_id) and read-only after, so a fault
     * handler can resolve its own slot with no lock and no GS.
     *
     * PLACED AT THE TAIL, not beside the other identity fields where it reads
     * more naturally: gs:104 through gs:136 are pinned by _Static_assert below
     * because the KPTI trampoline indexes them from assembly, and inserting
     * ahead of those shifts every one of them. */
    volatile uint32_t panic_safe_id_plus1;

#ifdef KERNEL_TESTS
    /* Per-CPU kmalloc fault-injection countdown. 0 disables the hook.
     * On each kmalloc() call, a non-zero value decrements; when the
     * decrement crosses from 1 to 0, that allocation returns NULL to
     * exercise caller failure-cleanup paths. Released builds compile
     * the field out via KERNEL_TESTS -- zero runtime cost.
     *
     * extensions (ALL per-CPU; no cross-CPU broadcast):
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

    /* + -- kmalloc fault injection (hook in src/kernel/mm/heap.c). */
    uint32_t          kmalloc_fail_countdown;
    uint32_t          kmalloc_fail_task_pid;
    uint32_t          kmalloc_fail_max_injections;
    uint32_t          kmalloc_fail_fired_counter;

    /* -- pmm fault injection (hook in src/kernel/mm/pmm.c). */
    uint32_t          pmm_alloc_fail_countdown;
    uint32_t          pmm_alloc_fail_task_pid;
    uint32_t          pmm_alloc_fail_max_injections;
    uint32_t          pmm_alloc_fail_fired_counter;

    /* -- vmm_map_page fault injection (hook in src/kernel/mm/vmm.c). */
    uint32_t          vmm_map_fail_countdown;
    uint32_t          vmm_map_fail_task_pid;
    uint32_t          vmm_map_fail_max_injections;
    uint32_t          vmm_map_fail_fired_counter;

    /* -- copy_to_user / copy_from_user fault injection (hook in
     * src/kernel/cpu_security.c). */
    uint32_t          copy_user_fail_countdown;
    uint32_t          copy_user_fail_task_pid;
    uint32_t          copy_user_fail_max_injections;
    uint32_t          copy_user_fail_fired_counter;
#endif

    /* -- per-CPU fast-path transition ring.
     *
     * Each entry is a snapshot of the register state captured at a
     * ring-0 <-> ring-3 boundary crossing. Used to reconstruct the
     * last 64 transitions on this CPU at panic time, turning a
     * silent user-mode hang into a replayable trace.
     *
     * Lock-free by construction: only the owning CPU writes to its
     * own ring; other CPUs (panic dump on the crashing CPU) read
     * only AFTER the CPU is quiesced by the panic recursion guard.
     * `head` is a write index that wraps at TRANSITION_RING_SIZE.
     * TSC is monotonically non-decreasing on the same CPU (x86-64
     * constant-TSC guarantee), so dumping oldest-first = dumping in
     * chronological order. */
    uint32_t                transition_head;
    uint32_t                transition_init_marker;  /* 0xC0CAF00D when initialized */
    struct transition_entry {
        uint64_t tsc;               /* RDTSC snapshot at the transition */
        uint32_t thread_id;         /* current task PID (or 0 for idle) */
        uint32_t direction;         /* 0 = TO_KERNEL, 1 = TO_USER */
        uint64_t cr3;               /* live CR3 at capture time */
        uint64_t rip;               /* user RIP (from iret frame or saved user RIP) */
        uint64_t rsp;               /* user RSP (from iret frame or scratch) */
        uint64_t gs_base;           /* MSR_IA32_GS_BASE at capture */
        uint64_t kernel_gs_base;    /* MSR_IA32_KERNEL_GS_BASE at capture */
    } transition_ring[64];

    /* DPC drain reentrancy guard (per-CPU, non-asm region). KeLowerIrql drains
     * pending DPCs when crossing below DISPATCH_LEVEL; a DPC that itself lowers
     * IRQL must not re-enter the drain on this CPU. DPCs run at DISPATCH and do
     * not yield, so a CPU-scoped guard is safe here (it never spans a context
     * switch). APC delivery, by contrast, uses NO CPU guard -- a NormalRoutine
     * may yield/block/exit, so its reentrancy is bounded per-thread by
     * kernel_apc_in_progress instead. Set/cleared by the owning CPU only -- no
     * lock. Appended after transition_ring so no pinned KPTI offset shifts. */
    uint8_t           dpc_draining;     /* 1 while KeLowerIrql drains DPCs */
    uint8_t           _drain_pad[3];    /* alignment */

    /* IRQL contract telemetry (per-CPU, owning-CPU writes, summed for health).
     * irql_violations: IRQL_REQUIRE_AT_MOST/AT_LEAST + monotonic raise/lower
     * contract failures. irql_forced_lowers: KeLowerIrqlForced calls (forced
     * lower-to-known-level recovery, e.g. task_exit -- counted, not a bug). */
    uint32_t          irql_violations;
    uint32_t          irql_forced_lowers;

    /* Accumulated halt cycles from pm_idle_c1() (todo/02-kernel-core/TODO-26-power-management.md section 2). An UPPER
     * BOUND over TERMINAL idle sites only, not processor idle time -- it also
     * covers the waking ISR, and the batching halt in compositor.c is excluded
     * by design. See include/kernel/pm.h for both qualifications.
     * Written ONLY by its owning CPU, inside that function; read by
     * pm_idle_cycles(). Appended at the TAIL because the _Static_asserts below
     * pin gs:0/24/32/104/112/120/128/136 for syscall_entry.asm, the ISR stubs
     * and the KPTI trampoline -- a field inserted ahead of those shifts every
     * one of them. */
    uint64_t          idle_tsc_cycles;
};

#define TRANSITION_RING_SIZE         64
#define TRANSITION_DIR_TO_KERNEL     0u
#define TRANSITION_DIR_TO_USER       1u
#define TRANSITION_INIT_MARKER       0xC0CAF00DU

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

/* ---- Online membership (TODO-10 S21) ----
 *
 * TWO counts, deliberately, because they answer different questions and a
 * single snapshot answered both wrongly:
 *
 *   smp_cpu_count()          -- how many CPUs are online RIGHT NOW. Live: a
 *                               CPU that parks (panic.c async fault) drops out
 *                               of it. Use for "how many active processors do
 *                               we have" -- NT processor reporting, async
 *                               parallelism decisions, NUMBER_OF_PROCESSORS.
 *   smp_cpu_present_count()  -- how many CPU slots bringup DISCOVERED. Fixed
 *                               after smp_init(). Use for machine-configuration
 *                               stamps and as the upper bound when iterating
 *                               logical slots.
 *
 * NEITHER is a dense index bound for a CPU id taken from an affinity mask:
 * slots are sparse, so slot 2 can be online while slot 1 is not. Test
 * membership with smp_cpu_is_online(), or take ONE smp_online_mask() snapshot
 * and test bits in it when several fields must agree with each other. */

/* Number of CPUs currently online (live; >= 1) */
uint32_t smp_cpu_count(void);

/* Number of CPU slots discovered at bringup (>= 1, fixed after smp_init) */
uint32_t smp_cpu_present_count(void);

/* Bit width of the online-mask word. Named because two files bound loops by
 * it, and widening the mask without updating both would leave the test
 * silently checking half the word. */
#define SMP_ONLINE_MASK_BITS   32u

/* One coherent snapshot of the live online set, bit N = logical CPU N */
uint32_t smp_online_mask(void);

/* 1 when logical CPU `cpu` is in the live online set */
int smp_cpu_is_online(uint32_t cpu);

/* `ap_bringup_state` is NOT part of this set. It is a BRINGUP-EPOCH word
 * recording which side won the handshake, and it is deliberately left at ONLINE
 * when a CPU later parks. A consumer asking "is this CPU running work RIGHT
 * NOW" must use the mask, never that word. Since TODO-10 S27 the BSP publishes
 * membership BEFORE storing ONLINE, so ONLINE implies is_online and the word is
 * a strictly coarser view of the same fact rather than an earlier one.
 *
 * Publish/retract a CPU's online membership. The ONLY transitions:
 * publish sets is_online then the mask bit, retract clears the mask bit then
 * is_online, so the mask is always a SUBSET of the true online set and can
 * never report a parked CPU as active. Retract is panic-path safe (atomics
 * only, no locks, no allocation).
 *
 * Publish returns 0 when the CPU is now a member and -1 when publication was
 * REFUSED (a slot past MAX_CPUS, or an open stop-the-world round). The verdict
 * is returned rather than logged-and-swallowed because the bringup caller must
 * be able to abandon an AP it could not publish: an AP released into normal
 * operation believing it is online, while carrying no mask bit, is the exact
 * live-but-uncounted state the publication order exists to prevent. */
int smp_publish_cpu_online(struct per_cpu_data *pcpu);
void smp_retract_cpu_online(struct per_cpu_data *pcpu);

/* ---- Stop-the-world CPU rendezvous (TODO-26 S26) ----------------------- *
 *
 * A RESUMABLE, GENERATION-TAGGED barrier: the owner CPU stops every other
 * online CPU inside an IPI handler, does work nothing else may observe, and
 * then releases them to resume exactly where they were interrupted. It is the
 * prerequisite for any ACPI system-sleep transition, because a PM1 SLP_EN
 * write with other processors live behind a local `cli` leaves those CPUs
 * driving devices across the transition.
 *
 * It is NOT smp_test_park_cpu(): that parks the BOOKKEEPING, is KERNEL_TESTS
 * only, and has no unpark path. It is NOT an online-mask popcount either --
 * smp_retract_cpu_online() clears a CPU's mask bit BEFORE that CPU has stopped
 * executing, so a count can never prove quiescence.
 *
 * THE PROTOCOL, in one invariant: a round is ACTIVE exactly while
 * `generation != released_gen`. Arming bumps `generation`; releasing stores
 * `generation` into `released_gen`. There is no separate "closed" flag to
 * drift out of step with the counter.
 *
 * Acknowledgement is PER-CPU AND GENERATION-VALUED, not a shared bitmap. Each
 * target stores the generation it observed into its own `ack_gen[]` slot, and
 * completion requires ack_gen[slot] == generation for every target. A shared
 * bitmap admits a real ABA: a CPU that reads generation N, stalls, and stores
 * its bit after the owner has timed out N and armed N+1 would satisfy round
 * N+1 while still running. A generation-valued slot cannot -- a stale N never
 * equals N+1.
 *
 * The counters are 64-BIT for that reason, not for range. A 32-bit generation
 * WRAPS, and the wrap re-creates precisely the ABA the generation exists to
 * kill: incrementing UINT32_MAX yields 0, which already equals the untouched
 * ack_gen of a CPU that has never acknowledged anything, so the round would
 * complete before that CPU had parked. At 64 bits the counter cannot be reused
 * within any machine's uptime, so there is no wrap policy and no extra error
 * return to get wrong.
 *
 * ---- DEADLOCK CONTRACT. Read this before calling smp_rendezvous_begin(). ----
 *
 * A parked CPU keeps EVERY lock it held when the IPI reached it, for the whole
 * barrier, and it services NO maskable interrupt while parked. Therefore:
 *
 *   1. Call begin() from THREAD context holding NO spinlock. A caller holding
 *      lock L deadlocks against any target that is spinning in
 *      spin_lock_irqsave(L): that target has IF=0 and can never take the IPI,
 *      so the round can only end by timing out.
 *   2. Between a successful begin() and its end(), do NOT take any lock,
 *      allocate, klog(), run a driver callback, or perform any SYNCHRONOUS
 *      cross-CPU operation. A TLB shootdown or reschedule IPI issued inside
 *      the window waits on a CPU that will never answer it. Device and
 *      firmware callbacks belong BEFORE begin(), never inside it.
 *   3. NMI and #MC still reach a parked CPU; only maskable delivery is
 *      blocked. Panic-path code must therefore stay parked-CPU safe.
 *
 * The window is an audited, lockless, non-blocking sequence. That is a
 * contract this kernel has no facility to enforce (there is no lock-depth or
 * IRQL query to assert on), which is exactly why it is stated here. */

/* Bit width of the rendezvous target mask. Same word as the online mask, and
 * pinned to it so the two can never disagree about how many slots exist. */
#define SMP_RENDEZVOUS_MASK_BITS  SMP_ONLINE_MASK_BITS

/* Default bound for a rendezvous that does not name its own. Long enough that
 * a CPU stalled in an SMI still answers, short enough that a wedged machine
 * fails closed instead of hanging. */
#define SMP_RENDEZVOUS_TIMEOUT_MS  100u

/* Rendezvous state. Exposed so the PURE protocol helpers below can be driven
 * over a caller-supplied instance in unit tests without stopping the live
 * machine -- the same discipline the async-claim and bringup-arbitration
 * helpers above follow. Live callers use the wrappers, never this struct. */
struct smp_rendezvous {
    uint64_t generation;                        /* bumped on arm */
    uint64_t released_gen;                      /* == generation when idle */
    uint64_t ack_gen[SMP_RENDEZVOUS_MASK_BITS]; /* per-CPU acknowledged gen */
    uint32_t target_mask;                       /* who must acknowledge */
    uint32_t owner_slot_plus1;                  /* 0 = no owner */
};

/* ---- Pure protocol (no hardware, no globals; safe to unit test) ---- */

/* 1 while a round is open. A NULL rv is inert and answers 0. */
int smp_rendezvous_round_active(const struct smp_rendezvous *rv);

/* Open a round over `target_mask`, owned by `owner_slot`. Returns 0 on
 * success, -1 if a round is already open, the owner slot is out of range, or
 * the target mask names a slot outside the mask width. Bumping the generation
 * is what closes admission -- there is no second flag. */
int smp_rendezvous_arm(struct smp_rendezvous *rv, uint32_t target_mask,
                       uint32_t owner_slot);

/* Replace the target set of an ALREADY-OPEN round owned by `owner_slot`.
 * Returns 0 on success and -1 if no round is open, the caller is not the
 * owner, the mask names the owner, or it names a slot outside the mask width.
 *
 * This exists so the live owner can close admission BEFORE it decides who the
 * targets are. Snapshotting the online set first and arming afterwards leaves
 * a window in which a CPU comes online between the scan and the arm: it misses
 * the snapshot, passes the admission check that is not yet closed, and ends up
 * online, executing, and not a target -- so the round completes while it runs,
 * which defeats the entire primitive. Arming with an empty set and setting the
 * real one afterwards makes the ordering "no CPU may join" then "here is who
 * must stop", which is the only order that is safe. */
int smp_rendezvous_set_targets(struct smp_rendezvous *rv, uint32_t target_mask,
                               uint32_t owner_slot);

/* Acknowledge the CURRENT generation on behalf of `slot` and return the
 * generation acknowledged. Idempotent: re-acknowledging the same generation is
 * a no-op, which is what makes a delayed duplicate IPI harmless. */
uint64_t smp_rendezvous_ack(struct smp_rendezvous *rv, uint32_t slot);

/* 1 once EVERY targeted slot has acknowledged the CURRENT generation. An empty
 * target mask is complete immediately -- the single-CPU case is a real case,
 * not a skip. */
int smp_rendezvous_complete(const struct smp_rendezvous *rv);

/* Release the open round. Returns 0 on success, -1 if no round is open or
 * `owner_slot` is not the CPU that armed it. Release is the ONLY way a parked
 * CPU resumes, so a non-owner must never be able to perform it. */
int smp_rendezvous_release(struct smp_rendezvous *rv, uint32_t owner_slot);

/* ONE iteration of the parked-CPU loop. Returns 1 when the caller must keep
 * parking (having acknowledged the current generation) and 0 when it may
 * resume. Re-reading the generation every iteration is deliberate: a CPU
 * delayed across a timeout and re-arm acknowledges the NEW round instead of
 * stranding it. Split out of the loop so the protocol is testable -- an
 * infinite spin is not. */
int smp_rendezvous_park_step(struct smp_rendezvous *rv, uint32_t slot);

/* ---- Live wrappers (drive the kernel's own rendezvous) ---- */

/* Register the barrier IPI handler. Called once by the BSP after SMP bringup,
 * exactly as cpu_cr_verify_ipi_init() is. Until this runs, begin() refuses. */
void smp_rendezvous_ipi_init(void);

/* Stop every other online CPU. BSP-only, single-flight. Returns 0 with the
 * world stopped, or -1 having stopped nothing (fail CLOSED: on timeout every
 * already-parked CPU is released before returning, so a partial rendezvous is
 * never reported as success). `timeout_ms` of 0 means SMP_RENDEZVOUS_TIMEOUT_MS.
 *
 * ON SUCCESS THE OWNER RETURNS WITH INTERRUPTS DISABLED, and smp_rendezvous_end()
 * restores the interrupt state the caller had on entry. That is not a
 * convenience: an owner that stayed preemptible could be switched out by its
 * own LAPIC timer with every AP already parked, and nothing would then be left
 * running to notice the timeout or perform the release. Both failure paths
 * restore the caller's state before returning, so a refused or timed-out
 * begin() leaves interrupts exactly as it found them.
 *
 * Read the DEADLOCK CONTRACT above before calling. */
int smp_rendezvous_begin(uint32_t timeout_ms);

/* Release the world. Legal only from the CPU that opened the round; returns 0
 * on success and -1 otherwise. */
int smp_rendezvous_end(void);

/* 1 while the kernel's own round is open. The online-publish path consults
 * this as a fail-closed backstop. */
int smp_rendezvous_in_progress(void);

/* ---- Bringup arbitration (TODO-10 S27) ----
 *
 * Pure transitions over a caller-supplied handshake word, so the terminality
 * property is testable without a live SMP system -- the same discipline the
 * async claim helpers above follow. A NULL `state` is inert and answers 0.
 *
 * AP side: STARTING -> READY. Returns 1 when this AP may now wait for a
 * verdict, and 0 when the BSP already abandoned it (park dark, publish
 * nothing). */
int smp_ap_bringup_ready(uint32_t *state);

/* BSP side, called once per discovered slot after the bringup budget expires.
 * Returns 1 when the AP reached READY and the caller MUST publish its
 * membership and then release it with smp_ap_bringup_accept(); returns 0 when
 * the slot was abandoned (it CASed STARTING -> ABANDONED, so a late AP loses
 * its own CAS and parks dark). Already-terminal slots answer by their state:
 * ONLINE reports 1 without re-publishing, ABANDONED reports 0. */
int smp_bsp_bringup_arbitrate(uint32_t *state);

/* BSP side: the release store that reveals an accepted AP. Call ONLY after
 * membership is published -- this is the transition that lets the AP leave its
 * wait and enable interrupts, so publishing after it would re-open the
 * live-but-unpublished window the protocol exists to close. */
void smp_ap_bringup_accept(uint32_t *state);

/* 1 when the slot has reached a terminal verdict (ONLINE or ABANDONED). After
 * smp_init() returns this holds for every discovered slot. */
int smp_ap_bringup_is_terminal(const uint32_t *state);

/* ---- Async claim transitions (TODO-10 S21) ----
 * Pure operations over a caller-supplied claim word so the state machine is
 * testable without a live SMP system. Each is ONE atomic transition.
 *
 * CONTRACT SHARED BY ALL OF THEM, so no caller has to read the implementation:
 *   - A NULL `claim` is inert: the int-returning helpers answer 0, the
 *   void ones do nothing. These run from an ISR and from the panic path,
 *   where refusing is the only safe answer.
 *   - `out_gen` is OPTIONAL everywhere it appears; pass NULL when the caller
 *   does not need the generation. A helper that returns 0 never writes it,
 *   so a caller's sentinel survives a refusal.
 *   - A refused transition leaves the word BYTE-IDENTICAL. Refusal is
 *   ordinary control flow (the CPU parked, another dispatch owns the slot),
 *   not an error condition, and the caller decides what it means.
 *   - Every `gen` argument is masked to SMP_ASYNC_GEN_MAX before comparison,
 *   so an out-of-range generation is normalised rather than rejected. Only
 *   generations this API produced are ever passed in practice. */

/* Phase 1: IDLE(g) -> RESERVED(g+1). Returns 1 and the new generation on
 * success; 0 when the slot is OFFLINE, RESERVED, or still BUSY from a dispatch
 * that never completed. The slot is NOT runnable yet. */
int  smp_async_claim_dispatch(uint32_t *claim, uint32_t *out_gen);

/* Phase 2: RESERVED(gen) -> BUSY(gen), exact-value. Call ONLY after the
 * payload is written and fenced -- this is the transition that makes the slot
 * runnable. Returns 0 for ANY word that is not exactly RESERVED(gen): the CPU
 * parked between reserve and arm, the slot is already armed, it is IDLE or
 * OFFLINE, or the generation is not the one this caller reserved. */
int  smp_async_claim_arm(uint32_t *claim, uint32_t gen);

/* BUSY(gen) -> IDLE(gen), exact-value: a worker whose dispatch was superseded
 * or whose CPU parked cannot retire the slot. Returns 1 on success, and 0 for
 * any other word -- a foreign generation, an already-completed (IDLE) slot, a
 * still-reserved one, or a parked one. Completing twice therefore fails the
 * second time rather than reopening the slot. */
int  smp_async_claim_complete(uint32_t *claim, uint32_t gen);

/* -> OFFLINE, preserving the generation, from any state. */
void smp_async_claim_park(uint32_t *claim);

/* OFFLINE(gen) -> IDLE(gen): a CPU publishing itself dispatchable. Refuses to
 * touch a RESERVED or BUSY slot -- republishing one as IDLE would erase an
 * in-flight worker's ownership and let a second dispatch land on a CPU still
 * running the first, which is the exact failure this claim exists to prevent.
 * An already-IDLE slot is a no-op. */
void smp_async_claim_publish_idle(uint32_t *claim);

/* 1 when the slot reads OFFLINE at exactly `gen` -- the dispatch identified by
 * `gen` will never complete. */
int  smp_async_claim_is_dead(const uint32_t *claim, uint32_t gen);

/* 1 when the slot reads BUSY; reports the owning generation via out_gen. */
int  smp_async_claim_is_busy(const uint32_t *claim, uint32_t *out_gen);

/* Pure mask helpers (same testability rationale). */
void     smp_mask_set(uint32_t *mask, uint32_t cpu);
void     smp_mask_clear(uint32_t *mask, uint32_t cpu);
int      smp_mask_test(uint32_t mask, uint32_t cpu);
uint32_t smp_mask_count(uint32_t mask);

#ifdef KERNEL_TESTS
/* Degraded-configuration injection (TODO-10 S27): park an ALREADY-ONLINE CPU
 * after bringup has finalized, then check the relations that a live/present
 * divergence must satisfy -- live count falls by one, present count and the
 * topology slot count hold, the mask bit clears, and popcount(mask) still
 * equals the live count that every NT-facing consumer reports.
 *
 * Returns 1 when all of them held, 0 on a rejected target (slot 0, out of
 * range, already offline) or a violated relation, each violation klogged. The
 * BSP calls this for ANOTHER slot, which is why it does not route through
 * smp_retract_cpu_online() -- see the note at the implementation. */
int smp_test_park_cpu(uint32_t cpu);
#endif

/* Current CPU's logical index (0 = BSP). Uses GS-based per-CPU data. */
uint32_t smp_cpu_id(void);

/* Get per-CPU data for current CPU */
struct per_cpu_data *smp_this_cpu(void);

/* Get per-CPU data for a specific CPU */
struct per_cpu_data *smp_get_cpu(uint32_t cpu_id);

/* Publish THIS CPU's panic-safe identity into its own slot, so a fault handler
 * can find the slot again without GS. Call once per CPU, from that CPU, before
 * it can run any work a panic would have to clean up after.
 *
 * RETURNS 0 WHEN IT REFUSES, and the caller must act on that rather than
 * carry on. A CPU that could not publish is not merely missing bookkeeping: a
 * later lookup for its id resolves to whichever slot DID publish it, so a fault
 * on the unpublished CPU would retract and publish completion into a LIVE CPU's
 * lifecycle fields and leave the dying one online. An AP that cannot publish
 * therefore parks instead of reaching READY, exactly as the live-LAPIC identity
 * guard beside it parks an impostor.
 *
 * A SEPARATE FIELD FROM lapic_id, and the separation is the point.
 * cpu_security.h states that a cpu_panic_safe_apic_id() value is only ever
 * compared against another value from THAT helper: it is the CPUID leaf-1
 * INITIAL apic id, while lapic_id carries the MADT/LAPIC-register id. The two
 * agree on every machine this repo supports and are permitted to diverge in
 * principle, so a lookup that matched one against the other would fail silently
 * and exactly once -- on the firmware that remaps them -- by returning the
 * wrong slot or none at all. */
int smp_publish_panic_safe_id(struct per_cpu_data *pcpu);

/* May a CPU observing `observed_panic_id` clear the SHARED online-mask bit of a
 * slot publishing `slot_panic_id_plus1`? 1 = identities agree, 0 = refuse.
 *
 * Pure, and split out precisely because the answer is otherwise unobservable on
 * the machines that run the tests. The gate used to compare the slot's
 * `lapic_id` against a CPUID-derived id -- two different derivations that agree
 * on every supported machine and are permitted to diverge on firmware that
 * remaps the LAPIC id. A regression back to that comparison would pass every
 * live assertion and, on the one firmware where it matters, refuse the clear and
 * leave a permanently parked CPU set in the mask for later async groups to
 * select and wait out. Here both sides are panic-safe ids, and a fixture can
 * hand it a deliberate mismatch. An unpublished slot reads 0 and is refused. */
int smp_retract_may_clear_mask(uint32_t slot_panic_id_plus1,
                               uint32_t observed_panic_id);

/* Resolve a per-CPU slot from a CPUID-derived local APIC id, WITHOUT reading GS.
 *
 * The panic path's identity rule, completed. smp_this_cpu() reads gs:0 and
 * silently substitutes &cpu_data[0] when that read yields 0, so on the corrupt-
 * GS CPU that most needs an answer it returns the BSP's slot and reports no
 * error -- which is why smp_retract_cpu_online has to re-check the identity
 * itself before touching the shared mask. Pair this with cpu_panic_safe_apic_id()
 * and the slot and the id name the same CPU by construction, so a fault handler
 * can mutate its own lifecycle state without trusting a segment base.
 *
 * ONE INDEXED LOAD into an owner table keyed by the 8-bit id, whose entries
 * hold the owning logical CPU index PLUS ONE (0 = unclaimed), followed by the
 * bounds-checked smp_get_cpu(). It reads no per-CPU block to find one, which is
 * what lets it answer on a CPU whose GS cannot be trusted, and it cannot fault:
 * the table is this file's own BSS and the extent is CPU_PANIC_SAFE_ID_COUNT,
 * exactly the id space the mask can produce.
 *
 * An unclaimed id returns NULL. It is NOT a scan over cpu_data[] any more --
 * that shape could not detect two slots claiming one id, it just returned
 * whichever came first, and ownership is now taken by compare-exchange on the
 * table so a duplicate is refused rather than silently aliased.
 * per_cpu_data.panic_safe_id_plus1 is no longer the lookup source; it remains
 * as the value smp_retract_may_clear_mask() gates the online-mask clear on. */
struct per_cpu_data *smp_cpu_by_apic_id(uint32_t apic_id);

/* Convenience macro */
#define this_cpu()    smp_this_cpu()
#define per_cpu(field) (smp_this_cpu()->field)
