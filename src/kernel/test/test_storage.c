/* ============================================================================
 * test_storage.c -- Storage driver unit tests (AHCI, VirtIO-blk)
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/ahci_internal.h"  /* NCQ completion helpers under test */
#include "kernel/drivers/virtio_blk.h"
#include "kernel/mm/heap.h"

/* ---- AHCI sector 0 read ---- */

static void test_ahci_read(void)
{
    if (!ahci_present()) {
        TEST_SKIP("no AHCI controller");
        return;
    }

    uint8_t buf[512];
    for (uint32_t i = 0; i < 512; i++) buf[i] = 0;

    int rc = ahci_read(0, 0, 1, buf);
    TEST_ASSERT(rc == 0, "AHCI sector 0 read succeeds");
}

/* ---- VirtIO-blk sector 0 read ---- */

static void test_virtio_read(void)
{
    if (!virtio_blk_present()) {
        TEST_SKIP("no VirtIO-blk device");
        return;
    }

    uint8_t buf[512];
    for (uint32_t i = 0; i < 512; i++) buf[i] = 0;

    int rc = virtio_blk_read(0, 1, buf);
    TEST_ASSERT(rc == 0, "VirtIO-blk sector 0 read succeeds");
}

/* ---- Registration ---- */

/* ATAPI capacity outcome -> usable: an empty, spinning-up or swapped drive is
 * healthy; a hardware, sense, transport or command failure is not. */
/* Which live links count as attempted AHCI storage devices: an enclosure
 * bridge must not turn a clean boot into DEGRADED. */
static void test_ahci_link_storage_attempt(void)
{
    TEST_ASSERT_EQ(ahci_link_is_storage_attempt(AHCI_SSTS_DET_OK, AHCI_SIG_ATA),
                   1, "SATA disk on a live link is an attempt");
    TEST_ASSERT_EQ(ahci_link_is_storage_attempt(AHCI_SSTS_DET_OK, AHCI_SIG_ATAPI),
                   1, "ATAPI device on a live link is an attempt");
    TEST_ASSERT_EQ(ahci_link_is_storage_attempt(AHCI_SSTS_DET_OK, AHCI_SIG_PM),
                   1, "port multiplier is an attempt (drives behind it)");
    TEST_ASSERT_EQ(ahci_link_is_storage_attempt(AHCI_SSTS_DET_OK | 0x100u,
                                                AHCI_SIG_ATA),
                   1, "IPM bits above DET do not hide a link");
    TEST_ASSERT_EQ(ahci_link_is_storage_attempt(AHCI_SSTS_DET_OK, AHCI_SIG_SEMB),
                   0, "enclosure bridge is not a storage attempt");
    TEST_ASSERT_EQ(ahci_link_is_storage_attempt(0x1u, AHCI_SIG_ATA),
                   0, "device without PHY link is not an attempt");
    TEST_ASSERT_EQ(ahci_link_is_storage_attempt(0u, 0xFFFFFFFFu),
                   0, "empty port is not an attempt");
}

static void test_atapi_capacity_usable(void)
{
    TEST_ASSERT_EQ(atapi_capacity_rc_usable(0), 1, "capacity read -> usable");
    TEST_ASSERT_EQ(atapi_capacity_rc_usable(ATAPI_ERR_NOMEDIUM), 1,
                   "no medium -> usable");
    TEST_ASSERT_EQ(atapi_capacity_rc_usable(ATAPI_ERR_BECOMING), 1,
                   "becoming ready -> usable");
    TEST_ASSERT_EQ(atapi_capacity_rc_usable(ATAPI_ERR_MEDIACHANGE), 1,
                   "media change -> usable");
    TEST_ASSERT_EQ(atapi_capacity_rc_usable(ATAPI_ERR_IO), 0,
                   "hardware error -> unusable");
    TEST_ASSERT_EQ(atapi_capacity_rc_usable(ATAPI_ERR_SENSE_FAIL), 0,
                   "sense failure -> unusable");
    TEST_ASSERT_EQ(atapi_capacity_rc_usable(-1), 0, "transport failure -> unusable");
    TEST_ASSERT_EQ(atapi_capacity_rc_usable(ATAPI_ERR_INVALID), 0,
                   "invalid command -> unusable");
}

