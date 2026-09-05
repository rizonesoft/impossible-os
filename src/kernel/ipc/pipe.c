/* ============================================================================
 * pipe.c -- Kernel IPC pipes
 *
 * 4 KiB ring buffer pipes with blocking read/write using mutex + semaphores.
 *
 * Write path:
 *   1. sem_wait(writable) -- block if buffer full
 *   2. mutex_lock(lock)
 *   3. Copy byte to ring buffer, advance write_pos
 *   4. mutex_unlock(lock)
 *   5. sem_signal(readable) -- wake a blocked reader
 *
 * Read path:
 *   1. sem_wait(readable) -- block if buffer empty
 *   2. mutex_lock(lock)
 *   3. Copy byte from ring buffer, advance read_pos
 *   4. mutex_unlock(lock)
 *   5. sem_signal(writable) -- wake a blocked writer
 * ============================================================================ */

#include "kernel/ipc/pipe.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"      /* pmm_alloc_pages_hhdm -- frame-backed pipe pool */
#include "kernel/klog.h"
#include "libc/string.h"        /* memset -- zero the frame-backed pipe pool */
#include "kernel/atomic.h"      /* atomic_t -- concurrency-safe pipe_init() */

/* ---- Global pipe table ----
 *
 * Frame-backed (TODO-33 s15): was `static pipe_t pipes[PIPE_MAX]`, 73,216
 * bytes of .bss. Every access site already uses pipes[i]/&pipes[i], which is
 * identical syntax whether pipes is an array or a pointer, so the real
 * changes are the declaration, the explicit-byte-count allocation in
 * pipe_init(), and a readiness guard at each of the four entry points.
 *
 * The guards are NOT optional bookkeeping. Over the old zeroed BSS array,
 * pipe_write/pipe_read/pipe_close could index pipes[pipe_id] before any init
 * ran and simply read in_use == 0, so an unreachable-pipe call returned -1 by
 * accident. Over a pointer the same code is a NULL dereference, so what used
 * to be an accident has to become the contract.
 *
 * pipe_init_state makes initialization concurrency-safe even though the one
 * production caller (boot_desktop.c, phase 3) is BSP-only: two CPUs both
 * observing UNINIT would double-allocate and publish mismatched phys/pages
 * against each other. The CAS makes exactly one caller the allocator;
 * atomic_set's release on the READY transition makes every write below it
 * (the pool contents, pipes_phys/pipes_pages, the pipes pointer itself)
 * visible to any CPU that acquire-reads READY before touching pipes. This
 * closes only the INIT race, and the scope of that claim matters: the PMM
 * bitmap has no internal SMP lock, so a concurrent unrelated PMM caller
 * remains a live, separately-tracked gap (pmm.h caller contract; owned by
 * 03-memory-concurrency/TODO-03 s1) -- and, larger, the pool's OWN slot
 * bookkeeping is still not SMP-safe. pipe_create() scans for a free slot and
 * claims it without a lock, and pipe_read()/pipe_write() read count,
 * read_open and write_open outside p->lock. Both are pre-existing, both are
 * ring-3-reachable, and neither is fixed here: this file is SMP-audited for
 * INITIALISATION only -> XREF: 03-memory-concurrency/TODO-09 s11.
 *
 * The CAS also fixes a latent bug in the old idempotence. pipe_init() is
 * documented "safe to call multiple times", but the old body re-zeroed in_use
 * for every slot, so a second call silently marked every LIVE pipe free while
 * its holders kept their ids. src/kernel/test/test_ipc.c:125 makes exactly
 * that second call, after boot_desktop.c:238 already ran. A repeat call is
 * now a no-op that reports the state it found. */
#define PIPE_INIT_UNINIT       0
#define PIPE_INIT_INITIALIZING 1
#define PIPE_INIT_READY        2
static atomic_t pipe_init_state = ATOMIC_INIT(PIPE_INIT_UNINIT);

static pipe_t   *pipes;
static uintptr_t pipes_phys;
static uint64_t  pipes_pages;

int pipe_ready(void)
{
    return atomic_read(&pipe_init_state) == PIPE_INIT_READY;
}

