/* ============================================================================
 * ahci.c — AHCI (SATA) Disk Driver
 *
 * MMIO-based AHCI driver using DMA for SATA disk access.
 * Detected via PCI class 0x01 / subclass 0x06.
 * Uses BAR5 (ABAR) for HBA register access.
 *
 * Init sequence:
 *   1. PCI scan for AHCI controller
 *   2. Map ABAR into kernel page tables
 *   3. Enable AHCI mode (GHC.AE), reset if needed
 *   4. Enumerate ports from PI register
 *   5. For each active port: allocate CLB + FB + command tables
 *   6. Issue IDENTIFY DEVICE to get drive info
 *
 * I/O uses command slot 0 with a single PRDT entry per operation.
 * ============================================================================ */

#include "kernel/drivers/ahci.h"
#include "registry.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/irq.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/vmm.h"
#include "kernel/sched/event.h"
#include "kernel/klog.h"
#include "kernel/printk.h"

/* ---- MMIO helpers (identity-mapped) ---- */
static inline uint32_t ahci_read32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void ahci_write32(volatile uint8_t *base, uint32_t off,
                                 uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

/* ---- memset / memcpy helpers ---- */
static void ahci_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = val;
}

/* ---- Driver state ---- */
static volatile uint8_t *abar;         /* HBA base address (MMIO) */
static struct ahci_port  ports[AHCI_MAX_PORTS];
static int               num_drives;   /* Detected SATA drives */
static int               atapi_map[AHCI_MAX_PORTS]; /* Index into ports[] for ATAPI */
static int               num_atapi;    /* Detected ATAPI devices */
static int               initialized;
static int               num_ports_total; /* Total active ports (SATA + ATAPI) */

/* ---- Interrupt state ---- */
static uint8_t           ahci_pci_bus;  /* PCI bus of AHCI controller */
static uint8_t           ahci_pci_slot; /* PCI slot of AHCI controller */
static uint8_t           ahci_pci_func; /* PCI function of AHCI controller */
static uint8_t           ahci_irq_vector; /* Allocated IDT vector */
static int               use_events;   /* 1 after IRQ setup, 0 during boot */
static int               ahci_clo_supported; /* 1 if CAP.SCLO is set */
static int               ahci_ncq_capable;   /* 1 if CAP.SNCQ is set */
static uint8_t           ahci_max_cmd_slots; /* CAP.NCS + 1 (1–32) */

/* ---- Map MMIO region ---- */
static void ahci_map_mmio(uint64_t phys, uint32_t size)
{
    uint64_t page_start = phys & ~(uint64_t)0xFFF;
    uint64_t page_end   = (phys + size + 0xFFF) & ~(uint64_t)0xFFF;
    uint64_t page;
    uint64_t flags = VMM_KERNEL_RW | VMM_FLAG_NOCACHE | VMM_FLAG_WRITETHROUGH;

    for (page = page_start; page < page_end; page += VMM_PAGE_SIZE) {
        if (vmm_get_physical(page) == 0)
            vmm_map_page(page, page, flags);
    }
}

/* ---- Port register access ---- */
static inline volatile uint8_t *port_regs(int port_num)
{
    return abar + AHCI_PORT_BASE + (uint32_t)port_num * AHCI_PORT_SIZE;
}

static inline uint32_t port_read(volatile uint8_t *pregs, uint32_t off)
{
    return ahci_read32(pregs, off);
}

static inline void port_write(volatile uint8_t *pregs, uint32_t off,
                               uint32_t val)
{
    ahci_write32(pregs, off, val);
}

/* ---- Stop command engine ---- */
static void port_stop_cmd(volatile uint8_t *pregs)
{
    uint32_t cmd = port_read(pregs, AHCI_PxCMD);
    uint32_t timeout;

    /* Clear ST (stop command list processing) */
    cmd &= ~AHCI_PxCMD_ST;
    port_write(pregs, AHCI_PxCMD, cmd);

    /* Wait for CR to clear */
    timeout = 500000;
    while (timeout--) {
        if (!(port_read(pregs, AHCI_PxCMD) & AHCI_PxCMD_CR))
            break;
        /* No PAUSE — triggers Hyper-V PLE (~100ms stall per iter) */
    }

    /* Clear FRE (stop FIS receive) */
    cmd = port_read(pregs, AHCI_PxCMD);
    cmd &= ~AHCI_PxCMD_FRE;
    port_write(pregs, AHCI_PxCMD, cmd);

    /* Wait for FR to clear */
    timeout = 500000;
    while (timeout--) {
        if (!(port_read(pregs, AHCI_PxCMD) & AHCI_PxCMD_FR))
            break;
    }
}

/* ---- Start command engine ---- */
static void port_start_cmd(volatile uint8_t *pregs)
{
    uint32_t cmd;
    uint32_t timeout = 500000;

    /* Wait for CR to clear before starting (with timeout) */
    while ((port_read(pregs, AHCI_PxCMD) & AHCI_PxCMD_CR) && timeout--)
        ;  /* MMIO read is yield point; no PAUSE (Hyper-V PLE) */

    cmd = port_read(pregs, AHCI_PxCMD);
    cmd |= AHCI_PxCMD_FRE;
    port_write(pregs, AHCI_PxCMD, cmd);

    cmd |= AHCI_PxCMD_ST;
    port_write(pregs, AHCI_PxCMD, cmd);
}

/* ---- Find a free command slot ---- */
static int port_find_slot(volatile uint8_t *pregs)
{
    uint32_t slots = port_read(pregs, AHCI_PxSACT) |
                     port_read(pregs, AHCI_PxCI);
    int i;
    for (i = 0; i < 32; i++) {
        if (!(slots & (1U << i)))
            return i;
    }
    return -1;
}

