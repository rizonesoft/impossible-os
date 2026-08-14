/* ============================================================================
 * boot_init.c -- Subsystem readiness oracle and boot progress tracker
 *
 * Provides:
 *   - g_subsys_ready[] table with get/set/dump helpers
 *   - boot_progress() serial progress emitter + TSC step recorder
 *   - _boot_require_failed() helper used by BOOT_REQUIRE macro
 * ============================================================================ */

#include "kernel/boot_init.h"
#include "kernel/boot_timing.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_progress.h"
#include "kernel/klog.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/watchdog.h"
#include "kernel/uefi_runtime.h"
#include "kernel/boot_info.h"

/* ---- Subsystem readiness table ------------------------------------------
 * SMP-safe: uses atomic load/store with acquire/release semantics.
 * Cross-CPU publication is required because async AP boot work
 * (boot_async_group) can set readiness on one CPU while another
 * CPU checks BOOT_REQUIRE. */

static uint8_t g_subsys_ready[SUBSYS_COUNT];

static const char *const s_subsys_names[SUBSYS_COUNT] = {
    "SERIAL",     /* 0  */
    "PMM",        /* 1  */
    "VMM",        /* 2  */
    "HEAP",       /* 3  */
    "KLOG",       /* 4  */
    "GDT",        /* 5  */
    "IDT",        /* 6  */
    "ACPI",       /* 7  */
    "LAPIC",      /* 8  */
    "IOAPIC",     /* 9  */
    "TIMER",      /* 10 */
    "RTC",        /* 11 */
    "FB",         /* 12 */
    "VFS",        /* 13 */
    "REGISTRY",   /* 14 */
    "SCHED",      /* 15 */
    "IPC",        /* 16 */
    "SMP",        /* 17 */
    "EXEC",       /* 18 */
    "DESKTOP",    /* 19 */
    "OB",         /* 20 */
    "UEFI_VARS",  /* 21 */
    "UEFI_TIME",  /* 22 */
    "SECUREBOOT", /* 23 */
    "TPM",        /* 24 */
    "XSAVE",      /* 25 */
    "PCID",       /* 26 */
    "EX",         /* 27 */
    "NLS",        /* 28 */
    "KNF",        /* 29 */
};

_Static_assert(sizeof(s_subsys_names) / sizeof(s_subsys_names[0]) == SUBSYS_COUNT,
               "s_subsys_names must have exactly SUBSYS_COUNT entries");

bool kernel_subsystem_ready(kernel_subsys_t subsys)
{
    if ((uint32_t)subsys >= SUBSYS_COUNT) return false;
    return __atomic_load_n(&g_subsys_ready[subsys], __ATOMIC_ACQUIRE) != 0;
}

void kernel_subsystem_set_ready(kernel_subsys_t subsys, bool ok)
{
    if ((uint32_t)subsys >= SUBSYS_COUNT) return;
    __atomic_store_n(&g_subsys_ready[subsys], ok ? 1 : 0, __ATOMIC_RELEASE);
}

void kernel_subsystem_dump(void)
{
    klog(LOG_INFO, "BOOT", "--- Subsystem readiness ---");
    for (uint32_t i = 0; i < SUBSYS_COUNT; i++) {
        klog(LOG_INFO, "BOOT", "  [%s] %s",
             __atomic_load_n(&g_subsys_ready[i], __ATOMIC_ACQUIRE) ? "OK  " : "FAIL",
             s_subsys_names[i]);
    }
}

const char *kernel_subsystem_name(kernel_subsys_t subsys)
{
    if ((uint32_t)subsys >= SUBSYS_COUNT) return "UNKNOWN";
    return s_subsys_names[subsys];
}

bool kernel_subsystem_apply_result(kernel_subsys_t subsys, boot_result_t r)
{
    if ((uint32_t)subsys >= SUBSYS_COUNT) return false;

    bool ready = (r == BOOT_OK) || (r == BOOT_DEGRADED);
    kernel_subsystem_set_ready(subsys, ready);

    if (r != BOOT_OK)
        g_boot_info.degraded_mask |= (1u << (uint32_t)subsys);

    return ready;
}

/* ---- BOOT_REQUIRE helper ------------------------------------------------- */

void _boot_require_failed(const char *subsys_name)
{
    /* If klog is already up, emit a framed LOG_ERROR line so the failure
     * blends into the rest of the boot log (this matters most when unit
     * tests deliberately trigger BOOT_REQUIRE failures from Phase 3 --
     * otherwise a raw serial write drops into the middle of the test
     * runner output looking like unrelated noise).
     *
     * Early Phase 0/1 callers hit BOOT_REQUIRE before klog_early_init(),
     * so fall back to direct serial in that window. */
    if (kernel_subsystem_ready(SUBSYS_KLOG)) {
        /* LOG_WARN (not LOG_ERROR) because BOOT_REQUIRE is a non-fatal
         * prerequisite check per its header contract: the caller
         * receives BOOT_FATAL and decides whether to halt, degrade, or
         * retry.  The red [FAIL] badge (LOG_ERROR) is visually
         * indistinguishable from a real test failure in the test
         * runner output; [WARN] correctly conveys "check failed,
         * caller will handle it". */
        klog(LOG_WARN, "boot", "REQUIRE failed: %s not ready", subsys_name);
        return;
    }
    serial_write("[BOOT] REQUIRE failed: ");
    serial_write(subsys_name);
    serial_write(" not ready\n");
}

/* ---- Deferred init registration ----------------------------------------- */

typedef struct {
    const char    *name;
    boot_result_t (*fn)(void);
} deferred_entry_t;

static deferred_entry_t g_deferred[BOOT_DEFERRED_MAX];
static uint32_t         g_deferred_count;
static int              g_deferred_ran;   /* run-once latch (idempotent) */

