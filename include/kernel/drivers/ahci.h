/* ============================================================================
 * ahci.h — AHCI (SATA) Disk Driver
 *
 * Advanced Host Controller Interface for SATA drives.
 * Detected via PCI class 0x01 (storage), subclass 0x06 (AHCI).
 * Uses MMIO via PCI BAR5 (ABAR) for HBA register access.
 *
 * References:
 *   - AHCI 1.3.1 specification
 *   - Serial ATA specification (FIS types)
 *
 * QEMU flag: -device ahci,id=ahci0
 *            -device ide-hd,drive=disk1,bus=ahci0.0
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/event.h"

/* ---- PCI identification ---- */
#define AHCI_PCI_CLASS      0x01   /* Mass Storage */
#define AHCI_PCI_SUBCLASS   0x06   /* SATA (AHCI) */

/* ---- AHCI HBA memory registers (offsets from ABAR) ---- */
#define AHCI_CAP            0x00   /* Host Capabilities */
#define AHCI_GHC            0x04   /* Global Host Control */
#define AHCI_IS             0x08   /* Interrupt Status */
#define AHCI_PI             0x0C   /* Ports Implemented */
#define AHCI_VS             0x10   /* Version */
#define AHCI_CAP2           0x24   /* Extended Capabilities */
#define AHCI_BOHC           0x28   /* BIOS/OS Handoff Control and Status */

/* CAP2 bits */
#define AHCI_CAP2_BOH       (1U << 0)   /* BIOS/OS Handoff supported */

/* BOHC bits */
#define AHCI_BOHC_BOS       (1U << 0)   /* BIOS Owned Semaphore */
#define AHCI_BOHC_OOS       (1U << 1)   /* OS Owned Semaphore */
#define AHCI_BOHC_BB        (1U << 4)   /* BIOS Busy (BIOS is cleaning up) */

/* GHC bits */
#define AHCI_GHC_AE         (1U << 31)  /* AHCI Enable */
#define AHCI_GHC_IE         (1U << 1)   /* Interrupt Enable */
#define AHCI_GHC_HR         (1U << 0)   /* HBA Reset */

/* CAP bits */
#define AHCI_CAP_NP_MASK    0x1F        /* Number of ports (0-based) */
#define AHCI_CAP_NCS_SHIFT  8           /* Number of command slots shift */
#define AHCI_CAP_NCS_MASK   0x1F00      /* Number of command slots mask */
#define AHCI_CAP_SCLO       (1U << 24)  /* Supports Command List Override */
#define AHCI_CAP_SNCQ       (1U << 30)  /* Supports Native Command Queuing */

/* ---- Port registers (offset = 0x100 + port * 0x80) ---- */
#define AHCI_PORT_BASE      0x100
#define AHCI_PORT_SIZE      0x80

#define AHCI_PxCLB          0x00   /* Command List Base Address (low) */
#define AHCI_PxCLBU         0x04   /* Command List Base Address (high) */
#define AHCI_PxFB           0x08   /* FIS Base Address (low) */
#define AHCI_PxFBU          0x0C   /* FIS Base Address (high) */
#define AHCI_PxIS           0x10   /* Interrupt Status */
#define AHCI_PxIE           0x14   /* Interrupt Enable */
#define AHCI_PxCMD          0x18   /* Command and Status */
#define AHCI_PxTFD          0x20   /* Task File Data */
#define AHCI_PxSIG          0x24   /* Signature */
#define AHCI_PxSSTS         0x28   /* SATA Status (SCR0: SStatus) */
#define AHCI_PxSCTL         0x2C   /* SATA Control (SCR2: SControl) */
#define AHCI_PxSERR         0x30   /* SATA Error (SCR1: SError) */
#define AHCI_PxSACT         0x34   /* SATA Active */
#define AHCI_PxCI           0x38   /* Command Issue */

