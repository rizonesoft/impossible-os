/* ============================================================================
 * ahci_internal.h -- AHCI driver internal shared state
 *
 * NOT for external consumers -- use "kernel/drivers/ahci.h" for the public API.
 * This header exposes driver-internal state and helpers shared across the
 * split AHCI source files (ahci_core.c, ahci_rw.c, ahci_ncq.c, etc.).
 * ============================================================================ */

#pragma once

#include "kernel/drivers/ahci.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/smp.h"
#include "kernel/panic.h"

/* ---- Shared driver state (defined in ahci_core.c) ---- */

extern volatile uint8_t *abar;
extern struct ahci_port  ports[AHCI_MAX_PORTS];
extern int               num_drives;
extern int               num_atapi;
extern int               initialized;
extern int               num_ports_total;
extern int               atapi_map[AHCI_MAX_PORTS];
extern int               use_events;
extern int               ahci_clo_supported;
extern int               ahci_ncq_capable;
extern uint8_t           ahci_max_cmd_slots;

/* ---- Internal helpers (ahci_core.c) ---- */

void ahci_memset(void *dst, uint8_t val, uint64_t n);
uint32_t port_read(volatile uint8_t *pregs, uint32_t off);
void port_write(volatile uint8_t *pregs, uint32_t off, uint32_t val);
int  port_stop_cmd(volatile uint8_t *pregs);   /* 1 = CR and FR confirmed clear */
int  port_comreset_regs(volatile uint8_t *pregs);  /* CLO + COMRESET, engine stopped, no lock */
int  ahci_hba_reset_for_panic(void);
/* Queue ncq_abort_port for a port whose ISR held failed tags. IRQ-safe. */
void ahci_ncq_queue_recovery(struct ahci_port *p);            /* GHC.HR; panic only, 1 = reset completed */
void port_start_cmd(volatile uint8_t *pregs);
int port_find_slot(volatile uint8_t *pregs);
int port_issue_cmd(struct ahci_port *p, int slot);
int port_clo_reset(struct ahci_port *p);

/* How many PxCI reads a synchronous LEGACY (slot) command spends polling
 * before it sleeps on the completion event (NCQ uses AHCI_NCQ_QUICK_POLLS and
 * re-polls after every wakeup instead). A fast device finishes inside it, so a sync
 * command needs no interrupt at all, which is what lets a CPU whose AHCI MSI
 * targets ANOTHER CPU (a panic owner on an AP, with the BSP parked) still
 * complete I/O. */
#define AHCI_QUICK_POLL_READS  100000u
/* NCQ waits poll only briefly before sleeping on the completion event: each
 * pending poll is two MMIO reads, so 4096 of them cover a fast command without
 * spinning a CPU through a slow one. */
#define AHCI_NCQ_QUICK_POLLS   4096u

/* Poll a legacy command slot: AHCI_POLL_DONE once PxCI clears the slot,
 * AHCI_POLL_ERR on TFD.ERR, AHCI_POLL_TIMEOUT after `budget` reads with the
 * command still outstanding. */
#define AHCI_POLL_SLOT_READS   5000000u
#define AHCI_POLL_DONE         0
#define AHCI_POLL_ERR          (-1)
#define AHCI_POLL_TIMEOUT      (-2)
int ahci_poll_slot(volatile uint8_t *pregs, int slot, uint32_t budget);

/* 1 when port_issue_cmd should recover the port (CLO/COMRESET) and reissue a
 * failed command: the device is stuck BSY/DRQ, retries remain, and no panic
 * is in progress (a panic judges the original failure, see port_issue_cmd). */
int port_issue_should_retry(int result, int retries, uint32_t tfd, int in_panic);

/* 1 when a legacy command sampled at reset generation `gen_at_issue` is void
 * at `gen_now`: any reset before issue (refused) or during it (failed). */
int ahci_reset_voids_cmd(uint32_t gen_at_issue, uint32_t gen_now);

/* One poll of NCQ tag `bit` (see the NCQ_POLL_* results). */
int ahci_ncq_poll_tag(struct ahci_port *p, uint32_t bit);

/* Close a port ahead of an HBA reset, under its completion lock, so its ISR
 * and pollers publish nothing from the reset-cleared registers. */
