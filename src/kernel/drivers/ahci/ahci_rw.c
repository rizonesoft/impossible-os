/* ============================================================================
 * ahci_rw.c — AHCI DMA read/write, IDENTIFY, TRIM, FUA, flush
 * ============================================================================ */

#include "kernel/drivers/ahci_internal.h"

/* ---- IDENTIFY DEVICE ---- */
int ahci_do_identify(struct ahci_port *p)
{
    int slot;
    struct ahci_cmd_header *hdr;
    struct ahci_cmd_tbl *tbl;
    struct fis_reg_h2d *fis;
    uint16_t *ident_buf;
    int i;

    ident_buf = (uint16_t *)kmalloc(512);
    if (!ident_buf) return -1;
    ahci_memset(ident_buf, 0, 512);

    slot = port_find_slot(p->regs);
    if (slot < 0) { return -1; }

    hdr = &p->cmdlist[slot];
    tbl = p->cmdtbl[slot];

    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = ATA_CMD_IDENTIFY;
    fis->device   = 0;

    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)ident_buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = 512 - 1;

    hdr->flags = (sizeof(struct fis_reg_h2d) / 4) & 0x1F;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    if (port_issue_cmd(p, slot) != 0) {
        return -1;
    }

    /* Extract LBA48 sector count (words 100-103) */
    p->sectors = (uint64_t)ident_buf[100]
               | ((uint64_t)ident_buf[101] << 16)
               | ((uint64_t)ident_buf[102] << 32)
               | ((uint64_t)ident_buf[103] << 48);

    if (p->sectors == 0) {
        p->sectors = (uint64_t)ident_buf[60]
                   | ((uint64_t)ident_buf[61] << 16);
    }

    /* Extract model string (words 27-46, byte-swapped) */
    for (i = 0; i < 20; i++) {
        p->model[i * 2]     = (char)(ident_buf[27 + i] >> 8);
        p->model[i * 2 + 1] = (char)(ident_buf[27 + i] & 0xFF);
    }
    p->model[40] = '\0';

    for (i = 39; i > 0; i--) {
        if (p->model[i] == ' ')
            p->model[i] = '\0';
        else
            break;
    }

    /* Extract serial number (words 10-19, byte-swapped) */
    for (i = 0; i < 10; i++) {
        p->serial[i * 2]     = (char)(ident_buf[10 + i] >> 8);
        p->serial[i * 2 + 1] = (char)(ident_buf[10 + i] & 0xFF);
    }
    p->serial[20] = '\0';
    for (i = 19; i > 0; i--) {
        if (p->serial[i] == ' ')
            p->serial[i] = '\0';
        else
            break;
    }

    /* Extract device NCQ capability (word 76 bit 8, word 75 bits 4:0) */
    if (ahci_ncq_capable && (ident_buf[76] & (1U << 8))) {
        uint8_t dev_depth = (uint8_t)(ident_buf[75] & 0x1F) + 1;
        p->ncq_supported = 1;
        p->ncq_depth = (dev_depth < ahci_max_cmd_slots)
                     ? dev_depth : ahci_max_cmd_slots;
        klog(LOG_INFO, "ahci", "Port %u: NCQ supported (depth %u)",
               (uint64_t)p->port_num, (uint64_t)p->ncq_depth);
    } else {
        p->ncq_supported = 0;
        p->ncq_depth = 1;
        klog(LOG_DEBUG, "ahci", "Port %u: NCQ not supported",
               (uint64_t)p->port_num);
    }

    /* Extract TRIM support (word 169 bit 0) */
    if (ident_buf[169] & (1U << 0)) {
        p->trim_supported = 1;
        p->trim_deterministic = (ident_buf[69] & (1U << 14)) ? 1 : 0;
        klog(LOG_INFO, "ahci", "Port %u: TRIM supported%s",
               (uint64_t)p->port_num,
               p->trim_deterministic ? " (deterministic read)" : "");
    } else {
        p->trim_supported = 0;
        p->trim_deterministic = 0;
        klog(LOG_DEBUG, "ahci", "Port %u: TRIM not supported",
               (uint64_t)p->port_num);
    }

    /* Extract FUA support (word 86 bit 6) and write cache status (word 85 bit 5) */
    p->write_cache_enabled = (ident_buf[85] & (1U << 5)) ? 1 : 0;
    if (ident_buf[86] & (1U << 6)) {
        p->fua_supported = 1;
        klog(LOG_INFO, "ahci", "Port %u: FUA supported, write cache %s",
               (uint64_t)p->port_num,
               p->write_cache_enabled ? "enabled" : "disabled");
    } else {
        p->fua_supported = 0;
        klog(LOG_DEBUG, "ahci", "Port %u: FUA not supported (flush fallback)",
               (uint64_t)p->port_num);
    }

    /* ---- Sector size detection (Advanced Format / 4Kn) ----
     * Word 106: Physical/Logical Sector Size
     *   Bit 14 must be 1, bit 15 must be 0 for word to be valid
     *   Bit 12: logical sector size > 256 words (i.e., > 512 bytes)
     *   Bits 3:0: 2^N logical sectors per physical sector
     * Words 117-118: logical sector size in words (if word 106 bit 12 set)
     * Word 209: logical-to-physical alignment offset */
    {
        uint16_t w106 = ident_buf[106];
        uint32_t log_sector  = 512;  /* default */
        uint32_t phys_sector = 512;  /* default */

        if ((w106 & (1U << 14)) && !(w106 & (1U << 15))) {
            /* Word 106 is valid */

            /* Logical sector size from words 117-118 */
            if (w106 & (1U << 12)) {
                log_sector = ((uint32_t)ident_buf[117]
                           | ((uint32_t)ident_buf[118] << 16)) * 2;
                if (log_sector < 512)
                    log_sector = 512;  /* sanity */
            }

            /* Physical sector size: 2^N logical sectors per physical */
            if (w106 & (1U << 13)) {
                uint32_t exp = w106 & 0x0F;
                phys_sector = log_sector * (1U << exp);
            } else {
                phys_sector = log_sector;
            }
        }

        p->sector_size = log_sector;
        p->physical_sector_size = phys_sector;

        /* Alignment offset (word 209) */
        {
            uint16_t w209 = ident_buf[209];
            if ((w209 & (1U << 14)) && !(w209 & (1U << 15))) {
                p->alignment_offset = w209 & 0x3FFF;
            } else {
                p->alignment_offset = 0;
            }
        }

        klog(LOG_INFO, "ahci",
               "Port %u: %u-byte logical, %u-byte physical sectors%s",
               (uint64_t)p->port_num,
               (uint64_t)log_sector, (uint64_t)phys_sector,
               (log_sector > 512) ? " (4Kn)" : "");

        if (p->alignment_offset != 0) {
            klog(LOG_DEBUG, "ahci",
                   "Port %u: alignment offset %u logical sectors",
                   (uint64_t)p->port_num,
                   (uint64_t)p->alignment_offset);
        }
    }

    return 0;
}