/* PxIS interrupt status bits (write-1-to-clear) */
#define AHCI_PxIS_DHRS      (1U << 0)   /* D2H Register FIS Interrupt */
#define AHCI_PxIS_PSS       (1U << 1)   /* PIO Setup FIS Interrupt */
#define AHCI_PxIS_DSS       (1U << 2)   /* DMA Setup FIS Interrupt */
#define AHCI_PxIS_SDBS      (1U << 3)   /* Set Device Bits Interrupt */
#define AHCI_PxIS_UFS       (1U << 4)   /* Unknown FIS Interrupt */
#define AHCI_PxIS_DPS       (1U << 5)   /* Descriptor Processed */
#define AHCI_PxIS_PCS       (1U << 6)   /* Port Connect Change Status */
#define AHCI_PxIS_PRCS      (1U << 22)  /* PhyRdy Change Status */
#define AHCI_PxIS_IPMS      (1U << 23)  /* Incorrect Port Multiplier Status */
#define AHCI_PxIS_OFS       (1U << 24)  /* Overflow Status (non-fatal) */
#define AHCI_PxIS_INFS      (1U << 26)  /* Interface Non-Fatal Error */
#define AHCI_PxIS_IFS       (1U << 27)  /* Interface Fatal Error */
#define AHCI_PxIS_HBDS      (1U << 28)  /* Host Bus Data Error (fatal) */
#define AHCI_PxIS_HBFS      (1U << 29)  /* Host Bus Fatal Error */
#define AHCI_PxIS_TFES      (1U << 30)  /* Task File Error Status */
#define AHCI_PxIS_CPDS      (1U << 31)  /* Cold Port Detect Status */

/* Composite error masks */
#define AHCI_PxIS_FATAL     (AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | \
                             AHCI_PxIS_IFS  | AHCI_PxIS_TFES)
#define AHCI_PxIS_NONFATAL  (AHCI_PxIS_INFS | AHCI_PxIS_OFS)

/* PxSERR diagnostic and error bits (write-1-to-clear) */
#define AHCI_PxSERR_DIAG_X  (1U << 26)  /* Exchanged (hot-plug event) */
#define AHCI_PxSERR_DIAG_N  (1U << 16)  /* PhyRdy change detected */
#define AHCI_PxSERR_ERR_E   (1U << 11)  /* Internal error */
#define AHCI_PxSERR_ERR_C   (1U << 9)   /* Non-recovered persistent comm error */
#define AHCI_PxSERR_ERR_T   (1U << 8)   /* Non-recovered transient data integrity */
#define AHCI_PxSERR_ERR_M   (1U << 1)   /* Recovered communication error (CRC) */

/* PxCMD bits */
#define AHCI_PxCMD_ST       (1U << 0)   /* Start */
#define AHCI_PxCMD_FRE      (1U << 4)   /* FIS Receive Enable */
#define AHCI_PxCMD_FR       (1U << 14)  /* FIS Receive Running */
#define AHCI_PxCMD_CR       (1U << 15)  /* Command List Running */

/* PxTFD bits */
#define AHCI_PxTFD_ERR      (1U << 0)   /* Error */
#define AHCI_PxTFD_DRQ      (1U << 3)   /* Data Request */
#define AHCI_PxTFD_BSY      (1U << 7)   /* Busy */

/* PxSSTS: Device Detection (bits 3:0) */
#define AHCI_SSTS_DET_MASK  0x0F
#define AHCI_SSTS_DET_OK    0x03        /* Device and PHY communication established */
#define AHCI_SSTS_IPM_MASK  0xF00
#define AHCI_SSTS_IPM_ACTIVE 0x100      /* Active state */

/* Port signature values */
#define AHCI_SIG_ATA        0x00000101  /* SATA drive */
#define AHCI_SIG_ATAPI      0xEB140101  /* SATAPI device */

/* ---- FIS types ---- */
#define FIS_TYPE_REG_H2D    0x27   /* Register FIS — Host to Device */
#define FIS_TYPE_REG_D2H    0x34   /* Register FIS — Device to Host */
#define FIS_TYPE_DMA_ACT    0x39   /* DMA Activate FIS */
#define FIS_TYPE_DMA_SETUP  0x41   /* DMA Setup FIS */
#define FIS_TYPE_DATA       0x46   /* Data FIS */
#define FIS_TYPE_PIO_SETUP  0x5F   /* PIO Setup FIS */

