/* ============================================================================
 * ahci_core.c — AHCI driver core: state, MMIO, port management, ISR, init
 * ============================================================================ */

#include "kernel/drivers/ahci_internal.h"
#include "registry.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/irq.h"
#include "kernel/mm/vmm.h"
#include "kernel/sched/event.h"
#include "kernel/printk.h"

/* ---- Driver state ---- */
volatile uint8_t *abar;
struct ahci_port  ports[AHCI_MAX_PORTS];
int               num_drives;
int               num_atapi;
int               initialized;
int               num_ports_total;
int               atapi_map[AHCI_MAX_PORTS];

/* ---- Interrupt state ---- */
static uint8_t    ahci_pci_bus;
static uint8_t    ahci_pci_slot;
static uint8_t    ahci_pci_func;
static uint8_t    ahci_irq_vector;
int               use_events;
int               ahci_clo_supported;
int               ahci_ncq_capable;
uint8_t           ahci_max_cmd_slots;

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

/* ---- memset helper ---- */
void ahci_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = val;
}

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

uint32_t port_read(volatile uint8_t *pregs, uint32_t off)
{
    return ahci_read32(pregs, off);
}

void port_write(volatile uint8_t *pregs, uint32_t off, uint32_t val)
{
    ahci_write32(pregs, off, val);
}

/* ---- Stop command engine ---- */
void port_stop_cmd(volatile uint8_t *pregs)
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
void port_start_cmd(volatile uint8_t *pregs)
{
    uint32_t cmd;
    uint32_t timeout = 500000;

    while ((port_read(pregs, AHCI_PxCMD) & AHCI_PxCMD_CR) && timeout--)
        ;

    cmd = port_read(pregs, AHCI_PxCMD);
    cmd |= AHCI_PxCMD_FRE;
    port_write(pregs, AHCI_PxCMD, cmd);

    cmd |= AHCI_PxCMD_ST;
    port_write(pregs, AHCI_PxCMD, cmd);
}

/* ---- Find a free command slot ---- */
int port_find_slot(volatile uint8_t *pregs)
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
        return -1;
    if ((ssts & AHCI_SSTS_IPM_MASK) != AHCI_SSTS_IPM_ACTIVE)
        return -1;

    /* Read signature */
    p->sig = port_read(pregs, AHCI_PxSIG);

    /* Accept both SATA drives and ATAPI (optical) devices */
    if (p->sig == AHCI_SIG_ATAPI) {
        p->is_atapi = 1;
        p->sector_size = 2048;
    } else if (p->sig == AHCI_SIG_ATA) {
        p->is_atapi = 0;
        p->sector_size = 512;
    } else {
        return -1;
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

        p->cmdlist[i].ctba  = (uint32_t)(uintptr_t)tbl_mem;
        p->cmdlist[i].ctbau = 0;
    }

    /* Clear error register */
    port_write(pregs, AHCI_PxSERR, 0xFFFFFFFF);

    /* Clear interrupt status */
    port_write(pregs, AHCI_PxIS, 0xFFFFFFFF);

    /* Enable per-port interrupts */
    port_write(pregs, AHCI_PxIE,
               AHCI_PxIS_DHRS | AHCI_PxIS_PSS | AHCI_PxIS_DSS |
               AHCI_PxIS_SDBS | AHCI_PxIS_TFES |
               AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS |
               AHCI_PxIS_INFS | AHCI_PxIS_OFS);

    /* Initialize per-port completion event */
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

    is = ahci_read32(abar, AHCI_IS);
    if (!is)
        return;

    for (i = 0; i < num_ports_total; i++) {
        if (!ports[i].active)
            continue;
        if (!(is & (1U << ports[i].port_num)))
            continue;

        uint32_t pxis = port_read(ports[i].regs, AHCI_PxIS);
        port_write(ports[i].regs, AHCI_PxIS, pxis);

        /* ---- Error classification ---- */
        if (pxis & AHCI_PxIS_FATAL) {
            ports[i].errors.fatal_errors++;

            uint32_t serr = port_read(ports[i].regs, AHCI_PxSERR);
            port_write(ports[i].regs, AHCI_PxSERR, serr);

            if (serr & AHCI_PxSERR_ERR_M)
                ports[i].errors.crc_errors++;

            klog(LOG_WARN, "ahci",
                   "Port %u: fatal error PxIS=0x%x PxSERR=0x%x",
                   (uint64_t)ports[i].port_num,
                   (uint64_t)pxis, (uint64_t)serr);

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
                if (ports[i].tag_callbacks[t]) {
                    ports[i].tag_callbacks[t](
                        i, t, ports[i].tag_status[t],
                        ports[i].tag_cb_ctx[t]);
                    ports[i].tag_callbacks[t] = (ahci_callback_t)0;
                }
            }
            ports[i].tags_pending &= ~completed;
        }

        event_set(&ports[i].completion);
    }

    ahci_write32(abar, AHCI_IS, is);
}

