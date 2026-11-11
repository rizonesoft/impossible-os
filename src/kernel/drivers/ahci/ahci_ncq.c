/* ============================================================================
 * ahci_ncq.c -- NCQ (Native Command Queuing) tag management and I/O
 * ============================================================================ */

#include "kernel/drivers/ahci_internal.h"
#include "kernel/sched/event.h"
#include "kernel/sched/workqueue.h"

/* ---- NCQ tag allocator (bitmap) ---- */

int ncq_alloc_tag(struct ahci_port *p)
{
    uint32_t mask = (p->ncq_depth >= 32) ? 0xFFFFFFFF
                  : ((1U << p->ncq_depth) - 1);
    uint32_t free = ~p->tags_allocated & mask;
    int i;
    if (!free) return -1;
    for (i = 0; i < 32; i++) {
        if (free & (1U << i)) {
            p->tags_allocated |= (1U << i);
            return i;
        }
    }
    return -1;
}

void ncq_free_tag(struct ahci_port *p, int tag)
{
    p->tags_allocated &= ~(1U << tag);
    p->tag_callbacks[tag] = (ahci_callback_t)0;
    p->tag_cb_ctx[tag] = (void *)0;
}

/* ---- Issue an NCQ (FPDMA) command ---- */

int ncq_issue_rw(struct ahci_port *p, int tag, uint64_t lba,
                 uint32_t count, void *buffer, int is_write, int fua)
{
    struct ahci_cmd_header *hdr = &p->cmdlist[tag];
    struct ahci_cmd_tbl *tbl = p->cmdtbl[tag];
    struct fis_reg_h2d *fis;
    uint32_t byte_count = count * p->sector_size;

    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = is_write ? ATA_CMD_WRITE_FPDMA : ATA_CMD_READ_FPDMA;

    fis->featurel = (uint8_t)(count & 0xFF);
    fis->featureh = (uint8_t)((count >> 8) & 0xFF);

    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

    fis->device = (1 << 6);
    if (fua && is_write)
        fis->device |= (1 << 7);

    fis->countl = (uint8_t)(tag << 3);
    fis->counth = 0;

    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)buffer;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = byte_count - 1;

    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F);
    if (is_write)
        hdr->flags |= (1 << 6);
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    p->tag_status[tag] = 0;

    /* Under the completion lock, so a completer's register sample and claim
     * can never straddle this tag being reissued (the pending bit alone is
     * the same bit for the old command and the new one). */
    {
        uint64_t flags;
        int closed;

        if (!ahci_tag_lock(p, &flags))
            return NCQ_ISSUE_CLOSED;   /* lock refused (nested panic): not issued */
        /* Held failures mean the engine halted on an error and needs the
         * recovery's ST cycle before it takes another command. */
        closed = p->ncq_closed ? NCQ_ISSUE_CLOSED
               : (p->ncq_recovering || p->tags_held) ? NCQ_ISSUE_BUSY : 0;
        if (!closed) {
            port_write(p->regs, AHCI_PxSACT, 1U << tag);
            __atomic_fetch_or(&p->tags_pending, 1U << tag, __ATOMIC_RELEASE);
            port_write(p->regs, AHCI_PxCI, 1U << tag);
        }
        ahci_tag_unlock(p, flags);
        if (closed)
            return closed;   /* not issued: the caller still owns the tag */
    }

    return 0;
}

int ahci_ncq_tag_hw_state(uint32_t sact, uint32_t ci, uint32_t pxis, uint32_t bit)
{
    if ((sact | ci) & bit)
        return 0;
    return (pxis & AHCI_PxIS_FATAL) ? -1 : 1;
}

/* Complete `bit` from the HBA registers if the hardware says it finished.
 *
 * The common case, a command still in flight, is answered WITHOUT the lock:
 * a tag still set in PxSACT or PxCI is pending whatever else is true, so a
 * lock-free sample can only ever say "not yet", which is always safe. Only a
 * tag the hardware reports gone is sampled again and claimed under the lock,
 * where the ISR acks PxIS too, so a failure is never read from a status the
 * ISR has already cleared.
 *
 * A clean finish publishes success. A fatal error does not prove the command
 * stopped moving data, so it is HELD for recovery (NCQ_POLL_RECOVER), as is a
 * tag the ISR already held, a port recovery owns, and a refused lock. */
