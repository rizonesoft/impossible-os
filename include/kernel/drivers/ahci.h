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

/* GHC bits */
#define AHCI_GHC_AE         (1U << 31)  /* AHCI Enable */
#define AHCI_GHC_IE         (1U << 1)   /* Interrupt Enable */
#define AHCI_GHC_HR         (1U << 0)   /* HBA Reset */

/* CAP bits */
#define AHCI_CAP_NP_MASK    0x1F        /* Number of ports (0-based) */
#define AHCI_CAP_NCS_SHIFT  8           /* Number of command slots shift */
#define AHCI_CAP_NCS_MASK   0x1F00      /* Number of command slots mask */

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
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_READ_DMA_EX  0x25   /* READ DMA EXT (LBA48) */
#define ATA_CMD_WRITE_DMA_EX 0x35   /* WRITE DMA EXT (LBA48) */
#define ATA_CMD_CACHE_FLUSH_EX 0xEA /* CACHE FLUSH EXT */

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

/* ---- AHCI port state ---- */
struct ahci_port {
    uint8_t  active;       /* 1 if SATA drive attached */
    uint8_t  port_num;     /* Physical port number */
    uint32_t sig;          /* Port signature */
    uint64_t sectors;      /* Total sector count (from IDENTIFY) */
    char     model[41];    /* Model string (null-terminated) */
    char     serial[21];   /* Serial number (null-terminated) */
    volatile uint8_t *regs; /* Port register base (MMIO) */
    struct ahci_cmd_header *cmdlist; /* Command list (32 headers) */
    void    *fis_base;     /* FIS receive buffer (256 bytes) */
    struct ahci_cmd_tbl *cmdtbl[32]; /* Command tables */
};

/* Max ports supported */
#define AHCI_MAX_PORTS  32

/* ---- API ---- */

/* Initialize the AHCI driver and detect SATA drives. Returns 0 on success. */
int ahci_init(void);

/* Read 'count' sectors starting at LBA into 'buffer'.
 * port_idx is the index into detected ports (0-based).
 * Returns 0 on success, -1 on error. */
int ahci_read(int port_idx, uint64_t lba, uint32_t count, void *buffer);

/* Write 'count' sectors starting at LBA from 'buffer'.
 * Returns 0 on success, -1 on error. */
int ahci_write(int port_idx, uint64_t lba, uint32_t count, const void *buffer);

/* Get total capacity in 512-byte sectors for a given port. */
uint64_t ahci_capacity(int port_idx);

/* Check if any AHCI device was detected and initialized. */
int ahci_present(void);

/* Get number of detected SATA drives. */
int ahci_drive_count(void);