/* ---- CLO + COMRESET recovery for stuck BSY/DRQ ---- */
int port_clo_reset(struct ahci_port *p)
{
    volatile uint8_t *pregs = p->regs;
    uint32_t cmd, wait;

    klog(LOG_WARN, "ahci", "Port %u: CLO recovery — BSY/DRQ stuck, link reset",
           (uint64_t)p->port_num);

    port_stop_cmd(pregs);

    if (ahci_clo_supported) {
        cmd = port_read(pregs, AHCI_PxCMD);
        cmd |= (1U << 3);
        port_write(pregs, AHCI_PxCMD, cmd);

        wait = 100000;
        while ((port_read(pregs, AHCI_PxCMD) & (1U << 3)) && wait--)
            ;
    }

    {
        uint32_t sctl = port_read(pregs, AHCI_PxSCTL);
        sctl = (sctl & ~0xFU) | 0x1;
        port_write(pregs, AHCI_PxSCTL, sctl);
    }

    {
        volatile uint32_t delay = 100000;
        while (delay--)
            ;
    }

    {
        uint32_t sctl = port_read(pregs, AHCI_PxSCTL);
        sctl &= ~0xFU;
        port_write(pregs, AHCI_PxSCTL, sctl);
    }

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

    port_write(pregs, AHCI_PxSERR, 0xFFFFFFFF);
    port_write(pregs, AHCI_PxIS, 0xFFFFFFFF);

    wait = 1000000;
    while (wait--) {
        uint32_t tfd = port_read(pregs, AHCI_PxTFD);
        if (!(tfd & AHCI_PxTFD_BSY))
            break;
    }

    port_start_cmd(pregs);

    klog(LOG_INFO, "ahci", "Port %u: CLO recovery complete",
           (uint64_t)p->port_num);
    p->errors.link_resets++;
    return 0;
}