/* ---- ATA commands ---- */
#define ATA_CMD_IDENTIFY         0xEC
#define ATA_CMD_IDENTIFY_PACKET  0xA1   /* IDENTIFY PACKET DEVICE (ATAPI) */
#define ATA_CMD_PACKET           0xA0   /* PACKET command (ATAPI) */
#define ATA_CMD_READ_DMA_EX      0x25   /* READ DMA EXT (LBA48) */
#define ATA_CMD_WRITE_DMA_EX     0x35   /* WRITE DMA EXT (LBA48) */
#define ATA_CMD_CACHE_FLUSH_EX   0xEA   /* CACHE FLUSH EXT */
#define ATA_CMD_READ_FPDMA       0x60   /* READ FPDMA QUEUED (NCQ) */
#define ATA_CMD_WRITE_FPDMA      0x61   /* WRITE FPDMA QUEUED (NCQ) */

/* ---- SCSI commands (used inside ATAPI PACKET) ---- */
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY    0x25   /* READ CAPACITY (10) */
#define SCSI_READ_10          0x28   /* READ (10) */

/* ---- FIS: Register Host to Device (20 bytes) ---- */
struct fis_reg_h2d {
    uint8_t  fis_type;     /* FIS_TYPE_REG_H2D */
    uint8_t  pmport_c;     /* [7:4] PM Port, [6] reserved, bit 7 = C (command) */
    uint8_t  command;      /* ATA command */
    uint8_t  featurel;     /* Feature low */

    uint8_t  lba0;         /* LBA bits  7:0 */
    uint8_t  lba1;         /* LBA bits 15:8 */
    uint8_t  lba2;         /* LBA bits 23:16 */
    uint8_t  device;       /* Device register (bit 6 = LBA mode) */

    uint8_t  lba3;         /* LBA bits 31:24 */
    uint8_t  lba4;         /* LBA bits 39:32 */
    uint8_t  lba5;         /* LBA bits 47:40 */
    uint8_t  featureh;     /* Feature high */

    uint8_t  countl;       /* Sector count low */
    uint8_t  counth;       /* Sector count high */
    uint8_t  icc;          /* Isochronous command completion */
    uint8_t  control;      /* Control register */

    uint8_t  reserved[4];
} __attribute__((packed));

/* ---- Command header (32 bytes, 32 per port) ---- */
struct ahci_cmd_header {
    uint16_t flags;        /* [4:0] CFL (CFIS len in DWORDs), bit 5=ATAPI,
                              bit 6=W (write), bit 7=P (prefetch) */
    uint16_t prdtl;        /* PRDT entry count */
    uint32_t prdbc;        /* PRD Byte Count (transferred) */
    uint32_t ctba;         /* Command Table Base Address (low) */
    uint32_t ctbau;        /* Command Table Base Address (high) */
    uint32_t reserved[4];
} __attribute__((packed));

/* ---- PRDT entry (16 bytes) ---- */
struct ahci_prdt_entry {
    uint32_t dba;          /* Data Base Address (low) */
    uint32_t dbau;         /* Data Base Address (high) */
    uint32_t reserved;
    uint32_t dbc;          /* Byte Count (bit 31 = interrupt on completion) */
} __attribute__((packed));

/* Max PRDT entries per command table */
#define AHCI_MAX_PRDT    8

/* ---- Command table (variable size) ---- */
struct ahci_cmd_tbl {
    uint8_t  cfis[64];     /* Command FIS (up to 64 bytes) */
    uint8_t  acmd[16];     /* ATAPI command (12–16 bytes) */
    uint8_t  reserved[48]; /* Reserved */
    struct ahci_prdt_entry prdt[AHCI_MAX_PRDT];
} __attribute__((packed));

/* ---- NCQ async callback ---- */
typedef void (*ahci_callback_t)(int port, int tag, int status, void *ctx);

/* ---- Per-port error counters ---- */
struct ahci_error_counters {
    uint32_t fatal_errors;      /* HBFS + HBDS + IFS + TFES count */
    uint32_t nonfatal_errors;   /* INFS + OFS count */
    uint32_t crc_errors;        /* PxSERR.ERR.M (recovered comm errors) */
    uint32_t link_resets;       /* COMRESET recovery count */
    uint32_t cmd_failures;      /* Commands that failed after all retries */
};

