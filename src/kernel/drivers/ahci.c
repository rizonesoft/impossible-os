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
#include "kernel/drivers/pci.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/vmm.h"
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
static int               initialized;

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
        __asm__ volatile ("pause");
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
        __asm__ volatile ("pause");
    }
}

/* ---- Start command engine ---- */
static void port_start_cmd(volatile uint8_t *pregs)
{
    uint32_t cmd;

    /* Wait for CR to clear before starting */
    while (port_read(pregs, AHCI_PxCMD) & AHCI_PxCMD_CR)
        __asm__ volatile ("pause");

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

    /* Only handle SATA drives (not ATAPI) */
    if (p->sig != AHCI_SIG_ATA)
        return -1;

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

    /* Enable interrupts for this port */
    port_write(pregs, AHCI_PxIE, 0x01);  /* D2H Register FIS interrupt */

    /* Start command engine */
    port_start_cmd(pregs);

    p->active = 1;
    return 0;
}

/* ---- Issue a command and wait for completion ---- */
static int port_issue_cmd(struct ahci_port *p, int slot)
{
    volatile uint8_t *pregs = p->regs;
    uint32_t timeout = 5000000;
    uint32_t tfd;

    /* Issue command */
    port_write(pregs, AHCI_PxCI, 1U << slot);

    /* Poll for completion: wait for CI bit to clear */
    while (timeout--) {
        uint32_t ci = port_read(pregs, AHCI_PxCI);
        if (!(ci & (1U << slot)))
            break;

        /* Check for errors */
        tfd = port_read(pregs, AHCI_PxTFD);
        if (tfd & (AHCI_PxTFD_ERR | AHCI_PxTFD_BSY)) {
            if (tfd & AHCI_PxTFD_ERR) {
                printk("[AHCI] Port %u: TFD error 0x%x\n",
                       (uint64_t)p->port_num, (uint64_t)tfd);
                return -1;
            }
        }
        __asm__ volatile ("pause");
    }

    if (timeout == 0) {
        printk("[AHCI] Port %u: command timeout\n",
               (uint64_t)p->port_num);
        return -1;
    }

    /* Check final TFD for errors */
    tfd = port_read(pregs, AHCI_PxTFD);
    if (tfd & AHCI_PxTFD_ERR) {
        printk("[AHCI] Port %u: command error TFD=0x%x\n",
               (uint64_t)p->port_num, (uint64_t)tfd);
        return -1;
    }

    /* Clear interrupt status */
    port_write(pregs, AHCI_PxIS, port_read(pregs, AHCI_PxIS));

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

    return 0;
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

int ahci_read(int port_idx, uint64_t lba, uint32_t count, void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    return ahci_do_rw(&ports[port_idx], lba, count, buffer, 0);
}

int ahci_write(int port_idx, uint64_t lba, uint32_t count,
               const void *buffer)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return -1;
    return ahci_do_rw(&ports[port_idx], lba, count, (void *)buffer, 1);
}

uint64_t ahci_capacity(int port_idx)
{
    if (port_idx < 0 || port_idx >= num_drives || !ports[port_idx].active)
        return 0;
    return ports[port_idx].sectors;
}

int ahci_present(void)
{
    return initialized && num_drives > 0;
}

int ahci_drive_count(void)
{
    return num_drives;
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
        printk("[--] AHCI: no controller found\n");
        return -1;
    }

    /* Adjust: the loop increments after break, so fix the values */
    if (!found) return -1;

    printk("[AHCI] Found controller at PCI %u:%u.%u\n",
           (uint64_t)bus, (uint64_t)slot, (uint64_t)func);

    /* Enable bus mastering, memory space, and interrupt disable */
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
        printk("[AHCI] BAR5 is zero — no ABAR\n");
        return -1;
    }

    /* Map AHCI MMIO region (4 KiB minimum, but map 8 KiB to be safe) */
    ahci_map_mmio(abar_addr, 0x2000);
    abar = (volatile uint8_t *)abar_addr;

    printk("[AHCI] ABAR at 0x%x\n", abar_addr);

    /* Read version */
    ver = ahci_read32(abar, AHCI_VS);
    printk("[AHCI] Version %u.%u\n",
           (uint64_t)((ver >> 16) & 0xFFFF),
           (uint64_t)(ver & 0xFFFF));

    /* Enable AHCI mode */
    ghc = ahci_read32(abar, AHCI_GHC);
    ghc |= AHCI_GHC_AE;
    ahci_write32(abar, AHCI_GHC, ghc);

    /* Read capabilities */
    cap = ahci_read32(abar, AHCI_CAP);
    {
        uint32_t max_ports = (cap & AHCI_CAP_NP_MASK) + 1;
        uint32_t max_slots = ((cap & AHCI_CAP_NCS_MASK) >> AHCI_CAP_NCS_SHIFT) + 1;
        printk("[AHCI] Ports: %u, Command slots: %u\n",
               (uint64_t)max_ports, (uint64_t)max_slots);
    }

    /* Read ports implemented */
    pi = ahci_read32(abar, AHCI_PI);
    printk("[AHCI] Ports implemented: 0x%x\n", (uint64_t)pi);

    /* Clear global interrupt status */
    ahci_write32(abar, AHCI_IS, ahci_read32(abar, AHCI_IS));

    /* Enable global interrupts */
    ghc = ahci_read32(abar, AHCI_GHC);
    ghc |= AHCI_GHC_IE;
    ahci_write32(abar, AHCI_GHC, ghc);

    /* Initialize each implemented port */
    num_drives = 0;
    for (port_num = 0; port_num < 32; port_num++) {
        if (!(pi & (1U << port_num)))
            continue;

        drive_idx = num_drives;
        if (drive_idx >= AHCI_MAX_PORTS)
            break;

        if (port_init(&ports[drive_idx], port_num) == 0) {
            /* Drive detected — try IDENTIFY */
            if (ahci_do_identify(&ports[drive_idx]) == 0) {
                uint64_t size_mb = ports[drive_idx].sectors / 2048;
                printk("[OK] AHCI port %u: \"%s\" (%u MiB, %u sectors)\n",
                       (uint64_t)port_num,
                       ports[drive_idx].model,
                       size_mb,
                       ports[drive_idx].sectors);
                num_drives++;
            } else {
                printk("[AHCI] Port %u: IDENTIFY failed\n",
                       (uint64_t)port_num);
                ports[drive_idx].active = 0;
            }
        }
    }

    if (num_drives == 0) {
        printk("[--] AHCI: no SATA drives detected\n");
        return -1;
    }

    initialized = 1;
    printk("[OK] AHCI: %u SATA drive(s) initialized\n",
           (uint64_t)num_drives);

    return 0;
}
