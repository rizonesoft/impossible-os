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
 * the SOLE active-membership API: per_cpu_data.is_online remains the AP's own
 * bringup publication word (the BSP's per-AP wait loops key off it), but every
 * consumer asking "which CPUs are active" reads this mask, so a parked CPU
 * disappears from the system's view of itself the moment it parks.
 *
 * The two words cannot disagree in the dangerous direction: publish sets
 * is_online BEFORE the mask bit and retract clears the mask bit BEFORE
 * is_online, so the mask is always a SUBSET of the true online set. An
 * interleaving can under-report a CPU that is coming up; none can report a
 * parked CPU as active. */
static uint32_t            online_mask = 0;

/* CPU slots bringup DISCOVERED (1 + APs enumerated from the MADT). Fixed once
 * smp_init() returns; this is the machine's configuration, not its live state.
 * Kept separate from the online mask because a consumer asking "how big is
 * this machine" and one asking "how many CPUs can run work" got the same
 * answer before this section, and that answer was wrong for one of them. */
static uint32_t            present_cpus = 0;

_Static_assert(MAX_CPUS <= 32,
    "online_mask is a uint32_t -- one bit per logical CPU slot");

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

    /* Publish this AP as online with a RELEASE store, as the LAST write before
     * parking. is_online is the SINGLE source of truth the BSP uses to (a) end
     * its per-AP bringup wait, (b) count total_cpus, and (c) gate the per-AP
     * audit -- there is no separate count hint that could race ahead of this
     * publication. An acquire-load that observes is_online==1 is guaranteed to
     * see every preceding write (lapic_id, the ap_cpu_harden() snapshot); an
     * AP that never reached here has is_online==0, so the BSP's waited / counted
     * / audited sets are identical. The AP emits NO serial output from here to
     * `sti` (klog busy-waits on the UART with IRQs masked); the BSP emits the
     * "online" line + hardening audit from the buffered per_cpu_data after
     * bringup -- see smp_init. */
    /* Bringup handshake (TODO-09-boot S10): claim ONLINE via CAS. If the BSP
     * already CAS'd us to ABANDONED (its per-AP wait timed out), we LOST the
     * race -- do NOT publish is_online and do NOT sti; park dark so a CPU the
     * BSP gave up on never goes live-but-uncounted (no scheduler visibility, no
     * stray IPI handling). Exactly one of {AP-ONLINE, BSP-ABANDONED} wins. */
    {
        uint32_t expected = AP_BRINGUP_STARTING;
        if (!__atomic_compare_exchange_n(&pcpu->ap_bringup_state, &expected,
                                         AP_BRINGUP_ONLINE, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            /* Abandoned -- park dark, never online. */
            for (;;)
                __asm__ volatile("cli; hlt");
        }
    }

    /* Won the handshake -- publish online (RELEASE; the BSP's acquire-load sees
     * every preceding write) as the LAST write before going live. The claim
     * word goes IDLE and the online-mask bit is set in the same helper, so this
     * AP becomes dispatchable and countable in one place. */
    smp_publish_cpu_online(pcpu);

    /* AP is parked -- enable interrupts and halt.
     * The LAPIC timer or IPI will wake it when the scheduler is ready. */
    __asm__ volatile("sti");
    for (;;)
        __asm__ volatile("hlt");
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

        /* Wait for THIS AP to publish online (its own online-mask bit, which
         * smp_publish_cpu_online writes LAST; timeout 100ms). Polling the
         * per-AP publication point -- not a separate cumulative count -- means
         * the BSP never proceeds while an AP is mid-publication, and the
         * wait/count/audit all key off one signal (TODO-10 S21: that signal is
         * the mask bit, because is_online lands before the claim word and the
         * mask and so could be observed with the sequence unfinished). */
        {
            uint32_t timeout = 100;
            while (!smp_cpu_is_online(ap_count) && timeout > 0) {
                delay_ms(1);
                timeout--;
            }
        }

        if (!smp_cpu_is_online(ap_count)) {
            /* Retry with second SIPI */
            lapic_send_sipi(ci->apic_id, AP_TRAMPOLINE_ADDR >> 12);
            delay_ms(1);

            /* Wait again (50ms) */
            {
                uint32_t timeout = 50;
                while (!smp_cpu_is_online(ap_count) && timeout > 0) {
                    delay_ms(1);
                    timeout--;
                }
            }
        }

        /* Bringup verdict (TODO-09-boot S10): if the AP still has not published
         * online, CAS its handshake STARTING->ABANDONED. Winning the CAS means
         * the AP has not yet claimed ONLINE, so a late arrival will lose its own
         * CAS and park dark instead of going live-but-uncounted. Losing the CAS
         * means the AP claimed ONLINE in the publication gap (between its ONLINE
         * CAS and its online-mask publication) -- it is committed to going live,
         * so we MUST wait (bounded) for that publication to land before the
         * count/audit/feature pass below runs. Skipping the wait would let the
         * count loop observe the CPU as offline and omit an AP that is about to
         * sti and handle IPIs -- the live-but-uncounted state this whole
         * protocol exists to prevent. ONLINE is the only state the failing CAS can observe: the
         * AP is the sole setter of ONLINE and the BSP is the sole setter of
         * ABANDONED, which we just failed to set. */
        if (!smp_cpu_is_online(ap_count)) {
            uint32_t expected = AP_BRINGUP_STARTING;
            if (__atomic_compare_exchange_n(&cpu_data[ap_count].ap_bringup_state,
                                            &expected, AP_BRINGUP_ABANDONED, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                klog(LOG_WARN, "smp",
                     "AP %u abandoned (bringup timeout) -- parked, not counted",
                     (uint64_t)ap_count);
            } else if (expected == AP_BRINGUP_ONLINE) {
                uint32_t timeout = 100;
                while (!smp_cpu_is_online(ap_count) && timeout > 0) {
                    delay_ms(1);
                    timeout--;
                }
            }
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

void smp_publish_cpu_online(struct per_cpu_data *pcpu)
{
    if (!pcpu)
        return;
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
    /* Mask bit FIRST, so no window exists in which a consumer still counts a
     * CPU that has already stopped answering. Panic-path safe: three atomics,
     * no lock, no allocation, no klog. */
    smp_mask_clear(&online_mask, pcpu->cpu_id);
    smp_async_claim_park(&pcpu->async_claim);
    __atomic_store_n(&pcpu->is_online, 0u, __ATOMIC_RELEASE);
}

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