/* Fill a port slot the way port_init does: raw allocations, with the DMA
 * pointers rounded up inside them so they differ from the allocation bases.
 * Allocates `tables` of the 32 command tables (a partial port). Returns 1 on
 * success. */
static int ahci_fixture_fill(struct ahci_port *p, int tables)
{
    int i;

    for (i = 0; i < (int)sizeof *p; i++)
        ((uint8_t *)p)[i] = 0;
    p->active = 1;
    p->cmdlist_raw = kmalloc(1024 + 1024);
    p->fis_raw = kmalloc(256 + 256);
    if (!p->cmdlist_raw || !p->fis_raw)
        return 0;
    p->cmdlist = (struct ahci_cmd_header *)
        (((uintptr_t)p->cmdlist_raw + 1024) & ~(uintptr_t)1023);
    p->fis_base = (void *)(((uintptr_t)p->fis_raw + 256) & ~(uintptr_t)255);
    for (i = 0; i < tables; i++) {
        p->cmdtbl_raw[i] = kmalloc(256 + 128);
        if (!p->cmdtbl_raw[i])
            return 0;
        p->cmdtbl[i] = (struct ahci_cmd_tbl *)
            (((uintptr_t)p->cmdtbl_raw[i] + 128) & ~(uintptr_t)127);
    }
    return 1;
}

static int ahci_slot_clear(const struct ahci_port *p)
{
    int i;

    if (p->cmdlist_raw || p->cmdlist || p->fis_raw || p->fis_base)
        return 0;
    for (i = 0; i < 32; i++)
        if (p->cmdtbl_raw[i] || p->cmdtbl[i])
            return 0;
    return 1;
}

/* A rejected port's buffers are freed through their allocation BASES (the
 * aligned DMA pointers are interior and distinct), and the slot is cleared.
 * The leak column fails the test if a base is not freed; freeing an interior
 * pointer is not a valid kfree. Buffers a still-running engine owns are kept
 * allocated, and a later port reusing the slot cannot free them. */
static void test_ahci_port_release_frees(void)
{
    static struct ahci_port p;
    void *held_list, *held_fis, *held_tbl[32];
    uint64_t used0, used1;
    int i;

    TEST_ASSERT(ahci_fixture_fill(&p, 32), "fixture buffers allocated");
    TEST_ASSERT(p.cmdlist != (void *)p.cmdlist_raw, "DMA pointer is interior");
    ahci_port_release(&p);   /* regs NULL: no device, buffers freed */
    TEST_ASSERT_EQ(p.active, 0, "released port is inactive");
    TEST_ASSERT(ahci_slot_clear(&p), "released slot is cleared");
    ahci_port_release(&p);   /* idempotent */
    TEST_ASSERT(ahci_slot_clear(&p), "second release is a no-op");

    /* Port A's engines did not stop: its buffers stay allocated, the slot
     * is cleared anyway. */
    TEST_ASSERT(ahci_fixture_fill(&p, 32), "port A buffers allocated");
    held_list = p.cmdlist_raw;
    held_fis = p.fis_raw;
    for (i = 0; i < 32; i++)
        held_tbl[i] = p.cmdtbl_raw[i];
    used0 = heap_get_used();
    ahci_port_drop_buffers(&p, 0);
    TEST_ASSERT_EQ(heap_get_used(), used0, "retained buffers are not freed");
    TEST_ASSERT(ahci_slot_clear(&p), "retained buffers leave the slot");

    /* Port B reuses the slot and fails part-way through allocation: its own
     * release frees exactly B's buffers and none of A's. */
    used1 = heap_get_used();
    p.cmdlist_raw = kmalloc(1024 + 1024);
    p.cmdlist = (struct ahci_cmd_header *)p.cmdlist_raw;
    TEST_ASSERT_NOT_NULL(p.cmdlist_raw, "port B partial allocation");
    ahci_port_release(&p);
    TEST_ASSERT_EQ(heap_get_used(), used1, "B's release frees only B's buffer");
    TEST_ASSERT(ahci_slot_clear(&p), "slot clear after B");

    kfree(held_list);
    kfree(held_fis);
    for (i = 0; i < 32; i++)
        kfree(held_tbl[i]);
}

