/* ============================================================================
 * smp.c -- SMP bringup (AP startup via INIT/SIPI IPI sequence)
 *
 * Copies the AP trampoline to physical 0x8000, then for each AP discovered
 * in the ACPI MADT, writes shared data to 0x8E00 and sends INIT → SIPI.
 *
 * Each AP transitions to long mode in the trampoline, then calls ap_entry()
 * here, which initializes the AP's LAPIC and per-CPU data.
 *
 * Per-CPU data is allocated from PMM (identity-mapped). Each CPU's GS base
 * is set via IA32_GS_BASE MSR to point to its per_cpu_data block.
 * ============================================================================ */

#include "kernel/smp.h"
#include "kernel/msr.h"
#include "kernel/cpu_security.h"
#include "kernel/drivers/lapic.h"
#include "kernel/acpi.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"
#include "kernel/barrier.h"
#include "kernel/atomic.h"
#include "kernel/config.h"
#include "kernel/pm.h"         /* pm_idle_c1 -- the AP park loop's halt */
#ifdef KERNEL_TESTS
#include "kernel/topology.h"   /* g_topo_cpu_count -- S27 park-injection check */
#endif

/* ---- External symbols ---- */

/* AP trampoline binary (embedded via objcopy from flat binary).
 * objcopy generates symbols: _binary_ap_trampoline_bin_start/end/size */
extern uint8_t _binary_ap_trampoline_bin_start[];
extern uint8_t _binary_ap_trampoline_bin_end[];

/* Convenience aliases */
#define ap_trampoline_start  _binary_ap_trampoline_bin_start
#define ap_trampoline_end    _binary_ap_trampoline_bin_end

/* BSP's GDT and IDT pointers (from gdt.c and idt.c) */
extern void gdt_get_gdtr(void *out_gdtr);  /* fills 10-byte GDTR */
extern void idt_get_idtr(void *out_idtr);  /* fills 10-byte IDTR */

/* ---- State ---- */

static struct per_cpu_data cpu_data[MAX_CPUS];

/* Live online set (TODO-10 S21). Bit N = logical CPU N is online NOW. This is
 * both the SOLE active-membership API and the PUBLICATION POINT: the BSP's
 * per-AP bringup waits, the live count, and every consumer asking "which CPUs
 * are active" read this word, so a parked CPU disappears from the system's
 * view of itself the moment it parks. per_cpu_data.is_online remains the AP's
 * own per-CPU flag and is written earlier in the same sequence.
 *
 * The two words cannot disagree in the dangerous direction: publish sets
 * is_online BEFORE the mask bit and retract clears the mask bit BEFORE
 * is_online, so the mask is always a SUBSET of the true online set. An
 * interleaving can under-report a CPU that is coming up; none can report a
 * parked CPU as active. smp_init verifies that subset relation once, at the
 * end of bringup, where the online set is fixed. */
static uint32_t            online_mask = 0;

/* CPU slots bringup DISCOVERED (1 + APs enumerated from the MADT). Fixed once
 * smp_init() returns; this is the machine's configuration, not its live state.
 * Kept separate from the online mask because a consumer asking "how big is
 * this machine" and one asking "how many CPUs can run work" got the same
 * answer before this section, and that answer was wrong for one of them. */
static uint32_t            present_cpus = 0;

_Static_assert(MAX_CPUS <= SMP_ONLINE_MASK_BITS,
    "online_mask is a uint32_t -- one bit per logical CPU slot");

#ifdef KERNEL_TESTS
/* Degraded-configuration injection (TODO-10 S27). BSP-only: written once at the
 * top of smp_init() from the `test_abandon_ap` boot key, read only by that same
 * loop on the same CPU, so no synchronisation is required or implied. 0 = off
 * (slot 0 is the BSP and is never an AP, so it is not a legal target). */
static uint32_t            s_inject_abandon_ap = 0;
#endif

/* 1 when AP slot `cpu` has announced READY -- the signal the BSP's bringup wait
 * keys off since TODO-10 S27, replacing the AP's own membership publication. */
static int ap_bringup_is_ready(uint32_t cpu)
{
    if (cpu >= MAX_CPUS)
        return 0;
    return __atomic_load_n(&cpu_data[cpu].ap_bringup_state, __ATOMIC_ACQUIRE)
           == AP_BRINGUP_READY;
}

/* ---- MSR helpers ---- */

/* wrmsr/rdmsr/MSR_GS_BASE now provided by kernel/msr.h
 * (MSR_IA32_GS_BASE = 0xC0000101) */

