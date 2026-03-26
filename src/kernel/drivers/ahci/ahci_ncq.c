/* ============================================================================
 * ahci_ncq.c — NCQ (Native Command Queuing) tag management and I/O
 * ============================================================================ */

#include "kernel/drivers/ahci_internal.h"
#include "kernel/sched/event.h"

/* ---- NCQ tag allocator (bitmap) ---- */

int ncq_alloc_tag(struct ahci_port *p)
{
    uint32_t mask = (p->ncq_depth >= 32) ? 0xFFFFFFFF
                  : ((1U << p->ncq_depth) - 1);
    uint32_t free = ~p->tags_allocated & mask;
    int i;
    if (!free) return -1;
    for (i = 0; i < 32; i++) {
        if (free & (1U << i)) {
            p->tags_allocated |= (1U << i);
            return i;
        }
    }
    return -1;
}

void ncq_free_tag(struct ahci_port *p, int tag)
{
    p->tags_allocated &= ~(1U << tag);
    p->tag_callbacks[tag] = (ahci_callback_t)0;
    p->tag_cb_ctx[tag] = (void *)0;
}

/* ---- Issue an NCQ (FPDMA) command ---- */

int ncq_issue_rw(struct ahci_port *p, int tag, uint64_t lba,
                 uint32_t count, void *buffer, int is_write, int fua)
{
    struct ahci_cmd_header *hdr = &p->cmdlist[tag];
    struct ahci_cmd_tbl *tbl = p->cmdtbl[tag];
    struct fis_reg_h2d *fis;
    uint32_t byte_count = count * p->sector_size;

    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = is_write ? ATA_CMD_WRITE_FPDMA : ATA_CMD_READ_FPDMA;

    fis->featurel = (uint8_t)(count & 0xFF);
    fis->featureh = (uint8_t)((count >> 8) & 0xFF);

    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

    fis->device = (1 << 6);
    if (fua && is_write)
        fis->device |= (1 << 7);

    fis->countl = (uint8_t)(tag << 3);
    fis->counth = 0;

    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)buffer;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = byte_count - 1;

    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F);
    if (is_write)
        hdr->flags |= (1 << 6);
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    p->tag_status[tag] = 0;

    port_write(p->regs, AHCI_PxSACT, 1U << tag);
    p->tags_pending |= (1U << tag);
    port_write(p->regs, AHCI_PxCI, 1U << tag);

    return 0;
}

/* ---- NCQ synchronous read/write with retry ---- */

int ncq_sync_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
                void *buffer, int is_write, int fua)
{
    int retries = 3;
    int tag, status;
    uint32_t tfd;

    while (retries > 0) {
        tag = ncq_alloc_tag(p);
        if (tag < 0)
            return ahci_do_rw(p, lba, count, buffer, is_write, fua);

        ncq_issue_rw(p, tag, lba, count, buffer, is_write, fua);

        {
            int timeout = 50;
            while (!(p->tags_completed & (1U << tag)) && timeout > 0) {
                event_wait_timeout(&p->completion, 100);
                timeout--;
            }
            if (!(p->tags_completed & (1U << tag))) {
                p->tag_status[tag] = -1;
                p->tags_completed |= (1U << tag);
                p->tags_pending &= ~(1U << tag);
                klog(LOG_WARN, "ahci", "Port %u: NCQ tag %u timeout",
                       (uint64_t)p->port_num, (uint64_t)tag);
            }
        }

        p->tags_completed &= ~(1U << tag);
        status = p->tag_status[tag];
        ncq_free_tag(p, tag);

        if (status == 0)
            return 0;

        retries--;
        tfd = port_read(p->regs, AHCI_PxTFD);
        if (tfd & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ)) {
            if (port_clo_reset(p) != 0)
                break;
        }
        klog(LOG_DEBUG, "ahci", "Port %u: NCQ retry (%d left)",
               (uint64_t)p->port_num, (uint64_t)retries);
    }

    p->errors.cmd_failures++;

    /* NCQ is repeatedly timing out on this port (common on VirtualBox where
     * AHCI INTx is not delivered reliably for FPDMA commands).
     * Disable NCQ permanently so that ahci_ncq_read/write fall back to
     * ahci_do_rw, which detects the first IRQ timeout, clears use_events,
     * and switches to polling -- recovering cleanly without further hangs. */
    if (p->errors.cmd_failures >= 3) {
        p->ncq_supported = 0;
        klog(LOG_WARN, "ahci",
               "Port %u: NCQ disabled after repeated failures -- "
               "falling back to legacy ATA DMA",
               (uint64_t)p->port_num);
    }

    return -1;
}

/* ---- Public NCQ API ---- */

int ahci_ncq_read(int port_idx, uint64_t lba, uint32_t count, void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (ports[port_idx].device_type == AHCI_DEV_ATAPI)
        return -1;
    if (!ports[port_idx].ncq_supported)
        return ahci_do_rw(&ports[port_idx], lba, count, buffer, 0, 0);
    return ncq_sync_rw(&ports[port_idx], lba, count, buffer, 0, 0);
}

int ahci_ncq_write(int port_idx, uint64_t lba, uint32_t count,
                   const void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (ports[port_idx].device_type == AHCI_DEV_ATAPI)
        return -1;
    if (!ports[port_idx].ncq_supported)
        return ahci_do_rw(&ports[port_idx], lba, count, (void *)buffer, 1, 0);
    return ncq_sync_rw(&ports[port_idx], lba, count, (void *)buffer, 1, 0);
}

int ahci_submit(int port_idx, uint64_t lba, uint32_t count, void *buffer,
                int is_write, ahci_callback_t callback, void *ctx)
{
    struct ahci_port *p;
    int tag;

    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    p = &ports[port_idx];
    if (!p->ncq_supported || !use_events)
        return -1;

    tag = ncq_alloc_tag(p);
    if (tag < 0)
        return -1;

    p->tag_callbacks[tag] = callback;
    p->tag_cb_ctx[tag] = ctx;

    ncq_issue_rw(p, tag, lba, count, buffer, is_write, 0);
    return tag;
}
