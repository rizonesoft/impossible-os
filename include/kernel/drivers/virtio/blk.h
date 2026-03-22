/* ============================================================================
 * virtio_blk.h — VirtIO Block Device Driver
 *
 * VirtIO 1.0 modern PCI transport block device driver.
 * Uses the generic virtio.c infrastructure for PCI capability walking,
 * BAR MMIO mapping, and split virtqueue management.
 *
 * Detected via PCI: vendor 0x1AF4, device 0x1042 (modern) or 0x1001 (legacy).
 * QEMU flag: -drive file=disk.img,format=raw,if=none,id=disk0
 *            -device virtio-blk-pci,drive=disk0
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/drivers/virtio/virtio.h"

/* PCI identification */
#define VIRTIO_BLK_VENDOR_ID     0x1AF4
#define VIRTIO_BLK_DEVICE_ID_MOD 0x1042  /* modern (non-transitional) */
#define VIRTIO_BLK_DEVICE_ID_LEG 0x1001  /* legacy / transitional */

/* VirtIO block request types (VirtIO 1.2 §5.2.6) */
#define VIRTIO_BLK_T_IN       0     /* read */
#define VIRTIO_BLK_T_OUT      1     /* write */
#define VIRTIO_BLK_T_FLUSH    4     /* flush volatile cache to persistent storage */
#define VIRTIO_BLK_T_GET_ID   8     /* retrieve device serial number (20 bytes) */
#define VIRTIO_BLK_T_DISCARD  11    /* discard (TRIM) — unmap sectors (§5.2.6.1) */
#define VIRTIO_BLK_T_WRITE_ZEROES 13 /* write zeroes (§5.2.6.1) */
#define VIRTIO_BLK_T_GET_LIFETIME 10 /* get device lifetime metrics (§5.2.6) */
#define VIRTIO_BLK_T_SECURE_ERASE 14 /* secure erase (crypto wipe) sectors */
#define VIRTIO_BLK_T_ZONE_REPORT  16 /* report zone descriptors */
#define VIRTIO_BLK_T_ZONE_OPEN    18 /* open zone for writing */
#define VIRTIO_BLK_T_ZONE_CLOSE   20 /* close zone */
#define VIRTIO_BLK_T_ZONE_FINISH  22 /* finish zone (fill + transition to full) */
#define VIRTIO_BLK_T_ZONE_RESET   24 /* reset zone write pointer */
#define VIRTIO_BLK_T_ZONE_APPEND  26 /* append write at zone write pointer */
#define VIRTIO_BLK_T_FUA_FLAG 0x80000000u  /* Force Unit Access: OR with T_OUT */

/* Crypto algorithm identifiers for inline encryption */
#define VIRTIO_BLK_CRYPTO_ALG_NONE       0
#define VIRTIO_BLK_CRYPTO_ALG_AES_128_XTS 1
#define VIRTIO_BLK_CRYPTO_ALG_AES_256_XTS 2
#define VIRTIO_BLK_CRYPTO_ALG_AES_128_CBC 3
#define VIRTIO_BLK_CRYPTO_ALG_AES_256_CBC 4

/* Crypto key slot limits */
#define VIRTIO_BLK_CRYPTO_MAX_KEY_SLOTS  16
#define VIRTIO_BLK_CRYPTO_KEY_SIZE       64  /* Max key size in bytes (AES-256-XTS) */

/* Device serial number length (VirtIO 1.2 §5.2.6.1) */
#define VIRTIO_BLK_ID_BYTES 20

/* VirtIO block request status values */
#define VIRTIO_BLK_S_OK        0
#define VIRTIO_BLK_S_IOERR     1
#define VIRTIO_BLK_S_UNSUPP    2

/* VirtIO block feature bits — bit INDICES per VirtIO 1.2 §5.2.3
 * Usage: (1 << VIRTIO_BLK_F_xxx) to get the bit mask
 * Note: bits 3, 8 are unused / reserved by the spec */