int boot_defer(const char *name, boot_result_t (*fn)(void))
{
    if (!fn) {
        klog(LOG_DEBUG, "boot", "deferred init: NULL function for '%s'",
             name ? name : "?");
        return -1;
    }
    if (g_deferred_ran) {
        /* boot_run_deferred() already executed -- a registration now would
         * never run. Reject so a late caller fails loudly instead of silently
         * registering a subsystem that never initializes. */
        klog(LOG_WARN, "boot", "deferred init already ran -- cannot defer %s",
             name ? name : "?");
        return -1;
    }
    if (g_deferred_count >= BOOT_DEFERRED_MAX) {
        klog(LOG_ERROR, "boot", "deferred init full (%u/%u) -- cannot defer %s",
             g_deferred_count, (uint32_t)BOOT_DEFERRED_MAX, name ? name : "?");
        return -1;
    }
    g_deferred[g_deferred_count].name = name;
    g_deferred[g_deferred_count].fn   = fn;
    g_deferred_count++;
    klog(LOG_DEBUG, "boot", "deferred: registered %s (slot %u)",
         name ? name : "?", g_deferred_count - 1);
    return 0;
}

void boot_run_deferred(void)
{
    extern uint64_t system_get_ticks(void);
    uint32_t i;

    /* Idempotent: each deferred fn re-initializes a driver (IRQ registration,
     * device reset), so a second call (retry / diagnostic / refactor) would
     * double-init. Latch on first entry; later calls are a no-op. */
    if (g_deferred_ran)
        return;
    g_deferred_ran = 1;

    if (g_deferred_count == 0)
        return;

    POST16(POST16_DEFERRED);
    klog(LOG_INFO, "boot", "--- Running %u deferred init(s) ---",
         g_deferred_count);

    for (i = 0; i < g_deferred_count; i++) {
        const char *name = g_deferred[i].name ? g_deferred[i].name : "?";
        uint64_t t0 = system_get_ticks();

        /* Runs inline on the BSP boot path. A fault here halts boot.
         * Thread isolation was attempted but starved the deferred inits
         * (compositor event loop took the CPU). Per-thread fault isolation
         * requires TODO-10 SEH. */
        boot_result_t r = g_deferred[i].fn();
        uint64_t elapsed_ms = (system_get_ticks() - t0) * 10;

        if (r == BOOT_OK || r == BOOT_DEGRADED) {
            klog(LOG_INFO, "boot", "[DEFERRED] %s +%ums", name,
                 (uint32_t)elapsed_ms);
        } else {
            klog(LOG_WARN, "boot", "[DEFERRED] %s FAILED +%ums (non-fatal)",
                 name, (uint32_t)elapsed_ms);
        }
    }

    POST16(POST16_DEFERRED_OK);
    boot_progress(3, "DEFERRED", POST16_DEFERRED_OK);
}

/* ---- Async subsystem init (SMP parallel) -------------------------------- */

#include "kernel/smp.h"
#include "kernel/idt.h"
#include "kernel/timer.h"
#include "kernel/barrier.h"
#include "kernel/config.h"

#ifdef KERNEL_TESTS
/* Release latch for the S27 worker hold (see the injection block below). Set
 * once by the BSP, read by the held worker; never cleared, because the hold is
 * a one-shot boot-time scenario and a second async group must not re-arm it. */
static volatile uint32_t s_async_hold_released = 0;

/* Tick at which the BSP created the group's barrier deadline, 0 before that.
 * The held worker's deadlock backstop is measured from HERE, not from the
 * worker's own entry: the AP starts spinning as soon as it takes the IPI, but
 * the BSP does not create its deadline until after running step 0 and every
 * overflow step. A cap anchored at AP entry is therefore racing BSP-side
 * storage work -- the SAME wrong-origin mistake the fixed-duration hold made,
 * with more slack but the same shape -- and on slow hardware it would fire
 * before the barrier had even started, releasing the worker into a normal
 * completion and reporting a healthy boot as degraded-path coverage. Anchored
 * at the barrier, the backstop cannot preempt the sequence it exists to
 * bound. */
static volatile uint64_t s_async_barrier_epoch = 0;

void boot_async_test_release_hold(void)
{
    __atomic_store_n(&s_async_hold_released, 1u, __ATOMIC_RELEASE);
}
#endif

/* IPI handler: runs on AP when it receives IPI_VECTOR_ASYNC_INIT.
 * Reads the work function from per-CPU data, executes it, writes result. */