/* Read CR3 (BSP's page table base) */
static inline uint64_t read_cr3(void)
{
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

/* ---- Early BSP per-CPU init ---- */

void smp_early_bsp_init(void)
{
    /* Minimal init so smp_this_cpu() works before smp_init().
     * The timer ISR uses smp_this_cpu() for IRQL tracking.
     * On bare metal, GS_BASE defaults to 0 and gs:0 reads IVT
     * garbage instead of NULL -- crashing the IRQL code. */
    cpu_data[0].self          = &cpu_data[0];
    cpu_data[0].cpu_id        = 0;
    /* BEFORE anything that could fault. The panic park path resolves its slot
     * through this field and nothing else, so a BSP that faulted between here
     * and a later publish point would be unable to find its own block. */
    smp_publish_panic_safe_id(&cpu_data[0]);
    /* Publishes is_online, the claim word (IDLE) and online-mask bit 0 in one
     * place, so smp_cpu_count() reports the BSP from Phase 0 onward rather
     * than depending on smp_init() having run. */
    smp_publish_cpu_online(&cpu_data[0]);
    cpu_data[0].current_irql  = PASSIVE_LEVEL;
    cpu_data[0].current_task  = (void *)0;
    cpu_data[0].irq_count     = 0;
    cpu_data[0].preempt_count = 0;

    /* KPTI: Initialize CR3 pair. kernel_cr3 = current boot PML4.
     * user_cr3 = kernel_cr3 (no isolation until allocates sparse PML4). */
    {
        uint64_t cr3_val;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
        cpu_data[0].kernel_cr3 = cr3_val;
        cpu_data[0].user_cr3   = cr3_val; /* no isolation yet */
    }

    msr_write(MSR_IA32_GS_BASE, (uint64_t)(uintptr_t)&cpu_data[0]);

    /* Runtime verify: read gs:0 back via inline asm and confirm it
     * matches the address we just wrote. If GS_BASE is wrong, every
     * per-CPU access in the kernel will read garbage. */
    {
        struct per_cpu_data *readback;
        __asm__ volatile("mov %%gs:0, %0" : "=r"(readback));
        if (readback != &cpu_data[0]) {
            /* Serial is the only output available this early */
            extern void serial_write(const char *s);
            serial_write("[FATAL] gs:0 self-pointer mismatch after GS_BASE write\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
    }

    /* fast-path transition ring: initialize BSP ring now that
     * gs:0 is valid. transition_ring_record is a no-op until the
     * init marker is set, so any pre-init ring-3 transition is
     * safely dropped rather than crashing. */
    {
        extern void transition_ring_init_this_cpu(void);
        transition_ring_init_this_cpu();
    }
}

/* ---- Delay helpers ---- */

/* Busy-wait delay using I/O port 0x80 (POST code port, ~1 µs per access) */
static void io_delay(void)
{
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

/* Approximate millisecond delay (calibrated to ~1ms with port I/O) */
static void delay_ms(uint32_t ms)
{
    uint32_t i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 1000; j++)
            io_delay();
}

/* ---- AP entry point (called from trampoline in 64-bit mode) ---- */

void ap_entry(uint32_t cpu_index)
{
    struct per_cpu_data *pcpu;

    /* Set up GS base to point to this CPU's per_cpu_data */
    pcpu = &cpu_data[cpu_index];
    pcpu->self   = pcpu;        /* self-pointer for gs:0 access */
    pcpu->cpu_id = cpu_index;   /* set BEFORE ap_cpu_harden: cpu_validate_ap_
                                 * features + the CR-pin BSP/AP fault routing
                                 * (cpu_id==0 means BSP) read smp_this_cpu()->
                                 * cpu_id; a late set left it 0 and made an AP
                                 * look like the BSP. */

    /* KPTI: AP gets same kernel CR3 as BSP (shared boot PML4).
     * user_cr3 = kernel_cr3 until allocates per-process PML4. */
    {
        uint64_t cr3_val;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
        pcpu->kernel_cr3 = cr3_val;
        pcpu->user_cr3   = cr3_val;
    }

    msr_write(MSR_IA32_GS_BASE, (uint64_t)(uintptr_t)pcpu);

    /* CPU security hardening on this AP: match the BSP XCR0, enable the
     * gated NX/UMIP/PKU/SMEP/SMAP set, replay the BSP MSR profile (PAT,
     * TSC_AUX), and verify against the BSP baseline (warn-only on drift;
     * force-after-validation of any missing CR4 bit is the feature
     * consistency validation's job, not this path). Page tables already
     * have U/S cleared by the BSP's vmm_apply_nx_policy(), so SMEP/SMAP are
     * safe here. Replaces the former bare cpu_harden() +
     * cpu_harden_post_pagetable() + ad-hoc TSC_AUX write. */
    ap_cpu_harden(cpu_index);

    /* Initialize this AP's LAPIC */
    lapic_init_ap();

    /* Identity guard (TODO-09-boot S10): the BSP pre-stored this slot's EXPECTED
     * apic_id into pcpu->lapic_id before the SIPI. cpu_index came from the single
     * shared AP_DATA page, which the BSP rewrites per AP; a slow AP that latches
     * AP_DATA after the BSP reassigned the slot would arrive here under the wrong
     * cpu_index. If our LIVE LAPIC ID does not match the slot's expected id, we
     * are that impostor -- do NOT claim the slot: park dark so the slot's real AP
     * (or the abandon CAS) owns it, and we are never counted/scheduled under a
     * mismatched identity. (This closes the misidentified-online outcome; the
     * deeper concurrent-stack isolation needs the AP_DATA consume-ack handshake
     * tracked as a separate S10 item.) */
    {
        uint32_t live = lapic_id();
        if (live != pcpu->lapic_id) {
            for (;;)
                __asm__ volatile("cli; hlt");
        }
        pcpu->lapic_id = live;
    }
    /* Published immediately after the identity guard above accepted this slot,
     * and long before any async init step can be dispatched here -- the panic
     * park path has no other way to find this block without GS.
     *
     * A REFUSAL PARKS THIS AP, exactly as the live-LAPIC guard above parks an
     * impostor, and for a sharper reason than missing bookkeeping. If this CPU
     * carried on unpublished, a later lookup for its id would resolve to the
     * slot that DID publish it -- so a fault here would retract a LIVE CPU and
     * publish failure into ITS async fields while leaving this one online.
     * Parking dark costs one CPU on a platform that is already reporting
     * duplicate CPUID identities; continuing costs a running one. */
    if (!smp_publish_panic_safe_id(pcpu)) {
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    /* PLACED AFTER THE GUARD, not before the hardening above, and the residual
     * window that leaves is filed rather than papered over. Claiming earlier
     * would shrink the interval in which a faulting AP resolves to another
     * CPU's slot -- but it would also let a slow AP that latched a reassigned
     * cpu_index stamp its id into a slot another CPU is already live on, which
     * is the failure the guard immediately above exists to prevent. The two
     * constraints genuinely conflict; the resolution belongs to the per-CPU
     * identity-authentication work, where both are already written down. */
    pcpu->irq_count  = 0;
    pcpu->preempt_count = 0;
    pcpu->current_irql  = PASSIVE_LEVEL;
    pcpu->current_task  = (void *)0;

    /* fast-path transition ring: initialize AP ring now that
     * gs points at this AP's per_cpu_data. Same idempotent no-op
     * rule as BSP init above. */
    {
        extern void transition_ring_init_this_cpu(void);
        transition_ring_init_this_cpu();
    }

    /* Memory barrier to ensure all writes are visible before incrementing count */
    smp_mb();

    /* Publish this AP as online as the LAST act before parking. The
     * ONLINE-MASK BIT is the single publication point the BSP uses to (a) end
     * its per-AP bringup wait, (b) derive the live CPU count, and (c) gate the
     * per-AP audit -- it is written last inside smp_publish_cpu_online, so
     * there is no partial publication a waiter can mistake for a complete one
     * (TODO-10 S21; before that section the waits keyed off is_online, which
     * lands earlier in the same sequence). An acquire-load that observes the
     * bit is guaranteed to see every preceding write (lapic_id, the
     * ap_cpu_harden() snapshot); an AP that never reached here has no bit, so
     * the BSP's waited / counted / audited sets are identical. The AP emits NO serial output from here to
     * `sti` (klog busy-waits on the UART with IRQs masked); the BSP emits the
     * "online" line + hardening audit from the buffered per_cpu_data after
     * bringup -- see smp_init. */
    /* Bringup handshake (TODO-09-boot S10, made terminal by TODO-10 S27):
     * announce READY and then WAIT for the BSP's verdict. This AP publishes
     * nothing of its own -- membership is the BSP's to grant, which is what
     * makes the live set final when smp_init() returns. Losing this CAS means
     * the BSP already abandoned us, so park dark. */
    if (!smp_ap_bringup_ready(&pcpu->ap_bringup_state)) {
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    /* Await the verdict with interrupts still masked. The BSP resolves EVERY
     * discovered slot inside its bringup loop, so this wait terminates without
     * a deadline of its own: either it accepts us (membership already
     * published, then ONLINE) or it abandons us. An AP stalled arbitrarily long
     * before reaching READY is abandoned and never gets here at all; one
     * stalled HERE simply observes the verdict late and acts on it, which is
     * exactly the property the old protocol lacked -- there, a stall after the
     * AP's own ONLINE CAS made it live with the BSP unable to intervene. */
    {
        uint32_t verdict;
        do {
            __asm__ volatile("pause");
            verdict = __atomic_load_n(&pcpu->ap_bringup_state, __ATOMIC_ACQUIRE);
        } while (verdict == AP_BRINGUP_READY);

        if (verdict != AP_BRINGUP_ONLINE) {
            /* Abandoned -- park dark, never online: no scheduler visibility and
             * no stray IPI handling for a CPU the BSP gave up on. */
            for (;;)
                __asm__ volatile("cli; hlt");
        }
    }

    /* Accepted. The BSP published this slot's membership BEFORE storing ONLINE,
     * so by the time the acquire-load above returned it, the async claim, the
     * is_online flag and the online-mask bit are all already visible -- there is
     * no window in which this CPU is live but uncounted.
     *
     * AP is parked -- enable interrupts and halt. The LAPIC timer or IPI will
     * wake it when the scheduler is ready. */
    __asm__ volatile("sti");
    for (;;)
        pm_idle_c1();   /* same STI;HLT, plus per-CPU idle accounting. It
                         * always halts -- a refusal here would spin forever,
                         * since an idle AP has no DPC drain trigger. */
}

/* ---- SMP initialization (BSP side) ---- */

void smp_init(void)
{
    uint32_t cpu_count;
    uint32_t bsp_index = 0;
    uint32_t ap_count;
    uint32_t i;
    uint64_t cr3;
    uint8_t *trampoline_dest;
    uint32_t trampoline_size;
    uint8_t gdtr[10];
    uint8_t idtr[10];

    /* Freeze the BSP's hardened register/MSR baseline now -- after all
     * Phase 0/1 xstate mutations (simd_enable_avx512 may have cleared AVX-512
     * XCR0 bits) and before any AP starts -- so each AP replicates the BSP's
     * final state, not a recomputed-from-CPUID approximation. Recorded on
     * every platform, including single-CPU, for the register audit trail. */
    cpu_record_bsp_profile();

#ifdef KERNEL_TESTS
    /* Read the injection target once, on the BSP, before any AP starts. */
    {
        int64_t v = boot_arg_resolved_ival("test_abandon_ap");
        s_inject_abandon_ap = (v > 0 && v < (int64_t)MAX_CPUS) ? (uint32_t)v : 0u;
        if (s_inject_abandon_ap)
            klog(LOG_WARN, "smp",
                 "test_abandon_ap=%u -- AP slot %u will be abandoned at bringup",
                 (uint64_t)s_inject_abandon_ap, (uint64_t)s_inject_abandon_ap);
    }
#endif

    cpu_count = acpi_get_cpu_count();
    if (cpu_count <= 1) {
        klog(LOG_INFO, "smp", "Single CPU -- skipping AP bringup");
        /* Initialize BSP per-CPU data (no LAPIC init needed on single-core) */
        cpu_data[0].self         = &cpu_data[0];
        cpu_data[0].cpu_id       = 0;
        cpu_data[0].lapic_id     = lapic_available() ? lapic_id() : 0;
        smp_publish_cpu_online(&cpu_data[0]);
        cpu_data[0].irq_count    = 0;
        cpu_data[0].preempt_count = 0;
        cpu_data[0].current_irql  = PASSIVE_LEVEL;
        cpu_data[0].current_task  = (void *)0;
        msr_write(MSR_IA32_GS_BASE, (uint64_t)(uintptr_t)&cpu_data[0]);
        __atomic_store_n(&present_cpus, 1u, __ATOMIC_RELEASE);
        /* No APs to validate; the global feature intersection is just the BSP's
         * probed feature set (TODO-09-boot S6). Publish it so consumers and the
         * cpu_feature_global_mask() query are valid on single-CPU systems. */
        cpu_features_finalize_global();
        /* Register audit trail (TODO-09-boot S9) runs on EVERY platform: capture
         * + emit the BSP's [CPU0 AUDIT] line and the (trivially consistent)
         * verdict here, since this path returns before the SMP audit below. */
        cpu_audit_registers(0);
        cpu_audit_log(0);
        cpu_audit_consistency_check(smp_cpu_count());
        return;
    }

    /* Initialize BSP per-CPU data first */
    cpu_data[0].self         = &cpu_data[0];
    cpu_data[0].cpu_id       = 0;
    cpu_data[0].lapic_id     = lapic_id();
    smp_publish_cpu_online(&cpu_data[0]);
    cpu_data[0].irq_count    = 0;
    cpu_data[0].preempt_count = 0;
    cpu_data[0].current_irql  = PASSIVE_LEVEL;
    cpu_data[0].current_task  = (void *)0;
    msr_write(MSR_IA32_GS_BASE, (uint64_t)(uintptr_t)&cpu_data[0]);

    /* Find which MADT entry is the BSP */
    for (i = 0; i < cpu_count; i++) {
        const struct cpu_info *ci = acpi_get_cpu_info(i);
        if (ci && ci->is_bsp) {
            bsp_index = i;
            break;
        }
    }

    /* Copy AP trampoline code to physical 0x8000 */
    trampoline_dest = (uint8_t *)(uintptr_t)AP_TRAMPOLINE_ADDR;
    trampoline_size = (uint32_t)(ap_trampoline_end - ap_trampoline_start);

    for (i = 0; i < trampoline_size; i++)
        trampoline_dest[i] = ap_trampoline_start[i];

    klog(LOG_INFO, "smp",
         "Trampoline: %u bytes copied to 0x%x",
         (uint64_t)trampoline_size, (uint64_t)AP_TRAMPOLINE_ADDR);

    /* Get BSP's CR3, GDT, and IDT for AP use */
    cr3 = read_cr3();
    gdt_get_gdtr(gdtr);
    idt_get_idtr(idtr);

    /* Write shared data that's constant across all APs */
    {
        volatile uint8_t *data = (volatile uint8_t *)(uintptr_t)AP_DATA_BASE;
        uint32_t j;

        /* CR3 */
        *(volatile uint64_t *)(data + AP_OFF_CR3) = cr3;

        /* GDT pointer (10 bytes) */
        for (j = 0; j < 10; j++)
            data[AP_OFF_GDT_PTR + j] = gdtr[j];

        /* IDT pointer (10 bytes) */
        for (j = 0; j < 10; j++)
            data[AP_OFF_IDT_PTR + j] = idtr[j];

        /* C entry point */
        *(volatile uint64_t *)(data + AP_OFF_ENTRY) =
            (uint64_t)(uintptr_t)ap_entry;

        /* Canary at offset 0x38 (unused by trampoline) */
        *(volatile uint32_t *)(data + AP_OFF_CANARY) = AP_CANARY_MAGIC;
    }

    /* Runtime verify: read back CR3 and entry point from the data area.
     * If the values don't match what we wrote, the trampoline page was
     * stomped (VBox AP parking firmware, DMA, etc.) or offsets are wrong. */
    {
        volatile uint8_t *data = (volatile uint8_t *)(uintptr_t)AP_DATA_BASE;
        uint64_t rb_cr3   = *(volatile uint64_t *)(data + AP_OFF_CR3);
        uint64_t rb_entry = *(volatile uint64_t *)(data + AP_OFF_ENTRY);
        uint32_t rb_canary = *(volatile uint32_t *)(data + AP_OFF_CANARY);

        if (rb_cr3 != cr3) {
            klog(LOG_FATAL, "smp",
                 "AP trampoline data corruption: CR3 wrote 0x%x, read back 0x%x",
                 cr3, rb_cr3);
            return;
        }
        if (rb_entry != (uint64_t)(uintptr_t)ap_entry) {
            klog(LOG_FATAL, "smp",
                 "AP trampoline data corruption: ENTRY wrote 0x%x, read back 0x%x",
                 (uint64_t)(uintptr_t)ap_entry, rb_entry);
            return;
        }
        if (rb_canary != AP_CANARY_MAGIC) {
            klog(LOG_FATAL, "smp",
                 "AP trampoline canary corrupted: expected 0xDEADC0DE, got 0x%x",
                 (uint64_t)rb_canary);
            return;
        }
    }

    /* Start each AP */
    ap_count = 0;
    for (i = 0; i < cpu_count; i++) {
        const struct cpu_info *ci = acpi_get_cpu_info(i);
        uintptr_t stack_phys;
        int guard_rc;

        if (!ci || !ci->enabled || i == bsp_index)
            continue;

        /* Allocate a per-AP kernel stack from PMM (4 pages + 1 guard = 5 pages).
         * Guard page at the bottom catches stack overflow. */
        stack_phys = pmm_alloc_contiguous(AP_STACK_SIZE / 4096 + 1);
        if (stack_phys == 0) {
            klog(LOG_ERROR, "smp", "Failed to allocate stack for AP %u",
                 (uint64_t)i);
            continue;
        }
        /* Never launch an AP on an unguarded stack. The install is failable
         * (guard table saturated, or no frame for the huge-page split) and
         * fails BEFORE clearing the PTE, so on failure the run is still mapped
         * and frees normally -- but its "guard" would be ordinary writable
         * memory, and an AP stack overflow would cross it silently into
         * whatever sits below. Skipping the AP costs one CPU and says so;
         * launching it costs silent memory corruption. */
        guard_rc = vmm_install_guard_page(stack_phys,
                                          "GUARD: AP kernel stack overflow");
        if (guard_rc != VMM_GUARD_OK) {
            klog(LOG_ERROR, "smp",
                 "No guard page for AP %u stack -- skipping this AP", (uint64_t)i);
            /* VMM_GUARD_VA_UNSAFE means this run's identity VA maps another
             * frame, so it is not reusable by anyone -- quarantine it instead
             * of handing the next PMM consumer a poisoned frame. */
            if (guard_rc != VMM_GUARD_VA_UNSAFE)
                pmm_free_contiguous(stack_phys, AP_STACK_SIZE / 4096 + 1);
            continue;
        }
        stack_phys += 4096;  /* usable stack starts after guard */

        /* Write per-AP shared data */
        {
            volatile uint8_t *data =
                (volatile uint8_t *)(uintptr_t)AP_DATA_BASE;

            /* Stack top (grows down) */
            *(volatile uint64_t *)(data + AP_OFF_STACK) =
                (uint64_t)(stack_phys + AP_STACK_SIZE);

            /* CPU index (logical, not MADT index) */
            ap_count++;
            *(volatile uint32_t *)(data + AP_OFF_CPUID) = ap_count;

            /* Store stack in per-CPU data for TSS */
            cpu_data[ap_count].rsp0 =
                (uint64_t)(stack_phys + AP_STACK_SIZE);

            /* Pre-store this slot's EXPECTED apic_id (TODO-09-boot S10): the AP
             * validates its live LAPIC ID against this before claiming ONLINE,
             * so a slow AP that latched a rewritten shared-AP_DATA cpu_index
             * parks instead of coming online under a mismatched identity. */
            cpu_data[ap_count].lapic_id = ci->apic_id;
        }

        /* Arm the bringup handshake (TODO-09-boot S10): STARTING is the value
         * the AP will CAS to ONLINE; published by the smp_mb() below before the
         * SIPI so the AP observes it. */
        cpu_data[ap_count].ap_bringup_state = AP_BRINGUP_STARTING;

        /* Memory fence to ensure all writes are visible before SIPI */
        smp_mb();

        /* ---- INIT → wait → SIPI → wait → SIPI (retry) ---- */
        klog(LOG_DEBUG, "smp", "Starting AP %u (LAPIC ID=%u)...",
             (uint64_t)ap_count, (uint64_t)ci->apic_id);

        /* Send INIT IPI */
        lapic_send_init(ci->apic_id);
        delay_ms(10);       /* 10ms delay after INIT */

        /* Send first SIPI (vector = 0x08 → phys addr 0x8000) */
        lapic_send_sipi(ci->apic_id, AP_TRAMPOLINE_ADDR >> 12);
        delay_ms(1);        /* 200µs minimum per Intel spec, use 1ms */

        /* Wait for THIS AP to announce READY (TODO-10 S27). The waited-on signal
         * is the AP's readiness, NOT its membership, because membership is now
         * this loop's to grant: an AP that reaches READY has completed all of
         * its local bringup and is parked awaiting the verdict, so there is
         * nothing left for it to do that the BSP could observe half-finished.
         * 100 ms, then a second SIPI and 50 ms, as before. */
        {
            uint32_t timeout = 100;
            while (!ap_bringup_is_ready(ap_count) && timeout > 0) {
                delay_ms(1);
                timeout--;
            }
        }

        if (!ap_bringup_is_ready(ap_count)) {
            /* Retry with second SIPI */
            lapic_send_sipi(ci->apic_id, AP_TRAMPOLINE_ADDR >> 12);
            delay_ms(1);

            /* Wait again (50ms) */
            {
                uint32_t timeout = 50;
                while (!ap_bringup_is_ready(ap_count) && timeout > 0) {
                    delay_ms(1);
                    timeout--;
                }
            }
        }

#ifdef KERNEL_TESTS
        /* Degraded-configuration injection (TODO-10 S27): force this slot down
         * the abandon path even though it may be perfectly healthy, so a live
         * boot can produce live-count < present-count on demand. Placed AFTER
         * the waits so the AP really is sitting in READY when we reject it --
         * that is the interesting case (a healthy AP told to park dark), not an
         * AP that simply never arrived. */
        if (s_inject_abandon_ap && s_inject_abandon_ap == ap_count) {
            klog(LOG_WARN, "smp",
                 "AP %u abandoned by test_abandon_ap injection", (uint64_t)ap_count);
            __atomic_store_n(&cpu_data[ap_count].ap_bringup_state,
                             AP_BRINGUP_ABANDONED, __ATOMIC_RELEASE);
            continue;
        }
#endif

        /* TERMINAL VERDICT (TODO-10 S27). Exactly one of {BSP-ABANDONED,
         * BSP-ONLINE} is reached for this slot, here, before the loop moves on
         * -- so no slot is left in a state a later AP write could change.
         * Publication order is load-bearing: membership FIRST, then the ONLINE
         * store that releases the AP. Reversed, the AP could sti and start
         * taking IPIs before its own mask bit existed, which is the
         * live-but-uncounted state this protocol exists to eliminate. */
        if (smp_bsp_bringup_arbitrate(&cpu_data[ap_count].ap_bringup_state)) {
            smp_publish_cpu_online(&cpu_data[ap_count]);
            smp_ap_bringup_accept(&cpu_data[ap_count].ap_bringup_state);
        } else {
            klog(LOG_WARN, "smp",
                 "AP %u abandoned (bringup timeout) -- parked, not counted",
                 (uint64_t)ap_count);
        }
    }

    /* Emit each online AP's "online" line + CPU-hardening audit from the BSP,
     * gated on that AP's own online-mask bit. The mask is the single
     * publication point (release-written last by the AP), so the live count and
     * the audit derive from the same word -- they cannot diverge, and the
     * acquire/release edge makes the AP's lapic_id + ap_cpu_harden() snapshot
     * visible here. An AP that never published is neither counted nor audited
     * (consistent). BSP-side emission
     * keeps unbounded serial I/O off the AP bringup critical path. */
    /* CPU register audit trail (TODO-09-boot S9): capture + emit the BSP's
     * consolidated [CPU0 AUDIT] line here (Phase 2, IDT loaded -> msr_try_read
     * is live -- it could NOT run at end of Phase 0). APs captured their own
     * snapshot at the ap_cpu_harden() tail; the BSP emits each AP's line in the
     * loop below from the buffered per_cpu_data. */
    cpu_audit_registers(0);
    cpu_audit_log(0);

    /* The DISCOVERED slot count, not the online one: this is what the machine
     * has, and it must not move when a CPU parks later (TODO-10 S21). The live
     * count comes from the online mask, which each AP published for itself. */
    __atomic_store_n(&present_cpus, 1u + ap_count, __ATOMIC_RELEASE);

    for (i = 1; i <= ap_count; i++) {
        if (smp_cpu_is_online(i)) {
            klog(LOG_INFO, "smp", "AP %u online (LAPIC ID=%u)",
                 (uint64_t)i, (uint64_t)cpu_data[i].lapic_id);
            ap_cpu_harden_log(i);
            cpu_audit_log(i);
        } else {
            klog(LOG_WARN, "smp", "AP %u did not respond", (uint64_t)i);
        }
    }

    /* If any AP failed feature validation it recorded the fault and halted
     * (it could not bug-check itself safely); raise 0x3E here on the BSP where
     * the panic path is feature-safe (TODO-09-boot S6). No-op if all APs passed. */
    cpu_features_check_ap_faults();

    /* Publish the global feature intersection now that the online set is fixed
     * (TODO-09-boot S6). Done here, after the is_online acquire pass above, so
     * a late/timed-out AP cannot downgrade the published mask. */
    cpu_features_finalize_global();

    /* Per-CPU register consistency verdict (TODO-09-boot S9), after the online
     * set is fixed: one [SMP] All N CPUs register-consistent line or per-CPU
     * divergence WARNs. */
    cpu_audit_consistency_check(smp_cpu_count());

    /* Arm the periodic CR-pin verify-IPI (TODO-09-boot S10): the online set is
     * fixed and APs are parked with IRQs enabled, so the BSP timer tick can now
     * broadcast re-verify IPIs that the APs service. */
    cpu_cr_verify_ipi_init();

    /* Layer 2 (runtime verification at init) for the two-word membership
     * design. The subset invariant -- every mask bit implies is_online -- is
     * asserted in prose and unit-tested over synthetic words, neither of which
     * can observe the LIVE kernel state where the two words could actually
     * diverge. Checked here, once, where the online set is fixed. A violation
     * is a WARN rather than a halt: the mask is the conservative word, so a
     * divergence under-reports rather than dispatching to a dead CPU. */
    {
        uint32_t mask = smp_online_mask();
        uint32_t bad = 0;
        for (i = 0; i < MAX_CPUS; i++) {
            if (smp_mask_test(mask, i) &&
                !__atomic_load_n(&cpu_data[i].is_online, __ATOMIC_ACQUIRE))
                bad++;
        }
        if (bad)
            klog(LOG_WARN, "smp",
                 "%u CPU(s) carry an online-mask bit without is_online -- "
                 "membership publication is inconsistent", (uint64_t)bad);
    }

    /* Layer 2 for the terminality property (TODO-10 S27). The unit tests prove
     * the state machine over synthetic words; only here can the LIVE claim be
     * checked -- that every discovered slot reached a verdict before this
     * function returns, so nothing can still join the live set afterwards. A
     * non-terminal slot means an AP is sitting in READY (or was never
     * arbitrated) with the loop already past it, which is precisely the
     * live-but-late join this section closes. WARN rather than halt: the slot
     * has no membership either way, so the machine is under-populated rather
     * than unsafe, and halting a boot over an under-populated CPU set would be
     * a worse outcome than reporting it. */
    {
        uint32_t nonterminal = 0;
        for (i = 1; i <= ap_count && i < MAX_CPUS; i++) {
            if (!smp_ap_bringup_is_terminal(&cpu_data[i].ap_bringup_state))
                nonterminal++;
        }
        if (nonterminal)
            klog(LOG_WARN, "smp",
                 "%u AP slot(s) left bringup without a terminal verdict -- "
                 "the live set is not sealed", (uint64_t)nonterminal);
    }

    {
        uint32_t live = smp_cpu_count();
        klog(LOG_INFO, "smp", "%u of %u CPUs online (BSP + %u APs)",
             (uint64_t)live, (uint64_t)smp_cpu_present_count(),
             (uint64_t)(live - 1));
    }
}

/* ---- Online membership + async claim (TODO-10 S21) ---- */

void smp_mask_set(uint32_t *mask, uint32_t cpu)
{
    if (!mask || cpu >= MAX_CPUS)
        return;
    __atomic_fetch_or(mask, 1u << cpu, __ATOMIC_RELEASE);
}

void smp_mask_clear(uint32_t *mask, uint32_t cpu)
{
    if (!mask || cpu >= MAX_CPUS)
        return;
    __atomic_fetch_and(mask, ~(1u << cpu), __ATOMIC_RELEASE);
}

int smp_mask_test(uint32_t mask, uint32_t cpu)
{
    if (cpu >= MAX_CPUS)
        return 0;
    return (mask & (1u << cpu)) ? 1 : 0;
}

/* Population count by shift-and-add rather than __builtin_popcount: the
 * freestanding kernel links no compiler-rt, and clang lowers the builtin to a
 * __popcountsi2 call whenever it cannot prove POPCNT is available. */
uint32_t smp_mask_count(uint32_t mask)
{
    uint32_t n = 0;
    while (mask) {
        mask &= mask - 1u;
        n++;
    }
    return n;
}

/* ---- Bringup arbitration (TODO-10 S27) ----
 *
 * Four pure transitions over a caller-supplied handshake word. Keeping them
 * word-local rather than pcpu-local is what makes the terminality property
 * testable: the failure this protocol prevents needs an AP stalled at an exact
 * instruction, which no boot on any emulator this repo runs can be asked to
 * produce, but every reachable interleaving of these four is reachable over
 * synthetic words. */

int smp_ap_bringup_ready(uint32_t *state)
{
    uint32_t expected = AP_BRINGUP_STARTING;

    if (!state)
        return 0;

    /* ACQ_REL: the AP's whole local bringup (lapic_id, the ap_cpu_harden()
     * register/feature snapshot) precedes this and must be visible to the BSP
     * that observes READY, because the BSP -- not the AP -- is what publishes
     * this slot's membership afterwards. */
    return __atomic_compare_exchange_n(state, &expected, AP_BRINGUP_READY, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

int smp_bsp_bringup_arbitrate(uint32_t *state)
{
    uint32_t expected = AP_BRINGUP_STARTING;

    if (!state)
        return 0;

    /* Try to REJECT first, and let the failure tell us what the slot really is.
     * Ordering it this way is what removes the window: there is no load-then-act
     * gap for the AP to slip through, because the only value that can defeat
     * this CAS is one the AP has already committed to (READY), and the AP never
     * leaves READY on its own. Reading the word first and branching would
     * reintroduce exactly the race the section closes. */
    if (__atomic_compare_exchange_n(state, &expected, AP_BRINGUP_ABANDONED, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;   /* we abandoned it -- a late AP now loses its own CAS */

    /* The CAS failed, so `expected` holds the observed value. READY means the
     * AP got there first and the caller must publish. ONLINE means this slot
     * was already accepted (re-arbitration is idempotent, not a second
     * publication). ABANDONED means it was already rejected. */
    if (expected == AP_BRINGUP_READY || expected == AP_BRINGUP_ONLINE)
        return 1;
    return 0;
}

void smp_ap_bringup_accept(uint32_t *state)
{
    if (!state)
        return;

    /* RELEASE, and the ordering here is the whole contract: membership is
     * already published, and this store is what lets the AP leave its wait and
     * enable interrupts. Publishing AFTER it would re-open the live-but-
     * unpublished window. A plain store is correct because the BSP is the sole
     * writer of ONLINE and it only reaches here having observed READY, which
     * the AP never leaves. */
    __atomic_store_n(state, AP_BRINGUP_ONLINE, __ATOMIC_RELEASE);
}

int smp_ap_bringup_is_terminal(const uint32_t *state)
{
    uint32_t s;

    if (!state)
        return 0;
    s = __atomic_load_n(state, __ATOMIC_ACQUIRE);
    return s == AP_BRINGUP_ONLINE || s == AP_BRINGUP_ABANDONED;
}

void smp_publish_cpu_online(struct per_cpu_data *pcpu)
{
    if (!pcpu)
        return;

    /* A slot past the mask's width would go live with no mask bit -- the
     * live-but-uncounted state this section exists to eliminate, produced
     * silently. Unreachable today (acpi.c caps discovery at MAX_CPUS and the
     * _Static_assert pins MAX_CPUS <= the mask width), so this is a loud
     * backstop rather than a live path, but a bare return here is exactly the
     * silence that would make the next bug unfindable. */
    if (pcpu->cpu_id >= MAX_CPUS) {
        klog(LOG_ERROR, "smp",
             "CPU %u is past MAX_CPUS (%u) -- cannot publish online membership",
             (uint64_t)pcpu->cpu_id, (uint64_t)MAX_CPUS);
        return;
    }
    /* The MASK BIT IS THE PUBLICATION POINT, and it is written LAST. Every
     * wait loop in bringup keys off it (smp_cpu_is_online), so an AP stalled
     * by an SMI part-way through this sequence cannot be observed as complete:
     * before this section the BSP waited on is_online, which meant a stall
     * after that store let smp_init finish, topology_init cache the CPU as
     * permanently offline, and the first async group omit a CPU that then went
     * live. Writing the mask last also keeps the mask a SUBSET of is_online,
     * so no consumer can ever count a CPU the per-CPU flag does not claim. */
    smp_async_claim_publish_idle(&pcpu->async_claim);
    __atomic_store_n(&pcpu->is_online, 1u, __ATOMIC_RELEASE);
    smp_mask_set(&online_mask, pcpu->cpu_id);
}

void smp_retract_cpu_online(struct per_cpu_data *pcpu)
{
    if (!pcpu)
        return;

    /* The GLOBAL word is gated on a GS-INDEPENDENT identity check; the per-CPU
     * words are not. The gate was added when the panic async-fault branch
     * reached its pcpu through smp_this_cpu(), which SILENTLY substitutes
     * &cpu_data[0] when GS reads zero -- and the mask clear is indexed by
     * pcpu->cpu_id, so that fallback would clear the BSP's bit and drop a
     * RUNNING BSP out of every membership consumer.
     *
     * THAT CALLER NO LONGER TOUCHES GS: it resolves its slot through
     * smp_cpu_by_apic_id(). The gate stays anyway, and the reason is now a
     * different and better one -- it makes the guarantee LOCAL to this function
     * instead of inherited from one caller's discipline, and a future caller
     * arriving with a GS-derived pointer is exactly what it must keep refusing.
     * cpu_panic_safe_apic_id() derives the id from CPUID, which no GS state can
     * corrupt -- the identity rule section 20 adopted for the serial lock.
     *
     * Scoping the check to the mask alone is deliberate: a CPU that cannot
     * prove its identity must not edit shared state, but the per-CPU park and
     * is_online clear below are exactly what this path did before this section,
     * so refusing them as well would trade a narrow new hazard for the old
     * 10-second async stall.
     *
     * Mask bit FIRST, so no window exists in which a consumer still counts a
     * CPU that has already stopped answering. Panic-path safe: three atomics,
     * no lock, no allocation, no klog. */
    /* MATCHED IN THE PANIC-SAFE DOMAIN, not against lapic_id. This test used to
     * compare `pcpu->lapic_id` with cpu_panic_safe_apic_id(), which are
     * different derivations -- the MADT/LAPIC-register id against the CPUID
     * leaf-1 initial apic id -- and cpu_security.h is explicit that a panic-safe
     * id is only ever compared with another panic-safe id. They agree on every
     * machine this repo supports, which is what would have made the failure
     * silent and confined to the firmware that remaps them: the caller resolves
     * its slot in the panic-safe domain and succeeds, this test then refuses,
     * and the CPU halts while still set in the online mask. Every later async
     * group would select it and wait out the full barrier deadline for a CPU
     * that can never answer, which is the stall the retract exists to prevent.
     *
     * A slot whose id was never published reads 0 and matches nothing, so the
     * mask clear is refused there too. That is the conservative direction: an
     * unidentifiable CPU must not edit shared state. */
    if (smp_retract_may_clear_mask(pcpu->panic_safe_id_plus1,
                                   cpu_panic_safe_apic_id()))
        smp_mask_clear(&online_mask, pcpu->cpu_id);
    smp_async_claim_park(&pcpu->async_claim);
    __atomic_store_n(&pcpu->is_online, 0u, __ATOMIC_RELEASE);
}

#ifdef KERNEL_TESTS
int smp_test_park_cpu(uint32_t cpu)
{
    struct per_cpu_data *pcpu;
    uint32_t live_before, present_before, topo_before, mask_before;
    uint32_t live_after,  present_after,  topo_after,  mask_after;
    int ok = 1;

    /* IT PARKS THE BOOKKEEPING, NOT THE CPU. panic.c's park is real because the
     * CPU that calls it is on its way to halting with interrupts masked; this
     * one runs on the BSP against a target that is sitting in `sti; hlt` and
     * stays there, taking interrupts. So after the injection the kernel
     * believes a still-executing CPU is gone -- which is exactly the state the
     * accounting consumers must survive and therefore what this exercises, but
     * it is NOT a model of a dead CPU. One consequence worth naming: the CR0/CR4
     * pin re-verify IPI filters on is_online, so the injected CPU permanently
     * drops out of security re-verification for the rest of that boot. There is
     * no unpark path. Test-only, one-shot, and never on a released kernel.
     *
     * Slot 0 is the BSP and is never a legal target: parking the CPU running
     * this code would retract the caller out from under itself. */
    /* A REJECTED TARGET IS A TEST FAILURE, NOT A NO-OP. Returning 0 silently
     * here let a requested degraded-CPU scenario simply not happen while the
     * boot continued to a clean "Boot complete" -- so a completion-based
     * harness would pass having proven nothing. Every rejection says so, with
     * the reason, on the same [S27] marker the success path uses. */
    if (cpu == 0 || cpu >= MAX_CPUS) {
        klog(LOG_ERROR, "SMP", "[S27] park REJECTED: CPU%u is not a legal "
             "target (slot 0 is the BSP; max is %u)", cpu, (uint32_t)MAX_CPUS - 1);
        return 0;
    }
    pcpu = smp_get_cpu(cpu);
    if (!pcpu) {
        klog(LOG_ERROR, "SMP", "[S27] park REJECTED: no per-CPU block for CPU%u",
             cpu);
        return 0;
    }
    if (!smp_cpu_is_online(cpu)) {
        klog(LOG_ERROR, "SMP", "[S27] park REJECTED: CPU%u is not online, so "
             "parking it would prove nothing about a live-count fall", cpu);
        return 0;
    }

    live_before    = smp_cpu_count();
    present_before = smp_cpu_present_count();
    topo_before    = g_topo_cpu_count;
    mask_before    = smp_online_mask();

    /* NOT smp_retract_cpu_online(): that gates its mask clear on the CALLER's
     * own CPUID identity, because its only real caller is a CPU parking ITSELF
     * from the panic path. The BSP retracting a DIFFERENT slot fails that check
     * and would clear is_online while leaving the mask bit set -- the exact
     * mask/is_online divergence the publication order exists to prevent. This
     * is an orderly BSP-driven injection, not a self-park, so it performs the
     * same three atomics in the same order, indexed by the TARGET.
     *
     * Mask bit FIRST (the publication point), then the async claim, then
     * is_online -- so no consumer can observe a CPU counted as live that has
     * already stopped answering. */
    smp_mask_clear(&online_mask, cpu);
    smp_async_claim_park(&pcpu->async_claim);
    __atomic_store_n(&pcpu->is_online, 0u, __ATOMIC_RELEASE);

    live_after    = smp_cpu_count();
    present_after = smp_cpu_present_count();
    topo_after    = g_topo_cpu_count;
    mask_after    = smp_online_mask();

    /* The four relations section 27 exists to prove. A CPU count is not a slot
     * bound and the two counts are not interchangeable: LIVE falls when a CPU
     * parks, PRESENT is the discovery snapshot and must not move, and topology
     * keeps its discovered slot binding either way. */
    if (live_after + 1u != live_before) {
        klog(LOG_ERROR, "SMP", "[S27] live count %u -> %u parking CPU%u "
             "(expected a fall of exactly 1)", live_before, live_after, cpu);
        ok = 0;
    }
    if (present_after != present_before) {
        klog(LOG_ERROR, "SMP", "[S27] PRESENT count moved %u -> %u parking "
             "CPU%u -- the discovery snapshot must not track liveness",
             present_before, present_after, cpu);
        ok = 0;
    }
    if (topo_after != topo_before) {
        klog(LOG_ERROR, "SMP", "[S27] topology count moved %u -> %u parking "
             "CPU%u -- slot bindings are discovery-scoped",
             topo_before, topo_after, cpu);
        ok = 0;
    }
    if (mask_after & (1u << cpu)) {
        klog(LOG_ERROR, "SMP", "[S27] CPU%u still set in the online mask "
             "(0x%x) after parking", cpu, mask_after);
        ok = 0;
    }
    /* The mask IS the live count's source, and every NT-facing consumer
     * (PEB NumberOfProcessors, the NUMBER_OF_PROCESSORS registry/env value)
     * reads smp_cpu_count(). Pinning popcount(mask) == live count is therefore
     * what makes "NtQuerySystemInformation agrees with the mask" hold, rather
     * than asserting each consumer's copy separately. */
    if (smp_mask_count(mask_after) != live_after) {
        klog(LOG_ERROR, "SMP", "[S27] online mask 0x%x has %u bits but the live "
             "count reads %u -- NT processor reporting would disagree",
             mask_after, smp_mask_count(mask_after), live_after);
        ok = 0;
    }
    if (smp_cpu_is_online(cpu)) {
        klog(LOG_ERROR, "SMP", "[S27] CPU%u still reports online after parking",
             cpu);
        ok = 0;
    }

    klog(ok ? LOG_INFO : LOG_ERROR, "SMP",
         "[S27] parked CPU%u: live %u->%u, present %u (held), topo %u (held), "
         "mask 0x%x->0x%x -- %s",
         cpu, live_before, live_after, present_after, topo_after,
         mask_before, mask_after, ok ? "consistent" : "INCONSISTENT");
    return ok;
}
#endif /* KERNEL_TESTS */

int smp_async_claim_dispatch(uint32_t *claim, uint32_t *out_gen)
{
    uint32_t cur;

    if (!claim)
        return 0;

    cur = __atomic_load_n(claim, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t gen, next;

        /* OFFLINE means the CPU cannot answer; BUSY means a dispatch this
         * word still owns never completed (a timed-out worker the BSP
         * deliberately left running); RESERVED means another dispatch is
         * mid-publication. None is dispatchable, and BUSY staying un-reusable
         * is the point of the claim, not a leak. */
        if (SMP_ASYNC_STATE_OF(cur) != SMP_ASYNC_IDLE)
            return 0;

        gen  = (SMP_ASYNC_GEN_OF(cur) + 1u) & SMP_ASYNC_GEN_MAX;
        next = SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, gen);

        if (__atomic_compare_exchange_n(claim, &cur, next, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (out_gen)
                *out_gen = gen;
            return 1;
        }
        /* cur was reloaded with the observed value; loop re-tests the state. */
    }
}

int smp_async_claim_arm(uint32_t *claim, uint32_t gen)
{
    uint32_t expect;

    if (!claim)
        return 0;

    /* RELEASE on success: everything the caller wrote to the payload while the
     * slot was RESERVED must be visible to the AP that observes BUSY. */
    expect = SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, gen);
    return __atomic_compare_exchange_n(claim, &expect,
                                       SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, gen), 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
           ? 1 : 0;
}

int smp_async_claim_complete(uint32_t *claim, uint32_t gen)
{
    uint32_t expect;

    if (!claim)
        return 0;

    expect = SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, gen);
    return __atomic_compare_exchange_n(claim, &expect,
                                       SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, gen), 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
           ? 1 : 0;
}

void smp_async_claim_park(uint32_t *claim)
{
    uint32_t cur;

    if (!claim)
        return;

    cur = __atomic_load_n(claim, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t next = SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, SMP_ASYNC_GEN_OF(cur));

        if (cur == next)
            return;   /* already parked at this generation */
        if (__atomic_compare_exchange_n(claim, &cur, next, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
    }
}

void smp_async_claim_publish_idle(uint32_t *claim)
{
    uint32_t cur;

    if (!claim)
        return;

    cur = __atomic_load_n(claim, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t next;

        /* ONLY from OFFLINE. Republishing a RESERVED or BUSY slot as IDLE
         * would erase an in-flight worker's ownership and let the next group
         * dispatch onto a CPU still running the previous step. */
        if (SMP_ASYNC_STATE_OF(cur) != SMP_ASYNC_OFFLINE)
            return;

        next = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, SMP_ASYNC_GEN_OF(cur));
        if (__atomic_compare_exchange_n(claim, &cur, next, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
    }
}

int smp_async_claim_is_dead(const uint32_t *claim, uint32_t gen)
{
    uint32_t w;

    if (!claim)
        return 0;

    w = __atomic_load_n(claim, __ATOMIC_ACQUIRE);
    return (SMP_ASYNC_STATE_OF(w) == SMP_ASYNC_OFFLINE &&
            SMP_ASYNC_GEN_OF(w) == (gen & SMP_ASYNC_GEN_MAX)) ? 1 : 0;
}

int smp_async_claim_is_busy(const uint32_t *claim, uint32_t *out_gen)
{
    uint32_t w;

    if (!claim)
        return 0;

    w = __atomic_load_n(claim, __ATOMIC_ACQUIRE);
    if (SMP_ASYNC_STATE_OF(w) != SMP_ASYNC_BUSY)
        return 0;
    if (out_gen)
        *out_gen = SMP_ASYNC_GEN_OF(w);
    return 1;
}

/* ---- Query API ---- */

uint32_t smp_cpu_count(void)
{
    uint32_t n = smp_mask_count(__atomic_load_n(&online_mask, __ATOMIC_ACQUIRE));
    /* The BSP is online before smp_early_bsp_init() has published anything
     * (Phase 0 code calls this), so the floor is 1 -- same guarantee the old
     * one-time snapshot gave. */
    return n > 0 ? n : 1;
}

uint32_t smp_cpu_present_count(void)
{
    uint32_t n = __atomic_load_n(&present_cpus, __ATOMIC_ACQUIRE);
    return n > 0 ? n : 1;
}

uint32_t smp_online_mask(void)
{
    return __atomic_load_n(&online_mask, __ATOMIC_ACQUIRE);
}

int smp_cpu_is_online(uint32_t cpu)
{
    return smp_mask_test(__atomic_load_n(&online_mask, __ATOMIC_ACQUIRE), cpu);
}

uint32_t smp_cpu_id(void)
{
    return smp_this_cpu()->cpu_id;
}

struct per_cpu_data *smp_this_cpu(void)
{
    struct per_cpu_data *pcpu;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(pcpu));
    /* If GS isn't set up yet, fall back to BSP */
    if (!pcpu)
        return &cpu_data[0];
    return pcpu;
}

struct per_cpu_data *smp_get_cpu(uint32_t cpu_id)
{
    if (cpu_id >= MAX_CPUS)
        return (struct per_cpu_data *)0;
    return &cpu_data[cpu_id];
}

/* OWNER TABLE FOR THE PANIC-SAFE ID SPACE: index = the 8-bit id, value = the
 * owning logical CPU index PLUS ONE, 0 = unclaimed.
 *
 * A TABLE RATHER THAN A SCAN, and a compare-exchange rather than check-then-act,
 * because AP bringup is NOT serialised the way an earlier version of this code
 * asserted. The BSP waits a bounded time for READY and then ABANDONS that AP and
 * sends the next SIPI (see the bringup loop), so a late AP can still be executing
 * ap_entry while its successor starts. Two APs reporting the same CPUID id could
 * therefore both scan, both find the id free, and both publish -- after which a
 * lookup resolves to whichever slot the scan reached first, and a fault on the
 * other one retracts and publishes completion into a CPU that is not it.
 *
 * The claim is for the slot's lifetime and is never released: the id space is a
 * hardware property, and a CPU that owned an id does not stop owning it. */
static volatile uint32_t s_panic_id_owner[CPU_PANIC_SAFE_ID_COUNT];

_Static_assert(MAX_CPUS < 0xFFFFFFFFu,
               "cpu index plus one must not overflow the owner encoding");

int smp_retract_may_clear_mask(uint32_t slot_panic_id_plus1,
                               uint32_t observed_panic_id)
{
    /* An unpublished slot encodes 0 and matches nothing, so it falls out here
     * without a special case: 0 can never equal a masked id plus one. */
    return slot_panic_id_plus1 ==
           (observed_panic_id & CPU_PANIC_SAFE_ID_MASK) + 1u;
}

int smp_publish_panic_safe_id(struct per_cpu_data *pcpu)
{
    if (!pcpu)
        return 0;

    /* Derived HERE, on the CPU being described, rather than passed in: the
     * whole guarantee is that the value came from this CPU's own CPUID, so a
     * caller cannot publish an identity it merely believes. */
    {
        uint32_t id    = cpu_panic_safe_apic_id() & CPU_PANIC_SAFE_ID_MASK;
        uint32_t want  = id + 1u;
        uint32_t mine  = pcpu->cpu_id + 1u;
        uint32_t owner = 0u;

        /* AMBIGUITY IS REFUSED RATHER THAN PUBLISHED. Two CPUs cannot report the
         * same CPUID leaf-1 initial apic id on any configuration this repo
         * supports -- cpu_security.h states outright that the 8-bit id aliases
         * only above 255 logical CPUs, and x2APIC systems that large are
         * unsupported repo-wide -- so this branch is a net for a broken or
         * lying platform, not an expected path.
         *
         * DECLINING TO PUBLISH IS NOT ON ITS OWN A SAFE OUTCOME: a lookup for
         * the duplicate id does not return NULL, it returns the CPU that DID
         * claim it. So an unpublished CPU that went on to run async work would,
         * on faulting, retract and publish completion into a LIVE CPU's
         * lifecycle fields and leave itself online. The refusal is half the fix;
         * the caller parking the AP is the other half, and that is why this
         * reports rather than failing quietly.
         *
         * AND THE CLAIM MUST BE ATOMIC, which a scan-then-store was not. An
         * earlier version argued a scan was sufficient because AP bringup is
         * serialised. IT IS NOT, on the path that matters: the BSP waits a
         * bounded time for READY and then ABANDONS that AP and sends the next
         * SIPI, so a late AP can still be executing ap_entry while its successor
         * starts. Two APs reporting the same id could both scan, both find it
         * free, and both publish. */
        /* ONE compare-exchange decides ownership of the id across ALL slots.
         * A CAS on this CPU's own field could not: the conflict is with a
         * DIFFERENT slot, which no operation on this one can observe. */
        if (!__atomic_compare_exchange_n(&s_panic_id_owner[id], &owner, mine, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (owner != mine) {
                klog(LOG_ERROR, "SMP",
                     "panic-safe id %u already owned by cpu %u -- cpu %u refused",
                     (uint64_t)id, (uint64_t)(owner - 1u),
                     (uint64_t)pcpu->cpu_id);
                return 0;
            }
            /* Already ours: publishing twice for the same CPU is idempotent. */
        }

        __atomic_store_n(&pcpu->panic_safe_id_plus1, want, __ATOMIC_RELEASE);
    }
    return 1;
}

struct per_cpu_data *smp_cpu_by_apic_id(uint32_t apic_id)
{
    uint32_t owner = __atomic_load_n(&s_panic_id_owner[apic_id &
                                                       CPU_PANIC_SAFE_ID_MASK],
                                     __ATOMIC_ACQUIRE);

    /* ONE INDEXED LOAD, into this file's own BSS. That is the whole value of
     * this function: it can answer however corrupt the executing CPU's GS is,
     * because it touches no per-CPU block to find one. It replaced a linear
     * scan over cpu_data[], which was both slower and unable to detect two
     * slots claiming the same id -- it simply returned whichever came first.
     *
     * KEYED ON THE PANIC-SAFE DOMAIN AND NOT ON lapic_id. Those are different
     * derivations -- CPUID leaf-1 initial apic id against the MADT/LAPIC
     * register id -- and cpu_security.h is explicit that a panic-safe id is
     * only ever compared with another panic-safe id. They agree on every
     * machine this repo supports, which is exactly what would make a
     * cross-domain compare fail silently and only on the firmware that remaps
     * them.
     *
     * 0 means unclaimed, so an id no CPU published resolves to NULL rather
     * than to slot 0. */
    if (owner == 0u)
        return (struct per_cpu_data *)0;
    return smp_get_cpu(owner - 1u);
}
