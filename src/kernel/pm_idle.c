/* ---------------------------------------------------------------------------
 * pm_idle.c -- race-safe C1 idle entry + per-CPU idle cycle accounting.
 *
 * todo/02-kernel-core/TODO-26-power-management.md section 2. See include/kernel/pm.h for why this is C1 and not S1.
 *
 * The whole point of this file is that the readiness decision and the halt are
 * ONE handoff. A bare `if (nothing to do) hlt;` loses wakeups: an interrupt
 * landing between the test and the HLT can queue a DPC or make a thread
 * runnable, return, and leave the CPU halted on work it should have run. So
 * the test happens with interrupts already masked and the halt is the `sti;
 * hlt` pair, whose STI interrupt shadow holds delivery off until the HLT has
 * begun.
 * ------------------------------------------------------------------------- */
#include "kernel/pm.h"
#include "kernel/smp.h"
#include "kernel/sched/dpc.h"
#include "kernel/sched/spinlock.h"
#include "registry.h"

/* x86-64 RFLAGS.IF. */
#define PM_RFLAGS_IF (1ull << 9)

/* Cached PowerIdleEnable. -1 = not read yet (treated as enabled, which is the
 * documented default), 0 = disabled by policy, 1 = enabled.
 *
 * Cached rather than read per idle: src/kernel/registry.c takes no lock today
 * (its own comments flag that as unfinished), so reading it from the idle path
 * would race a concurrent writer at timer rate.
 *
 * ATOMIC, and that is load-bearing rather than defensive: smp_init() runs at
 * boot_storage.c:271 and pm_idle_init() at boot_storage.c:1090, both inside
 * boot_phase2(), so every AP is already parked in pm_idle_c1() reading this
 * word while the BSP writes it. Local interrupt masking orders nothing across
 * CPUs. Release on the write, acquire on the read. */
static int s_power_idle_enable = -1;

#ifdef KERNEL_TESTS
/* Counts arrivals at the instruction after the halt. Test-only.
 *
 * PER-CPU, and that is not tidiness: every parked AP runs this same halt in
 * its park loop, so a single global would be incremented continuously by every
 * other processor and both of its assertions ("advanced by exactly one",
 * "did not advance at all") would be races that pass only on a 1-CPU boot.
 * Indexed by CPU so each test observes only its own calls. */
static uint64_t s_halt_site_hits[MAX_CPUS];

uint64_t pm_idle_halt_hits_test(void)
{
    uint32_t me = smp_cpu_id();

    if (me >= MAX_CPUS)
        return 0;
    return __atomic_load_n(&s_halt_site_hits[me], __ATOMIC_RELAXED);
}
#endif

/* Local TSC read. Matches the file-local convention in boot_timing.c and
 * hw_profile.c -- there is no shared kernel rdtsc helper, and rdtscp_read() in
 * cpuid.h needs a CPUID-gated instruction this path must not depend on. */
/* The accepted contribution of one halt interval. Split out so the guard is
 * testable without a way to make the hardware TSC run backwards. */
uint64_t pm_idle_delta(uint64_t t0, uint64_t t1)
{
    return (t1 > t0) ? (t1 - t0) : 0;
}

static inline uint64_t pm_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void pm_idle_init(void)
{
    uint32_t value = 1;
    uint32_t size  = sizeof(value);
    uint32_t type  = 0;
    long     rc;

    if (__atomic_load_n(&s_power_idle_enable, __ATOMIC_ACQUIRE) >= 0)
        return;                       /* idempotent */

    /* Windows-parity path. Absent value means enabled: HLT-on-idle is what
     * every idle site already did before this section, so defaulting to
     * disabled would be a power regression on a machine that never set it. */
    rc = RegGetValue(HKEY_LOCAL_MACHINE,
                     "System\\CurrentControlSet\\Control\\Power",
                     "PowerIdleEnable", RRF_RT_REG_DWORD, &type,
                     &value, &size);

    if (rc != ERROR_SUCCESS || size != sizeof(value))
        value = 1;

    __atomic_store_n(&s_power_idle_enable, (value != 0) ? 1 : 0,
                     __ATOMIC_RELEASE);
}