/* ---- Initialize a single port ---- */
static int port_init(struct ahci_port *p, int port_num)
{
    volatile uint8_t *pregs = port_regs(port_num);
    uint32_t ssts;
    uint8_t *clb_mem;
    uint8_t *fb_mem;
    int i;

    p->port_num = (uint8_t)port_num;
    p->active = 0;
    p->regs = pregs;

    /* Check if device is present and PHY communication established */
    ssts = port_read(pregs, AHCI_PxSSTS);
    if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_OK)
        return -1;  /* No device */
    if ((ssts & AHCI_SSTS_IPM_MASK) != AHCI_SSTS_IPM_ACTIVE)
        return -1;  /* Not active */

    /* Read signature */
    p->sig = port_read(pregs, AHCI_PxSIG);

    /* Accept both SATA drives and ATAPI (optical) devices */
    if (p->sig == AHCI_SIG_ATAPI) {
        p->is_atapi = 1;
        p->sector_size = 2048;  /* Optical media default */
    } else if (p->sig == AHCI_SIG_ATA) {
        p->is_atapi = 0;
        p->sector_size = 512;
    } else {
        return -1;  /* Unknown device type */
    }

    /* Stop command engine before reconfiguring */
    port_stop_cmd(pregs);

    /* Allocate command list (1 KiB, 1024-byte aligned) */
    clb_mem = (uint8_t *)kmalloc(1024 + 1024);
    if (!clb_mem) return -1;
    clb_mem = (uint8_t *)(((uintptr_t)clb_mem + 1023) & ~(uintptr_t)1023);
    ahci_memset(clb_mem, 0, 1024);
    p->cmdlist = (struct ahci_cmd_header *)clb_mem;

    /* Set CLB register */
    port_write(pregs, AHCI_PxCLB, (uint32_t)(uintptr_t)clb_mem);
    port_write(pregs, AHCI_PxCLBU, 0);

    /* Allocate FIS receive buffer (256 bytes, 256-byte aligned) */
    fb_mem = (uint8_t *)kmalloc(256 + 256);
    if (!fb_mem) return -1;
    fb_mem = (uint8_t *)(((uintptr_t)fb_mem + 255) & ~(uintptr_t)255);
    ahci_memset(fb_mem, 0, 256);
    p->fis_base = fb_mem;

    /* Set FB register */
    port_write(pregs, AHCI_PxFB, (uint32_t)(uintptr_t)fb_mem);
    port_write(pregs, AHCI_PxFBU, 0);

    /* Allocate command tables (one per command slot, 128-byte aligned) */
    for (i = 0; i < 32; i++) {
        uint32_t tbl_size = (uint32_t)sizeof(struct ahci_cmd_tbl);
        uint8_t *tbl_mem = (uint8_t *)kmalloc(tbl_size + 128);
        if (!tbl_mem) return -1;
        tbl_mem = (uint8_t *)(((uintptr_t)tbl_mem + 127) & ~(uintptr_t)127);
        ahci_memset(tbl_mem, 0, tbl_size);
        p->cmdtbl[i] = (struct ahci_cmd_tbl *)tbl_mem;

        /* Point command header to its command table */
        p->cmdlist[i].ctba  = (uint32_t)(uintptr_t)tbl_mem;
        p->cmdlist[i].ctbau = 0;
    }

    /* Clear error register */
    port_write(pregs, AHCI_PxSERR, 0xFFFFFFFF);

    /* Clear interrupt status */
    port_write(pregs, AHCI_PxIS, 0xFFFFFFFF);

    /* Enable per-port interrupts for completion and error events.
     * The ISR is registered later in ahci_init(); during early port init
     * we use polling (use_events == 0) so these bits are harmless until
     * GHC.IE is enabled globally. */
    port_write(pregs, AHCI_PxIE,
               AHCI_PxIS_DHRS | AHCI_PxIS_PSS | AHCI_PxIS_DSS |
               AHCI_PxIS_SDBS | AHCI_PxIS_TFES |
               AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS |
               AHCI_PxIS_INFS | AHCI_PxIS_OFS);

    /* Initialize per-port completion event (auto-reset, initially unsignalled) */
    event_init(&p->completion, "ahci_port", EVENT_AUTO_RESET, 0);

    /* Start command engine */
    port_start_cmd(pregs);

    p->active = 1;
    return 0;
}