#define VIRTIO_BLK_F_SIZE_MAX    0   /* Max segment size in config */
#define VIRTIO_BLK_F_SEG_MAX     1   /* Max segments per request in config */
#define VIRTIO_BLK_F_GEOMETRY    2   /* Legacy CHS geometry in config */
#define VIRTIO_BLK_F_RO          4   /* Device is read-only */
#define VIRTIO_BLK_F_BLK_SIZE    5   /* Block size in config (may differ from 512) */
#define VIRTIO_BLK_F_FLUSH       6   /* Cache flush command supported */
#define VIRTIO_BLK_F_TOPOLOGY    7   /* Topology info in config */
#define VIRTIO_BLK_F_CONFIG_WCE  9   /* Writeback cache enable is negotiable */
#define VIRTIO_BLK_F_DISCARD    11   /* Discard (TRIM/UNMAP) supported */
#define VIRTIO_BLK_F_WRITE_ZEROES 12 /* Write-zeroes command supported */
#define VIRTIO_BLK_F_LIFETIME   13   /* Device lifetime metrics (VirtIO 1.2+) */
#define VIRTIO_BLK_F_FUA        14   /* Force Unit Access per-request (proposed) */
#define VIRTIO_BLK_F_INLINE_CRYPTO 15 /* Inline encryption/decryption (proposed) */
#define VIRTIO_BLK_F_SECURE_ERASE 16  /* Secure erase (crypto wipe) support */
#define VIRTIO_BLK_F_ZONED      17   /* Zoned block device (ZBD/ZNS) support */
#define VIRTIO_BLK_F_MQ         22   /* Multi-queue (per-CPU) supported */
#define VIRTIO_F_RING_INDIRECT_DESC 28 /* Indirect descriptor tables */
#define VIRTIO_F_RING_EVENT_IDX 29     /* Event index for int coalescing */
#define VIRTIO_F_VERSION_1      32   /* VirtIO 1.0 modern */

/* Maximum number of virtqueues the driver supports */
#define VIRTIO_BLK_MAX_QUEUES   8

/* VirtIO block device config offsets (within device_cfg MMIO region)
 * See VirtIO 1.2 §5.2.4 — offsets within DEVICE_CFG capability */
#define VIRTIO_BLK_CFG_CAPACITY        0x00  /* uint64_t: total 512-byte sectors */
#define VIRTIO_BLK_CFG_SIZE_MAX        0x08  /* uint32_t: max bytes per segment */
#define VIRTIO_BLK_CFG_SEG_MAX         0x0C  /* uint32_t: max segments per request */
#define VIRTIO_BLK_CFG_BLK_SIZE        0x14  /* uint32_t: logical block size */
#define VIRTIO_BLK_CFG_PHYS_BLK_EXP    0x18  /* uint8_t:  log2(phys/logical) */
#define VIRTIO_BLK_CFG_ALIGN_OFFSET    0x19  /* uint8_t:  offset of first aligned logical block */
#define VIRTIO_BLK_CFG_MIN_IO_SIZE     0x1A  /* uint16_t: suggested minimum I/O size (blocks) */
#define VIRTIO_BLK_CFG_OPT_IO_SIZE     0x1C  /* uint32_t: optimal (suggested max) I/O size (blocks) */
#define VIRTIO_BLK_CFG_WRITEBACK       0x20  /* uint8_t:  0=writethrough, 1=writeback */
#define VIRTIO_BLK_CFG_NUM_QUEUES      0x22  /* uint16_t: num_queues (F_MQ) */
/* Discard config offsets (VirtIO 1.2 §5.2.4, present when F_DISCARD) */
#define VIRTIO_BLK_CFG_MAX_DISCARD_SECTORS 0x24  /* uint32_t: max sectors per discard */
#define VIRTIO_BLK_CFG_MAX_DISCARD_SEG     0x28  /* uint32_t: max discard segments */
#define VIRTIO_BLK_CFG_DISCARD_ALIGN       0x2C  /* uint32_t: sector alignment */

/* Secure erase config offsets (present when F_SECURE_ERASE) */
#define VIRTIO_BLK_CFG_MAX_SERASE_SECTORS  0x3C  /* uint32_t: max sectors per erase */
#define VIRTIO_BLK_CFG_MAX_SERASE_SEG      0x40  /* uint32_t: max erase segments */
#define VIRTIO_BLK_CFG_SERASE_ALIGN        0x44  /* uint32_t: sector alignment */

/* Write-zeroes config offsets (VirtIO 1.2 §5.2.4, present when F_WRITE_ZEROES) */
#define VIRTIO_BLK_CFG_MAX_WZ_SECTORS      0x30  /* uint32_t: max sectors per write-zeroes */
#define VIRTIO_BLK_CFG_MAX_WZ_SEG          0x34  /* uint32_t: max write-zeroes segments */
#define VIRTIO_BLK_CFG_WZ_MAY_UNMAP        0x38  /* uint8_t:  1 = unmap flag allowed */

/* VirtIO block request header */
struct virtio_blk_req {
    uint32_t type;     /* VIRTIO_BLK_T_* */
    uint32_t reserved;
    uint64_t sector;   /* starting sector (LBA) */
} __attribute__((packed));

/* Discard/write-zeroes segment descriptor (VirtIO 1.2 §5.2.6.1) */
struct virtio_blk_discard_write_zeroes {
    uint64_t sector;       /* Starting sector to discard */
    uint32_t num_sectors;  /* Number of sectors to discard */
    uint32_t flags;        /* 0 = discard, bit 0 set = unmap (for write-zeroes) */
} __attribute__((packed));