static uint64_t async_ipi_handler(struct interrupt_frame *frame)
{
    struct per_cpu_data *pcpu = smp_this_cpu();
    uint32_t gen = 0;

    /* The claim is the authority on whether there is work here, not async_fn
     * (TODO-10 S21). A stray or misdelivered async IPI arriving at a slot the
     * BSP never claimed would otherwise rerun whatever function pointer was
     * last left in the slot. */
    if (!pcpu || !smp_async_claim_is_busy(&pcpu->async_claim, &gen) ||
        !pcpu->async_fn) {
        /* Spurious -- no work assigned */
        extern void lapic_eoi(void);
        lapic_eoi();
        return (uint64_t)frame;
    }

    pcpu->in_async_work = 1;
    smp_mb();

    boot_result_t (*fn)(void) = (boot_result_t (*)(void))pcpu->async_fn;
    const char *name = pcpu->async_name ? pcpu->async_name : "?";

    klog(LOG_INFO, "ASYNC", "[ASYNC] %s started on CPU%u", name, pcpu->cpu_id);
    POST16(POST16_ASYNC_AP);

    uint64_t t0 = system_get_ticks();

#ifdef KERNEL_TESTS
    /* Degraded-configuration injection (TODO-10 S27): hold this worker inside
     * its step so a live boot can produce a timed-out-but-still-running worker
     * on demand -- the case the quiescence rule exists for and that no
     * automated leg can otherwise reach. Held BEFORE fn() so the initializer
     * has not started: the point is to exercise the BSP's ownership decision,
     * not to actually wedge a controller. The claim stays BUSY at this
     * generation throughout, which is exactly what the BSP must observe.
     *
     * A HANDSHAKE, NOT A DURATION. This was a fixed `t0 + BARRIER_MS` hold, and
     * that cannot work: t0 is THIS AP's start, while the BSP creates its
     * barrier deadline later, after running step 0 and every overflow step. Any
     * BSP step costing more than a tick pushes the deadline past the AP's
     * release, so the worker finishes normally and the injected scenario never
     * happens -- silently, reporting a healthy boot as coverage. The release
     * now comes FROM the BSP, after it has recorded the timeout, decided the
     * fallback, and run quiescence to its own deadline, so every stage of the
     * degraded path observes a genuinely still-running worker.
     *
     * The cap is a deadlock backstop, not the mechanism: if the release never
     * arrives (a BSP path that fails before reaching it), the worker gives up
     * rather than holding a boot open forever. */
    {
        int64_t hold = boot_arg_resolved_ival("test_hold_async_cpu");
        if (hold > 0 && (uint32_t)hold == pcpu->cpu_id) {
            /* TWO PHASES, because each origin alone has a hole.
             *
             * Barrier-relative alone: the backstop arms only once the BSP
             * publishes its epoch, and the BSP sends the worker IPIs BEFORE it
             * runs its own step and the overflow steps. A BSP-side initializer
             * that blocks before the barrier leaves both the epoch and the
             * release latch at zero, and the worker spins forever without ever
             * emitting the failure marker this backstop exists to promise.
             *
             * Entry-relative alone: that is the wrong-origin bug this replaced
             * -- BSP pre-barrier work would expire the cap before the barrier
             * had even started, releasing the worker into a normal completion
             * and reporting a healthy boot as degraded-path coverage.
             *
             * So: before the epoch is visible, bound the wait from AP entry
             * (nothing else exists yet, and a boot stuck there is broken
             * regardless); once it is visible, switch to the barrier-relative
             * deadline, which is the one that has to outlast barrier+quiesce.
             * Both exits name which phase fired. */
            uint64_t pre_cap = t0 + (BOOT_ASYNC_HOLD_CAP_MS / 10);

            for (;;) {
                uint64_t epoch;

                if (__atomic_load_n(&s_async_hold_released, __ATOMIC_ACQUIRE))
                    break;
                epoch = __atomic_load_n(&s_async_barrier_epoch, __ATOMIC_ACQUIRE);
                if (epoch) {
                    if (system_get_ticks() >
                            epoch + (BOOT_ASYNC_HOLD_CAP_MS / 10)) {
                        klog(LOG_ERROR, "ASYNC",
                             "[S27] FAILED coverage: held worker on CPU%u hit "
                             "its %ums post-barrier backstop -- the BSP never "
                             "released it", pcpu->cpu_id, BOOT_ASYNC_HOLD_CAP_MS);
                        break;
                    }
                } else if (system_get_ticks() > pre_cap) {
                    klog(LOG_ERROR, "ASYNC",
                         "[S27] FAILED coverage: held worker on CPU%u waited "
                         "%ums and the BSP never reached the group barrier",
                         pcpu->cpu_id, BOOT_ASYNC_HOLD_CAP_MS);
                    break;
                }
                __asm__ volatile("pause");
            }
        }
    }
#endif

    boot_result_t r = fn();
    uint64_t elapsed_ms = (system_get_ticks() - t0) * 10;

    pcpu->async_result = (uint8_t)r;
    pcpu->in_async_work = 0;
    smp_mb();
    pcpu->async_done = 1;
    smp_mb();

    /* Witness the retirement BEFORE performing it (TODO-10 S27), so a reader
     * that later finds this slot OFFLINE can tell "completed, then this CPU
     * parked" from "faulted mid-step and was parked by panic.c" -- the claim
     * word reaches OFFLINE(gen) either way, and only one of them is safe to
     * re-run. Ordered before the retirement: a witness published afterwards
     * could be missed by a reader that observed the retirement first. */
    __atomic_store_n(&pcpu->async_retired,
                     SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, gen), __ATOMIC_RELEASE);

    /* Retire the claim LAST, and only for the generation this worker was
     * dispatched under: a worker the BSP already timed out cannot hand its
     * slot back into the idle pool, because the exact-value compare-exchange
     * fails against whatever state the slot has reached since. */
    smp_async_claim_complete(&pcpu->async_claim, gen);

    if (r == BOOT_OK || r == BOOT_DEGRADED) {
        klog(LOG_INFO, "ASYNC", "[ASYNC] %s completed on CPU%u in %ums",
             name, pcpu->cpu_id, (uint32_t)elapsed_ms);
    } else {
        klog(LOG_WARN, "ASYNC", "[ASYNC] %s FAILED on CPU%u in %ums",
             name, pcpu->cpu_id, (uint32_t)elapsed_ms);
    }

    extern void lapic_eoi(void);
    lapic_eoi();
    return (uint64_t)frame;
}

void boot_async_init(void)
{
    idt_register_handler(IPI_VECTOR_ASYNC_INIT, async_ipi_handler);
    klog(LOG_DEBUG, "ASYNC", "Async init IPI handler registered (vector 0x%x)",
         (uint32_t)IPI_VECTOR_ASYNC_INIT);
}

/* Severity order for the async-group worst-pick. The boot_result_t enum is NOT
 * monotonically severity-ordered (BOOT_DEFERRED=3 numerically outranks
 * BOOT_FATAL=2), so a plain `r > worst` lets a DEFERRED step mask a FATAL one.
 * Rank explicitly: FATAL must halt > DEGRADED continues > DEFERRED retries
 * later > OK. */
static int boot_result_severity(boot_result_t r)
{
    switch (r) {
        case BOOT_FATAL:    return 3;
        case BOOT_DEGRADED: return 2;
        case BOOT_DEFERRED: return 1;
        case BOOT_OK:
        default:            return 0;
    }
}

/* Record one step's disposition. Inert when the caller did not ask for an
 * outcome record, and bounds-checked so a group larger than the record cannot
 * write past it. */
static void boot_async_record(boot_async_outcome_t *out, uint32_t step,
                              boot_async_step_state_t state, boot_result_t result,
                              uint32_t worker_cpu, uint32_t dispatch_gen)
{
    if (!out || step >= BOOT_ASYNC_MAX_STEPS)
        return;
    out->step[step].state        = state;
    out->step[step].result       = result;
    out->step[step].worker_cpu   = worker_cpu;
    out->step[step].dispatch_gen = dispatch_gen;
}

