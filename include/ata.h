/* ============================================================================
 * ata.h — ATA PIO Disk Driver
 *
 * Supports primary ATA bus (ports 0x1F0–0x1F7, control 0x3F6).
 * Uses PIO (Programmed I/O) mode for sector read/write.
 * ============================================================================ */

#pragma once

#include "types.h"

/* Sector size */
#define ATA_SECTOR_SIZE  512

/* ATA drive identifiers */
#define ATA_MASTER  0xE0
#define ATA_SLAVE   0xF0

/* Drive info structure */
struct ata_drive {
    uint8_t  present;       /* 1 if drive detected */
    uint8_t  is_ata;        /* 1 if ATA, 0 if ATAPI or other */
    uint8_t  drive_select;  /* ATA_MASTER or ATA_SLAVE */
    uint32_t sectors;       /* total LBA28 sector count */
    char     model[41];     /* model string (null-terminated) */
};

/* Initialize the ATA driver and detect drives */
void ata_init(void);

/* Read 'count' sectors starting at LBA into 'buffer'.
 * Returns 0 on success, -1 on error. */
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);

/* Write 'count' sectors starting at LBA from 'buffer'.
 * Returns 0 on success, -1 on error. */
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);

/* Get info about detected drives */
const struct ata_drive *ata_get_drive(uint8_t drive_num);
