/* ============================================================================
 * ahci_atapi.c — ATAPI (CD/DVD) device support
 * ============================================================================ */

#include "kernel/drivers/ahci_internal.h"

/* ---- ATAPI: Send a SCSI packet command ---- */
static int atapi_packet_cmd(struct ahci_port *p, const uint8_t cdb[12],
                            void *buffer, uint32_t buf_len)
{
    int slot;
    struct ahci_cmd_header *hdr;
    struct ahci_cmd_tbl *tbl;
    struct fis_reg_h2d *fis;
    int i;

    slot = port_find_slot(p->regs);
    if (slot < 0) return -1;

    hdr = &p->cmdlist[slot];
    tbl = p->cmdtbl[slot];

    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = ATA_CMD_PACKET;
    fis->featurel = 1;
    fis->lba1     = (uint8_t)(buf_len & 0xFF);
    fis->lba2     = (uint8_t)((buf_len >> 8) & 0xFF);
    fis->device   = 0;

    for (i = 0; i < 12; i++)
        tbl->acmd[i] = cdb[i];

    if (buffer && buf_len > 0) {
        tbl->prdt[0].dba  = (uint32_t)(uintptr_t)buffer;
        tbl->prdt[0].dbau = 0;
        tbl->prdt[0].dbc  = buf_len - 1;
        hdr->prdtl = 1;
    } else {
        hdr->prdtl = 0;
    }

    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F) | (1 << 5);
    hdr->prdbc = 0;

    return port_issue_cmd(p, slot);
}

/* ---- ATAPI: IDENTIFY PACKET DEVICE ---- */
int atapi_do_identify(struct ahci_port *p)
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
    fis->command  = ATA_CMD_IDENTIFY_PACKET;
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

    /* ---- Word 0: General Configuration ---- */
    {
        uint16_t w0 = ident_buf[0];

        /* Bits 15-14: protocol type (10b = ATAPI) */
        if (((w0 >> 14) & 0x3) != 0x2) {
            klog(LOG_WARN, "ahci",
                 "Port %u: IDENTIFY PACKET w0=0x%x — not ATAPI protocol",
                 (uint64_t)p->port_num, (uint64_t)w0);
        }

        /* Bits 12-8: SCSI peripheral device type */
        p->atapi_scsi_type = (uint8_t)((w0 >> 8) & 0x1F);

        /* Bits 6-5: DRQ timing (00=slow 3ms, 01=IRQ within 50µs, 10=accelerated 50µs) */
        p->atapi_drq_type = (uint8_t)((w0 >> 5) & 0x3);

        /* Bits 1-0: command packet size (00=12 bytes, 01=16 bytes) */
        p->atapi_packet_size = (w0 & 0x3) == 0x01 ? 16 : 12;
    }

    /* ---- Words 10-19: Serial number (20 chars, byte-swapped) ---- */
    for (i = 0; i < 10; i++) {
        p->serial[i * 2]     = (char)(ident_buf[10 + i] >> 8);
        p->serial[i * 2 + 1] = (char)(ident_buf[10 + i] & 0xFF);
    }
    p->serial[20] = '\0';
    for (i = 19; i > 0; i--) {
        if (p->serial[i] == ' ') p->serial[i] = '\0';
        else break;
    }

    /* ---- Words 23-26: Firmware revision (8 chars, byte-swapped) ---- */
    for (i = 0; i < 4; i++) {
        p->firmware[i * 2]     = (char)(ident_buf[23 + i] >> 8);
        p->firmware[i * 2 + 1] = (char)(ident_buf[23 + i] & 0xFF);
    }
    p->firmware[8] = '\0';
    for (i = 7; i > 0; i--) {
        if (p->firmware[i] == ' ') p->firmware[i] = '\0';
        else break;
    }

    /* ---- Words 27-46: Model number (40 chars, byte-swapped) ---- */
    for (i = 0; i < 20; i++) {
        p->model[i * 2]     = (char)(ident_buf[27 + i] >> 8);
        p->model[i * 2 + 1] = (char)(ident_buf[27 + i] & 0xFF);
    }
    p->model[40] = '\0';
    for (i = 39; i > 0; i--) {
        if (p->model[i] == ' ') p->model[i] = '\0';
        else break;
    }

    /* ---- Word 49: Capabilities ---- */
    /* bit 8 = LBA supported, bit 11 = IORDY supported (informational) */

    /* ---- Word 63: Multiword DMA modes ---- */
    {
        uint16_t w63 = ident_buf[63];
        uint8_t supported = (uint8_t)(w63 & 0x07);

        p->atapi_dma_mode = 0;
        if (supported & 0x04)      p->atapi_dma_mode = 2;
        else if (supported & 0x02) p->atapi_dma_mode = 1;
        else if (supported & 0x01) p->atapi_dma_mode = 0;
    }

    /* ---- Word 88: Ultra DMA modes ---- */
    {
        uint16_t w88 = ident_buf[88];
        uint8_t supported = (uint8_t)(w88 & 0x7F);

        p->atapi_udma_mode = 0xFF;  /* None */
        if (supported) {
            /* Find highest set bit (mode 6 → UDMA/133, down to 0 → UDMA/16) */
            int m;
            for (m = 6; m >= 0; m--) {
                if (supported & (1U << m)) {
                    p->atapi_udma_mode = (uint8_t)m;
                    break;
                }
            }
        }
    }

    /* ---- Word 76: SATA capabilities ---- */
    {
        uint16_t w76 = ident_buf[76];
        /* 0x0000 and 0xFFFF mean "not reported" per ACS spec */
        p->atapi_sata_caps = (w76 == 0x0000 || w76 == 0xFFFF) ? 0 : w76;
    }

    p->sectors = 0;  /* Set later by READ CAPACITY */

    /* ---- Log device summary ---- */
    {
        const char *type_name;
        const char *dma_name;

        switch (p->atapi_scsi_type) {
        case 0x05: type_name = "CD/DVD-ROM";     break;
        case 0x00: type_name = "direct-access";  break;
        case 0x01: type_name = "tape";           break;
        case 0x07: type_name = "optical-memory"; break;
        default:   type_name = "unknown";        break;
        }

        if (p->atapi_udma_mode != 0xFF)
            dma_name = "UDMA";
        else if (p->atapi_dma_mode > 0)
            dma_name = "MDMA";
        else
            dma_name = "PIO";

        klog(LOG_INFO, "ahci",
             "ATAPI port %u: \"%s\" type=%s, pkt=%u, DMA=%s",
             (uint64_t)p->port_num, p->model,
             type_name, (uint64_t)p->atapi_packet_size, dma_name);
    }

    return 0;
}