/* ---- Issue a command and wait for completion (with retry) ---- */
int port_issue_cmd(struct ahci_port *p, int slot)
{
    volatile uint8_t *pregs = p->regs;
    uint32_t tfd;
    int retries = 3;
    int result;

retry:
    result = 0;

    port_write(pregs, AHCI_PxCI, 1U << slot);

    if (use_events) {
        int signalled = event_wait_timeout(&p->completion, 5000);
        if (!signalled) {
            klog(LOG_DEBUG, "ahci", "Port %u: command timeout (IRQ)",
                   (uint64_t)p->port_num);
            result = -1;
        }
    } else {
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

        port_write(pregs, AHCI_PxIS, port_read(pregs, AHCI_PxIS));
    }

    tfd = port_read(pregs, AHCI_PxTFD);
    if (tfd & AHCI_PxTFD_ERR)
        result = -1;

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

/* ---- Initialization ---- */

int ahci_init(void)
{
    uint8_t bus, slot, func;
    int found = 0;
    uint32_t bar5, pi, ghc, cap, ver;
    uint64_t abar_addr;
    int port_num, drive_idx;

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

    if (!found) return -1;

    klog(LOG_DEBUG, "ahci", "Found controller at PCI %u:%u.%u",
           (uint64_t)bus, (uint64_t)slot, (uint64_t)func);

    ahci_pci_bus  = bus;
    ahci_pci_slot = slot;
    ahci_pci_func = func;

    {
        uint16_t cmd = pci_read16(bus, slot, func, PCI_COMMAND);
        cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE;
        cmd &= ~PCI_CMD_INT_DISABLE;
        pci_write16(bus, slot, func, PCI_COMMAND, cmd);
    }

    bar5 = pci_read32(bus, slot, func, PCI_BAR5);
    abar_addr = (uint64_t)(bar5 & 0xFFFFF000);

    if (abar_addr == 0) {
        klog(LOG_DEBUG, "ahci", "BAR5 is zero — no ABAR");
        return -1;
    }

    ahci_map_mmio(abar_addr, 0x2000);
    abar = (volatile uint8_t *)abar_addr;

    klog(LOG_DEBUG, "ahci", "ABAR at 0x%x", abar_addr);

    /* ---- BIOS/OS Handoff (BOHC) ---- */
    {
        uint32_t cap2 = ahci_read32(abar, AHCI_CAP2);
        if (cap2 & AHCI_CAP2_BOH) {
            uint32_t bohc = ahci_read32(abar, AHCI_BOHC);

            bohc |= AHCI_BOHC_OOS;
            ahci_write32(abar, AHCI_BOHC, bohc);

            {
                uint32_t wait = 25000;
                while (wait--) {
                    bohc = ahci_read32(abar, AHCI_BOHC);
                    if (!(bohc & AHCI_BOHC_BOS))
                        break;
                }

                if (bohc & AHCI_BOHC_BOS) {
                    bohc |= (1U << 3);
                    ahci_write32(abar, AHCI_BOHC, bohc);
                    klog(LOG_WARN, "ahci",
                           "BOHC: BIOS did not release, forced ownership");
                }
            }

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

    ver = ahci_read32(abar, AHCI_VS);
    klog(LOG_DEBUG, "ahci", "Version %u.%u",
           (uint64_t)((ver >> 16) & 0xFFFF),
           (uint64_t)(ver & 0xFFFF));

    ghc = ahci_read32(abar, AHCI_GHC);
    ghc |= AHCI_GHC_AE;
    ahci_write32(abar, AHCI_GHC, ghc);

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

    pi = ahci_read32(abar, AHCI_PI);
    klog(LOG_DEBUG, "ahci", "Ports implemented: 0x%x", (uint64_t)pi);

    ahci_write32(abar, AHCI_IS, ahci_read32(abar, AHCI_IS));

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
                if (atapi_do_identify(&ports[drive_idx]) == 0) {
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

    /* ---- Set up interrupt-driven I/O (MSI preferred, INTx fallback) ---- */
    {
        int irq_ok = 0;

        /* --- Try MSI first ---
         * Walk PCI Capabilities List searching for MSI (Cap ID 0x05).
         * PCI Status bit 4 indicates capabilities list is present.
         * First capability pointer is at config offset 0x34.
         * Each capability: byte 0 = Cap ID, byte 1 = next pointer. */
        {
            uint16_t pci_status = pci_read16(ahci_pci_bus, ahci_pci_slot,
                                              ahci_pci_func, PCI_STATUS);
            if (pci_status & (1U << 4)) {
                /* Capabilities list present */
                uint8_t cap_off = pci_read8(ahci_pci_bus, ahci_pci_slot,
                                             ahci_pci_func, 0x34) & 0xFC;

                while (cap_off >= 0x40) {
                    uint8_t cap_id = pci_read8(ahci_pci_bus, ahci_pci_slot,
                                               ahci_pci_func, cap_off);

                    if (cap_id == 0x05) {
                        /* Found MSI capability */
                        uint16_t msi_ctrl;
                        uint8_t  msi_addr_off;
                        uint8_t  msi_data_off;

                        msi_ctrl = pci_read16(ahci_pci_bus, ahci_pci_slot,
                                              ahci_pci_func, cap_off + 2);

                        ahci_irq_vector = irq_alloc_vector();
                        if (ahci_irq_vector) {
                            irq_register(ahci_irq_vector, ahci_irq_handler,
                                         NULL, "ahci");

                            /* Message Address: 0xFEE00000 targets BSP
                             * (LAPIC ID 0, no redirection) */
                            msi_addr_off = cap_off + 4;
                            pci_write32(ahci_pci_bus, ahci_pci_slot,
                                        ahci_pci_func, msi_addr_off,
                                        0xFEE00000);

                            /* Check 64-bit capable (bit 7 of MSI Control) */
                            if (msi_ctrl & (1U << 7)) {
                                /* 64-bit: upper address = 0 */
                                pci_write32(ahci_pci_bus, ahci_pci_slot,
                                            ahci_pci_func, msi_addr_off + 4,
                                            0);
                                msi_data_off = cap_off + 12;
                            } else {
                                msi_data_off = cap_off + 8;
                            }

                            /* Message Data: vector number, edge trigger,
                             * fixed delivery mode */
                            pci_write16(ahci_pci_bus, ahci_pci_slot,
                                        ahci_pci_func, msi_data_off,
                                        (uint16_t)ahci_irq_vector);

                            /* Enable MSI: set bit 0 of MSI Control,
                             * keep Multiple Message Enable at 0 (1 vector) */
                            msi_ctrl &= ~(0x7U << 4);  /* MME = 0 (1 msg) */
                            msi_ctrl |= (1U << 0);     /* MSI Enable */
                            pci_write16(ahci_pci_bus, ahci_pci_slot,
                                        ahci_pci_func, cap_off + 2,
                                        msi_ctrl);

                            /* Disable legacy INTx — MSI takes over */
                            {
                                uint16_t cmd = pci_read16(ahci_pci_bus,
                                                          ahci_pci_slot,
                                                          ahci_pci_func,
                                                          PCI_COMMAND);
                                cmd |= PCI_CMD_INT_DISABLE;
                                pci_write16(ahci_pci_bus, ahci_pci_slot,
                                            ahci_pci_func, PCI_COMMAND, cmd);
                            }

                            /* Enable global AHCI interrupts */
                            ghc = ahci_read32(abar, AHCI_GHC);
                            ghc |= AHCI_GHC_IE;
                            ahci_write32(abar, AHCI_GHC, ghc);

                            use_events = 1;
                            irq_ok = 1;
                            klog(LOG_INFO, "ahci",
                                   "AHCI: MSI vector 0x%x (%s-bit)",
                                   (uint64_t)ahci_irq_vector,
                                   (msi_ctrl & (1U << 7)) ? "64" : "32");
                        }
                        break;
                    }

                    /* Next capability */
                    cap_off = pci_read8(ahci_pci_bus, ahci_pci_slot,
                                        ahci_pci_func, cap_off + 1) & 0xFC;
                }
            }
        }

        /* --- Fallback to legacy INTx via IOAPIC --- */
        if (!irq_ok) {
            uint8_t pci_irq_line = pci_read8(ahci_pci_bus, ahci_pci_slot,
                                              ahci_pci_func, 0x3C);

            if (pci_irq_line != 0 && pci_irq_line != 0xFF &&
                ioapic_available()) {
                ahci_irq_vector = irq_alloc_vector();
                if (ahci_irq_vector) {
                    irq_register(ahci_irq_vector, ahci_irq_handler,
                                 NULL, "ahci");

                    ioapic_route_irq(pci_irq_line, ahci_irq_vector,
                                     0, 0x0F);

                    ghc = ahci_read32(abar, AHCI_GHC);
                    ghc |= AHCI_GHC_IE;
                    ahci_write32(abar, AHCI_GHC, ghc);

                    use_events = 1;
                    irq_ok = 1;
                    klog(LOG_INFO, "ahci",
                           "AHCI: INTx IRQ %u -> vector 0x%x (legacy)",
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
    }

    initialized = 1;
    klog(LOG_DEBUG, "ahci", "AHCI: %u SATA drive(s), %u ATAPI device(s)",
           (uint64_t)num_drives, (uint64_t)num_atapi);

    return 0;
}