int boot_async_claim_released(uint32_t claim_word, uint32_t dispatch_gen)
{
    uint32_t state = SMP_ASYNC_STATE_OF(claim_word);
    uint32_t gen   = SMP_ASYNC_GEN_OF(claim_word);

    /* Only BUSY or RESERVED at the dispatch generation means that CPU is still
     * inside this step. Any other word -- IDLE at this generation, any later
     * generation, OFFLINE -- means it is not. */
    if (gen == (dispatch_gen & SMP_ASYNC_GEN_MAX) &&
        (state == SMP_ASYNC_BUSY || state == SMP_ASYNC_RESERVED))
        return 0;
    return 1;
}

int boot_async_release_attributable(uint32_t claim_word, uint32_t witness,
                                    uint32_t dispatch_gen)
{
    uint32_t gen = dispatch_gen & SMP_ASYNC_GEN_MAX;

    /* A slot stops being owned two ways, and they mean OPPOSITE things.
     *
     *   IDLE(gen)    -- the worker retired its own claim, which it only does
     *                   after storing async_result (async_ipi_handler above).
     *   OFFLINE(gen) -- panic.c parked it on the worker's behalf after a fault
     *                   INSIDE the step, and it parks BEFORE publishing any
     *                   result (panic.c: smp_retract_cpu_online, then
     *                   async_result). A worker that COMPLETED and whose CPU
     *                   parked afterwards reaches the same OFFLINE(gen) word,
     *                   so the claim alone cannot separate them.
     *
     * The witness is what separates them: the worker publishes
     * SMP_ASYNC_CLAIM(IDLE, gen) into async_retired BEFORE retiring, and
     * panic.c never publishes it -- so an exact match is proof the release was
     * the worker's own and its result is attributable to THIS dispatch. Any
     * other generation is a slot re-dispatched since; whatever async_result
     * holds then belongs to that dispatch, not to ours.
     *
     * The STATE test is not redundant with the caller's release check. The
     * worker publishes the witness BEFORE its retiring CAS, so there is a real
     * window where the claim reads BUSY(gen) and the witness already reads
     * IDLE(gen) -- attributable-looking, but the worker is still executing.
     * Every live caller pre-checks boot_async_claim_released(), so that window
     * is unreachable today; making the predicate self-contained is what keeps a
     * future caller from inheriting the assumption silently. */
    if (SMP_ASYNC_STATE_OF(claim_word) == SMP_ASYNC_BUSY ||
        SMP_ASYNC_STATE_OF(claim_word) == SMP_ASYNC_RESERVED)
        return 0;
    return witness == SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, gen) &&
           SMP_ASYNC_GEN_OF(claim_word) == gen;
}

int boot_async_fallback_decision(boot_async_step_state_t st, boot_result_t result,
                                 uint32_t claim_word, uint32_t dispatch_gen)
{
    /* A worker that overran the deadline may have retired its slot since. Ask
     * the claim, not the barrier's memory of it. `result` MUST then be the
     * worker's LIVE published result, not the timeout sentinel the barrier
     * recorded -- passing the sentinel here is what would re-run a driver that
     * merely finished late, which is the same non-idempotent re-entry this
     * whole decision exists to prevent. boot_async_step_fallback() is the
     * caller that gets that provenance right. */
    if (st == BOOT_ASYNC_STEP_RUNNING) {
        if (!boot_async_claim_released(claim_word, dispatch_gen))
            return BOOT_ASYNC_FALLBACK_SKIP;

        st = BOOT_ASYNC_STEP_COMPLETED;
    }

    switch (st) {
        case BOOT_ASYNC_STEP_COMPLETED:
            /* Success is KEPT, not redone. None of the storage initializers is
             * idempotent: each resets its own module-global counters and
             * reprograms the controller, so re-running one that already
             * succeeded reallocates its queues and resets a working device. */
            if (result == BOOT_OK || result == BOOT_DEGRADED)
                return BOOT_ASYNC_FALLBACK_KEEP;
            return BOOT_ASYNC_FALLBACK_RUN;

        case BOOT_ASYNC_STEP_POISONED:
            /* Nobody is executing it, and that is not the same as safe to
             * restart. The worker was parked at an arbitrary instruction inside
             * the initializer, so the controller may be half-programmed, a DMA
             * queue may be partially installed, and any lock it took is
             * abandoned. Degrade the driver instead. */
            return BOOT_ASYNC_FALLBACK_SKIP;

        case BOOT_ASYNC_STEP_UNDISPATCHED:
        default:
            return BOOT_ASYNC_FALLBACK_RUN;
    }
}