/* ---- AHCI port state ---- */
struct ahci_port {
    uint8_t  active;       /* 1 if drive attached */
    uint8_t  port_num;     /* Physical port number */
    uint8_t  is_atapi;     /* 1 if ATAPI (optical) device */
    uint32_t sig;          /* Port signature */
    uint32_t sector_size;  /* Bytes per sector (512 for HDD, 2048 for optical) */
    uint64_t sectors;      /* Total sector count */
    char     model[41];    /* Model string (null-terminated) */
    char     serial[21];   /* Serial number (null-terminated) */
    volatile uint8_t *regs; /* Port register base (MMIO) */
    struct ahci_cmd_header *cmdlist; /* Command list (32 headers) */
    void    *fis_base;     /* FIS receive buffer (256 bytes) */
    struct ahci_cmd_tbl *cmdtbl[32]; /* Command tables */
    event_t  completion;   /* Per-port I/O completion event (IRQ-driven) */
    struct ahci_error_counters errors; /* Error tracking */

    /* NCQ state */
    uint8_t  ncq_supported;        /* 1 if both HBA and device support NCQ */
    uint8_t  ncq_depth;            /* Effective max queue depth (1–32) */
    uint32_t tags_allocated;       /* Bitmap of allocated tags */
    uint32_t tags_pending;         /* Bitmap of issued/pending tags */
    uint32_t tags_completed;       /* Bitmap of completed tags (set by ISR) */
    int8_t   tag_status[32];       /* Per-tag completion status (0=ok, -1=err) */
    ahci_callback_t tag_callbacks[32]; /* Async callbacks (NULL = sync) */
    void    *tag_cb_ctx[32];       /* Per-tag callback context */

    /* TRIM state */
    uint8_t  trim_supported;       /* 1 if device supports DATA SET MANAGEMENT */
    uint8_t  trim_deterministic;   /* 1 if deterministic read after TRIM */
};

/* Max ports supported */
#define AHCI_MAX_PORTS  32

/* ---- API: SATA drives ---- */

/* Initialize the AHCI driver and detect SATA/ATAPI devices. */
int ahci_init(void);

/* Read 'count' 512-byte sectors starting at LBA into 'buffer'. */
int ahci_read(int port_idx, uint64_t lba, uint32_t count, void *buffer);

/* Write 'count' 512-byte sectors starting at LBA from 'buffer'. */
int ahci_write(int port_idx, uint64_t lba, uint32_t count, const void *buffer);

/* Get total capacity in 512-byte sectors for a given SATA port. */
uint64_t ahci_capacity(int port_idx);

/* Check if any AHCI device was detected and initialized. */
int ahci_present(void);

/* Get number of detected SATA drives. */
int ahci_drive_count(void);

/* Flush per-port error counters to HKLM\HARDWARE\AHCI\PortN\Errors. */
void ahci_flush_error_counters(void);

/* Issue TRIM (DATA SET MANAGEMENT) for a range of sectors.
 * Notifies SSDs that deleted blocks can be erased internally. */
int ahci_trim(int port_idx, uint64_t lba, uint32_t count);

/* ---- API: NCQ (Native Command Queuing) ---- */

/* Synchronous NCQ read — falls back to DMA if NCQ not supported. */
int ahci_ncq_read(int port_idx, uint64_t lba, uint32_t count, void *buffer);

/* Synchronous NCQ write — falls back to DMA if NCQ not supported. */
int ahci_ncq_write(int port_idx, uint64_t lba, uint32_t count,
                   const void *buffer);

/* Asynchronous NCQ submit — returns tag (0–31) or -1 on error.
 * Callback is called from ISR context on completion. */
int ahci_submit(int port_idx, uint64_t lba, uint32_t count, void *buffer,
                int is_write, ahci_callback_t callback, void *ctx);

/* ---- API: ATAPI (optical) devices ---- */

/* Get number of detected ATAPI devices. */
int ahci_atapi_count(void);

/* Read 'count' 2048-byte sectors starting at LBA from ATAPI device. */
int ahci_atapi_read(int atapi_idx, uint64_t lba, uint32_t count, void *buffer);

/* Get total capacity in 2048-byte sectors for an ATAPI device. */
uint64_t ahci_atapi_capacity(int atapi_idx);

/* Get sector size for an ATAPI device (typically 2048). */
uint32_t ahci_atapi_sector_size(int atapi_idx);