/* ---- AHCI interrupt service routine ---- */
static void ahci_irq_handler(uint8_t vector, void *ctx)
{
    uint32_t is;
    int i;

    (void)vector;
    (void)ctx;

    /* Read global interrupt status — one bit per port */
    is = ahci_read32(abar, AHCI_IS);
    if (!is)
        return;  /* Spurious */

    for (i = 0; i < num_ports_total; i++) {
        if (!ports[i].active)
            continue;
        if (!(is & (1U << ports[i].port_num)))
            continue;

        /* Read and clear per-port interrupt status (write-1-to-clear).
         * Must clear PxIS BEFORE clearing global IS — AHCI uses
         * level-triggered interrupts; if PxIS still has bits set when
         * IS is cleared, the HBA re-asserts the interrupt line. */
        uint32_t pxis = port_read(ports[i].regs, AHCI_PxIS);
        port_write(ports[i].regs, AHCI_PxIS, pxis);

        /* ---- Error classification ---- */
        if (pxis & AHCI_PxIS_FATAL) {
            ports[i].errors.fatal_errors++;

            /* Read PxSERR for detailed diagnostics */
            uint32_t serr = port_read(ports[i].regs, AHCI_PxSERR);
            port_write(ports[i].regs, AHCI_PxSERR, serr);

            if (serr & AHCI_PxSERR_ERR_M)
                ports[i].errors.crc_errors++;

            klog(LOG_WARN, "ahci",
                   "Port %u: fatal error PxIS=0x%x PxSERR=0x%x",
                   (uint64_t)ports[i].port_num,
                   (uint64_t)pxis, (uint64_t)serr);

            /* NCQ error: mark ALL pending tags as failed.
             * Proper per-tag recovery via NCQ Error Log deferred to §2.4. */
            if (ports[i].ncq_supported && ports[i].tags_pending) {
                uint32_t pending = ports[i].tags_pending;
                int t;
                for (t = 0; t < 32; t++) {
                    if (pending & (1U << t)) {
                        ports[i].tag_status[t] = -1;
                        ports[i].tags_completed |= (1U << t);
                        if (ports[i].tag_callbacks[t]) {
                            ports[i].tag_callbacks[t](
                                i, t, -1, ports[i].tag_cb_ctx[t]);
                            ports[i].tag_callbacks[t] =
                                (ahci_callback_t)0;
                        }
                    }
                }
                ports[i].tags_pending = 0;
            }
        }

        if (pxis & AHCI_PxIS_NONFATAL) {
            ports[i].errors.nonfatal_errors++;
            klog(LOG_DEBUG, "ahci",
                   "Port %u: non-fatal error PxIS=0x%x",
                   (uint64_t)ports[i].port_num, (uint64_t)pxis);
        }

        /* ---- NCQ completion via Set Device Bits FIS ---- */
        if ((pxis & AHCI_PxIS_SDBS) && ports[i].ncq_supported) {
            uint32_t sact = port_read(ports[i].regs, AHCI_PxSACT);
            uint32_t completed = ports[i].tags_pending & ~sact;
            int t;

            for (t = 0; t < 32; t++) {
                if (!(completed & (1U << t)))
                    continue;
                ports[i].tags_completed |= (1U << t);
                /* Call async callback if registered */
                if (ports[i].tag_callbacks[t]) {
                    ports[i].tag_callbacks[t](
                        i, t, ports[i].tag_status[t],
                        ports[i].tag_cb_ctx[t]);
                    ports[i].tag_callbacks[t] = (ahci_callback_t)0;
                }
            }
            ports[i].tags_pending &= ~completed;
        }

        /* Wake the thread waiting on this port */
        event_set(&ports[i].completion);
    }

    /* Clear global IS (write-1-to-clear) */
    ahci_write32(abar, AHCI_IS, is);
}

/* ---- CLO + COMRESET recovery for stuck BSY/DRQ ---- */
static int port_clo_reset(struct ahci_port *p)
{
    volatile uint8_t *pregs = p->regs;
    uint32_t cmd, wait;

    klog(LOG_WARN, "ahci", "Port %u: CLO recovery — BSY/DRQ stuck, link reset",
           (uint64_t)p->port_num);

    /* 1. Stop command engine: PxCMD.ST = 0, wait for PxCMD.CR = 0 */
    port_stop_cmd(pregs);

    /* 2. If HBA supports CLO, use it to clear BSY/DRQ forcefully */
    if (ahci_clo_supported) {
        cmd = port_read(pregs, AHCI_PxCMD);
        cmd |= (1U << 3);  /* PxCMD.CLO */
        port_write(pregs, AHCI_PxCMD, cmd);

        /* Wait for CLO to auto-clear (HBA clears it when done) */
        wait = 100000;
        while ((port_read(pregs, AHCI_PxCMD) & (1U << 3)) && wait--)
            ;
    }

    /* 3. Issue COMRESET: write PxSCTL.DET = 1 to initiate OOB */
    {
        uint32_t sctl = port_read(pregs, AHCI_PxSCTL);
        sctl = (sctl & ~0xFU) | 0x1;  /* DET = 1 (perform interface reset) */
        port_write(pregs, AHCI_PxSCTL, sctl);
    }

    /* Wait ~1ms for COMRESET signaling */
    {
        volatile uint32_t delay = 100000;
        while (delay--)
            ;
    }

    /* 4. Clear DET to allow link re-establishment */
    {
        uint32_t sctl = port_read(pregs, AHCI_PxSCTL);
        sctl &= ~0xFU;  /* DET = 0 (no device detection action) */
        port_write(pregs, AHCI_PxSCTL, sctl);
    }

    /* 5. Wait for PxSSTS.DET = 3 (device present + PHY communication) */
    wait = 1000000;
    while (wait--) {
        uint32_t ssts = port_read(pregs, AHCI_PxSSTS);
        if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_OK)
            break;
    }
    if (wait == 0) {
        klog(LOG_WARN, "ahci", "Port %u: device not present after COMRESET",
               (uint64_t)p->port_num);
        return -1;
    }

    /* 6. Clear error bits */
    port_write(pregs, AHCI_PxSERR, 0xFFFFFFFF);
    port_write(pregs, AHCI_PxIS, 0xFFFFFFFF);

    /* 7. Wait for BSY to clear (device ready) */
    wait = 1000000;
    while (wait--) {
        uint32_t tfd = port_read(pregs, AHCI_PxTFD);
        if (!(tfd & AHCI_PxTFD_BSY))
            break;
    }

    /* 8. Restart command engine */
    port_start_cmd(pregs);

    klog(LOG_INFO, "ahci", "Port %u: CLO recovery complete",
           (uint64_t)p->port_num);
    p->errors.link_resets++;
    return 0;
}

