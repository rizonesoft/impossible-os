/* ============================================================================
 * virtio_blk.h — VirtIO Block Device Driver
 *
 * VirtIO legacy (0.9.5) PIO-based block device driver.
 * Detected via PCI: vendor 0x1AF4, device 0x1001.
 * Uses BAR0 for I/O port base, virtqueues for async I/O.
 *
 * QEMU flag: -drive file=disk.img,format=raw,if=virtio
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* PCI identification */
#define VIRTIO_VENDOR_ID     0x1AF4
#define VIRTIO_BLK_DEVICE_ID 0x1001  /* legacy; transitional = 0x1042 */

/* VirtIO legacy PIO register offsets (from BAR0) */
#define VIRTIO_DEVICE_FEATURES   0x00  /* 32-bit: device feature bits */
#define VIRTIO_GUEST_FEATURES    0x04  /* 32-bit: guest feature bits */
#define VIRTIO_QUEUE_ADDRESS     0x08  /* 32-bit: queue PFN */
#define VIRTIO_QUEUE_SIZE        0x0C  /* 16-bit: queue size */
#define VIRTIO_QUEUE_SELECT      0x0E  /* 16-bit: queue index */
#define VIRTIO_QUEUE_NOTIFY      0x10  /* 16-bit: queue notify */
#define VIRTIO_DEVICE_STATUS     0x12  /* 8-bit:  device status */
#define VIRTIO_ISR_STATUS        0x13  /* 8-bit:  ISR status */
/* Block device config starts at offset 0x14 */
#define VIRTIO_BLK_CAPACITY_LO   0x14  /* 32-bit: total sectors (low) */
#define VIRTIO_BLK_CAPACITY_HI   0x18  /* 32-bit: total sectors (high) */

/* Device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

/* VirtIO block request types */
#define VIRTIO_BLK_T_IN    0   /* read */
#define VIRTIO_BLK_T_OUT   1   /* write */

/* VirtIO block request status values */
#define VIRTIO_BLK_S_OK        0
#define VIRTIO_BLK_S_IOERR     1
#define VIRTIO_BLK_S_UNSUPP    2

/* Virtqueue descriptor flags */
#define VRING_DESC_F_NEXT      1   /* buffer continues via 'next' field */
#define VRING_DESC_F_WRITE     2   /* buffer is device-writable (read by device) */

/* Virtqueue sizes */
#define VIRTIO_QUEUE_SIZE_MAX  128

/* --- Virtqueue structures (VirtIO 1.0 spec, legacy compatible) --- */

/* Descriptor: describes a single buffer segment */
struct vring_desc {
    uint64_t addr;     /* physical address of buffer */
    uint32_t len;      /* length of buffer */
    uint16_t flags;    /* VRING_DESC_F_* */
    uint16_t next;     /* next descriptor index (if NEXT flag set) */
} __attribute__((packed));

/* Available ring: guest writes, device reads */
struct vring_avail {
    uint16_t flags;
    uint16_t idx;      /* next index guest will write to */
    uint16_t ring[];   /* descriptor chain heads */
} __attribute__((packed));

/* Used ring element */
struct vring_used_elem {
    uint32_t id;       /* descriptor chain head index */
    uint32_t len;      /* total bytes written */
} __attribute__((packed));

/* Used ring: device writes, guest reads */
struct vring_used {
    uint16_t flags;
    uint16_t idx;      /* next index device will write to */
    struct vring_used_elem ring[];
} __attribute__((packed));

/* VirtIO block request header */
struct virtio_blk_req {
    uint32_t type;     /* VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT */
    uint32_t reserved;
    uint64_t sector;   /* starting sector (LBA) */
} __attribute__((packed));

/* Complete virtqueue state */
struct virtqueue {
    struct vring_desc  *desc;      /* descriptor table */
    struct vring_avail *avail;     /* available ring */
    struct vring_used  *used;      /* used ring */
    uint16_t           size;       /* number of entries */
    uint16_t           free_head;  /* head of free descriptor list */
    uint16_t           last_used;  /* last seen used index */
    uint16_t           num_free;   /* number of free descriptors */
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

/* Get total disk capacity in 512-byte sectors. */
uint64_t virtio_blk_capacity(void);

/* Check if a virtio-blk device was detected and initialized. */
int virtio_blk_present(void);