int ahci_ncq_poll_tag(struct ahci_port *p, uint32_t bit)
{
    uint64_t flags;
    int hw;

    if (__atomic_load_n(&p->tags_completed, __ATOMIC_ACQUIRE) & bit)
        return NCQ_POLL_DONE;
    if (__atomic_load_n(&p->ncq_recovering, __ATOMIC_ACQUIRE) ||
        (__atomic_load_n(&p->tags_held, __ATOMIC_ACQUIRE) & bit))
        return NCQ_POLL_RECOVER;
    if (__atomic_load_n(&p->ncq_closed, __ATOMIC_ACQUIRE) ||
        ((port_read(p->regs, AHCI_PxSACT) | port_read(p->regs, AHCI_PxCI)) & bit))
        return NCQ_POLL_PENDING;   /* in flight, or teardown will publish it */

    if (!ahci_tag_lock(p, &flags))
        return NCQ_POLL_RECOVER;
    hw = (p->ncq_recovering || p->ncq_closed) ? 0
       : ahci_ncq_tag_hw_state(port_read(p->regs, AHCI_PxSACT),
                               port_read(p->regs, AHCI_PxCI),
                               port_read(p->regs, AHCI_PxIS), bit);
    if (hw != 0 && ahci_tags_claim(p, bit)) {
        if (hw > 0) {
            ahci_tags_complete(p, bit);
        } else {
            p->tag_status[__builtin_ctz(bit)] = -1;
            p->tags_held |= bit;
        }
    }
    ahci_tag_unlock(p, flags);
    if (__atomic_load_n(&p->tags_completed, __ATOMIC_ACQUIRE) & bit)
        return NCQ_POLL_DONE;
    if (__atomic_load_n(&p->ncq_recovering, __ATOMIC_ACQUIRE) ||
        (__atomic_load_n(&p->tags_held, __ATOMIC_ACQUIRE) & bit))
        return NCQ_POLL_RECOVER;
    return NCQ_POLL_PENDING;
}

/* Recover the port after a request failed or did not complete. ONE owned
 * sequence, so nothing can issue into, or complete from, a port in the middle
 * of being reset (AHCI 1.3.1 section 6.2.2: stop, recover, then restart):
 *
 *   1. Under the completion lock: claim recovery (ahci_recovery_claim; a
 *      caller finding it already owned waits for that recovery instead and
 *      touches nothing), which makes issuers refuse with NCQ_ISSUE_BUSY; stop
 *      the engine and confirm it, then claim, fail
 *      and publish every tag still pending. Stopping under the lock is what
 *      keeps an ISR from reading the cleared PxSACT/PxCI as completions.
 *   2. Outside it (milliseconds of MMIO polling): a COMRESET when the device
 *      still shows BSY or DRQ. Nothing is pending and nothing can issue, so
 *      the cleared registers mean nothing to anyone.
 *   3. Under the lock again: clear SERR and IS, restart the engine (or close
 *      the port when no device came back, or under a panic), end recovery.
 *
 * The confirmed stop is also what lets a request that did not complete hand
 * its buffer back: before it, the HBA may still DMA into it. Under a panic an
 * engine that will not stop gets an HBA reset; with the lock refused (a panic
 * whose holder is parked or interrupted for good) only the stop is done and
 * the port is closed. Returns 1 once the HBA confirmed the stop, 0 if not. */
#define AHCI_RECOVERY_PANIC_WAITS  500u   /* x 10 ms */

/* One wait step of the NCQ paths. Normally a sleep on the port's completion
 * event; under a panic a pure `pause` spin, because event_wait_timeout reads
 * uptime_ns(), which stops advancing once timer interrupts stop (the panic
 * owner runs with them masked), so its timeout could never expire. The panic
 * form is an ITERATION bound, not a time: every panic-path wait stays finite
 * whatever the clock does. */
#define AHCI_PANIC_PAUSES_PER_MS  1000u

static void ncq_wait_step(struct ahci_port *p, uint32_t ms)
{
    uint32_t n;

    if (!panic_in_progress()) {
        event_wait_timeout(&p->completion, ms);
        return;
    }
    for (n = 0; n < ms * AHCI_PANIC_PAUSES_PER_MS; n++)
        __asm__ volatile ("pause" ::: "memory");
}

