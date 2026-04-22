/* ============================================================================
 * transition_ring.c -- Per-CPU fast-path transition ring buffer (fast-path transition ring)
 *
 * See include/kernel/sched/transition_ring.h for the API contract.
 * Design notes belong there; this file holds the implementation.
 * ============================================================================ */

#include "kernel/sched/transition_ring.h"
#include "kernel/smp.h"
#include "kernel/sched/task.h"
#include "kernel/msr.h"
#include "kernel/klog.h"

/* -------- Register sampling helpers -------- */

/* Read CR3 via inline asm. Safe in both ring-0 entry and ring-3 exit
 * paths (we never swap CR3 during the record window). */
static inline uint64_t read_cr3_raw(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* TSC snapshot. Kernel-side CLAUDE.md note: per-CPU TSC offsets can
 * differ on bare metal; we record the raw local value and leave
 * interpretation to the panic-time reader. */
static inline uint64_t read_tsc_raw(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* -------- Init -------- */

void transition_ring_init_this_cpu(void)
{
    struct per_cpu_data *pcpu = smp_this_cpu();
    if (!pcpu)
        return;
    if (pcpu->transition_init_marker == TRANSITION_INIT_MARKER)
        return;  /* idempotent */

    /* Zero every entry so a partial-dump after a few records shows
     * the populated slots as non-zero against a clean background. */
    for (uint32_t i = 0; i < TRANSITION_RING_SIZE; i++) {
        pcpu->transition_ring[i].tsc = 0;
        pcpu->transition_ring[i].thread_id = 0;
        pcpu->transition_ring[i].direction = 0;
        pcpu->transition_ring[i].cr3 = 0;
        pcpu->transition_ring[i].rip = 0;
        pcpu->transition_ring[i].rsp = 0;
        pcpu->transition_ring[i].gs_base = 0;
        pcpu->transition_ring[i].kernel_gs_base = 0;
    }
    pcpu->transition_head = 0;
    /* Set marker LAST so a partial init can be detected as "not yet
     * initialized" by the dump path -- mirrors the KUSD
     * AbiMagic-written-last invariant from §18. */
    pcpu->transition_init_marker = TRANSITION_INIT_MARKER;
}

/* -------- Record -------- */

void transition_ring_record(uint32_t direction, uint64_t rip, uint64_t rsp)
{
    struct per_cpu_data *pcpu = smp_this_cpu();
    if (!pcpu)
        return;  /* pre-init or smp_this_cpu() via bad gs; bail silently */
    if (pcpu->transition_init_marker != TRANSITION_INIT_MARKER)
        return;  /* ring not initialized on this CPU yet */

    uint32_t idx = pcpu->transition_head & (TRANSITION_RING_SIZE - 1);
    struct transition_entry *e = &pcpu->transition_ring[idx];

    e->tsc             = read_tsc_raw();
    e->direction       = direction;
    e->rip             = rip;
    e->rsp             = rsp;
    e->cr3             = read_cr3_raw();
    e->gs_base         = msr_read(MSR_IA32_GS_BASE);
    e->kernel_gs_base  = msr_read(MSR_IA32_KERNEL_GS_BASE);

    /* Thread id: task_current() is safe HERE because we run fully
     * in kernel context with per-CPU GS and a valid task table (by
     * the time any ring-3 transition happens the scheduler has
     * long initialized). Use PID rather than thread-pointer so a
     * dump line has a stable human-readable identifier. */
    {
        struct task *t = task_current();
        e->thread_id = t ? t->pid : 0;
    }

    /* Advance head LAST so a concurrent reader (dump path) that
     * catches head at index N sees a fully-populated entry N-1.
     * On same-CPU single-writer there is no reorder concern, but
     * the write order makes the NMI-dump-from-another-CPU case
     * safer if that ever lands. */
    pcpu->transition_head = idx + 1;
}

/* -------- Dump -------- */

/* Emit one ring entry as a single klog line. Keeping the format
 * dense lets a 64-entry dump fit in a reasonable serial window
 * (~64 * 150 chars = 9 KiB at 115200 baud = ~800 ms worst case). */
static void emit_entry(uint32_t slot, const struct transition_entry *e)
{
    const char *dir = (e->direction == TRANSITION_DIR_TO_USER) ? "U" : "K";
    klog(LOG_INFO, "RING",
         "[%u] tsc=0x%X %s pid=%u rip=0x%X rsp=0x%X cr3=0x%X gs=0x%X kgs=0x%X",
         (uint64_t)slot, e->tsc, dir,
         (uint64_t)e->thread_id,
         e->rip, e->rsp, e->cr3, e->gs_base, e->kernel_gs_base);
}

void transition_ring_dump_to_serial(struct per_cpu_data *pcpu)
{
    if (!pcpu) {
        klog(LOG_WARN, "RING", "dump: pcpu is NULL");
        return;
    }
    if (pcpu->transition_init_marker != TRANSITION_INIT_MARKER) {
        klog(LOG_WARN, "RING", "dump: not-initialized on CPU %u",
             (uint64_t)pcpu->cpu_id);
        return;
    }

    uint32_t head = pcpu->transition_head;
    /* Emit header so a serial-log reader can find the block. */
    klog(LOG_INFO, "RING",
         "==== transition ring dump (CPU %u, head=%u) ====",
         (uint64_t)pcpu->cpu_id, (uint64_t)head);

    /* Walk oldest-first. If the ring has wrapped (head > RING_SIZE),
     * the oldest live entry is at (head - RING_SIZE) & mask = head &
     * mask; if it hasn't wrapped, slot 0 is the oldest. Walking
     * `head..head+RING_SIZE` mod mask covers both cases uniformly
     * and skips uninitialized (tsc=0) entries for clean output on
     * a short-lived boot. */
    for (uint32_t i = 0; i < TRANSITION_RING_SIZE; i++) {
        uint32_t slot = (head + i) & (TRANSITION_RING_SIZE - 1);
        const struct transition_entry *e = &pcpu->transition_ring[slot];
        if (e->tsc == 0 && head < TRANSITION_RING_SIZE)
            continue;  /* never populated; skip */
        emit_entry(slot, e);
    }
    klog(LOG_INFO, "RING", "==== end transition ring ====");
}