/* NCQ completion is read from the HBA, not inferred from an interrupt.
 * Before 2026-11-11 the sync wait trusted only the ISR's bit, so a panic owner
 * on an AP (its MSI aimed at the parked BSP) timed out three times and
 * disabled NCQ while writing the crash dump. */
static void test_ahci_ncq_tag_hw_state(void)
{
    const uint32_t bit = 1u << 5;

    TEST_ASSERT_EQ(ahci_ncq_tag_hw_state(bit, 0u, 0u, bit), 0,
                   "a tag still in PxSACT is outstanding");
    TEST_ASSERT_EQ(ahci_ncq_tag_hw_state(0u, bit, 0u, bit), 0,
                   "a tag still in PxCI is outstanding");
    TEST_ASSERT_EQ(ahci_ncq_tag_hw_state(~bit, ~bit, 0u, bit), 1,
                   "gone from both, other tags busy: finished, theirs untouched");
    TEST_ASSERT_EQ(ahci_ncq_tag_hw_state(0u, 0u, AHCI_PxIS_SDBS, bit), 1,
                   "gone from both with only SDBS raised: finished OK");
    TEST_ASSERT_EQ(ahci_ncq_tag_hw_state(0u, 0u, AHCI_PxIS_TFES, bit), -1,
                   "gone from both under a task file error: failed");
    /* Every bit the ISR treats as fatal, one at a time: the poller must never
     * call success what the ISR would have recorded as a failure. */
    TEST_ASSERT_EQ(ahci_ncq_tag_hw_state(0u, 0u, AHCI_PxIS_HBFS, bit), -1,
                   "a host bus fatal error fails the tag");
    TEST_ASSERT_EQ(ahci_ncq_tag_hw_state(0u, 0u, AHCI_PxIS_HBDS, bit), -1,
                   "a host bus data error fails the tag");
    TEST_ASSERT_EQ(ahci_ncq_tag_hw_state(0u, 0u, AHCI_PxIS_IFS, bit), -1,
                   "an interface fatal error fails the tag");
    TEST_ASSERT_EQ(ahci_ncq_tag_hw_state(0u, 0u, AHCI_PxIS_INFS | AHCI_PxIS_OFS, bit), 1,
                   "non-fatal errors alone leave a finished tag a success, as the ISR does");
}

/* A tag completes ONCE however the ISR and the waiter race: the claim returns
 * only the pending bits this caller cleared. */
static void test_ahci_tags_claim_once(void)
{
    static struct ahci_port p;   /* static: the port struct is large for a stack */

    ahci_memset(&p, 0, sizeof p);
    p.tags_pending = (1u << 2) | (1u << 7);
    TEST_ASSERT_EQ(ahci_tags_claim(&p, 1u << 7), 1u << 7, "the first claim of tag 7 owns it");
    TEST_ASSERT_EQ(ahci_tags_claim(&p, 1u << 7), 0u, "a second claim of tag 7 owns nothing");
    TEST_ASSERT_EQ(p.tags_pending, 1u << 2, "tag 2 is still pending");
    TEST_ASSERT_EQ(ahci_tags_claim(&p, ~(1u << 2)), 0u,
                   "an SDB claim over SACT=tag 2 takes nothing more");
    TEST_ASSERT_EQ(ahci_tags_claim(&p, 0xFFFFFFFFu), 1u << 2, "a fatal-error sweep takes the rest");
    ahci_tags_complete(&p, 1u << 2);
    TEST_ASSERT_EQ(p.tags_completed, 1u << 2, "completion sets exactly the claimed bit");
}