boot_result_t pipe_init(void)
{
    /* Explicit byte count -- sizeof(pipes) would silently collapse to the
     * size of a pointer now that this is frame-backed, not a BSS array. */
    const uint64_t bytes = (uint64_t)PIPE_MAX * sizeof(pipe_t);
    uintptr_t phys = 0;
    uint64_t  pages = 0;
    pipe_t   *pool;
    int32_t   prev;

    prev = atomic_cmpxchg(&pipe_init_state, PIPE_INIT_UNINIT,
                          PIPE_INIT_INITIALIZING);
    if (prev != PIPE_INIT_UNINIT) {
        /* Repeat call, or another CPU is mid-init. Report what is true NOW
         * rather than a blanket BOOT_OK: READY is a usable pool, and
         * INITIALIZING is not one yet, so claiming success would hand the
         * caller a pool it must not touch. */
        return (prev == PIPE_INIT_READY) ? BOOT_OK : BOOT_DEGRADED;
    }

    pool = (pipe_t *)pmm_alloc_pages_hhdm(bytes, &phys, &pages);
    if (!pool) {
        /* Degraded, not fatal. boot_desktop.c halts only on BOOT_FATAL and
         * routes BOOT_DEGRADED into the degraded-subsystem path, and every
         * entry point below refuses through pipe_ready() rather than
         * dereferencing NULL. Back to UNINIT rather than a terminal failed
         * state, so the state machine does not foreclose a retry -- but be
         * honest that no production retry EXISTS: boot_desktop.c:238 is the
         * only non-test caller and it runs once at phase 3, so a genuine
         * allocation failure there is the last word for the session. The
         * rollback is what lets the KERNEL_TESTS recovery path re-init, and
         * what a future on-demand retry would need; it is not one itself. */
        klog(LOG_ERROR, "ipc",
             "pipe: failed to allocate pipe pool (%u bytes) -- IPC pipes degraded",
             bytes);
        atomic_set(&pipe_init_state, PIPE_INIT_UNINIT);
        return BOOT_DEGRADED;
    }

    /* pmm_alloc_pages_hhdm does not zero. The old body set only in_use = 0
     * per slot and relied on BSS-zero for the rest, so an all-zero pool
     * reproduces the previous initial state exactly: pipe_create() assigns
     * every other field of a slot at the moment it claims it. */
    memset(pool, 0, bytes);   /* size_t: do NOT narrow -- PIPE_MAX is a header constant */

    pipes_phys  = phys;
    pipes_pages = pages;
    pipes       = pool;          /* publication word before the release below */
    atomic_set(&pipe_init_state, PIPE_INIT_READY);
    return BOOT_OK;
}

#ifdef KERNEL_TESTS
/* Test-only: free the pool and reset to uninitialized so a test can exercise
 * pipe_init()'s OOM path via pmm_alloc_fail_next(), then re-initialize for
 * real afterward. Never called outside KERNEL_TESTS.
 *
 * Un-publish BEFORE freeing -- the mirror image of pipe_init()'s publish
 * order. Once the state is UNINIT, pipe_ready() and the four entry guards
 * refuse before pipes is ever touched, so nothing can observe READY while
 * the frames underneath it are being freed.
 *
 * Un-publishing is NOT sufficient on its own, and this is the one place the
 * ctrl_windows precedent does not transfer. Release/acquire publication
 * orders VISIBILITY, not STORAGE LIFETIME: a thread that already passed
 * pipe_ready() and is parked inside pipe_read() -> sem_wait() holds a
 * pipe_t * into these frames, and freeing underneath it is a use-after-free
 * that no amount of store ordering prevents. So the reset REFUSES while any
 * slot is still claimed, and returns 0 so its caller fails loudly instead of
 * proceeding over a pool it could not reclaim. Refusing is the safe half:
 * the worst case is a test that cannot run, against a corrupted heap.
 *
 * One window is NOT closed here, deliberately. A caller that passed
 * pipe_ready() before admission was withdrawn, and has not yet set in_use,
 * is invisible to both the gate and the scan. Closing it needs an admission
 * refcount or a lock taken by every pipe operation -- on pipe_write and
 * pipe_read, which already run a per-BYTE sem_wait/mutex loop. That is a
 * real cost on a production hot path bought solely to harden a KERNEL_TESTS
 * helper, so it is not paid. What makes the residual safe is the calling
 * context rather than the code: this runs from
 * test_pipe_pool_degrades_on_oom() inside boot_tests_run(), a sequential
 * BSP-only suite with no concurrent pipe user in flight.
 *
 * Returns 1 if the pool was reset and freed, 0 if it refused. */