/* ---- ATAPI: READ CAPACITY (10) ---- */
int atapi_read_capacity(struct ahci_port *p)
{
    uint8_t cdb[12];
    uint32_t *resp;
    uint32_t last_lba, block_size;

    resp = (uint32_t *)kmalloc(8);
    if (!resp) return -1;
    ahci_memset(resp, 0, 8);
    ahci_memset(cdb, 0, 12);

    cdb[0] = SCSI_READ_CAPACITY;

    if (atapi_packet_cmd(p, cdb, resp, 8) != 0) {
        return -1;
    }

    {
        uint8_t *r = (uint8_t *)resp;
        last_lba   = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16)
                   | ((uint32_t)r[2] << 8)  | (uint32_t)r[3];
        block_size = ((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16)
                   | ((uint32_t)r[6] << 8)  | (uint32_t)r[7];
    }

    p->sectors     = (uint64_t)last_lba + 1;
    p->sector_size = block_size ? block_size : 2048;

    return 0;
}

/* ---- ATAPI: READ (10) ---- */
int atapi_do_read(struct ahci_port *p, uint64_t lba, uint32_t count,
                  void *buffer)
{
    uint8_t cdb[12];
    uint32_t byte_count = count * p->sector_size;
    uint32_t lba32 = (uint32_t)lba;

    ahci_memset(cdb, 0, 12);
    cdb[0] = SCSI_READ_10;
    cdb[2] = (uint8_t)((lba32 >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba32 >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba32 >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba32 & 0xFF);
    cdb[7] = (uint8_t)((count >> 8) & 0xFF);
    cdb[8] = (uint8_t)(count & 0xFF);

    return atapi_packet_cmd(p, cdb, buffer, byte_count);
}

/* ---- ATAPI public API ---- */

int ahci_atapi_count(void)
{
    return num_atapi;
}

int ahci_atapi_read(int atapi_idx, uint64_t lba, uint32_t count, void *buffer)
{
    int pi;
    if (atapi_idx < 0 || atapi_idx >= num_atapi)
        return -1;
    pi = atapi_map[atapi_idx];
    if (!ports[pi].active || !ports[pi].is_atapi)
        return -1;
    return atapi_do_read(&ports[pi], lba, count, buffer);
}

uint64_t ahci_atapi_capacity(int atapi_idx)
{
    int pi;
    if (atapi_idx < 0 || atapi_idx >= num_atapi)
        return 0;
    pi = atapi_map[atapi_idx];
    return ports[pi].sectors;
}

uint32_t ahci_atapi_sector_size(int atapi_idx)
{
    int pi;
    if (atapi_idx < 0 || atapi_idx >= num_atapi)
        return 0;
    pi = atapi_map[atapi_idx];
    return ports[pi].sector_size;
}