/* Async completions are detached under the completion lock and run after it:
 * after the unlock a waiter may reissue the tag, so the slots must already be
 * empty and nothing may read them again. Each callback runs exactly once. */
static int s_cb_calls;
static int s_cb_last_status;
static void test_ahci_cb(int port, int tag, int status, void *ctx)
{
    (void)port; (void)tag; (void)ctx;
    s_cb_calls++;
    s_cb_last_status = status;
}

static void test_ahci_tags_detach_then_run(void)
{
    static struct ahci_port p;
    static struct ahci_tag_done done[32];

    ahci_memset(&p, 0, sizeof p);
    p.tag_callbacks[3] = test_ahci_cb;
    p.tag_status[3] = 0;
    p.tag_callbacks[9] = test_ahci_cb;   /* not claimed: must be left alone */
    s_cb_calls = 0;
    ahci_tags_detach(&p, 1u << 3, 1, done);
    TEST_ASSERT(p.tag_callbacks[3] == (ahci_callback_t)0,
                "the claimed tag's slot is empty before the lock is released");
    TEST_ASSERT(p.tag_callbacks[9] == test_ahci_cb, "an unclaimed tag's callback is untouched");
    TEST_ASSERT_EQ(s_cb_calls, 0, "nothing runs under the lock");
    p.tag_callbacks[3] = test_ahci_cb;   /* a reissue after the unlock */
    ahci_tags_run(0, done);
    TEST_ASSERT_EQ(s_cb_calls, 1, "the captured callback runs once");
    TEST_ASSERT_EQ(s_cb_last_status, -1, "with the failure the claim recorded");
    TEST_ASSERT(p.tag_callbacks[3] == test_ahci_cb, "and the reissued request's slot is not touched");
}

/* The completion lock refuses, rather than spins on, a holder on this same
 * CPU: an NMI that lands inside the section and writes the crash dump through
 * this driver must not wait forever on the code it interrupted. */
static void test_ahci_tag_lock_refuses_own_cpu(void)
{
    static struct ahci_port p;
    uint64_t flags, inner;

    ahci_memset(&p, 0, sizeof p);
    TEST_ASSERT_EQ(ahci_tag_lock(&p, &flags), 1, "a free completion lock is taken");
    TEST_ASSERT_EQ(p.tag_owner, ahci_tag_owner_id(), "the lock word names this CPU");
    TEST_ASSERT_EQ(ahci_tag_lock(&p, &inner), 0, "re-entry on the holder's CPU is refused, not spun on");
    TEST_ASSERT_EQ(p.tag_owner, ahci_tag_owner_id(), "a refusal leaves the holder's lock intact");
    ahci_tag_unlock(&p, flags);
    TEST_ASSERT_EQ(p.tag_owner, 0u, "unlock frees the lock word");
    TEST_ASSERT_EQ(ahci_tag_lock(&p, &flags), 1, "and it is taken again");
    ahci_tag_unlock(&p, flags);
}

/* COMRESET decides "device present" from PxSSTS.DET, not from its loop
 * counter: the old `while (wait--)` left `wait` at UINT32_MAX when the bound ran
 * out, so its `wait == 0` test reported a missing device as present. Run against
 * a plain memory block standing in for the port registers. */
static void test_ahci_comreset_reports_missing_device(void)
{
    static uint32_t regs[0x80 / 4];

    ahci_memset(regs, 0, sizeof regs);
    TEST_ASSERT_EQ(port_comreset_regs((volatile uint8_t *)regs), -1,
                   "no device on the link (DET=0) after COMRESET reports -1");
    ahci_memset(regs, 0, sizeof regs);
    regs[AHCI_PxSSTS / 4] = AHCI_SSTS_DET_OK;
    TEST_ASSERT_EQ(port_comreset_regs((volatile uint8_t *)regs), 0,
                   "a device on the link (DET=3) with BSY clear reports 0");
}