int pipe_test_reset_for_fault_injection(void)
{
    uintptr_t phys;
    uint64_t  pages;
    uint32_t  i;

    if (!pipes)
        return 1;   /* already reset -- nothing to reclaim, nothing to refuse */

    /* WITHDRAW ADMISSION FIRST, then scan. Scanning while the state still
     * says READY is trivially wrong: pipe_create() could pass the gate on
     * another CPU and claim a slot this loop has already walked past, and
     * the frames would then be freed underneath it.
     *
     * Be precise about what this buys, because it is less than it looks.
     * Holding INITIALIZING does exclude other initialisers and every new
     * consumer for the whole teardown. What it does NOT do is retract a
     * READY that some CPU already loaded: a caller already past pipe_ready()
     * and not yet holding a slot is invisible to both the gate and the scan.
     * So the scan is not a proof of quiescence on its own -- for that
     * residual the safety comes from the caller (see the header comment), a
     * sequential BSP-only test suite.
     *
     * Restore READY on refusal, so a refused reset leaves the subsystem
     * exactly as it found it. */
    if (atomic_cmpxchg(&pipe_init_state, PIPE_INIT_READY,
                       PIPE_INIT_INITIALIZING) != PIPE_INIT_READY) {
        klog(LOG_ERROR, "ipc", "pipe pool reset refused: pool not READY");
        return 0;
    }

    /* We now HOLD the token, and holding INITIALIZING is the point: an
     * earlier draft of this function CAS'd READY -> UNINIT, which reads like
     * ownership and is not. UNINIT is precisely the state pipe_init()'s own
     * CAS accepts, so that draft withdrew consumers and in the same
     * instruction invited an initialiser to allocate a replacement pool over
     * the one being scanned and freed -- and its restore-on-refusal would
     * then have stored READY over that initialiser's INITIALIZING.
     * INITIALIZING excludes both: pipe_ready() is false on it, so no
     * consumer is admitted, and pipe_init()'s CAS requires UNINIT, so no
     * initialiser is either. UNINIT is published LAST, after teardown. */

    for (i = 0; i < PIPE_MAX; i++) {
        if (pipes[i].in_use) {
            klog(LOG_ERROR, "ipc",
                 "pipe pool reset refused: slot %u still in use", (uint64_t)i);
            /* Safe because we hold INITIALIZING: nothing else can have
             * moved the state, so this returns exactly what the CAS took. */
            atomic_set(&pipe_init_state, PIPE_INIT_READY);
            return 0;
        }
    }

    phys  = pipes_phys;
    pages = pipes_pages;

    pipes       = (pipe_t *)0;
    pipes_phys  = 0;
    pipes_pages = 0;

    pmm_free_contiguous(phys, pages);

    /* Release the token LAST. Publishing UNINIT any earlier would re-open
     * pipe_init() over frames this function had not finished reclaiming. */
    atomic_set(&pipe_init_state, PIPE_INIT_UNINIT);
    return 1;
}
#endif

int pipe_create(int fds[2])
{
    uint32_t i;

    /* No lazy init: pmm_alloc_pages_hhdm's caller contract forbids allocating
     * on a call path reached once the scheduler is live, because it races the
     * unlocked PMM bitmap unconditionally. The old `if (!pipe_inited)
     * pipe_init();` here was exactly that path. boot_desktop.c:238 initializes
     * the subsystem at phase 3, before any consumer can reach this. */
    if (!pipe_ready()) {
        klog(LOG_ERROR, "ipc", "pipe_create on a degraded pipe pool");
        return -1;
    }

    /* Find a free pipe slot */
    for (i = 0; i < PIPE_MAX; i++) {
        if (!pipes[i].in_use)
            break;
    }

    if (i >= PIPE_MAX) {
        klog(LOG_DEBUG, "ipc", "No free pipe slots");
        return -1;
    }

    /* Initialize the pipe */
    pipes[i].read_pos = 0;
    pipes[i].write_pos = 0;
    pipes[i].count = 0;
    pipes[i].read_open = 1;
    pipes[i].write_open = 1;
    pipes[i].in_use = 1;

    mutex_init(&pipes[i].lock, "pipe_lock");
    sem_init(&pipes[i].readable, "pipe_readable", 0);
    sem_init(&pipes[i].writable, "pipe_writable", (int32_t)PIPE_BUF_SIZE);

    /* Both fds reference the same pipe ID */
    fds[0] = (int)i;  /* read end */
    fds[1] = (int)i;  /* write end */

    klog(LOG_DEBUG, "ipc", "Pipe %u created", (uint64_t)i);
    return 0;
}

