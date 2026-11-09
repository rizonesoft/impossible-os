/* ============================================================================
 * test_storage.c -- Storage driver unit tests (AHCI, VirtIO-blk)
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/ahci.h"
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

void test_register_storage(void)
{
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