/* ---- Issue a command and wait for completion (with retry) ---- */
static int port_issue_cmd(struct ahci_port *p, int slot)
{
    volatile uint8_t *pregs = p->regs;
    uint32_t tfd;
    int retries = 3;
    int result;

retry:
    result = 0;

    /* Issue command */
    port_write(pregs, AHCI_PxCI, 1U << slot);

    if (use_events) {
        /* ---- Interrupt-driven path ---- */
        int signalled = event_wait_timeout(&p->completion, 5000);
        if (!signalled) {
            klog(LOG_DEBUG, "ahci", "Port %u: command timeout (IRQ)",
                   (uint64_t)p->port_num);
            result = -1;
        }
    } else {
        /* ---- Polling fallback (pre-scheduler boot) ---- */
        uint32_t timeout = 5000000;
        while (timeout--) {
            uint32_t ci = port_read(pregs, AHCI_PxCI);
            if (!(ci & (1U << slot)))
                break;

            tfd = port_read(pregs, AHCI_PxTFD);
            if (tfd & AHCI_PxTFD_ERR) {
                klog(LOG_DEBUG, "ahci", "Port %u: TFD error 0x%x",
                       (uint64_t)p->port_num, (uint64_t)tfd);
                result = -1;
                break;
            }
        }
        if (timeout == 0 && result == 0) {
            klog(LOG_DEBUG, "ahci", "Port %u: command timeout (poll)",
                   (uint64_t)p->port_num);
            result = -1;
        }

        /* Clear interrupt status (polling path) */
        port_write(pregs, AHCI_PxIS, port_read(pregs, AHCI_PxIS));
    }

    /* Check final TFD for errors */
    tfd = port_read(pregs, AHCI_PxTFD);
    if (tfd & AHCI_PxTFD_ERR)
        result = -1;

    /* ---- CLO recovery + retry on failure ---- */
    if (result != 0 && retries > 0) {
        tfd = port_read(pregs, AHCI_PxTFD);
        if (tfd & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ)) {
            if (port_clo_reset(p) == 0) {
                retries--;
                klog(LOG_DEBUG, "ahci",
                       "Port %u: retrying command (%d retries left)",
                       (uint64_t)p->port_num, (uint64_t)retries);
                goto retry;
            }
        }
    }

    if (result != 0)
        p->errors.cmd_failures++;

    return result;
}

/* ---- NCQ tag allocator (bitmap) ---- */

static int ncq_alloc_tag(struct ahci_port *p)
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

static void ncq_free_tag(struct ahci_port *p, int tag)
{
    p->tags_allocated &= ~(1U << tag);
    p->tag_callbacks[tag] = (ahci_callback_t)0;
    p->tag_cb_ctx[tag] = (void *)0;
}

/* ---- Issue an NCQ (FPDMA) command ---- */

static int ncq_issue_rw(struct ahci_port *p, int tag, uint64_t lba,
                        uint32_t count, void *buffer, int is_write)
{
    /* Tag == command slot for FPDMA (1:1 mapping per AHCI spec) */
    struct ahci_cmd_header *hdr = &p->cmdlist[tag];
    struct ahci_cmd_tbl *tbl = p->cmdtbl[tag];
    struct fis_reg_h2d *fis;
    uint32_t byte_count = count * 512;

    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    /* Build FPDMA FIS — different from standard DMA:
     *   Sector count → Features register (NOT Count)
     *   Tag → Count register bits [7:3]
     *   Command: 0x60 (read) or 0x61 (write) */
    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;  /* C = 1 (command) */
    fis->command  = is_write ? ATA_CMD_WRITE_FPDMA : ATA_CMD_READ_FPDMA;

    /* Sector count in Features (FPDMA encoding) */
    fis->featurel = (uint8_t)(count & 0xFF);
    fis->featureh = (uint8_t)((count >> 8) & 0xFF);

    /* LBA48 */
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

    fis->device = (1 << 6); /* LBA mode */

    /* Tag in Count[7:3] — bits 2:0 reserved (must be 0) */
    fis->countl = (uint8_t)(tag << 3);
    fis->counth = 0;

    /* PRDT: one entry */
    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)buffer;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = byte_count - 1;

    /* Command header */
    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F);
    if (is_write)
        hdr->flags |= (1 << 6);  /* W bit */
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    /* Initialize tag status */
    p->tag_status[tag] = 0;

    /* CRITICAL: Set PxSACT BEFORE PxCI for NCQ commands.
     * If PxCI is set first, the HBA interprets this as standard DMA
     * and silently corrupts completion tracking (AHCI spec §3.3.14). */
    port_write(p->regs, AHCI_PxSACT, 1U << tag);
    p->tags_pending |= (1U << tag);
    port_write(p->regs, AHCI_PxCI, 1U << tag);

    return 0;
}

/* ---- IDENTIFY DEVICE ---- */
static int ahci_do_identify(struct ahci_port *p)
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

    /* Clear command table */
    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    /* Build FIS */
    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;  /* C bit = 1 (command) */
    fis->command  = ATA_CMD_IDENTIFY;
    fis->device   = 0;

    /* PRDT: one entry pointing to ident_buf */
    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)ident_buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = 512 - 1;  /* Byte count minus 1 */

    /* Command header */
    hdr->flags = (sizeof(struct fis_reg_h2d) / 4) & 0x1F; /* CFL in DWORDs */
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

    /* If LBA48 count is 0, fall back to LBA28 (words 60-61) */
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

    /* Trim trailing spaces */
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
        /* Deterministic read after TRIM (word 69 bit 14) */
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

    return 0;
}

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

    /* Build H2D FIS: ATA PACKET command */
    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;      /* C = 1 (command) */
    fis->command  = ATA_CMD_PACKET;
    fis->featurel = 1;         /* DMA bit in features (use DMA transfer) */
    fis->lba1     = (uint8_t)(buf_len & 0xFF);       /* Byte count low */
    fis->lba2     = (uint8_t)((buf_len >> 8) & 0xFF); /* Byte count high */
    fis->device   = 0;

    /* Copy 12-byte SCSI CDB into ATAPI command area */
    for (i = 0; i < 12; i++)
        tbl->acmd[i] = cdb[i];

    /* PRDT: buffer for response data */
    if (buffer && buf_len > 0) {
        tbl->prdt[0].dba  = (uint32_t)(uintptr_t)buffer;
        tbl->prdt[0].dbau = 0;
        tbl->prdt[0].dbc  = buf_len - 1;
        hdr->prdtl = 1;
    } else {
        hdr->prdtl = 0;
    }

    /* Command header: CFL=5 DWORDs, bit 5 = ATAPI */
    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F) | (1 << 5);
    hdr->prdbc = 0;

    return port_issue_cmd(p, slot);
}