/* Another caller owns recovery: wait for it to finish and take its outcome.
 * A finished recovery confirmed the engine stopped (one that cannot stop never
 * finishes, leaving the port busy), so 1 means every buffer it failed is out of
 * DMA reach. Bounded only under a panic, whose owner may be parked; 0 then. */
static int ncq_wait_recovery(struct ahci_port *p)
{
    uint32_t waits = 0;

    while (__atomic_load_n(&p->ncq_recovering, __ATOMIC_ACQUIRE)) {
        if (panic_in_progress() && ++waits > AHCI_RECOVERY_PANIC_WAITS)
            return 0;
        ncq_wait_step(p, 10);
    }
    return 1;
}

static int ncq_abort_port(struct ahci_port *p, int wait_for_owner)
{
    struct ahci_tag_done done[32];
    uint32_t failed;
    uint64_t flags;
    int locked = ahci_tag_lock(p, &flags);
    int stopped, linked = 1, t;

    /* Step 1: own the recovery. Released at once: the stop below polls for up
     * to a million MMIO reads, which must not run with interrupts masked or
     * with other CPUs spinning on this lock. ncq_recovering is what keeps the
     * ISR and the pollers from claiming anything off the registers while the
     * engine stops. */
    if (locked) {
        int mine = ahci_recovery_claim(p);

        ahci_tag_unlock(p, flags);
        if (!mine) {
            if (!wait_for_owner)
                return 0;
            if (ncq_wait_recovery(p))
                return 1;
            /* Under a panic the owner never finished: it is parked for good.
             * Take over without its ownership, as for a refused lock: stop the
             * engine (HBA reset if need be) and close the port, rather than
             * treat a controller that may stop fine as unstoppable. */
            locked = 0;
        }
    }
    stopped = port_stop_cmd(p->regs);
    if (!stopped && panic_in_progress())
        stopped = ahci_hba_reset_for_panic();
    if (!locked) {
        if (stopped)
            __atomic_store_n(&p->ncq_closed, 1, __ATOMIC_RELEASE);
        return stopped;
    }
    if (!stopped)
        return 0;   /* recovery stays owned: the port stays busy, held tags stay held */

    /* Step 2: DMA is confirmed stopped, so every tag still pending and every
     * one the ISR held can now be failed and handed back. */
    if (!ahci_tag_lock(p, &flags))
        return stopped;   /* only a nested panic can refuse here; the port stays shut */
    failed = ahci_tags_claim(p, 0xFFFFFFFFu) | p->tags_held;
    p->tags_held = 0;
    for (t = 0; t < 32; t++)
        if (failed & (1U << t))
            p->tag_status[t] = -1;
    ahci_tags_detach(p, failed, 1, done);
    ahci_tags_complete(p, failed);
    ahci_tag_unlock(p, flags);
    ahci_tags_run((int)(p - ports), done);

    /* Step 3: a COMRESET when the device is still stuck. */
    if (!panic_in_progress() &&
        (port_read(p->regs, AHCI_PxTFD) & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ))) {
        linked = (port_comreset_regs(p->regs) == 0);
        p->errors.link_resets++;
    }

    /* Step 4: reopen (or close) the port and end the recovery. */
    if (!ahci_tag_lock(p, &flags))
        return stopped;
    port_write(p->regs, AHCI_PxSERR, 0xFFFFFFFFu);
    port_write(p->regs, AHCI_PxIS, 0xFFFFFFFFu);
    if (!linked || panic_in_progress())
        p->ncq_closed = 1;   /* no device, or the HBA may have been reset */
    else if (!p->ncq_closed)
        port_start_cmd(p->regs);
    ahci_recovery_end(p);
    ahci_tag_unlock(p, flags);
    /* Not under a panic: event_set takes the event's own lock without a
     * bound, and a parked CPU may hold it. Panic waiters poll (ncq_wait_step). */
    if (!panic_in_progress())
        event_set(&p->completion);
    if (!linked)
        klog(LOG_WARN, "ahci", "Port %u: no device after NCQ recovery COMRESET; port closed",
             (uint64_t)p->port_num);
    return stopped;
}

