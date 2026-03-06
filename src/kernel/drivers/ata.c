/* ============================================================================
 * ata.c — ATA PIO Disk Driver
 *
 * Detects ATA drives on the primary bus and provides PIO-mode sector
 * read/write using LBA28 addressing.
 *
 * Primary bus I/O ports:
 *   0x1F0 — Data register (16-bit)
 *   0x1F1 — Error / Features
 *   0x1F2 — Sector count
 *   0x1F3 — LBA low (bits 0-7)
 *   0x1F4 — LBA mid (bits 8-15)
 *   0x1F5 — LBA high (bits 16-23)
 *   0x1F6 — Drive/Head (bits 24-27 of LBA + drive select)
 *   0x1F7 — Status (read) / Command (write)
 *   0x3F6 — Alternate status / Device control
 * ============================================================================ */

#include "ata.h"
#include "printk.h"

/* Primary ATA bus ports */
#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECT_CNT   0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_ALT_STATUS 0x3F6
#define ATA_DEV_CTRL   0x3F6

/* ATA commands */
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH  0xE7

/* Status register bits */
#define ATA_SR_BSY   0x80   /* Busy */
#define ATA_SR_DRDY  0x40   /* Drive ready */
#define ATA_SR_DF    0x20   /* Drive fault */
#define ATA_SR_DSC   0x10   /* Drive seek complete */
#define ATA_SR_DRQ   0x08   /* Data request */
#define ATA_SR_CORR  0x04   /* Corrected data */
#define ATA_SR_IDX   0x02   /* Index */
#define ATA_SR_ERR   0x01   /* Error */

/* Inline port I/O */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* Detected drives */
static struct ata_drive drives[2];  /* [0] = master, [1] = slave */

/* --- Wait for BSY to clear --- */
static void ata_wait_bsy(void)
{
    while (inb(ATA_STATUS) & ATA_SR_BSY)
        ;
}

/* --- Wait for DRQ to set --- */
static int ata_wait_drq(void)
{
    uint32_t timeout = 100000;
    uint8_t status;

    while (timeout--) {
        status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR)
            return -1;
        if (status & ATA_SR_DF)
            return -1;
        if (status & ATA_SR_DRQ)
            return 0;
    }

    return -1;  /* timeout */
}

/* --- 400ns delay by reading alternate status 4 times --- */
static void ata_delay(void)
{
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
}

/* --- Identify a single drive --- */
static void ata_identify_drive(uint8_t drive_num)
{
    uint8_t select = (drive_num == 0) ? ATA_MASTER : ATA_SLAVE;
    uint16_t identify_data[256];
    uint8_t status;
    uint32_t i;

    drives[drive_num].present = 0;
    drives[drive_num].is_ata = 0;
    drives[drive_num].drive_select = select;

    /* Select drive */
    outb(ATA_DRIVE_HEAD, select);
    ata_delay();

    /* Send IDENTIFY command */
    outb(ATA_SECT_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay();

    /* Check if drive exists */
    status = inb(ATA_STATUS);
    if (status == 0)
        return;  /* no drive */

    /* Wait for BSY to clear */
    ata_wait_bsy();

    /* Check for non-ATA drives (ATAPI, SATA, etc.) */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0)
        return;  /* not ATA */

    /* Wait for DRQ or ERR */
    if (ata_wait_drq() != 0)
        return;  /* error or timeout */

    /* Read 256 words of identify data */
    for (i = 0; i < 256; i++)
        identify_data[i] = inw(ATA_DATA);

    drives[drive_num].present = 1;
    drives[drive_num].is_ata = 1;

    /* Extract LBA28 sector count (words 60-61) */
    drives[drive_num].sectors = (uint32_t)identify_data[60]
                              | ((uint32_t)identify_data[61] << 16);

    /* Extract model string (words 27-46, byte-swapped) */
    for (i = 0; i < 20; i++) {
        drives[drive_num].model[i * 2]     = (char)(identify_data[27 + i] >> 8);
        drives[drive_num].model[i * 2 + 1] = (char)(identify_data[27 + i] & 0xFF);
    }
    drives[drive_num].model[40] = '\0';

    /* Trim trailing spaces */
    for (i = 39; i > 0; i--) {
        if (drives[drive_num].model[i] == ' ')
            drives[drive_num].model[i] = '\0';
        else
            break;
    }
}

void ata_init(void)
{
    /* Soft reset the primary ATA bus */
    outb(ATA_DEV_CTRL, 0x04);    /* Set SRST bit */
    ata_delay();
    outb(ATA_DEV_CTRL, 0x00);    /* Clear SRST bit */
    ata_delay();

    /* Wait for reset to complete */
    ata_wait_bsy();

    /* Identify both drives */
    ata_identify_drive(0);  /* master */
    ata_identify_drive(1);  /* slave */

    if (drives[0].present) {
        uint64_t size_mb = (uint64_t)drives[0].sectors * ATA_SECTOR_SIZE / (1024 * 1024);
        printk("[OK] ATA master: \"%s\" (%u MiB, %u sectors)\n",
               drives[0].model, size_mb, (uint64_t)drives[0].sectors);
    } else {
        printk("[--] ATA master: not detected\n");
    }

    if (drives[1].present) {
        uint64_t size_mb = (uint64_t)drives[1].sectors * ATA_SECTOR_SIZE / (1024 * 1024);
        printk("[OK] ATA slave: \"%s\" (%u MiB, %u sectors)\n",
               drives[1].model, size_mb, (uint64_t)drives[1].sectors);
    }
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer)
{
    uint16_t *buf = (uint16_t *)buffer;
    uint8_t sc = count;
    uint32_t i, j;

    if (!drives[0].present)
        return -1;

    if (count == 0)
        return -1;

    /* Wait for drive to be ready */
    ata_wait_bsy();

    /* Select drive + LBA mode + top 4 bits of LBA */
    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));

    /* Set sector count and LBA */
    outb(ATA_SECT_CNT, sc);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));

    /* Send READ SECTORS command */
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);

    /* Read each sector */
    for (i = 0; i < sc; i++) {
        ata_delay();

        if (ata_wait_drq() != 0)
            return -1;

        /* Read 256 words (512 bytes) */
        for (j = 0; j < 256; j++)
            buf[i * 256 + j] = inw(ATA_DATA);
    }

    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer)
{
    const uint16_t *buf = (const uint16_t *)buffer;
    uint8_t sc = count;
    uint32_t i, j;

    if (!drives[0].present)
        return -1;

    if (count == 0)
        return -1;

    ata_wait_bsy();

    /* Select drive + LBA mode + top 4 bits */
    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));

    outb(ATA_SECT_CNT, sc);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));

    /* Send WRITE SECTORS command */
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (i = 0; i < sc; i++) {
        ata_delay();

        if (ata_wait_drq() != 0)
            return -1;

        /* Write 256 words (512 bytes) */
        for (j = 0; j < 256; j++)
            outw(ATA_DATA, buf[i * 256 + j]);
    }

    /* Flush write cache */
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_bsy();

    return 0;
}

const struct ata_drive *ata_get_drive(uint8_t drive_num)
{
    if (drive_num > 1)
        return (const struct ata_drive *)0;
    return &drives[drive_num];
}