int boot_async_step_fallback(const boot_async_step_outcome_t *o)
{
    struct per_cpu_data *ap;
    uint32_t claim;
    boot_result_t result;

    if (!o)
        return BOOT_ASYNC_FALLBACK_RUN;

    ap = o->worker_cpu ? smp_get_cpu(o->worker_cpu) : (struct per_cpu_data *)0;
    if (!ap) {
        /* A step with no resolvable worker cannot be interrogated, so it FAILS
         * CLOSED. Passing claim_word 0 here (as this did) decodes as OFFLINE at
         * generation 0, which boot_async_claim_released() reports as released
         * for any real dispatch generation -- turning an un-interrogable RUNNING
         * step into COMPLETED carrying the barrier's own BOOT_FATAL sentinel,
         * i.e. RUN, re-entering an initializer a CPU may still be inside. A step
         * we cannot ask about is exactly the one not to re-enter.
         *
         * Only a step that never had a worker is safe to run: worker_cpu 0 means
         * the BSP ran it inline, so no AP owns anything. */
        if (o->state == BOOT_ASYNC_STEP_RUNNING ||
            o->state == BOOT_ASYNC_STEP_POISONED)
            return BOOT_ASYNC_FALLBACK_SKIP;
        return boot_async_fallback_decision(o->state, o->result, 0u,
                                            o->dispatch_gen);
    }

    /* ONE acquire load of the claim, reused for both the release test and the
     * result-provenance decision -- two loads could straddle a late completion
     * and mix a "still running" verdict with a "finished" result. */
    claim  = __atomic_load_n(&ap->async_claim, __ATOMIC_ACQUIRE);
    result = o->result;

    /* RESULT PROVENANCE. A timed-out step carries BOOT_FATAL as a SENTINEL, not
     * as something the worker said. Reading the sentinel would re-run a driver
     * that simply finished late, so a released slot licenses substituting the
     * worker's published result -- but ONLY when that release was the worker's
     * own retirement.
     *
     * "Released" is not "finished" (TODO-10 S27). A worker that faults INSIDE
     * the step is parked to OFFLINE by panic.c, which boot_async_claim_released
     * also reports as released, and panic.c parks BEFORE it stores
     * async_result. Accepting the result there reads whichever side of that
     * race won: the dispatch-time BOOT_OK seed, which becomes a KEEP for a
     * driver that never initialized, or the BOOT_FATAL, which becomes a RUN
     * that re-enters an initializer a CPU was cut out of. The step is POISONED
     * in both readings, so decide it as POISONED -- SKIP and degrade.
     *
     * The witness load is ordered AFTER the claim load deliberately: the worker
     * publishes it with RELEASE before its retiring CAS, so the acquire load
     * above is what makes it visible, and loading it first could miss it. */
    if (o->state == BOOT_ASYNC_STEP_RUNNING &&
        boot_async_claim_released(claim, o->dispatch_gen)) {
        uint32_t witness = __atomic_load_n(&ap->async_retired, __ATOMIC_ACQUIRE);

        if (!boot_async_release_attributable(claim, witness, o->dispatch_gen))
            return boot_async_fallback_decision(BOOT_ASYNC_STEP_POISONED,
                                                result, claim, o->dispatch_gen);

        result = (boot_result_t)ap->async_result;
    }

    return boot_async_fallback_decision(o->state, result, claim,
                                        o->dispatch_gen);
}

uint32_t boot_async_quiesce(const boot_async_outcome_t *out, uint32_t timeout_ms)
{
    uint32_t outstanding = 0;
    uint64_t deadline;

    if (!out)
        return 0;

    /* ONE budget for the whole group, not one per step. Deliberate: this is a
     * grace period for workers that already overran a 10-second deadline, and
     * N stuck workers must not cost N x timeout_ms of extra boot. The cost is
     * that a later step can be evaluated against an already-spent budget and
     * counted outstanding without a wait of its own -- which is the right
     * answer (it did not release in the time we had) but is NOT "it still owns
     * its step after timeout_ms", so the message below does not claim that. */
    deadline = system_get_ticks() + timeout_ms / 10;

    for (uint32_t i = 0; i < out->count && i < BOOT_ASYNC_MAX_STEPS; i++) {
        const boot_async_step_outcome_t *o = &out->step[i];
        struct per_cpu_data *ap;

        if (o->state != BOOT_ASYNC_STEP_RUNNING || !o->worker_cpu)
            continue;
        ap = smp_get_cpu(o->worker_cpu);
        if (!ap)
            continue;

        while (!boot_async_claim_released(
                   __atomic_load_n(&ap->async_claim, __ATOMIC_ACQUIRE),
                   o->dispatch_gen)) {
            if (system_get_ticks() > deadline) {
                klog(LOG_ERROR, "ASYNC",
                     "[ASYNC] %s on CPU%u still owns its step at the end of the "
                     "%ums group grace period -- downstream consumers of that "
                     "driver are NOT safe",
                     o->worker_cpu ? (ap->async_name ? ap->async_name : "?") : "?",
                     (uint32_t)o->worker_cpu, timeout_ms);
                outstanding++;
                break;
            }
            __asm__ volatile("pause");
        }
    }

    return outstanding;
}

boot_result_t boot_async_group(const char *group_name,
                               boot_async_step_t *steps, uint32_t count)
{
    return boot_async_group_ex(group_name, steps, count, (void *)0);
}