int32_t pipe_write(int pipe_id, const void *data, uint32_t len)
{
    pipe_t *p;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t i;

    if (!pipe_ready())
        return -1;

    if (pipe_id < 0 || pipe_id >= (int)PIPE_MAX)
        return -1;

    p = &pipes[pipe_id];

    if (!p->in_use || !p->write_open)
        return -1;

    /* Check if read end is closed -- broken pipe */
    if (!p->read_open) {
        klog(LOG_DEBUG, "ipc", "Broken pipe (write to closed read end)");
        return -1;  /* SIGPIPE equivalent */
    }

    /* Write one byte at a time, blocking if buffer full */
    for (i = 0; i < len; i++) {
        /* Check for broken pipe before each byte */
        if (!p->read_open) {
            return (i > 0) ? (int32_t)i : -1;
        }

        /* Wait for space in the buffer */
        sem_wait(&p->writable);

        mutex_lock(&p->lock);
        p->buf[p->write_pos] = src[i];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        p->count++;
        mutex_unlock(&p->lock);

        /* Signal that data is available */
        sem_signal(&p->readable);
    }

    return (int32_t)len;
}

int32_t pipe_read(int pipe_id, void *buf, uint32_t len)
{
    pipe_t *p;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t i;

    if (!pipe_ready())
        return -1;

    if (pipe_id < 0 || pipe_id >= (int)PIPE_MAX)
        return -1;

    p = &pipes[pipe_id];

    if (!p->in_use || !p->read_open)
        return -1;

    /* Read one byte at a time */
    for (i = 0; i < len; i++) {
        /* EOF check: write end closed and buffer empty */
        if (!p->write_open && p->count == 0)
            return (int32_t)i;  /* EOF */

        /* If no data and write end still open, block on first byte.
         * For subsequent bytes, only read what's available. */
        if (p->count == 0) {
            if (i > 0)
                return (int32_t)i;  /* return what we have so far */

            /* Block waiting for data (or EOF) */
            while (p->count == 0 && p->write_open) {
                sem_wait(&p->readable);
                /* Re-check conditions after wakeup */
                if (!p->write_open && p->count == 0)
                    return 0;  /* EOF */
            }

            /* If writer closed while we were waiting and buffer is empty */
            if (p->count == 0)
                return 0;  /* EOF */
        } else {
            /* Non-blocking consume of a semaphore token */
            sem_wait(&p->readable);
        }

        mutex_lock(&p->lock);
        dst[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->count--;
        mutex_unlock(&p->lock);

        /* Signal that space is available */
        sem_signal(&p->writable);
    }

    return (int32_t)len;
}

void pipe_close(int pipe_id, int end)
{
    pipe_t *p;

    if (!pipe_ready())
        return;

    if (pipe_id < 0 || pipe_id >= (int)PIPE_MAX)
        return;

    p = &pipes[pipe_id];

    if (!p->in_use)
        return;

    if (end == PIPE_READ) {
        p->read_open = 0;
        /* Wake any blocked writers */
        sem_signal(&p->writable);
    } else if (end == PIPE_WRITE) {
        p->write_open = 0;
        /* Wake any blocked readers so they see EOF */
        sem_signal(&p->readable);
    }

    /* Free pipe if both ends are closed */
    if (!p->read_open && !p->write_open) {
        p->in_use = 0;
        klog(LOG_DEBUG, "ipc", "Pipe %u destroyed", (uint64_t)pipe_id);
    }
}