/* Recovery for failures the ISR held when no synchronous waiter will run it
 * (an async request, or a waiter that already left), on sys_wq. Delivery is
 * guaranteed: each port owns its work item (workqueue_enqueue_item takes no
 * pool node) and queues it at most once at a time (recovery_queued). The
 * worker CLAIMS OR RETURNS: if another caller owns recovery, that caller
 * publishes the held tags, so the shared worker leaves instead of waiting, and
 * a recovery whose engine will not stop holds its buffers without holding the
 * worker. Under a panic no worker runs, and the dump's own waiter recovers. */
static work_item_t s_recovery_item[AHCI_MAX_PORTS];

static void ncq_recovery_work(void *arg)
{
    struct ahci_port *p = (struct ahci_port *)arg;

    __atomic_store_n(&p->recovery_queued, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(&p->tags_held, __ATOMIC_ACQUIRE))
        (void)ncq_abort_port(p, 0);
}

void ahci_ncq_queue_recovery(struct ahci_port *p)
{
    if (!sys_wq || panic_in_progress())
        return;
    if (__atomic_exchange_n(&p->recovery_queued, 1, __ATOMIC_ACQ_REL))
        return;
    if (!workqueue_enqueue_item(sys_wq, &s_recovery_item[p - ports], ncq_recovery_work, p))
        __atomic_store_n(&p->recovery_queued, 0, __ATOMIC_RELEASE);
}

/* ---- NCQ synchronous read/write with retry ---- */

int ncq_sync_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
                void *buffer, int is_write, int fua)
{
    int retries = 3;
    int tag, status;

    while (retries > 0) {
        int aborted = 0;

        tag = ncq_alloc_tag(p);
        if (tag < 0)
            return ahci_do_rw(p, lba, count, buffer, is_write, fua);

        {
            int rc;
            uint32_t busy_waits = 0;

            /* A port in recovery is busy, not broken: wait it out. Recovery
             * always ends (every wait in it is bounded), unless a panic parked
             * the CPU running it, hence the bound under a panic. */
            while ((rc = ncq_issue_rw(p, tag, lba, count, buffer, is_write, fua)) == NCQ_ISSUE_BUSY &&
                   !(panic_in_progress() && ++busy_waits > AHCI_RECOVERY_PANIC_WAITS)) {
                /* Held failures with nobody recovering them (their waiter is
                 * gone, or a panic stopped the worker): recover them here. */
                if (__atomic_load_n(&p->tags_held, __ATOMIC_ACQUIRE) &&
                    !__atomic_load_n(&p->ncq_recovering, __ATOMIC_ACQUIRE))
                    (void)ncq_abort_port(p, 1);
                else
                    ncq_wait_step(p, 10);
            }
            if (rc != 0) {
                ncq_free_tag(p, tag);
                return -1;
            }
        }

        {
            uint32_t bit = 1U << tag;
            uint32_t quick = AHCI_NCQ_QUICK_POLLS;
            int timeout = 50;
            int r = NCQ_POLL_PENDING;

            /* The interrupt is a wakeup, not the completion: the HBA's
             * registers are. Poll them briefly before sleeping (a fast
             * command never pays a sleep) and after every wakeup, so a lost
             * or misrouted MSI costs at most one 100 ms wait. A pending poll
             * is two lock-free MMIO reads. A request that needs recovery
             * ends the wait at once. */
            while (quick-- && (r = ahci_ncq_poll_tag(p, bit)) == NCQ_POLL_PENDING)
                __asm__ volatile ("pause" ::: "memory");
            while (r == NCQ_POLL_PENDING && timeout > 0) {
                ncq_wait_step(p, 100);
                timeout--;
                r = ahci_ncq_poll_tag(p, bit);
            }
            if (r != NCQ_POLL_DONE) {
                aborted = 1;
                if (!ncq_abort_port(p, 1)) {
                    /* Nothing could stop the HBA, so it may still be moving
                     * data into the caller's buffer: never hand it back. Under
                     * a panic that ends the panic (reset or halt, as
                     * configured), since the dump cannot finish; otherwise
                     * the request is held. */
                    if (panic_in_progress())
                        panic_abandon_storage("AHCI engine and HBA reset did not stop DMA");
                    klog(LOG_ERROR, "ahci",
                         "Port %u: NCQ tag %u timed out and the engine will not stop; request held",
                         (uint64_t)p->port_num, (uint64_t)tag);
                    for (;;)
                        ncq_wait_step(p, 1000);
                }
                if (!(__atomic_load_n(&p->tags_completed, __ATOMIC_ACQUIRE) & bit)) {
                    /* DMA is stopped, so the buffer is safe, but another
                     * completer (teardown) owns the tag and will still publish
                     * it: keep the tag so that bit never lands on a reissue. */
                    klog(LOG_WARN, "ahci",
                         "Port %u: NCQ tag %u failed while its completer held it; tag retired",
                         (uint64_t)p->port_num, (uint64_t)tag);
                    return -1;
                }
                klog(LOG_WARN, "ahci", "Port %u: NCQ tag %u failed or timed out; port recovered",
                     (uint64_t)p->port_num, (uint64_t)tag);
            }
        }

        __atomic_fetch_and(&p->tags_completed, ~(1U << tag), __ATOMIC_ACQ_REL);
        /* tag_status holds 0 or -1 in an int8_t; the sign extension is the point. */
        status = p->tag_status[tag]; /* NOLINT(bugprone-signed-char-misuse) signed status code, not a character */
        ncq_free_tag(p, tag);

        if (status == 0)
            return 0;

        retries--;
        /* A task file error leaves the port needing an ST stop/start (and a
         * COMRESET when BSY/DRQ stick) before it takes another command; a
         * timeout was already recovered above. Either way the recovery is the
         * one owned sequence, never a reset concurrent issuers can race. */
        if (!aborted && !ncq_abort_port(p, 1))
            break;
        if (__atomic_load_n(&p->ncq_closed, __ATOMIC_ACQUIRE))
            break;
        klog(LOG_DEBUG, "ahci", "Port %u: NCQ retry (%d left)",
               (uint64_t)p->port_num, (uint64_t)retries);
    }

    p->errors.cmd_failures++;

    /* Every retry ended with the tag still in PxSACT or PxCI, or a task file
     * error: the device is not completing queued commands. (A missing
     * interrupt no longer lands here: the wait polls the HBA registers, so
     * only the hardware itself failing does.) Disable NCQ on this port so
     * ahci_ncq_read/write fall back to ahci_do_rw, which polls PxCI the same
     * way and degrades to pure polling on an event timeout. */
    p->ncq_supported = 0;
    klog(LOG_WARN, "ahci",
           "Port %u: NCQ disabled (cmd_failures=%u) -- "
           "falling back to legacy ATA DMA",
           (uint64_t)p->port_num, (uint64_t)p->errors.cmd_failures);

    return -1;
}