boot_result_t boot_async_group_ex(const char *group_name,
                                  boot_async_step_t *steps, uint32_t count,
                                  boot_async_outcome_t *out)
{
    extern void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector);

    /* Seed every slot UNDISPATCHED so a caller reading an index this group never
     * touched gets the conservative answer rather than a stale one. */
    if (out) {
        out->count = count < BOOT_ASYNC_MAX_STEPS ? count : BOOT_ASYNC_MAX_STEPS;
        for (uint32_t i = 0; i < BOOT_ASYNC_MAX_STEPS; i++) {
            out->step[i].state        = BOOT_ASYNC_STEP_UNDISPATCHED;
            out->step[i].result       = BOOT_OK;
            out->step[i].worker_cpu   = 0;
            out->step[i].dispatch_gen = 0;
        }
    }

    if (count == 0) return BOOT_OK;

    uint32_t ncpus = smp_cpu_count();
    uint32_t bsp_id = smp_cpu_id();

    POST16(POST16_ASYNC);
    klog(LOG_INFO, "ASYNC", "--- Async group '%s': %u step(s) on %u CPU(s) ---",
         group_name, count, ncpus);

    /* If only 1 CPU, run everything sequentially on BSP */
    if (ncpus <= 1) {
        boot_result_t worst = BOOT_OK;
        for (uint32_t i = 0; i < count; i++) {
            klog(LOG_INFO, "ASYNC", "[ASYNC] %s (sequential, 1 CPU)",
                 steps[i].name);
            uint64_t t0 = system_get_ticks();
            boot_result_t r = steps[i].fn();
            uint64_t elapsed_ms = (system_get_ticks() - t0) * 10;
            klog(LOG_INFO, "ASYNC", "[ASYNC] %s completed in %ums",
                 steps[i].name, (uint32_t)elapsed_ms);
            boot_async_record(out, i, BOOT_ASYNC_STEP_COMPLETED, r, 0, 0);
            if (boot_result_severity(r) > boot_result_severity(worst)) worst = r;
        }
        POST16(POST16_ASYNC_DONE);
        return worst;
    }

    /* CLAIM the AP slots this group will use (TODO-10 S21). Logical CPU IDs are
     * slot-allocated, so after a partial bringup the online set can be sparse
     * (slot 1 abandoned, slot 2 live). smp_cpu_count() is a DENSE count; using
     * it as a slot bound (the old `ap_idx < ncpus`) skipped a live high slot and
     * left it without work. Walk every slot and CLAIM the ones that are both
     * online and idle; one async step goes to each, step 0 stays on the BSP,
     * overflow to BSP.
     *
     * A CLAIM, not the is_online snapshot it replaces. The snapshot was an
     * acquire-load that could not be retracted: a worker this group selected
     * could park (panic.c async fault) before the dispatch reached it, and the
     * group still assigned it a step and then waited out the whole 10s barrier
     * for a CPU that can never answer. The claim CAS makes selection and
     * ownership ONE transition, and it also refuses a slot still BUSY from an
     * earlier group's timed-out worker -- that worker is deliberately left
     * running (see the barrier below), so its slot must stay un-reusable until
     * it retires itself. */
    uint32_t claimed_aps[MAX_CPUS];
    uint32_t claimed_gen[MAX_CPUS];
    uint32_t n_workers = 0;
    uint32_t max_workers = count - 1;

    {
        struct per_cpu_data *bsp = smp_get_cpu(0);
        if (bsp) {
            bsp->async_done = 0; bsp->async_result = (uint8_t)BOOT_OK;
            bsp->async_fn = (void *)0; bsp->async_name = (void *)0;
            bsp->in_async_work = 0;
        }
    }

    for (uint32_t s = 1; s < MAX_CPUS && n_workers < max_workers; s++) {
        struct per_cpu_data *ap = smp_get_cpu(s);
        uint32_t gen = 0;

        if (!ap || !smp_cpu_is_online(s))
            continue;
        if (!smp_async_claim_dispatch(&ap->async_claim, &gen))
            continue;   /* parked, or still owned by an earlier dispatch */

        /* RESERVED, not yet runnable. The slot is ours for `gen`, so nothing
         * else may write these fields, and a stale async_fn from a larger
         * earlier group is overwritten here rather than left armed. Arming
         * before the payload landed would let a delayed or misdelivered IPI
         * run the PREVIOUS group's function under this generation and retire
         * this dispatch's claim, reporting a step complete that never ran. */
        ap->async_done = 0; ap->async_result = (uint8_t)BOOT_OK;
        ap->in_async_work = 0;
        /* No retirement witnessed for THIS dispatch yet. Cleared inside the
         * reserve window, where the slot is ours and nothing else may write it. */
        __atomic_store_n(&ap->async_retired, 0u, __ATOMIC_RELAXED);
        ap->async_name = steps[n_workers + 1].name;
        ap->async_fn   = (void *)steps[n_workers + 1].fn;
        smp_mb();

        /* RESERVED -> BUSY: the payload is published, the slot is runnable.
         * Fails only if the CPU parked in between, in which case it is not a
         * worker of this group at all and its step falls to the BSP overflow
         * loop below. */
        if (!smp_async_claim_arm(&ap->async_claim, gen)) {
            klog(LOG_WARN, "ASYNC",
                 "[ASYNC] CPU%u parked between reserve and dispatch -- step "
                 "falls back to the BSP", (uint32_t)s);
            ap->async_fn = (void *)0; ap->async_name = (void *)0;
            continue;
        }

        claimed_aps[n_workers] = s;
        claimed_gen[n_workers] = gen;
        /* Provisional: this step now has an owner. The barrier below refines it
         * to COMPLETED / RUNNING / POISONED. Recording the owner HERE (not at
         * the barrier) is what lets the fallback re-read the right claim word
         * even for a step whose worker never answered. */
        boot_async_record(out, n_workers + 1, BOOT_ASYNC_STEP_RUNNING,
                          BOOT_FATAL, s, gen);
        n_workers++;
    }
    smp_mb();

    /* Wake the claimed workers only after EVERY payload is published, so a fast
     * AP cannot observe its own async_fn before the fence. */
    for (uint32_t w = 0; w < n_workers; w++) {
        struct per_cpu_data *ap = smp_get_cpu(claimed_aps[w]);
        if (!ap) continue;
        lapic_send_ipi(ap->lapic_id, IPI_VECTOR_ASYNC_INIT);
    }

    /* BSP runs step 0 directly */
    klog(LOG_INFO, "ASYNC", "[ASYNC] %s started on CPU%u (BSP)",
         steps[0].name, bsp_id);
    uint64_t t0 = system_get_ticks();
    boot_result_t bsp_result = steps[0].fn();
    uint64_t bsp_ms = (system_get_ticks() - t0) * 10;
    klog(LOG_INFO, "ASYNC", "[ASYNC] %s completed on CPU%u (BSP) in %ums",
         steps[0].name, bsp_id, (uint32_t)bsp_ms);
    boot_async_record(out, 0, BOOT_ASYNC_STEP_COMPLETED, bsp_result, 0, 0);

    /* If more steps than workers, BSP runs the remaining ones sequentially
     * (step 0 + n_workers worker steps already assigned). */
    for (uint32_t i = 1 + n_workers; i < count; i++) {
        klog(LOG_INFO, "ASYNC", "[ASYNC] %s (overflow, BSP)", steps[i].name);
        boot_result_t r = steps[i].fn();
        boot_async_record(out, i, BOOT_ASYNC_STEP_COMPLETED, r, 0, 0);
        if (boot_result_severity(r) > boot_result_severity(bsp_result)) bsp_result = r;
    }

    /* Barrier: wait for all APs to complete */
    POST16(POST16_ASYNC_BARRIER);
    uint32_t timeout_ms = BOOT_ASYNC_BARRIER_MS;
    uint64_t deadline = system_get_ticks() + timeout_ms / 10;

#ifdef KERNEL_TESTS
    /* Anchor the held worker's deadlock backstop (TODO-10 S27). Published HERE,
     * at the same instant as the deadline it must outlast, so the backstop
     * measures the barrier rather than racing the BSP's pre-barrier steps. */
    __atomic_store_n(&s_async_barrier_epoch, system_get_ticks(),
                     __ATOMIC_RELEASE);