void ahci_port_close_for_reset(struct ahci_port *p);

/* ---- NCQ completion ownership ----
 *
 * tags_pending and tags_completed are read-modify-written by the ISR (on
 * whichever CPU the MSI targets) and by the synchronous waiter (on any CPU),
 * so every update is atomic, and a tag's completion belongs to whichever side
 * CLEARS its pending bit: ahci_tags_claim returns exactly the bits this
 * caller cleared, so the status write, the callback and the completed bit
 * happen once per tag however the two race. */
static inline uint32_t ahci_tags_claim(struct ahci_port *p, uint32_t mask)
{
    return __atomic_fetch_and(&p->tags_pending, ~mask, __ATOMIC_ACQ_REL) & mask;
}

static inline void ahci_tags_complete(struct ahci_port *p, uint32_t mask)
{
    __atomic_fetch_or(&p->tags_completed, mask, __ATOMIC_RELEASE);
}

/* The per-port completion lock. An ORDINARY lock, deliberately not a bounded
 * one: the section it guards is a handful of MMIO reads and bit operations with
 * no calls, taken with local interrupts masked, so another CPU waits only for
 * those instructions. A bounded lock had to give every caller a "lock expired"
 * path, and each such path reopened a race or stranded a request (review of
 * the double-panic checkpoint work, 2026-11-11).
 *
 * The one wait that could never end is this CPU's OWN: local_irq_save does not
 * mask NMI, and an NMI inside the section panics and writes the crash dump
 * through this driver. So the owner is recorded in the lock word itself, in
 * the same compare-exchange that takes it, and a caller finding its own CPU as
 * the holder is REFUSED (returns 0) rather than spinning. The same holds for
 * a FOREIGN holder once a panic owns the machine: a secondary panic parks its
 * CPU for good, possibly inside this section, so under panic_in_progress() the
 * wait is bounded (AHCI_TAG_PANIC_SPINS) and then refused. Outside a panic
 * the wait is unbounded. Every caller fails closed on a refusal without
 * touching completion state: no issue, "not complete", or a failed request
 * whose tag stays allocated.
 *
 * Returns 1 with the lock held (release with ahci_tag_unlock(p, *flags)), or
 * 0, not held, interrupts as they were. */
static inline uint32_t ahci_tag_owner_id(void)
{
    struct per_cpu_data *pc = smp_this_cpu();

    return (pc ? pc->cpu_id : 0u) + 1u;
}

#define AHCI_TAG_PANIC_SPINS  (1u << 24)

