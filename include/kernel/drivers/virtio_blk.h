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
#include "kernel/drivers/virtio.h"

/* PCI identification */
#define VIRTIO_BLK_VENDOR_ID     0x1AF4
#define VIRTIO_BLK_DEVICE_ID_MOD 0x1042  /* modern (non-transitional) */
#define VIRTIO_BLK_DEVICE_ID_LEG 0x1001  /* legacy / transitional */

/* VirtIO block request types */
#define VIRTIO_BLK_T_IN    0   /* read */
#define VIRTIO_BLK_T_OUT   1   /* write */

/* VirtIO block request status values */
#define VIRTIO_BLK_S_OK        0
#define VIRTIO_BLK_S_IOERR     1
#define VIRTIO_BLK_S_UNSUPP    2

/* VirtIO block feature bits */
#define VIRTIO_BLK_F_SIZE_MAX   1
#define VIRTIO_BLK_F_SEG_MAX    2
#define VIRTIO_BLK_F_GEOMETRY   4
#define VIRTIO_BLK_F_RO         5
#define VIRTIO_BLK_F_BLK_SIZE   6
#define VIRTIO_BLK_F_FLUSH      9
#define VIRTIO_BLK_F_TOPOLOGY  10
#define VIRTIO_F_VERSION_1     32   /* VirtIO 1.0 modern */

/* VirtIO block device config offsets (within device_cfg MMIO region) */
#define VIRTIO_BLK_CFG_CAPACITY  0x00   /* uint64_t: total 512-byte sectors */

/* VirtIO block request header */
struct virtio_blk_req {
    uint32_t type;     /* VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT */
    uint32_t reserved;
    uint64_t sector;   /* starting sector (LBA) */
} __attribute__((packed));

/* --- API --- */

/* Initialize the virtio-blk driver. Returns 0 on success, -1 if no device. */
int virtio_blk_init(void);

/* Read 'count' sectors starting at LBA into 'buffer'.
 * Returns 0 on success, -1 on error. */
int virtio_blk_read(uint64_t lba, uint32_t count, void *buffer);

/* Write 'count' sectors starting at LBA from 'buffer'.
 * Returns 0 on success, -1 on error. */
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buffer);

/* Get total disk capacity in 512-byte sectors. */
uint64_t virtio_blk_capacity(void);

/* Check if a virtio-blk device was detected and initialized. */
int virtio_blk_present(void);