#endif

    /* BSP-local, authoritative record of which workers did not deliver a result
     * of their own -- timed out, or parked before completing. A timed-out AP is
     * still running and may later overwrite its async_result with
     * BOOT_OK/DEGRADED before the collect loop reads it; deriving the result
     * from this local flag (not the shared async_result) makes the failure
     * sticky, so a late AP completion cannot make the FATAL vanish. The parked
     * case needs the same treatment for a sharper reason: panic.c retracts
     * online membership BEFORE it stores async_result and async_done, so a
     * barrier that exited on the retraction and then read async_result would
     * collect the PREVIOUS group's BOOT_OK and call a faulted step successful. */
    uint8_t forced_fatal[MAX_CPUS] = {0};

    for (uint32_t w = 0; w < n_workers; w++) {
        struct per_cpu_data *ap = smp_get_cpu(claimed_aps[w]);
        if (!ap) continue;

        while (!ap->async_done) {
            /* The worker's CPU parked while owning THIS dispatch: it will never
             * publish, so stop waiting now instead of burning the remaining
             * deadline on a CPU that cannot answer. */
            if (smp_async_claim_is_dead(&ap->async_claim, claimed_gen[w])) {
                klog(LOG_WARN, "ASYNC",
                     "[ASYNC] OFFLINE: %s on CPU%u parked before completing",
                     ap->async_name ? ap->async_name : "?", ap->cpu_id);
                forced_fatal[w] = 1;
                boot_async_record(out, w + 1, BOOT_ASYNC_STEP_POISONED,
                                  BOOT_FATAL, claimed_aps[w], claimed_gen[w]);
                break;
            }
            if (system_get_ticks() > deadline) {
                klog(LOG_WARN, "ASYNC",
                     "[ASYNC] TIMEOUT: %s on CPU%u did not complete in %ums -- "
                     "worker still owns its slot",
                     ap->async_name ? ap->async_name : "?",
                     ap->cpu_id, timeout_ms);
                forced_fatal[w] = 1;
                /* The completion is NOT fabricated (TODO-10 S27). Writing
                 * async_done=1 and async_result=BOOT_FATAL here used to make a
                 * still-running worker look finished, which is precisely what
                 * licensed the caller to re-enter the initializer that worker
                 * was inside. The slot stays BUSY at its dispatch generation --
                 * the truth -- and the outcome record carries that fact to the
                 * caller, which re-reads the claim before deciding.
                 *
                 * Dropping the write costs nothing else: the only reader of
                 * async_done is this loop, which exits through this branch
                 * anyway, and the group result comes from the BSP-local
                 * forced_fatal[] flag rather than the shared field. It also
                 * removes a real two-writer race, since the worker may store
                 * its own result into the same bytes at any moment. */
                boot_async_record(out, w + 1, BOOT_ASYNC_STEP_RUNNING,
                                  BOOT_FATAL, claimed_aps[w], claimed_gen[w]);
                break;
            }
            __asm__ volatile("pause");
        }
    }

    /* Acquire barrier: pair the AP-side release (store result -> smp_mb ->
     * store async_done) so the BSP cannot observe async_done=1 with a stale
     * async_result and mask an AP FATAL/DEGRADED in the worst-pick below. */
    smp_mb();

    /* Collect results. A timed-out or parked worker is forced to BOOT_FATAL
     * from the BSP-local flag regardless of any value a late AP completion may
     * have stored, so a fired failure always drives the worst-pick (and the
     * async-fatal fallback) rather than silently disappearing. */
    boot_result_t worst = bsp_result;
    for (uint32_t w = 0; w < n_workers; w++) {
        struct per_cpu_data *ap = smp_get_cpu(claimed_aps[w]);
        if (!ap) continue;
        boot_result_t r = forced_fatal[w] ? BOOT_FATAL
                                          : (boot_result_t)ap->async_result;
        /* Recorded HERE, after the acquire fence above, because this is the
         * first point at which the worker's result is guaranteed visible. The
         * failed workers already recorded their true disposition (RUNNING /
         * POISONED) in the barrier, and must not be overwritten with a result
         * they never delivered.
         *
         * The barrier's dead-claim check lives INSIDE `while (!ap->async_done)`,
         * so it is skipped entirely when the fault landed before the BSP first
         * evaluated that condition -- panic.c parks the claim and only then
         * publishes async_done. Re-checking here catches that interleaving,
         * which would otherwise record a worker that faulted mid-step as a
         * clean COMPLETED and license the fallback to re-enter it. The witness
         * is what keeps this from over-firing: a worker that completed and
         * whose CPU parked afterwards leaves the same OFFLINE(gen) word, and it
         * is a KEEP, not a poisoning. */
        if (!forced_fatal[w]) {
            int dead = smp_async_claim_is_dead(&ap->async_claim, claimed_gen[w]);
            uint32_t witness = __atomic_load_n(&ap->async_retired, __ATOMIC_ACQUIRE);
            int retired = witness ==
                SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, claimed_gen[w]);

            boot_async_record(out, w + 1,
                              (dead && !retired) ? BOOT_ASYNC_STEP_POISONED
                                                 : BOOT_ASYNC_STEP_COMPLETED,
                              r, claimed_aps[w], claimed_gen[w]);
        }
        if (boot_result_severity(r) > boot_result_severity(worst))
            worst = r;
    }

    POST16(POST16_ASYNC_DONE);
    klog(LOG_INFO, "ASYNC", "--- Async group '%s' complete (worst=%u) ---",
         group_name, (uint32_t)worst);

    return worst;
}

/* ---- UEFI NVRAM POST code persistence ----------------------------------- */

/* Impossible OS POST GUID: {494D504F-5354-4F53-504F-535447554944} */
static const struct boot_uefi_guid s_post_guid = {
    0x494D504F, 0x5354, 0x4F53,
    { 0x50, 0x4F, 0x53, 0x54, 0x47, 0x55, 0x49, 0x44 }
};

/* UCS-2 variable name: "ImpossiblePOST" */
static const uint16_t s_post_name[] = {
    'I','m','p','o','s','s','i','b','l','e','P','O','S','T', 0
};