/* Device lifetime response (VirtIO 1.2+ §5.2.6, JESD84-B50) */
struct virtio_blk_lifetime {
    uint16_t pre_eol_info;              /* Pre End-of-Life indicator */
    uint16_t device_lifetime_est_typ_a; /* SLC wear: 0=undef, 1=0-10%...10=90-100%, 11=exceeded */
    uint16_t device_lifetime_est_typ_b; /* MLC wear: same encoding as typ_a */
} __attribute__((packed));

/* Pre-EOL info constants (§5.2.6) */
#define VIRTIO_BLK_PRE_EOL_UNDEFINED 0  /* Not defined */
#define VIRTIO_BLK_PRE_EOL_NORMAL    1  /* Normal (<80% consumed) */
#define VIRTIO_BLK_PRE_EOL_WARNING   2  /* Warning (80% consumed) */
#define VIRTIO_BLK_PRE_EOL_URGENT    3  /* Urgent (90% consumed) */

/* Zone condition values (per-zone state) */
#define VIRTIO_BLK_ZONE_COND_NOT_WP   0x0  /* Not a write pointer zone (conv.) */
#define VIRTIO_BLK_ZONE_COND_EMPTY    0x1  /* Empty */
#define VIRTIO_BLK_ZONE_COND_IMP_OPEN 0x2  /* Implicitly opened */
#define VIRTIO_BLK_ZONE_COND_EXP_OPEN 0x3  /* Explicitly opened */
#define VIRTIO_BLK_ZONE_COND_CLOSED   0x4  /* Closed */
#define VIRTIO_BLK_ZONE_COND_RDONLY   0xD  /* Read-only */
#define VIRTIO_BLK_ZONE_COND_FULL     0xE  /* Full */
#define VIRTIO_BLK_ZONE_COND_OFFLINE  0xF  /* Offline */

/* Zone type values */
#define VIRTIO_BLK_ZONE_TYPE_CONV     0x1  /* Conventional (random write OK) */
#define VIRTIO_BLK_ZONE_TYPE_SEQ_WR   0x2  /* Sequential write required */
#define VIRTIO_BLK_ZONE_TYPE_SEQ_PREF 0x3  /* Sequential write preferred */

/* Zone descriptor (returned by ZONE_REPORT) */
struct virtio_blk_zone_descriptor {
    uint64_t z_start;    /* Zone start LBA (sectors) */
    uint64_t z_cap;      /* Zone capacity (sectors, may be < z_len) */
    uint64_t z_wp;       /* Write pointer (sectors) */
    uint64_t z_len;      /* Zone length (sectors) */
    uint8_t  z_type;     /* VIRTIO_BLK_ZONE_TYPE_* */
    uint8_t  z_cond;     /* VIRTIO_BLK_ZONE_COND_* */
    uint8_t  reserved[6];
} __attribute__((packed));

/* Zone report header (precedes zone descriptors in response) */
struct virtio_blk_zone_report {
    uint32_t nr_zones;   /* Number of zone descriptors following */
    uint8_t  reserved[4];
} __attribute__((packed));

/* Topology and segment limit information (populated during init) */
struct virtio_blk_topology {
    uint32_t blk_size;           /* Logical block size in bytes (default 512) */
    uint8_t  physical_block_exp; /* log2(phys_size / blk_size) */
    uint8_t  alignment_offset;   /* Offset of first aligned logical block */
    uint16_t min_io_size;        /* Minimum I/O size in logical blocks */
    uint32_t opt_io_size;        /* Optimal I/O size in logical blocks */
    uint32_t size_max;           /* Max bytes per segment (0 = no limit) */
    uint32_t seg_max;            /* Max segments per request (0 = no limit) */
    uint32_t max_discard_sectors; /* Max sectors per discard cmd (0 = no limit) */
    uint32_t max_discard_seg;    /* Max discard segments per cmd (0 = no limit) */
    uint32_t discard_sector_alignment; /* Discard sector alignment (0 = none) */
    uint32_t max_wz_sectors;     /* Max sectors per write-zeroes (0 = no limit) */
    uint32_t max_wz_seg;         /* Max write-zeroes segments (0 = no limit) */
    uint8_t  wz_may_unmap;       /* 1 = unmap flag allowed in write-zeroes */
    uint32_t max_serase_sectors;  /* Max sectors per secure erase (0 = no limit) */
    uint32_t max_serase_seg;      /* Max secure erase segments (0 = no limit) */
    uint32_t serase_sector_alignment; /* Secure erase sector alignment (0 = none) */
    uint16_t num_queues;         /* Number of request queues (F_MQ, default 1) */
};

/* --- API --- */

/* Initialize the virtio-blk driver. Returns 0 on success, -1 if no device. */
int virtio_blk_init(void);

/* Read 'count' sectors starting at LBA into 'buffer'.
 * Returns 0 on success, -1 on error. */