/* ---- DMA read/write ---- */
int ahci_do_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
               void *buffer, int is_write, int fua)
{
    int slot;
    struct ahci_cmd_header *hdr;
    struct ahci_cmd_tbl *tbl;
    struct fis_reg_h2d *fis;
    uint32_t byte_count = count * p->sector_size;

    slot = port_find_slot(p->regs);
    if (slot < 0) return -1;

    hdr = &p->cmdlist[slot];
    tbl = p->cmdtbl[slot];

    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = is_write ? (fua ? ATA_CMD_WRITE_DMA_FUA_EX
                                     : ATA_CMD_WRITE_DMA_EX)
                              : ATA_CMD_READ_DMA_EX;
    fis->device   = (1 << 6);

    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)buffer;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = byte_count - 1;

    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F);
    if (is_write)
        hdr->flags |= (1 << 6);
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    return port_issue_cmd(p, slot);
}

/* ---- Public API ---- */

int ahci_read(int port_idx, uint64_t lba, uint32_t count, void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (ports[port_idx].ncq_supported && use_events)
        return ncq_sync_rw(&ports[port_idx], lba, count, buffer, 0, 0);
    return ahci_do_rw(&ports[port_idx], lba, count, buffer, 0, 0);
}

int ahci_write(int port_idx, uint64_t lba, uint32_t count,
               const void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (ports[port_idx].ncq_supported && use_events)
        return ncq_sync_rw(&ports[port_idx], lba, count,
                           (void *)buffer, 1, 0);
    return ahci_do_rw(&ports[port_idx], lba, count, (void *)buffer, 1, 0);
}