/* Attributes for persistent boot-visible variable */
#define POST_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                    EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                    EFI_VARIABLE_RUNTIME_ACCESS)

/* ---- 16-bit POST code system --------------------------------------------- */

/* Plain-RAM shadow of the last POST code. boot_post_read16() reads NVRAM (UEFI
 * GetVariable, sleepable mutex + firmware), which is NOT safe from a faulted
 * panic context; the panic collector reads this shadow instead. */
static volatile uint16_t s_last_post16 = 0;

uint16_t boot_post_last_shadow(void)
{
    return s_last_post16;
}

void boot_post_write16(uint16_t code)
{
    s_last_post16 = code;   /* fault-safe RAM shadow for the panic collector */

    /* I/O port 0x80: always write -- zero cost, works before anything */
    __asm__ volatile("outb %0, $0x80" :: "a"((uint8_t)(code >> 8)));

    /* On-screen display -- direct VRAM, works before fb_init */
    post_display16(code);

    /* No NVRAM write here -- flash has limited endurance (~100K cycles).
     * NVRAM is updated only at a few key milestones via
     * boot_post_nvram_write16(): the Phase 0 "booting" entry code, a small set
     * of mid-boot stage markers (POST16_SIMD_OK, POST16_TIMER_OK,
     * POST16_REGISTRY_OK), the Phase 3 "succeeded" mark (POST16_BOOT_OK), and
     * POST16_BOOT_FAILED from the panic handler. On the next boot the value is
     * POST16_BOOT_OK iff the previous boot completed; anything else means it
     * did not (the crash banner reads it via boot_post_read16). Note: a clean
     * panic overwrites the last stage marker with the generic
     * POST16_BOOT_FAILED, so the crash banner shows a generic failure rather
     * than the exact failing stage (a stage-preserving enhancement is tracked
     * in the VPD crash-persistence follow-up). */
}

void boot_post_nvram_write16(uint16_t code)
{
    /* boot.conf postcode=0 disables POST persistence: skip the NVRAM write
     * entirely (flash endurance is ~100K cycles, and the user opted out).
     * Mirrors the on-screen POST display gate in boot_progress.c. When no
     * boot.conf was found, default behavior (persist) is preserved. */
    if (g_boot_info.config.config_found && !g_boot_info.config.postcode)
        return;

    /* No per-call LAPIC timer mask here: uefi_set_variable() serializes on the
     * RT mutex and quiesces the timer INSIDE that lock (timer_hal_quiesce in
     * uefi_runtime.c), bounding the mask to the firmware call. An outer mask
     * here would instead span the (sleepable) mutex wait and stall scheduling. */
    uint64_t status = uefi_set_variable(&s_post_guid, s_post_name,
                                        POST_ATTRS, sizeof(code), &code);

    /* A silently-dropped write leaves the OLD ImpossiblePOST in NVRAM, which
     * the next boot reads as authoritative -- so a failed success-mark can
     * masquerade as a prior failure (or vice versa). Surface it on serial.
     * Skip the panic POST-FAILED mark: serial_write() takes g_serial_lock and
     * the panic-path NVRAM write itself is the panic-safe-fatal follow-up
     * (Failure Policy section). */
    if (status != 0 && code != POST16_BOOT_FAILED)
        serial_write("[POST] NVRAM SetVariable failed -- prior code may be stale\n");
}

int boot_post_read16(void)
{
    uint16_t val = 0;
    uint64_t sz = sizeof(val);
    uint32_t attrs = 0;

    uint64_t status = uefi_get_variable(&s_post_guid, s_post_name,
                                        &attrs, &sz, &val);
    /* Only trust a value that WE wrote: name+GUID alone can collide with an
     * externally-created or stale variable that has different attributes, and
     * UEFI SetVariable cannot rewrite attributes in place (it would need a
     * delete+recreate), so a wrong-attr value could be reported as the prior
     * boot outcome indefinitely. Require the canonical POST_ATTRS. */
    if (status != 0 || attrs != POST_ATTRS)
        return -1;
    /* Backward-compatible: if only 1 byte was stored, treat as high byte */
    if (sz == 1)
        return (int)((uint16_t)(*(uint8_t *)&val) << 8);
    if (sz == 2)
        return (int)val;
    return -1;
}

/* ---- Boot progress emitter ----------------------------------------------- */

static void serial_write_hex16(uint16_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_putchar(hex[(v >> 12) & 0xF]);
    serial_putchar(hex[(v >>  8) & 0xF]);
    serial_putchar(hex[(v >>  4) & 0xF]);
    serial_putchar(hex[ v        & 0xF]);
}

void boot_progress(uint8_t phase, const char *step, uint16_t postcode)
{
    /* Pet the hardware watchdog at every boot milestone (no-op unless a WDAT
     * watchdog is armed). This is the primary pet path -- a boot phase that
     * stops calling boot_progress() lets the timer expire and reboots. */
    hw_watchdog_pet();

    /* "[PHASEn] step (0xNNNN)\n" */
    const char *safe_step = step ? step : "(null)";
    serial_write("[PHASE");
    serial_putchar('0' + (phase & 0x0F));
    serial_write("] ");
    serial_write(safe_step);
    serial_write(" (0x");
    serial_write_hex16(postcode);
    serial_write(")\n");

    /* Don't pollute perf data with NULL-step test calls */
    if (step)
        boot_timing_record_step(phase, safe_step, postcode);
    boot_post_write16(postcode);  /* writes port 0x80 + RAM shadow + post_display16 */

    /* VPD Tier 1: show named stage with status indicator */
    {
        extern void vpd_stage_begin(uint8_t, const char *, uint16_t);
        extern int vpd_is_active(void);
        if (vpd_is_active())
            vpd_stage_begin(phase, safe_step, postcode);
    }

    /* NOTE: do NOT call boot_splash_status() here -- the step name is an
     * internal identifier (e.g. "EXEC", "PCI_NET"), not a user-friendly
     * message.  Friendly status is set by explicit boot_splash_status()
     * calls before each boot_progress() in the boot sequence. */
}