static inline int ahci_tag_lock(struct ahci_port *p, uint64_t *flags)
{
    uint32_t me = ahci_tag_owner_id();
    uint32_t spins = 0;

    *flags = local_irq_save();
    for (;;) {
        uint32_t expected = 0u;

        if (__atomic_compare_exchange_n(&p->tag_owner, &expected, me, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 1;
        if (expected == me) {
            local_irq_restore(*flags);
            return 0;   /* re-entered on the holder's CPU (NMI or nested panic) */
        }
        if (++spins >= AHCI_TAG_PANIC_SPINS && panic_in_progress()) {
            local_irq_restore(*flags);
            return 0;   /* the holder may be a CPU a secondary panic parked */
        }
        __asm__ volatile ("pause" ::: "memory");
    }
}

static inline void ahci_tag_unlock(struct ahci_port *p, uint64_t flags)
{
    __atomic_store_n(&p->tag_owner, 0u, __ATOMIC_RELEASE);
    local_irq_restore(flags);
}

/* A claimed tag's async completion, detached under the lock and invoked after
 * it is released: once the lock drops, a waiter may free and reissue the tag,
 * so nothing after the unlock may read or write that tag's slots. */
struct ahci_tag_done {
    ahci_callback_t cb;
    void           *ctx;
    int             status;
};

/* Under the lock: detach the callback of every tag in `claimed` into out[t]
 * (cb NULL for a synchronous tag) with `fail` overriding the recorded status. */
static inline void ahci_tags_detach(struct ahci_port *p, uint32_t claimed, int fail,
                                    struct ahci_tag_done out[32])
{
    int t;

    for (t = 0; t < 32; t++) {
        out[t].cb = (ahci_callback_t)0;
        if (!(claimed & (1U << t)))
            continue;
        out[t].cb = p->tag_callbacks[t];
        out[t].ctx = p->tag_cb_ctx[t];
        out[t].status = fail ? -1 : p->tag_status[t];
        p->tag_callbacks[t] = (ahci_callback_t)0;
    }
}

/* After the lock: run what ahci_tags_detach captured. */
static inline void ahci_tags_run(int port_idx, const struct ahci_tag_done done[32])
{
    int t;

    for (t = 0; t < 32; t++)
        if (done[t].cb)
            done[t].cb(port_idx, t, done[t].status, done[t].ctx);
}

/* Recovery ownership, decided under the completion lock: exactly one caller
 * of ncq_abort_port owns a recovery from its first locked step to its last;
 * any other finds it owned and waits for it instead of touching the port.
 * Returns 1 when this caller now owns recovery, 0 when another one does. */
static inline int ahci_recovery_claim(struct ahci_port *p)
{
    if (__atomic_load_n(&p->ncq_recovering, __ATOMIC_ACQUIRE))
        return 0;
    __atomic_store_n(&p->ncq_recovering, 1, __ATOMIC_RELEASE);
    return 1;
}

static inline void ahci_recovery_end(struct ahci_port *p)
{
    __atomic_store_n(&p->ncq_recovering, 0, __ATOMIC_RELEASE);
}

/* ahci_ncq_poll_tag results: the tag is finished (completed bit published), still
 * in flight, or needs ncq_abort_port: it failed and is held until a confirmed
 * engine stop, recovery owns the port, or the completion lock was refused. */
#define NCQ_POLL_PENDING   0
#define NCQ_POLL_DONE      1
#define NCQ_POLL_RECOVER  (-1)   /* held failure, recovery running, or lock refused */

/* ncq_issue_rw results besides 0 (issued): the port is closed (removed, or
 * shut by recovery or a panic), or busy in ncq_abort_port's recovery. Either
 * way nothing was issued and the caller still owns the tag. */
#define NCQ_ISSUE_CLOSED  (-1)
#define NCQ_ISSUE_BUSY    (-2)

/* What the HBA's own registers say about NCQ tag `bit`, so completion does
 * not depend on an interrupt arriving:
 *   0   still outstanding (in PxSACT or PxCI);
 *   1   finished without error (gone from both);
 *  -1   gone from both, but PxIS carries a fatal error (AHCI_PxIS_FATAL, the
 *       same classification the ISR applies).
 * Pure, for the unit test; the caller reads the three registers. */
int ahci_ncq_tag_hw_state(uint32_t sact, uint32_t ci, uint32_t pxis, uint32_t bit);

/* ---- Cross-file internal functions ---- */

/* ahci_rw.c */
int ahci_do_identify(struct ahci_port *p);
int ahci_do_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
               void *buffer, int is_write, int fua);

/* ahci_ncq.c */
int ncq_alloc_tag(struct ahci_port *p);
void ncq_free_tag(struct ahci_port *p, int tag);
int ncq_issue_rw(struct ahci_port *p, int tag, uint64_t lba,
                 uint32_t count, void *buffer, int is_write, int fua);
int ncq_sync_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
                void *buffer, int is_write, int fua);

/* ahci_atapi.c */
int atapi_do_identify(struct ahci_port *p);
int atapi_read_capacity(struct ahci_port *p);
int atapi_do_read(struct ahci_port *p, uint64_t lba, uint32_t count,
                  void *buffer);
int atapi_do_read12(struct ahci_port *p, uint64_t lba, uint32_t count,
                    void *buffer);
int atapi_request_sense(struct ahci_port *p);
int atapi_test_unit_ready(struct ahci_port *p);
int atapi_inquiry(struct ahci_port *p);
int atapi_get_configuration(struct ahci_port *p);
int atapi_dma_command(struct ahci_port *p, const uint8_t *cdb,
                      uint32_t cdb_len, void *buffer, uint32_t buf_len,
                      int direction);

/* ahci_hotplug.c */
void ahci_hotplug_check(int port_idx);
void ahci_hotplug_insert(int port_idx);
void ahci_hotplug_remove(int port_idx);
void ahci_hotplug_flush_stats(void);