/* This CPU's queue depth, as ONE relaxed atomic load.
 *
 * The field is NOT exclusively this CPU's to write, which an earlier version of
 * the comment here got wrong: dpc_insert_core() resolves an arbitrary target
 * CPU, takes THAT queue's lock and increments its depth (src/kernel/sched/dpc.c
 * :586-598, :648), and KeRemoveQueueDpc() lets another CPU remove the last
 * entry (:720-758). `cli` excludes none of that -- it is local only. Every
 * access to the field, here and in dpc.c, uses the same relaxed-atomic
 * discipline, because a plain write racing an atomic read is undefined however
 * benign the emitted code looks. */
static uint32_t pm_dpc_depth(void)
{
    struct dpc_queue *q = dpc_this_cpu_queue();

    return q ? __atomic_load_n(&q->depth, __ATOMIC_RELAXED) : 0u;
}

/* The predicate, as a pure function of ONE depth sample. Split out so the
 * test-only probe below can report the verdict AND the exact depth it was
 * computed from: two SEPARATE observations of a shared queue cannot be
 * compared, because a remote enqueue or a cross-CPU cancel between them yields
 * a contradiction that is nobody's bug. */
static int pm_deep_idle_from_depth(uint32_t depth)
{
    /* Policy. A value that was never read is enabled -- see pm_idle_init().
     * Acquire: an AP parked before the BSP published this must observe the
     * published value, not a cached register. */
    if (__atomic_load_n(&s_power_idle_enable, __ATOMIC_ACQUIRE) == 0)
        return 0;

    return (depth == 0) ? 1 : 0;
}

int pm_deep_idle_allowed(void)
{
    return pm_deep_idle_from_depth(pm_dpc_depth());
}

#ifdef KERNEL_TESTS
int pm_deep_idle_probe_test(uint32_t *out_depth)
{
    uint32_t depth = pm_dpc_depth();

    if (out_depth)
        *out_depth = depth;
    return pm_deep_idle_from_depth(depth);
}
#endif


/* ---- MWAIT capability and hint selection --------------------------------
 *
 * Pure logic only; see the contract in include/kernel/pm.h for why nothing
 * here executes MONITOR or MWAIT. Taking the CPUID words as arguments rather
 * than executing CPUID is what lets the suite prove these on a host whose CPU
 * reports no leaf 5 -- which is every CI host this repo runs on. */

uint32_t pm_mwait_hint_encode(uint32_t cclass, uint32_t substate)
{
    if (cclass < PM_MWAIT_CLASS_MIN || cclass > PM_MWAIT_CLASS_MAX)
        return PM_MWAIT_HINT_INVALID;
    if (substate > PM_MWAIT_SUBSTATE_MAX)
        return PM_MWAIT_HINT_INVALID;
    /* SDM Table 4-11: bits 7:4 are (class - 1), so 0 names C1. */
    return ((cclass - 1u) << 4) | substate;
}

int pm_mwait_deepest_hint(uint32_t leaf5_edx, uint32_t *out_hint)
{
    uint32_t cclass;

    if (!out_hint)
        return 0;

    /* CPUID.05H:EDX packs one 4-bit sub-state count per class, class Cn at
     * bits 4n+3:4n. C0 (bits 3:0) is the running state, not an idle target,
     * so the search stops at C1 and never encodes class 0.
     *
     * Descending order is the point: the DEEPEST implemented class wins, and
     * a class whose count is zero is unimplemented and skipped rather than
     * encoded with sub-state 0. Encoding an unimplemented class would name a
     * state the CPU does not have. */
    for (cclass = PM_MWAIT_CLASS_MAX; cclass >= PM_MWAIT_CLASS_MIN; cclass--) {
        uint32_t count = (leaf5_edx >> (cclass * 4u)) & 0xFu;

        if (count) {
            /* count sub-states means valid indices 0..count-1, so the deepest
             * is count-1. Off-by-one here would request a sub-state the class
             * does not implement. */
            uint32_t hint = pm_mwait_hint_encode(cclass, count - 1u);

            if (hint == PM_MWAIT_HINT_INVALID)
                return 0;
            *out_hint = hint;
            return 1;
        }
    }
    return 0;
}

int pm_mwait_idle_allowed(int if_set, int irq_break_supported)
{
    /* Interrupts enabled: any arriving interrupt is a break event, so the
     * wait is bounded by the same mechanism that bounds HLT. */
    if (if_set)
        return 1;

    /* Interrupts masked: the ONLY break event is a masked interrupt, and that
     * requires CPUID.05H:ECX[1] paired with MWAIT ECX[0]. Without it there is
     * no guaranteed wake, so the caller must not execute MWAIT at all. This
     * is a refusal, not a downgrade -- returning 1 here would trade a power
     * saving for a machine that never wakes. */
    return irq_break_supported ? 1 : 0;
}

