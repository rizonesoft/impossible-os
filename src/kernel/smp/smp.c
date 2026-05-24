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
static uint32_t            total_cpus = 0;

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
    cpu_data[0].is_online     = 1;
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
    pcpu->self = pcpu;  /* self-pointer for gs:0 access */

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

    /* Fill per-CPU data (is_online is published LAST, below, as the
     * authoritative online flag -- see the release store before sti). */
    pcpu->cpu_id     = cpu_index;
    pcpu->lapic_id   = lapic_id();
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
    __atomic_store_n(&pcpu->is_online, 1, __ATOMIC_RELEASE);

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
        cpu_data[0].is_online    = 1;
        cpu_data[0].irq_count    = 0;
        cpu_data[0].preempt_count = 0;
        cpu_data[0].current_irql  = PASSIVE_LEVEL;
        cpu_data[0].current_task  = (void *)0;
        msr_write(MSR_IA32_GS_BASE, (uint64_t)(uintptr_t)&cpu_data[0]);
        total_cpus = 1;
        /* No APs to validate; the global feature intersection is just the BSP's
         * probed feature set (TODO-09-boot S6). Publish it so consumers and the
         * cpu_feature_global_mask() query are valid on single-CPU systems. */
        cpu_features_finalize_global();
        return;
    }

    /* Initialize BSP per-CPU data first */
    cpu_data[0].self         = &cpu_data[0];
    cpu_data[0].cpu_id       = 0;
    cpu_data[0].lapic_id     = lapic_id();
    cpu_data[0].is_online    = 1;
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
        vmm_install_guard_page(stack_phys, "GUARD: AP kernel stack overflow");
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
        }

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

        /* Wait for THIS AP to publish online (acquire-load its own is_online
         * flag; timeout 100ms). Polling the per-AP authoritative flag -- not a
         * separate cumulative count -- means the BSP never proceeds while an AP
         * is mid-publication, and the wait/count/audit all key off one signal. */
        {
            uint32_t timeout = 100;
            while (!__atomic_load_n(&cpu_data[ap_count].is_online,
                                    __ATOMIC_ACQUIRE) && timeout > 0) {
                delay_ms(1);
                timeout--;
            }
        }

        if (!__atomic_load_n(&cpu_data[ap_count].is_online, __ATOMIC_ACQUIRE)) {
            /* Retry with second SIPI */
            lapic_send_sipi(ci->apic_id, AP_TRAMPOLINE_ADDR >> 12);
            delay_ms(1);

            /* Wait again (50ms) */
            {
                uint32_t timeout = 50;
                while (!__atomic_load_n(&cpu_data[ap_count].is_online,
                                        __ATOMIC_ACQUIRE) && timeout > 0) {
                    delay_ms(1);
                    timeout--;
                }
            }
        }

    }

    /* Count online APs and emit each one's "online" line + CPU-hardening audit
     * from the BSP, both gated on that AP's OWN is_online flag via an ACQUIRE
     * load. is_online is the single publication source (release-stored last by
     * the AP), so the count and the audit derive from the same flags -- they
     * cannot diverge, and the acquire/release edge makes the AP's lapic_id +
     * ap_cpu_harden() snapshot visible here. An AP that never published
     * is_online is neither counted nor audited (consistent). BSP-side emission
     * keeps unbounded serial I/O off the AP bringup critical path. */
    {
        uint32_t online = 0;
        for (i = 1; i <= ap_count; i++) {
            if (__atomic_load_n(&cpu_data[i].is_online, __ATOMIC_ACQUIRE)) {
                online++;
                klog(LOG_INFO, "smp", "AP %u online (LAPIC ID=%u)",
                     (uint64_t)i, (uint64_t)cpu_data[i].lapic_id);
                ap_cpu_harden_log(i);
            } else {
                klog(LOG_WARN, "smp", "AP %u did not respond", (uint64_t)i);
            }
        }
        total_cpus = 1 + online;
    }

    /* If any AP failed feature validation it recorded the fault and halted
     * (it could not bug-check itself safely); raise 0x3E here on the BSP where
     * the panic path is feature-safe (TODO-09-boot S6). No-op if all APs passed. */
    cpu_features_check_ap_faults();

    /* Publish the global feature intersection now that the online set is fixed
     * (TODO-09-boot S6). Done here, after the is_online acquire pass above, so
     * a late/timed-out AP cannot downgrade the published mask. */
    cpu_features_finalize_global();

    klog(LOG_INFO, "smp", "%u CPUs online (BSP + %u APs)",
         (uint64_t)total_cpus, (uint64_t)(total_cpus - 1));
}

/* ---- Query API ---- */

uint32_t smp_cpu_count(void)
{
    return total_cpus > 0 ? total_cpus : 1;
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