/* A legacy slot poll OBSERVES completion: a command that never leaves PxCI is a
 * timeout (not success, which an exhausted `while (n--)` counter once implied),
 * TFD.ERR is an error, and a cleared slot is done. */
static void test_ahci_poll_slot_reports_timeout(void)
{
    static uint32_t regs[0x80 / 4];

    ahci_memset(regs, 0, sizeof regs);
    regs[AHCI_PxCI / 4] = 1U << 3;
    TEST_ASSERT_EQ(ahci_poll_slot((volatile uint8_t *)regs, 3, 1000), AHCI_POLL_TIMEOUT,
                   "a slot that stays set with TFD.ERR clear times out");
    TEST_ASSERT_EQ(ahci_poll_slot((volatile uint8_t *)regs, 2, 1000), AHCI_POLL_DONE,
                   "another slot's bit does not hold this one");
    regs[AHCI_PxTFD / 4] = AHCI_PxTFD_ERR;
    TEST_ASSERT_EQ(ahci_poll_slot((volatile uint8_t *)regs, 3, 1000), AHCI_POLL_ERR,
                   "TFD.ERR with the slot still set is an error");
    regs[AHCI_PxCI / 4] = 0;
    TEST_ASSERT_EQ(ahci_poll_slot((volatile uint8_t *)regs, 3, 1000), AHCI_POLL_DONE,
                   "a cleared slot is done");
    TEST_ASSERT_EQ(ahci_poll_slot((volatile uint8_t *)regs, 3, 0), AHCI_POLL_TIMEOUT,
                   "a zero budget observes nothing and times out");
}

/* The legacy retry decision: only a stuck device (BSY/DRQ) with retries left
 * is recovered and reissued, and never under a panic, whose DMA-stop check must
 * see PxCI as the failure left it. */
static void test_ahci_issue_retry_decision(void)
{
    TEST_ASSERT_EQ(port_issue_should_retry(-1, 3, AHCI_PxTFD_BSY, 0), 1, "a stuck BSY device is retried");
    TEST_ASSERT_EQ(port_issue_should_retry(-1, 3, AHCI_PxTFD_DRQ, 0), 1, "and a stuck DRQ one");
    TEST_ASSERT_EQ(port_issue_should_retry(-1, 3, AHCI_PxTFD_BSY, 1), 0, "never under a panic");
    TEST_ASSERT_EQ(port_issue_should_retry(-1, 3, AHCI_PxTFD_ERR, 0), 0, "an error with the device idle is not");
    TEST_ASSERT_EQ(port_issue_should_retry(-1, 0, AHCI_PxTFD_BSY, 0), 0, "nor once retries run out");
    TEST_ASSERT_EQ(port_issue_should_retry(0, 3, AHCI_PxTFD_BSY, 0), 0, "nor a command that succeeded");
}

/* An HBA reset clears every port's PxSACT/PxCI. On an open port a cleared
 * register reads as completion, which is why the reset closes every port first:
 * on a closed one the same registers publish nothing. */