int pm_idle_c1(void)
{
    struct per_cpu_data *cpu;
    uint64_t flags, t0, t1;
    int ready;

    flags = local_irq_save();

    /* A caller that arrived with interrupts already disabled is not idle in
     * any sense this function can serve: halting would need `sti`, which would
     * run an ISR inside a region the caller believes is interrupt-free. */
    if (!(flags & PM_RFLAGS_IF)) {
        local_irq_restore(flags);
        return 0;
    }

    /* Sampled with interrupts masked so the reported value is coherent with
     * the halt below rather than a stale answer from before it. It is
     * REPORTED, never acted on: see the contract in pm.h for why a refusal
     * here would be a busy-spin and not a latency win. */
    ready = pm_deep_idle_allowed();

    t0 = pm_rdtsc();

    /* STI's interrupt shadow defers delivery until HLT has begun, so a wake
     * that arrives in this instant halts-then-wakes rather than being lost.
     *
     * The global label makes the halt site addressable so a unit test can
     * assert the two bytes ARE 0xFB 0xF4 at exactly this address, instead of
     * searching a window that runs off the end of the function into unrelated
     * code. If the halt is ever removed, the label goes with it and the test
     * fails to LINK -- which is the strongest form this check can take. */
    __asm__ volatile (".globl pm_idle_hlt_site\n"
                      "pm_idle_hlt_site:\n"
                      "sti; hlt");

    t1 = pm_rdtsc();

#ifdef KERNEL_TESTS
    /* Reachability, not identity. The opcode check proves the halt EXISTS at
     * the labelled site; only this proves control flow REACHED it on a given
     * call. Without it, a regression that skipped the halt when `ready == 0`
     * would pass every other assertion -- accounting runs either way, and the
     * opcode check would still find the bytes in the taken branch -- which is
     * precisely the AP busy-spin this section already fixed once. */
    {
        uint32_t me = smp_cpu_id();
        if (me < MAX_CPUS)
            __atomic_fetch_add(&s_halt_site_hits[me], 1ull, __ATOMIC_RELAXED);
    }
#endif

    /* Owning CPU writes its own counter; no other CPU writes this field.
     *
     * The guard is not paranoia: TSC is not guaranteed monotonic across a
     * migration or a firmware write, and an unsigned wrap turns one backwards
     * sample into a ~2^64 increment that never washes out. A rejected sample
     * loses one idle episode; an accepted bad one destroys the counter.
     *
     * This interval is an UPPER BOUND on idle, not the halted time: the
     * instruction after HLT retires only once the waking ISR has returned, and
     * on the BSP the scheduler may switch away first. See the parked precision
     * item in todo/02-kernel-core/TODO-26-power-management.md section 2. */
    cpu = smp_this_cpu();
    if (cpu) {
        /* Single writer, but NOT a single accessor: pm_idle_cycles() reads
         * this slot from another CPU while the owner is still updating it, so
         * a plain += paired with a plain load is an unsynchronized C data
         * race even though x86 will not tear an aligned 64-bit access.
         * Relaxed is the right strength -- the value carries no ordering. */
        __atomic_fetch_add(&cpu->idle_tsc_cycles, pm_idle_delta(t0, t1),
                           __ATOMIC_RELAXED);
    }

    local_irq_restore(flags);
    return ready;
}

uint64_t pm_idle_cycles(uint32_t cpu_id)
{
    struct per_cpu_data *cpu;

    if (!smp_cpu_is_online(cpu_id))
        return 0;

    cpu = smp_get_cpu(cpu_id);
    if (!cpu)
        return 0;
    /* Matches the owner-side relaxed accumulate; the owning CPU may be
     * updating this slot right now. */
    return __atomic_load_n(&cpu->idle_tsc_cycles, __ATOMIC_RELAXED);
}

#ifdef KERNEL_TESTS
int pm_idle_set_enable_test(int enable)
{
    int prev = __atomic_load_n(&s_power_idle_enable, __ATOMIC_ACQUIRE);
    /* Same publication rule as pm_idle_init(): APs are parked in the predicate
     * while a test flips it, so a plain store would race them too. */
    __atomic_store_n(&s_power_idle_enable, enable ? 1 : 0, __ATOMIC_RELEASE);
    return prev;
}
#endif /* KERNEL_TESTS */