int virtio_blk_read(uint64_t lba, uint32_t count, void *buffer);

/* Write 'count' sectors starting at LBA from 'buffer'.
 * Returns 0 on success, -1 on error. */
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buffer);

/* Force Unit Access write — data guaranteed on stable storage on return.
 * Uses F_FUA if negotiated, otherwise falls back to T_OUT + T_FLUSH. */
int virtio_blk_write_fua(uint64_t lba, uint32_t count, const void *buffer);

/* Flush volatile write cache to persistent storage.
 * Only available when VIRTIO_BLK_F_FLUSH was negotiated.
 * Returns 0 on success, -1 on error, 1 if flush not supported. */
int virtio_blk_flush(void);

/* Discard (TRIM) sectors starting at 'sector' for 'num_sectors'.
 * Tells the device that these sectors are no longer in use.
 * Only available when VIRTIO_BLK_F_DISCARD was negotiated.
 * Returns 0 on success, -1 on error, 1 if discard not supported. */
int virtio_blk_discard(uint64_t sector, uint32_t num_sectors);

/* Secure erase: cryptographically erase sectors for data sanitization.
 * Only available when VIRTIO_BLK_F_SECURE_ERASE was negotiated.
 * Returns 0 on success, -1 on error, 1 if not supported. */
int virtio_blk_secure_erase(uint64_t sector, uint32_t num_sectors);

/* Write-zeroes: zero out sectors starting at 'sector' for 'num_sectors'.
 * If unmap=1 and device supports it (F_WRITE_ZEROES + write_zeroes_may_unmap),
 * the device may deallocate the zeroed region (thin provisioning).
 * Returns 0 on success, -1 on error, 1 if write-zeroes not supported. */
int virtio_blk_write_zeroes(uint64_t sector, uint32_t num_sectors, int unmap);

/* Enable/disable writeback cache mode.
 * Only available when VIRTIO_BLK_F_CONFIG_WCE was negotiated.
 * enable=1: writeback (faster, data at risk), enable=0: writethrough (safer).
 * Returns 0 on success, -1 if not supported. */
int virtio_blk_set_write_cache(int enable);

/* Get total disk capacity in 512-byte sectors. */
uint64_t virtio_blk_capacity(void);

/* Get the negotiated logical block size in bytes (default 512). */
uint32_t virtio_blk_block_size(void);

/* Get topology and segment limit information. Returns pointer to
 * internal struct (valid while driver is initialized). */
const struct virtio_blk_topology *virtio_blk_topology(void);

/* Check if a virtio-blk device was detected and initialized. */
int virtio_blk_present(void);

/* Get the number of active virtqueues (1 without F_MQ, up to 8 with F_MQ). */
uint16_t virtio_blk_num_queues(void);

/* Reset the device and re-run the full initialization sequence.
 * Called automatically on I/O timeout or DEVICE_NEEDS_RESET.
 * Returns 0 on success, -1 on failure. */
int virtio_blk_reset(void);

/* Retrieve device serial number (up to 20 bytes).
 * Copies into 'buf' (null-terminated), at most 'len' bytes.
 * Returns 0 on success, -1 on error. */
int virtio_blk_get_id(char *buf, uint32_t len);

/* Retrieve device lifetime metrics (JESD84-B50).
 * Only available when VIRTIO_BLK_F_LIFETIME was negotiated.
 * Returns 0 on success, -1 on error, 1 if not supported. */
int virtio_blk_get_lifetime(struct virtio_blk_lifetime *out);

/* Get the cached serial string (null-terminated, max 20 chars).
 * Returns empty string if GET_ID was not performed. */
const char *virtio_blk_serial(void);

/* Handle a device configuration change event (hot-resize, topology, writeback).
 * Called from the MSI-X config change ISR. Atomically re-reads device config
 * using config_generation loop, detects changes, and updates driver state.
 * Also callable from a deferred work context if ISR deferral is needed. */
void virtio_blk_handle_config_change(void);

/* Shutdown the virtio-blk driver: quiesce I/O, flush caches, tear down
 * virtqueues, free MSI-X vectors, unregister block device.
 * Called during managed hot-unplug or system shutdown. */
void virtio_blk_shutdown(void);

/* Hot-plug: scan a new PCI function for a VirtIO block device,
 * run full init, and register with the block device layer.
 * Returns 0 on success, -1 if not a VirtIO block device. */
int virtio_blk_hotplug(uint8_t bus, uint8_t dev, uint8_t func);

/* Hot-unplug (managed): gracefully shut down and unregister.
 * Flushes caches, drains I/O, tears down virtqueues. */
void virtio_blk_hotunplug(void);

/* Check if the device has been surprise-removed (PCI function gone).
 * Returns 1 if the device is no longer present, 0 if present. */
int virtio_blk_is_surprise_removed(void);