/* ---- Public NCQ API ---- */

int ahci_ncq_read(int port_idx, uint64_t lba, uint32_t count, void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (ports[port_idx].device_type == AHCI_DEV_ATAPI)
        return -1;
    if (!ports[port_idx].ncq_supported)
        return ahci_do_rw(&ports[port_idx], lba, count, buffer, 0, 0);
    return ncq_sync_rw(&ports[port_idx], lba, count, buffer, 0, 0);
}

int ahci_ncq_write(int port_idx, uint64_t lba, uint32_t count,
                   const void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (ports[port_idx].device_type == AHCI_DEV_ATAPI)
        return -1;
    if (!ports[port_idx].ncq_supported)
        return ahci_do_rw(&ports[port_idx], lba, count, (void *)buffer, 1, 0);
    return ncq_sync_rw(&ports[port_idx], lba, count, (void *)buffer, 1, 0);
}

int ahci_submit(int port_idx, uint64_t lba, uint32_t count, void *buffer,
                int is_write, ahci_callback_t callback, void *ctx)
{
    struct ahci_port *p;
    int tag;

    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    p = &ports[port_idx];
    if (!p->ncq_supported || !use_events)
        return -1;

    tag = ncq_alloc_tag(p);
    if (tag < 0)
        return -1;

    p->tag_callbacks[tag] = callback;
    p->tag_cb_ctx[tag] = ctx;

    if (ncq_issue_rw(p, tag, lba, count, buffer, is_write, 0) != 0) {
        p->tag_callbacks[tag] = (ahci_callback_t)0;
        ncq_free_tag(p, tag);
        return -1;
    }
    return tag;
}