/* ---- ATAPI: IDENTIFY PACKET DEVICE ---- */
static int atapi_do_identify(struct ahci_port *p)
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

    /* Build FIS: IDENTIFY PACKET DEVICE */
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

    /* Extract model string (words 27-46, byte-swapped) */
    for (i = 0; i < 20; i++) {
        p->model[i * 2]     = (char)(ident_buf[27 + i] >> 8);
        p->model[i * 2 + 1] = (char)(ident_buf[27 + i] & 0xFF);
    }
    p->model[40] = '\0';
    for (i = 39; i > 0; i--) {
        if (p->model[i] == ' ') p->model[i] = '\0';
        else break;
    }

    /* Extract serial number (words 10-19, byte-swapped) */
    for (i = 0; i < 10; i++) {
        p->serial[i * 2]     = (char)(ident_buf[10 + i] >> 8);
        p->serial[i * 2 + 1] = (char)(ident_buf[10 + i] & 0xFF);
    }
    p->serial[20] = '\0';
    for (i = 19; i > 0; i--) {
        if (p->serial[i] == ' ') p->serial[i] = '\0';
        else break;
    }

    /* Sector count is not in IDENTIFY for ATAPI — use READ CAPACITY later */
    p->sectors = 0;

    return 0;
}

/* ---- ATAPI: READ CAPACITY (10) ---- */
static int atapi_read_capacity(struct ahci_port *p)
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

    /* Response is big-endian: [0]=last LBA, [1]=block size */
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
static int atapi_do_read(struct ahci_port *p, uint64_t lba, uint32_t count,
                         void *buffer)
{
    uint8_t cdb[12];
    uint32_t byte_count = count * p->sector_size;
    uint32_t lba32 = (uint32_t)lba;

    ahci_memset(cdb, 0, 12);
    cdb[0] = SCSI_READ_10;
    /* LBA (big-endian) */
    cdb[2] = (uint8_t)((lba32 >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba32 >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba32 >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba32 & 0xFF);
    /* Transfer length in sectors (big-endian) */
    cdb[7] = (uint8_t)((count >> 8) & 0xFF);
    cdb[8] = (uint8_t)(count & 0xFF);

    return atapi_packet_cmd(p, cdb, buffer, byte_count);
}

/* ---- DMA read/write ---- */
static int ahci_do_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
                       void *buffer, int is_write)
{
    int slot;
    struct ahci_cmd_header *hdr;
    struct ahci_cmd_tbl *tbl;
    struct fis_reg_h2d *fis;
    uint32_t byte_count = count * 512;

    slot = port_find_slot(p->regs);
    if (slot < 0) return -1;

    hdr = &p->cmdlist[slot];
    tbl = p->cmdtbl[slot];

    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    /* Build FIS */
    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;  /* C = 1 (command) */
    fis->command  = is_write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;
    fis->device   = (1 << 6); /* LBA mode */

    /* LBA48 */
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

    /* Sector count */
    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    /* PRDT: one entry */
    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)buffer;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = byte_count - 1;  /* Byte count minus 1 */

    /* Command header */
    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F);
    if (is_write)
        hdr->flags |= (1 << 6);  /* W bit for writes */
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    return port_issue_cmd(p, slot);
}

/* ---- Public API ---- */

static int ncq_sync_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
                       void *buffer, int is_write);

int ahci_read(int port_idx, uint64_t lba, uint32_t count, void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (ports[port_idx].ncq_supported && use_events)
        return ncq_sync_rw(&ports[port_idx], lba, count, buffer, 0);
    return ahci_do_rw(&ports[port_idx], lba, count, buffer, 0);
}

int ahci_write(int port_idx, uint64_t lba, uint32_t count,
               const void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (ports[port_idx].ncq_supported && use_events)
        return ncq_sync_rw(&ports[port_idx], lba, count,
                           (void *)buffer, 1);
    return ahci_do_rw(&ports[port_idx], lba, count, (void *)buffer, 1);
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
        return 0;  /* Not an error — just a no-op on non-SSD */
    }

    /* Allocate and zero a 512-byte TRIM range descriptor buffer.
     * Each entry is 8 bytes: LBA (6 bytes, little-endian) + count (2 bytes).
     * Max 64 entries per 512-byte sector. */
    trim_buf = (uint8_t *)kmalloc(512);
    if (!trim_buf)
        return -1;
    ahci_memset(trim_buf, 0, 512);

    /* Split the range into 64-entry chunks if needed.
     * For now, a single TRIM command handles one contiguous range. */
    entries = 0;
    while (count > 0 && entries < 64) {
        uint32_t chunk = (count > 0xFFFF) ? 0xFFFF : count;
        uint32_t off = entries * 8;

        /* LBA: 6 bytes little-endian */
        trim_buf[off + 0] = (uint8_t)(lba & 0xFF);
        trim_buf[off + 1] = (uint8_t)((lba >> 8) & 0xFF);
        trim_buf[off + 2] = (uint8_t)((lba >> 16) & 0xFF);
        trim_buf[off + 3] = (uint8_t)((lba >> 24) & 0xFF);
        trim_buf[off + 4] = (uint8_t)((lba >> 32) & 0xFF);
        trim_buf[off + 5] = (uint8_t)((lba >> 40) & 0xFF);
        /* Count: 2 bytes little-endian */
        trim_buf[off + 6] = (uint8_t)(chunk & 0xFF);
        trim_buf[off + 7] = (uint8_t)((chunk >> 8) & 0xFF);

        lba += chunk;
        count -= chunk;
        entries++;
    }

    /* Issue DATA SET MANAGEMENT command (0x06) */
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
    fis->pmport_c = 0x80;       /* C = 1 (command) */
    fis->command  = 0x06;       /* DATA SET MANAGEMENT */
    fis->featurel = 0x01;       /* Bit 0 = TRIM */
    fis->countl   = 1;          /* 1 sector of range descriptors */
    fis->counth   = 0;
    fis->device   = 0;

    /* PRDT: one entry pointing to the TRIM buffer (write direction) */
    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)trim_buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = 512 - 1;  /* Byte count minus 1 */

    /* Command header — W bit set (data flows host→device) */
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

