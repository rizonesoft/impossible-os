/* ============================================================================
 * virtio.h — VirtIO PCI modern transport
 *
 * Implements split virtqueues over PCI MMIO (modern interface).
 * QEMU virtio-tablet-pci uses device ID 0x1052 (modern, non-transitional)
 * which requires PCI capability-based configuration.
 *
 * References:
 *   - VirtIO spec 1.1 §4.1 (Virtio Over PCI Bus)
 *   - VirtIO spec 1.1 §2.6 (Split Virtqueues)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- VirtIO PCI vendor/device IDs ---- */
#define VIRTIO_PCI_VENDOR       0x1AF4
#define VIRTIO_PCI_DEV_INPUT    0x1052  /* modern: 0x1040 + 18 (input) */

/* ---- VirtIO PCI capability types ---- */
#define VIRTIO_PCI_CAP_COMMON_CFG   1  /* Common configuration */
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2  /* Notifications */
#define VIRTIO_PCI_CAP_ISR_CFG      3  /* ISR access */
#define VIRTIO_PCI_CAP_DEVICE_CFG   4  /* Device-specific config */
#define VIRTIO_PCI_CAP_PCI_CFG      5  /* PCI config access */

/* PCI capability ID for vendor-specific (VirtIO uses this) */
#define PCI_CAP_ID_VENDOR       0x09

/* ---- VirtIO common configuration registers (MMIO offsets) ---- */
/* These are offsets within the common config MMIO region */
#define VIRTIO_COMMON_DFSELECT       0x00  /* 32-bit: device feature select */
#define VIRTIO_COMMON_DF             0x04  /* 32-bit: device feature bits */
#define VIRTIO_COMMON_GFSELECT       0x08  /* 32-bit: driver feature select */
#define VIRTIO_COMMON_GF             0x0C  /* 32-bit: driver feature bits */
#define VIRTIO_COMMON_MSIX_CONFIG    0x10  /* 16-bit: MSI-X config vector */
#define VIRTIO_COMMON_NUM_QUEUES     0x12  /* 16-bit: number of queues */
#define VIRTIO_COMMON_STATUS         0x14  /* 8-bit: device status */
#define VIRTIO_COMMON_CFGGEN         0x15  /* 8-bit: config generation */
#define VIRTIO_COMMON_Q_SELECT       0x16  /* 16-bit: queue select */
#define VIRTIO_COMMON_Q_SIZE         0x18  /* 16-bit: queue size */
#define VIRTIO_COMMON_Q_MSIX_VECTOR  0x1A  /* 16-bit: queue MSI-X vector */
#define VIRTIO_COMMON_Q_ENABLE       0x1C  /* 16-bit: queue enable */
#define VIRTIO_COMMON_Q_NOTIFY_OFF   0x1E  /* 16-bit: queue notify offset */
#define VIRTIO_COMMON_Q_DESC         0x20  /* 64-bit: descriptor table addr */
#define VIRTIO_COMMON_Q_AVAIL        0x28  /* 64-bit: avail ring addr */
#define VIRTIO_COMMON_Q_USED         0x30  /* 64-bit: used ring addr */

/* ---- VirtIO device status bits ---- */
#define VIRTIO_STATUS_ACKNOWLEDGE  0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FEATURES_OK  0x08
#define VIRTIO_STATUS_FAILED       0x80

/* ---- Virtqueue descriptor flags ---- */
#define VIRTQ_DESC_F_NEXT      0x01  /* Descriptor continues via 'next' */
#define VIRTQ_DESC_F_WRITE     0x02  /* Device writes (vs reads) */

/* ---- Virtqueue structures (split virtqueue layout) ---- */

/* Single descriptor in the descriptor table */
struct virtq_desc {
    uint64_t addr;     /* Physical address of the buffer */
    uint32_t len;      /* Length of the buffer */
    uint16_t flags;    /* VIRTQ_DESC_F_* */
    uint16_t next;     /* Next descriptor index if NEXT flag set */
} __attribute__((packed));

/* Available ring (driver → device) */
struct virtq_avail {
    uint16_t flags;
    uint16_t idx;       /* Next index the driver will write to */
    uint16_t ring[];    /* Array of descriptor chain heads */
} __attribute__((packed));

/* Used ring element */
struct virtq_used_elem {
    uint32_t id;        /* Index of the descriptor chain head */
    uint32_t len;       /* Total bytes written by device */
} __attribute__((packed));

/* Used ring (device → driver) */
struct virtq_used {
    uint16_t flags;
    uint16_t idx;       /* Next index the device will write to */
    struct virtq_used_elem ring[];
} __attribute__((packed));

/* ---- VirtIO PCI modern device state ---- */
/* Holds MMIO base addresses for each config region */
struct virtio_pci_dev {
    volatile uint8_t *common_cfg;   /* Mapped common config region */
    volatile uint8_t *notify_base;  /* Mapped notification region */
    volatile uint8_t *isr_cfg;      /* Mapped ISR region */
    volatile uint8_t *device_cfg;   /* Mapped device-specific config */
    uint32_t notify_off_multiplier; /* Bytes per queue notification offset */
};

/* Complete virtqueue state */
struct virtqueue {
    uint16_t            size;       /* Number of descriptors */
    uint16_t            free_head;  /* Head of free descriptor list */
    uint16_t            last_used;  /* Last used ring index we processed */
    uint16_t            num_free;   /* Number of free descriptors */
    struct virtq_desc  *desc;       /* Descriptor table */
    struct virtq_avail *avail;      /* Available ring */
    struct virtq_used  *used;       /* Used ring */
    uint16_t            notify_off; /* Queue notify offset from device */
    struct virtio_pci_dev *dev;     /* Parent device (for notifications) */
    uint16_t            queue_idx;  /* Queue index (0, 1, ...) */
};

/* ---- API ---- */

/* Discover VirtIO PCI capabilities and set up MMIO pointers.
 * pci_bus/dev/func identify the device. Returns 0 on success. */
int virtio_pci_init(struct virtio_pci_dev *dev,
                    uint8_t bus, uint8_t pci_dev, uint8_t func);

/* Initialize a virtqueue for a given device and queue index.
 * Returns 0 on success, -1 on failure. */
int virtq_init(struct virtqueue *vq, struct virtio_pci_dev *dev,
               uint16_t queue_idx);

/* Add a single write-only buffer to the virtqueue (for device to fill).
 * Returns the descriptor index, or -1 if no free descriptors. */
int virtq_add_buf(struct virtqueue *vq, void *buf, uint32_t len,
                  uint16_t flags);

/* Notify the device that new buffers are available in the given queue. */
void virtq_kick(struct virtqueue *vq);

/* Check if there are completed buffers in the used ring.
 * Returns the descriptor index and sets *len, or -1 if empty. */
int virtq_get_buf(struct virtqueue *vq, uint32_t *len);

/* Free a descriptor back to the free list. */
void virtq_free_desc(struct virtqueue *vq, uint16_t idx);

/* Read/write device status via common config MMIO */
uint8_t virtio_get_status(struct virtio_pci_dev *dev);
void    virtio_set_status(struct virtio_pci_dev *dev, uint8_t status);

/* Read ISR status (clears interrupt) */
uint8_t virtio_read_isr(struct virtio_pci_dev *dev);
