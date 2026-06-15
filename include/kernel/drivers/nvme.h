/* ============================================================================
 * nvme.h -- NVMe storage driver
 *
 * NVMe controller discovery, BAR0 MMIO mapping, and register definitions.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/atomic.h"

/* ---- PCI class identification ---- */
#define NVME_PCI_CLASS          0x01    /* Mass Storage Controller */
#define NVME_PCI_SUBCLASS       0x08    /* Non-Volatile Memory Controller */
#define NVME_PCI_PROG_IF        0x02    /* NVM Express */

/* ---- NVMe controller register offsets (BAR0 MMIO) ---- */
#define NVME_REG_CAP            0x00    /* Controller Capabilities (64-bit) */
#define NVME_REG_VS             0x08    /* Version (32-bit) */
#define NVME_REG_INTMS          0x0C    /* Interrupt Mask Set (32-bit) */
#define NVME_REG_INTMC          0x10    /* Interrupt Mask Clear (32-bit) */
#define NVME_REG_CC             0x14    /* Controller Configuration (32-bit) */
#define NVME_REG_CSTS           0x1C    /* Controller Status (32-bit) */
#define NVME_REG_AQA            0x24    /* Admin Queue Attributes (32-bit) */
#define NVME_REG_ASQ            0x28    /* Admin Submission Queue Base (64-bit) */
#define NVME_REG_ACQ            0x30    /* Admin Completion Queue Base (64-bit) */

/* ---- CAP register fields (64-bit at offset 0x00) ---- */
#define NVME_CAP_MQES_MASK      0xFFFF              /* bits 15:0  -- Max Queue Entries Supported (0-based) */
#define NVME_CAP_TO_SHIFT       24                  /* bits 31:24 -- Timeout (in 500ms units) */
#define NVME_CAP_TO_MASK        0xFF
#define NVME_CAP_DSTRD_SHIFT    32                  /* bits 35:32 -- Doorbell Stride (2^(2+DSTRD) bytes) */
#define NVME_CAP_DSTRD_MASK     0x0F
#define NVME_CAP_CSS_SHIFT      37                  /* bits 44:37 -- Command Sets Supported */
#define NVME_CAP_CSS_MASK       0xFF
#define NVME_CAP_CSS_NVM        (1 << 0)            /* NVM command set supported */
#define NVME_CAP_MPSMIN_SHIFT   48                  /* bits 51:48 -- Memory Page Size Minimum (2^(12+MPSMIN)) */
#define NVME_CAP_MPSMIN_MASK    0x0F
#define NVME_CAP_MPSMAX_SHIFT   52                  /* bits 55:52 -- Memory Page Size Maximum */
#define NVME_CAP_MPSMAX_MASK    0x0F

/* ---- CC register fields (32-bit at offset 0x14) ---- */
#define NVME_CC_EN              (1 << 0)            /* Enable */
#define NVME_CC_CSS_SHIFT       4                   /* bits 6:4  -- I/O Command Set Selected */
#define NVME_CC_CSS_NVM         (0 << 4)            /* NVM command set */
#define NVME_CC_MPS_SHIFT       7                   /* bits 10:7 -- Memory Page Size (2^(12+MPS)) */
#define NVME_CC_AMS_SHIFT       11                  /* bits 13:11 -- Arbitration Mechanism */
#define NVME_CC_IOSQES_SHIFT    16                  /* bits 19:16 -- I/O SQ Entry Size (2^N) */
#define NVME_CC_IOCQES_SHIFT    20                  /* bits 23:20 -- I/O CQ Entry Size (2^N) */
#define NVME_CC_SHN_SHIFT       14                  /* bits 15:14 -- Shutdown Notification */
#define NVME_CC_SHN_MASK        (3u << NVME_CC_SHN_SHIFT)
#define NVME_CC_SHN_NORMAL      (1u << NVME_CC_SHN_SHIFT)  /* 01b = normal shutdown */

/* ---- CSTS register fields (32-bit at offset 0x1C) ---- */
#define NVME_CSTS_RDY           (1 << 0)            /* Ready */
#define NVME_CSTS_CFS           (1 << 1)            /* Controller Fatal Status */
#define NVME_CSTS_SHST_SHIFT    2                   /* bits 3:2 -- Shutdown Status */
#define NVME_CSTS_SHST_MASK     (3u << NVME_CSTS_SHST_SHIFT)
#define NVME_CSTS_SHST_COMPLETE (2u << NVME_CSTS_SHST_SHIFT)  /* 10b = shutdown complete */