/* ---- Initialization ---- */

int ahci_init(void)
{
    uint8_t bus, slot, func;
    int found = 0;
    uint32_t bar5, pi, ghc, cap, ver;
    uint64_t abar_addr;
    int port_num, drive_idx;

    /* Scan PCI for AHCI controller (class 0x01, subclass 0x06) */
    for (bus = 0; bus < 8 && !found; bus++) {
        for (slot = 0; slot < 32 && !found; slot++) {
            for (func = 0; func < 8 && !found; func++) {
                uint16_t vid = pci_read16(bus, slot, func, 0x00);
                if (vid == 0xFFFF)
                    continue;

                uint8_t cls = pci_read8(bus, slot, func, PCI_CLASS);
                uint8_t sub = pci_read8(bus, slot, func, PCI_SUBCLASS);

                if (cls == AHCI_PCI_CLASS && sub == AHCI_PCI_SUBCLASS) {
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }
        if (found) break;
    }

    if (!found) {
        klog(LOG_WARN, "ahci", "AHCI: no controller found");
        return -1;
    }

    /* Adjust: the loop increments after break, so fix the values */
    if (!found) return -1;

    klog(LOG_DEBUG, "ahci", "Found controller at PCI %u:%u.%u",
           (uint64_t)bus, (uint64_t)slot, (uint64_t)func);

    /* Save PCI location for IRQ setup later */
    ahci_pci_bus  = bus;
    ahci_pci_slot = slot;
    ahci_pci_func = func;

    /* Enable bus mastering, memory space, and clear interrupt disable */
    {
        uint16_t cmd = pci_read16(bus, slot, func, PCI_COMMAND);
        cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE;
        cmd &= ~PCI_CMD_INT_DISABLE;
        pci_write16(bus, slot, func, PCI_COMMAND, cmd);
    }

    /* Read BAR5 (ABAR) */
    bar5 = pci_read32(bus, slot, func, PCI_BAR5);
    abar_addr = (uint64_t)(bar5 & 0xFFFFF000);

    if (abar_addr == 0) {
        klog(LOG_DEBUG, "ahci", "BAR5 is zero — no ABAR");
        return -1;
    }

    /* Map AHCI MMIO region (4 KiB minimum, but map 8 KiB to be safe) */
    ahci_map_mmio(abar_addr, 0x2000);
    abar = (volatile uint8_t *)abar_addr;

    klog(LOG_DEBUG, "ahci", "ABAR at 0x%x", abar_addr);

    /* ---- BIOS/OS Handoff (BOHC) ----
     * The BIOS may own the AHCI controller via System Management Mode (SMM).
     * We must request ownership before touching any HBA registers.
     * See AHCI 1.3.1 spec §10.6 "BIOS/OS Handoff". */
    {
        uint32_t cap2 = ahci_read32(abar, AHCI_CAP2);
        if (cap2 & AHCI_CAP2_BOH) {
            uint32_t bohc = ahci_read32(abar, AHCI_BOHC);

            /* Request OS ownership */
            bohc |= AHCI_BOHC_OOS;
            ahci_write32(abar, AHCI_BOHC, bohc);

            /* Wait up to 25ms for BIOS to release (BOS → 0) */
            {
                uint32_t wait = 25000;
                while (wait--) {
                    bohc = ahci_read32(abar, AHCI_BOHC);
                    if (!(bohc & AHCI_BOHC_BOS))
                        break;
                    /* ~1µs per MMIO read iteration */
                }

                if (bohc & AHCI_BOHC_BOS) {
                    /* BIOS didn't release — force ownership change.
                     * OOC (bit 3) is defined in the spec but some HBAs
                     * don't implement it; we set it and proceed. */
                    bohc |= (1U << 3);  /* BOHC.OOC */
                    ahci_write32(abar, AHCI_BOHC, bohc);
                    klog(LOG_WARN, "ahci",
                           "BOHC: BIOS did not release, forced ownership");
                }
            }

            /* If BIOS Busy (BB) is set, the BIOS is still cleaning up.
             * Wait up to 2 seconds for BB to clear (per AHCI spec §10.6). */
            {
                uint32_t wait = 2000000;
                bohc = ahci_read32(abar, AHCI_BOHC);
                while ((bohc & AHCI_BOHC_BB) && wait--) {
                    bohc = ahci_read32(abar, AHCI_BOHC);
                }
            }

            klog(LOG_INFO, "ahci", "BIOS/OS handoff complete");
        } else {
            klog(LOG_DEBUG, "ahci", "BOHC not supported — skipping handoff");
        }
    }

    /* Read version */
    ver = ahci_read32(abar, AHCI_VS);
    klog(LOG_DEBUG, "ahci", "Version %u.%u",
           (uint64_t)((ver >> 16) & 0xFFFF),
           (uint64_t)(ver & 0xFFFF));

    /* Enable AHCI mode */
    ghc = ahci_read32(abar, AHCI_GHC);
    ghc |= AHCI_GHC_AE;
    ahci_write32(abar, AHCI_GHC, ghc);

    /* Read capabilities */
    cap = ahci_read32(abar, AHCI_CAP);
    ahci_clo_supported = (cap & AHCI_CAP_SCLO) ? 1 : 0;
    ahci_ncq_capable   = (cap & AHCI_CAP_SNCQ) ? 1 : 0;
    ahci_max_cmd_slots = (uint8_t)(((cap & AHCI_CAP_NCS_MASK)
                                    >> AHCI_CAP_NCS_SHIFT) + 1);
    {
        uint32_t max_ports = (cap & AHCI_CAP_NP_MASK) + 1;
        klog(LOG_DEBUG, "ahci", "Ports: %u, Slots: %u, CLO: %s, NCQ: %s",
               (uint64_t)max_ports, (uint64_t)ahci_max_cmd_slots,
               ahci_clo_supported ? "yes" : "no",
               ahci_ncq_capable ? "yes" : "no");
    }

    /* Read ports implemented */
    pi = ahci_read32(abar, AHCI_PI);
    klog(LOG_DEBUG, "ahci", "Ports implemented: 0x%x", (uint64_t)pi);

    /* Clear global interrupt status */
    ahci_write32(abar, AHCI_IS, ahci_read32(abar, AHCI_IS));

    /* Keep GHC.IE disabled during port enumeration — we use polling
     * for IDENTIFY commands. IRQs are enabled after port init. */

    /* Initialize each implemented port */
    num_drives = 0;
    num_atapi  = 0;
    for (port_num = 0; port_num < 32; port_num++) {
        if (!(pi & (1U << port_num)))
            continue;

        drive_idx = num_drives + num_atapi;
        if (drive_idx >= AHCI_MAX_PORTS)
            break;

        if (port_init(&ports[drive_idx], port_num) == 0) {
            if (ports[drive_idx].is_atapi) {
                /* ATAPI device — use IDENTIFY PACKET DEVICE */
                if (atapi_do_identify(&ports[drive_idx]) == 0) {
                    /* Try to read disc capacity (may fail if no disc inserted) */
                    if (atapi_read_capacity(&ports[drive_idx]) != 0) {
                        ports[drive_idx].sectors = 0;
                    }

                    klog(LOG_INFO, "ahci", "AHCI port %u: ATAPI \"%s\" (%u sectors, %u B/sect)",
                           (uint64_t)port_num,
                           ports[drive_idx].model,
                           ports[drive_idx].sectors,
                           (uint64_t)ports[drive_idx].sector_size);

                    atapi_map[num_atapi] = drive_idx;
                    num_atapi++;
                } else {
                    klog(LOG_DEBUG, "ahci", "Port %u: ATAPI IDENTIFY failed",
                           (uint64_t)port_num);
                    ports[drive_idx].active = 0;
                }
            } else {
                /* SATA drive — use standard IDENTIFY */
                if (ahci_do_identify(&ports[drive_idx]) == 0) {
                    uint64_t size_mb = ports[drive_idx].sectors / 2048;
                    klog(LOG_DEBUG, "ahci", "AHCI port %u: \"%s\" (%u MiB, %u sectors)",
                           (uint64_t)port_num,
                           ports[drive_idx].model,
                           size_mb,
                           ports[drive_idx].sectors);
                    num_drives++;
                } else {
                    klog(LOG_DEBUG, "ahci", "Port %u: IDENTIFY failed",
                           (uint64_t)port_num);
                    ports[drive_idx].active = 0;
                }
            }
        }
    }

    if (num_drives == 0 && num_atapi == 0) {
        klog(LOG_WARN, "ahci", "AHCI: no devices detected");
        return -1;
    }

    num_ports_total = num_drives + num_atapi;

    /* ---- Set up interrupt-driven I/O ---- */
    {
        uint8_t pci_irq_line = pci_read8(ahci_pci_bus, ahci_pci_slot,
                                          ahci_pci_func, 0x3C);

        if (pci_irq_line != 0 && pci_irq_line != 0xFF &&
            ioapic_available()) {
            ahci_irq_vector = irq_alloc_vector();
            if (ahci_irq_vector) {
                irq_register(ahci_irq_vector, ahci_irq_handler,
                             NULL, "ahci");

                /* Route PCI interrupt line via IOAPIC.
                 * PCI INTx is level-triggered, active-low:
                 *   flags bits 0-1 = 0x03 (active low)
                 *   flags bits 2-3 = 0x0C (level triggered)
                 *   combined = 0x0F */
                ioapic_route_irq(pci_irq_line, ahci_irq_vector,
                                 0, 0x0F);

                /* Enable global AHCI interrupts */
                ghc = ahci_read32(abar, AHCI_GHC);
                ghc |= AHCI_GHC_IE;
                ahci_write32(abar, AHCI_GHC, ghc);

                use_events = 1;
                klog(LOG_INFO, "ahci",
                       "AHCI: IRQ %u -> vector 0x%x (interrupt-driven I/O)",
                       (uint64_t)pci_irq_line,
                       (uint64_t)ahci_irq_vector);
            } else {
                klog(LOG_WARN, "ahci",
                       "AHCI: no free IRQ vector, using polling");
            }
        } else {
            klog(LOG_WARN, "ahci",
                   "AHCI: no PCI IRQ line (0x%x), using polling",
                   (uint64_t)pci_irq_line);
        }
    }

    initialized = 1;
    klog(LOG_DEBUG, "ahci", "AHCI: %u SATA drive(s), %u ATAPI device(s)",
           (uint64_t)num_drives, (uint64_t)num_atapi);

    return 0;
}

/* ---- Flush per-port error counters to Registry ---- */

static uint32_t ahci_reg_throttle;          /* only flush every Nth call */
static HKEY     ahci_reg_port_keys[32];     /* Cached per-port HKEY (0 = not open) */
static int      ahci_reg_keys_init;         /* 1 after first successful init */

/* One-time setup: open/create all port error keys and keep them cached. */
static void ahci_reg_keys_open(void)
{
    HKEY hAhci;
    long rc;
    int i;

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\AHCI", 0,
                        (char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                        &hAhci, (uint32_t *)0);
    if (rc != 0) return;

    for (i = 0; i < num_ports_total; i++) {
        char port_path[32];
        if (!ports[i].active) continue;

        /* Build sub-key path: "Port0\Errors", "Port1\Errors", ... */
        {
            int n = ports[i].port_num;
            char *p = port_path;
            *p++ = 'P'; *p++ = 'o'; *p++ = 'r'; *p++ = 't';
            if (n >= 10) { *p++ = (char)('0' + n / 10); }
            *p++ = (char)('0' + n % 10);
            *p++ = '\\'; *p++ = 'E'; *p++ = 'r'; *p++ = 'r';
            *p++ = 'o'; *p++ = 'r'; *p++ = 's'; *p = '\0';
        }

        rc = RegCreateKeyEx(hAhci, port_path, 0,
                            (char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                            &ahci_reg_port_keys[i], (uint32_t *)0);
    }

    RegCloseKey(hAhci);  /* Only the parent handle — port keys stay open */
    ahci_reg_keys_init = 1;
}

void ahci_flush_error_counters(void)
{
    int i;

    if (!initialized) return;

    /* Throttle: only flush every 256 iterations (~4s at 60 fps) */
    if ((ahci_reg_throttle++ & 0xFF) != 0) return;

    /* Lazy init: open cached keys on first flush */
    if (!ahci_reg_keys_init)
        ahci_reg_keys_open();

    for (i = 0; i < num_ports_total; i++) {
        HKEY hPort = ahci_reg_port_keys[i];
        if (!hPort || !ports[i].active) continue;

        RegSetDword(hPort, "FatalErrors",   ports[i].errors.fatal_errors);
        RegSetDword(hPort, "NonfatalErrors", ports[i].errors.nonfatal_errors);
        RegSetDword(hPort, "CrcErrors",     ports[i].errors.crc_errors);
        RegSetDword(hPort, "LinkResets",    ports[i].errors.link_resets);
        RegSetDword(hPort, "CmdFailures",   ports[i].errors.cmd_failures);
    }
}


/* ---- NCQ synchronous read/write with retry ---- */

static int ncq_sync_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
                       void *buffer, int is_write)
{
    int retries = 3;
    int tag, status;
    uint32_t tfd;

    while (retries > 0) {
        tag = ncq_alloc_tag(p);
        if (tag < 0) {
            /* All tags busy — fallback to DMA */
            return ahci_do_rw(p, lba, count, buffer, is_write);
        }

        ncq_issue_rw(p, tag, lba, count, buffer, is_write);

        /* Wait for this tag to complete */
        {
            int timeout = 50;  /* 50 × 100ms = 5s max */
            while (!(p->tags_completed & (1U << tag)) && timeout > 0) {
                event_wait_timeout(&p->completion, 100);
                timeout--;
            }
            if (!(p->tags_completed & (1U << tag))) {
                /* Timeout — mark as failed */
                p->tag_status[tag] = -1;
                p->tags_completed |= (1U << tag);
                p->tags_pending &= ~(1U << tag);
                klog(LOG_WARN, "ahci",
                       "Port %u: NCQ tag %u timeout",
                       (uint64_t)p->port_num, (uint64_t)tag);
            }
        }

        p->tags_completed &= ~(1U << tag);
        status = p->tag_status[tag];
        ncq_free_tag(p, tag);

        if (status == 0)
            return 0;  /* Success */

        /* CLO recovery on failure */
        retries--;
        tfd = port_read(p->regs, AHCI_PxTFD);
        if (tfd & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ)) {
            if (port_clo_reset(p) != 0)
                break;  /* Port unrecoverable */
        }
        klog(LOG_DEBUG, "ahci",
               "Port %u: NCQ retry (%d left)",
               (uint64_t)p->port_num, (uint64_t)retries);
    }

    p->errors.cmd_failures++;
    return -1;
}

int ahci_ncq_read(int port_idx, uint64_t lba, uint32_t count, void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (!ports[port_idx].ncq_supported)
        return ahci_do_rw(&ports[port_idx], lba, count, buffer, 0);
    return ncq_sync_rw(&ports[port_idx], lba, count, buffer, 0);
}

int ahci_ncq_write(int port_idx, uint64_t lba, uint32_t count,
                   const void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    if (!ports[port_idx].ncq_supported)
        return ahci_do_rw(&ports[port_idx], lba, count, (void *)buffer, 1);
    return ncq_sync_rw(&ports[port_idx], lba, count, (void *)buffer, 1);
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
        return -1;  /* Async only available with NCQ + IRQ */

    tag = ncq_alloc_tag(p);
    if (tag < 0)
        return -1;  /* No free tags */

    p->tag_callbacks[tag] = callback;
    p->tag_cb_ctx[tag] = ctx;

    ncq_issue_rw(p, tag, lba, count, buffer, is_write);
    return tag;
}