/* ---- Force Unit Access write ---- */

int ahci_write_fua(int port_idx, uint64_t lba, uint32_t count,
                   const void *buffer)
{
    struct ahci_port *p;

    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;

    p = &ports[port_idx];

    if (p->fua_supported) {
        if (p->ncq_supported && use_events)
            return ncq_sync_rw(p, lba, count, (void *)buffer, 1, 1);
        return ahci_do_rw(p, lba, count, (void *)buffer, 1, 1);
    }

    {
        int ret = ahci_write(port_idx, lba, count, buffer);
        if (ret != 0)
            return ret;
        return ahci_flush(port_idx);
    }
}

/* ---- FLUSH CACHE EXT ---- */

int ahci_flush(int port_idx)
{
    struct ahci_port *p;
    int slot;
    struct ahci_cmd_header *hdr;
    struct ahci_cmd_tbl *tbl;
    struct fis_reg_h2d *fis;

    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;

    p = &ports[port_idx];

    if (!p->write_cache_enabled)
        return 0;

    slot = port_find_slot(p->regs);
    if (slot < 0)
        return -1;

    hdr = &p->cmdlist[slot];
    tbl = p->cmdtbl[slot];
    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = ATA_CMD_CACHE_FLUSH_EX;
    fis->device   = 0;

    hdr->flags = (sizeof(struct fis_reg_h2d) / 4) & 0x1F;
    hdr->prdtl = 0;
    hdr->prdbc = 0;

    return port_issue_cmd(p, slot);
}

/* ---- TRIM (DATA SET MANAGEMENT) ---- */

int ahci_trim(int port_idx, uint64_t lba, uint32_t count)
{
    struct ahci_port *p;
    int slot;
    struct ahci_cmd_header *hdr;
    struct ahci_cmd_tbl *tbl;
    struct fis_reg_h2d *fis;
    uint8_t *trim_buf;
    uint32_t entries, i;

    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;

    p = &ports[port_idx];

    if (!p->trim_supported) {
        klog(LOG_DEBUG, "ahci", "Port %u: TRIM not supported, ignoring",
               (uint64_t)p->port_num);
        return 0;
    }

    trim_buf = (uint8_t *)kmalloc(512);
    if (!trim_buf)
        return -1;
    ahci_memset(trim_buf, 0, 512);

    entries = 0;
    while (count > 0 && entries < 64) {
        uint32_t chunk = (count > 0xFFFF) ? 0xFFFF : count;
        uint32_t off = entries * 8;

        trim_buf[off + 0] = (uint8_t)(lba & 0xFF);
        trim_buf[off + 1] = (uint8_t)((lba >> 8) & 0xFF);
        trim_buf[off + 2] = (uint8_t)((lba >> 16) & 0xFF);
        trim_buf[off + 3] = (uint8_t)((lba >> 24) & 0xFF);
        trim_buf[off + 4] = (uint8_t)((lba >> 32) & 0xFF);
        trim_buf[off + 5] = (uint8_t)((lba >> 40) & 0xFF);
        trim_buf[off + 6] = (uint8_t)(chunk & 0xFF);
        trim_buf[off + 7] = (uint8_t)((chunk >> 8) & 0xFF);

        lba += chunk;
        count -= chunk;
        entries++;
    }

    slot = port_find_slot(p->regs);
    if (slot < 0) {
        kfree(trim_buf);
        return -1;
    }

    hdr = &p->cmdlist[slot];
    tbl = p->cmdtbl[slot];
    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = 0x06;
    fis->featurel = 0x01;
    fis->countl   = 1;
    fis->counth   = 0;
    fis->device   = 0;

    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)trim_buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = 512 - 1;

    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F) | (1 << 6);
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    i = (uint32_t)port_issue_cmd(p, slot);

    kfree(trim_buf);

    if (i != 0) {
        klog(LOG_WARN, "ahci", "Port %u: TRIM command failed",
               (uint64_t)p->port_num);
    }

    return (int)i;
}

uint64_t ahci_capacity(int port_idx)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return 0;
    return ports[port_idx].sectors;
}

int ahci_present(void)
{
    return initialized && (num_drives > 0 || num_atapi > 0);
}

int ahci_drive_count(void)
{
    return num_drives;
}

uint32_t ahci_sector_size(int port_idx)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return 512;
    return ports[port_idx].sector_size;
}