/* ---- Admin command opcodes ---- */
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_CREATE_IOCQ  0x05
#define NVME_ADMIN_CREATE_IOSQ  0x01

/* ---- NVM I/O command opcodes ---- */
#define NVME_IO_READ            0x02
#define NVME_IO_WRITE           0x01
#define NVME_IO_FLUSH           0x00    /* Flush volatile write cache */

/* ---- Identify CNS values ---- */
#define NVME_IDENTIFY_CNS_CTRL  0x01
#define NVME_IDENTIFY_CNS_NS    0x00

/* ---- Queue depths ---- */
#define NVME_ADMIN_QUEUE_DEPTH  32
#define NVME_IO_QUEUE_DEPTH     64

/* ---- Limits ---- */
#define NVME_MAX_CONTROLLERS    4

/* ---- Submission Queue Entry (64 bytes) ---- */
struct nvme_sqe {
    uint32_t cdw0;      /* opcode[7:0], fuse[9:8], psdt[15:14], cid[31:16] */
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

/* ---- Completion Queue Entry (16 bytes) ---- */
struct nvme_cqe {
    uint32_t dw0;       /* command-specific result */
    uint32_t dw1;       /* reserved */
    uint16_t sq_head;   /* SQ head pointer */
    uint16_t sq_id;     /* SQ identifier */
    uint16_t cid;       /* command identifier */
    uint16_t status;    /* phase bit[0], status[15:1] */
};

/* ---- Controller state ---- */
struct nvme_controller {
    /* PCI location */
    uint8_t             bus;
    uint8_t             dev;
    uint8_t             func;
    uint8_t             active;

    /* MMIO mapping */
    volatile uint8_t   *mmio_base;
    uint64_t            mmio_phys;
    uint32_t            mmio_size;

    /* CAP cache */
    uint16_t            mqes;       /* Max Queue Entries Supported (0-based) */
    uint8_t             dstrd;      /* Doorbell Stride */
    uint8_t             to;         /* Timeout (in 500ms units) */
    uint8_t             css;        /* Command Sets Supported */
    uint8_t             mpsmin;     /* Memory Page Size Minimum */
    uint8_t             mpsmax;     /* Memory Page Size Maximum */

    /* Version */
    uint16_t            ver_major;
    uint8_t             ver_minor;
    uint8_t             ver_ter;

    /*: Admin queue */
    uintptr_t           admin_sq_phys;
    uintptr_t           admin_cq_phys;
    volatile struct nvme_sqe *admin_sq;
    volatile struct nvme_cqe *admin_cq;
    uint16_t            admin_sq_tail;
    uint16_t            admin_cq_head;
    uint8_t             admin_cq_phase;

    /*: I/O queue (QID=1) */
    uintptr_t           io_sq_phys;
    uintptr_t           io_cq_phys;
    volatile struct nvme_sqe *io_sq;
    volatile struct nvme_cqe *io_cq;
    uint16_t            io_sq_tail;
    uint16_t            io_cq_head;
    uint16_t            io_queue_depth; /* actual depth = min(NVME_IO_QUEUE_DEPTH, MQES+1) */
    uint8_t             io_cq_phase;
    uint8_t             io_queue_active;
    atomic_t            io_busy;        /* SMP-safe single-in-flight CAS gate; see nvme_submit_io_cmd */
    atomic_t            shutting_down;  /* set by nvme_shutdown; rejects new I/O before CC.SHN */

    /*: Namespace info */
    uint64_t            ns_lba_count;
    uint32_t            ns_sector_size;
    char                model[41];      /* 40 chars + NUL */
    char                serial[21];     /* 20 chars + NUL */
};

/* ---- Public API ---- */
int                     nvme_init(void);
int                     nvme_controller_count(void);
struct nvme_controller *nvme_get_controller(int idx);
int                     nvme_read_sectors(int ctrl_idx, uint64_t lba,
                                          uint32_t count, void *buf);
int                     nvme_write_sectors(int ctrl_idx, uint64_t lba,
                                           uint32_t count, const void *buf);
int                     nvme_flush(int ctrl_idx);    /* NVM Flush -- flush volatile write cache */
int                     nvme_shutdown(int ctrl_idx); /* CC.SHN normal shutdown + poll CSTS.SHST */
void                    nvme_shutdown_all(void);     /* shutdown every active controller (poweroff/reboot) */