static void test_ahci_reset_closed_port_publishes_nothing(void)
{
    static struct ahci_port p;
    static uint32_t regs[0x80 / 4];
    uint32_t bit = 1U << 5;

    ahci_memset(&p, 0, sizeof p);
    ahci_memset(regs, 0, sizeof regs);   /* SACT = CI = IS = 0: what a reset leaves */
    p.regs = (volatile uint8_t *)regs;
    p.tags_pending = bit;
    TEST_ASSERT_EQ(ahci_ncq_poll_tag(&p, bit), NCQ_POLL_DONE,
                   "on an open port cleared registers complete the tag");
    TEST_ASSERT_EQ(p.tag_status[5], 0, "as a success");

    ahci_memset(&p, 0, sizeof p);
    p.regs = (volatile uint8_t *)regs;
    p.tags_pending = bit;
    ahci_port_close_for_reset(&p);
    TEST_ASSERT_EQ(p.ncq_closed, 1, "the reset closes the port");
    TEST_ASSERT_EQ(p.tag_owner, 0u, "and releases its completion lock");
    TEST_ASSERT_EQ(ahci_ncq_poll_tag(&p, bit), NCQ_POLL_PENDING,
                   "a closed port publishes nothing from the cleared registers");
    TEST_ASSERT_EQ(p.tags_completed & bit, 0u, "the tag is not completed");
    TEST_ASSERT_EQ(p.tags_pending & bit, bit, "and is still pending for recovery to fail");
}

/* A panic HBA reset voids legacy commands on both sides of it: one issued
 * after the generation was bumped (even before HR lands) is refused, one in
 * flight across the bump fails, and only a command that saw no reset stands. */
static void test_ahci_reset_voids_legacy_cmd(void)
{
    TEST_ASSERT_EQ(ahci_reset_voids_cmd(0, 0), 0, "no reset: the command stands");
    TEST_ASSERT_EQ(ahci_reset_voids_cmd(0, 1), 1, "a reset during the command fails it");
    TEST_ASSERT_EQ(ahci_reset_voids_cmd(1, 1), 1,
                   "issued after the bump but before HR: refused, not trusted");
    TEST_ASSERT_EQ(ahci_reset_voids_cmd(1, 2), 1, "and across a second reset");
}

/* Exactly one owner per recovery: a second failed waiter that enters
 * ncq_abort_port while the first is between its locked steps must find it owned
 * (and wait) rather than run a second stop/COMRESET/restart over it. */
static void test_ahci_recovery_single_owner(void)
{
    static struct ahci_port p;

    ahci_memset(&p, 0, sizeof p);
    TEST_ASSERT_EQ(ahci_recovery_claim(&p), 1, "the first failed waiter owns recovery");
    TEST_ASSERT_EQ(ahci_recovery_claim(&p), 0, "a second finds it owned and does not take it");
    TEST_ASSERT_EQ(p.ncq_recovering, 1, "and leaves the owner's flag set");
    ahci_recovery_end(&p);
    TEST_ASSERT_EQ(p.ncq_recovering, 0, "the owner ends it");
    TEST_ASSERT_EQ(ahci_recovery_claim(&p), 1, "the next failure can own a new recovery");
    ahci_recovery_end(&p);
}

void test_register_storage(void)
{
    test_suite_register_cat("Storage: NCQ completion from HBA registers",
                            test_ahci_ncq_tag_hw_state, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: NCQ recovery has one owner",
                            test_ahci_recovery_single_owner, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: COMRESET reports a missing device",
                            test_ahci_comreset_reports_missing_device, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: HBA reset voids legacy commands",
                            test_ahci_reset_voids_legacy_cmd, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: HBA reset closes ports before clearing them",
                            test_ahci_reset_closed_port_publishes_nothing, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: legacy retry never runs under a panic",
                            test_ahci_issue_retry_decision, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: legacy slot poll observes completion",
                            test_ahci_poll_slot_reports_timeout, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: NCQ completion lock refuses its own CPU",
                            test_ahci_tag_lock_refuses_own_cpu, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: NCQ callbacks detach under the lock",
                            test_ahci_tags_detach_then_run, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: NCQ tag completes once",
                            test_ahci_tags_claim_once, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: AHCI port release",
                            test_ahci_port_release_frees, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: ATAPI capacity usability",
                            test_atapi_capacity_usable, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: AHCI link attempt",
                            test_ahci_link_storage_attempt, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: AHCI read", test_ahci_read, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: VirtIO read", test_virtio_read, TEST_CAT_STORAGE);
}

#endif /* KERNEL_TESTS */
