/* ============================================================================
 * nvme.c — NVMe storage driver
 *
 * §1: PCI scan, BAR0 UC mapping, CAP/VS read, controller disable/enable.
 * §2: Admin Queue setup, Identify Controller + Identify Namespace.
 * §3: I/O Queue creation, sector read/write via polled completion.
 * ============================================================================ */

#include "kernel/drivers/nvme.h"
#include "kernel/drivers/pci.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"
#include "kernel/timer.h"
#include "kernel/barrier.h"

/* ---- Static state ---- */
static struct nvme_controller controllers[NVME_MAX_CONTROLLERS];
static int num_controllers;

/* ---- MMIO helpers ---- */

static inline uint32_t nvme_read32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void nvme_write32(volatile uint8_t *base, uint32_t off,
                                uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

static inline uint64_t nvme_read64(volatile uint8_t *base, uint32_t off)
{
    uint32_t lo = *(volatile uint32_t *)(base + off);
    uint32_t hi = *(volatile uint32_t *)(base + off + 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static inline void nvme_write64(volatile uint8_t *base, uint32_t off,
                                uint64_t val)
{
    *(volatile uint32_t *)(base + off)     = (uint32_t)val;
    *(volatile uint32_t *)(base + off + 4) = (uint32_t)(val >> 32);
}

static void nvme_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--)
        *p++ = val;
}

/* ---- Admin command submission ---- */

static int nvme_submit_admin_cmd(struct nvme_controller *nc,
                                 struct nvme_sqe *cmd,
                                 uint32_t timeout_ms)
{
    volatile struct nvme_sqe *sqe;
    volatile struct nvme_cqe *cqe;
    uint32_t db_stride, elapsed;
    uint16_t status;

    /* Doorbell stride in bytes: 4 << DSTRD */
    db_stride = 4u << nc->dstrd;

    /* Write command into SQ at tail */
    sqe = &nc->admin_sq[nc->admin_sq_tail];
    {
        const uint32_t *src = (const uint32_t *)cmd;
        volatile uint32_t *dst = (volatile uint32_t *)sqe;
        uint32_t i;
        for (i = 0; i < 16; i++)
            dst[i] = src[i];
    }

    /* Advance tail and ring SQ 0 tail doorbell.
     * wmb() ensures the SQ entry writes are globally visible before the
     * doorbell MMIO write.  Without this, the controller (or WHPX device
     * model) can read a stale/empty SQ entry → command never completes. */
    nc->admin_sq_tail = (nc->admin_sq_tail + 1) % NVME_ADMIN_QUEUE_DEPTH;
    wmb();
    nvme_write32(nc->mmio_base, 0x1000, nc->admin_sq_tail);

    /* Poll CQ for completion.
     * rmb() forces re-read of the CQE from RAM on each iteration —
     * without it, the CPU can cache the old status word and spin forever. */
    elapsed = 0;
    while (elapsed < timeout_ms) {
        rmb();
        cqe = &nc->admin_cq[nc->admin_cq_head];
        status = cqe->status;

        /* Phase bit is bit 0 of status word */
        if ((status & 1) == nc->admin_cq_phase)
            goto done;

        sleep_ms(1);
        elapsed++;
    }
    return -1;  /* timeout */

done:
    /* Check status (bits 15:1, shift right 1 to get SCT+SC) */
    if ((status >> 1) != 0) {
        klog(LOG_ERROR, "nvme", "admin cmd failed: status=0x%x",
             (uint64_t)(status >> 1));
        /* Still advance CQ head to keep queue in sync */
    }

    /* Advance CQ head, flip phase on wrap */
    nc->admin_cq_head = (nc->admin_cq_head + 1) % NVME_ADMIN_QUEUE_DEPTH;
    if (nc->admin_cq_head == 0)
        nc->admin_cq_phase ^= 1;

    /* Ring CQ 0 head doorbell */
    nvme_write32(nc->mmio_base, 0x1000 + db_stride, nc->admin_cq_head);

    return (status >> 1) != 0 ? -1 : 0;
}

/* ---- Identify Controller + Namespace ---- */

static void nvme_identify(struct nvme_controller *nc, uint32_t timeout_ms)
{
    struct nvme_sqe cmd;
    uintptr_t data_phys;
    uint8_t *data;
    uint32_t i;

    POST16(POST16_NVME_ADMIN);

    /* Allocate 4 KiB page for Identify data */
    data_phys = pmm_alloc_contiguous(1);
    if (!data_phys) {
        klog(LOG_WARN, "nvme", "failed to allocate Identify buffer");
        POST16(POST16_NVME_ADMIN_OK);
        return;
    }
    data = (uint8_t *)data_phys;

    /* ---- Identify Controller (CNS=1) ---- */
    nvme_memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;  /* opcode in bits 7:0 */
    cmd.nsid = 0;
    cmd.prp1 = (uint64_t)data_phys;
    cmd.cdw10 = NVME_IDENTIFY_CNS_CTRL;

    nvme_memset(data, 0, 4096);
    if (nvme_submit_admin_cmd(nc, &cmd, timeout_ms) != 0) {
        klog(LOG_WARN, "nvme", "Identify Controller timeout");
        POST16(POST16_NVME_ADMIN_OK);
        return;
    }

    /* Parse serial number (bytes 4–23, 20 chars) */
    for (i = 0; i < 20; i++)
        nc->serial[i] = (char)data[4 + i];
    nc->serial[20] = '\0';
    /* Trim trailing spaces */
    for (i = 19; i < 20 && nc->serial[i] == ' '; i--)
        nc->serial[i] = '\0';

    /* Parse model number (bytes 24–63, 40 chars) */
    for (i = 0; i < 40; i++)
        nc->model[i] = (char)data[24 + i];
    nc->model[40] = '\0';
    /* Trim trailing spaces */
    for (i = 39; i < 40 && nc->model[i] == ' '; i--)
        nc->model[i] = '\0';

    klog(LOG_DEBUG, "nvme", "Identify: serial=\"%s\" model=\"%s\"",
         nc->serial, nc->model);

    /* ---- Identify Namespace (CNS=0, NSID=1) ---- */
    nvme_memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 1;
    cmd.prp1 = (uint64_t)data_phys;
    cmd.cdw10 = NVME_IDENTIFY_CNS_NS;

    nvme_memset(data, 0, 4096);
    if (nvme_submit_admin_cmd(nc, &cmd, timeout_ms) != 0) {
        klog(LOG_WARN, "nvme", "Identify Namespace timeout");
        POST16(POST16_NVME_ADMIN_OK);
        return;
    }

    /* NSZE: namespace size in LBAs (bytes 0–7, 64-bit LE) */
    nc->ns_lba_count = *(uint64_t *)&data[0];

    /* FLBAS: formatted LBA size (byte 26, bits 3:0 = active format index) */
    {
        uint8_t flbas_idx = data[26] & 0x0F;
        /* LBAF[n] at offset 128 + n*4, DS field in bits 23:16 */
        uint32_t lbaf = *(uint32_t *)&data[128 + flbas_idx * 4];
        uint8_t ds = (uint8_t)((lbaf >> 16) & 0xFF);
        nc->ns_sector_size = (ds > 0) ? (1u << ds) : 512;
    }

    /* Log summary */
    {
        uint64_t total_bytes = nc->ns_lba_count * (uint64_t)nc->ns_sector_size;
        uint64_t gib = total_bytes / (1024ULL * 1024 * 1024);
        klog(LOG_INFO, "nvme", "\"%s\" %u GiB, %u-byte sectors, %llu LBAs",
             nc->model, gib, (uint64_t)nc->ns_sector_size,
             nc->ns_lba_count);
    }

    POST16(POST16_NVME_ADMIN_OK);
}

/* Forward declaration */
static int nvme_submit_io_cmd(struct nvme_controller *nc,
                              struct nvme_sqe *cmd,
                              uint32_t timeout_ms);

/* ---- I/O Queue creation ---- */

static int nvme_create_io_queues(struct nvme_controller *nc, uint32_t timeout_ms)
{
    struct nvme_sqe cmd;
    uint32_t db_stride = 4u << nc->dstrd;
    uint16_t io_depth;

    POST16(POST16_NVME_IO);

    /* Cap I/O queue depth to controller's MQES+1 */
    io_depth = NVME_IO_QUEUE_DEPTH;
    if (io_depth > (nc->mqes + 1))
        io_depth = nc->mqes + 1;

    /* Allocate I/O SQ and CQ pages */
    nc->io_sq_phys = pmm_alloc_contiguous(1);
    nc->io_cq_phys = pmm_alloc_contiguous(1);
    if (!nc->io_sq_phys || !nc->io_cq_phys) {
        klog(LOG_ERROR, "nvme", "failed to allocate I/O queues");
        POST16(POST16_NVME_IO_OK);
        return -1;
    }
    nc->io_sq = (volatile struct nvme_sqe *)nc->io_sq_phys;
    nc->io_cq = (volatile struct nvme_cqe *)nc->io_cq_phys;
    nvme_memset((void *)nc->io_sq_phys, 0, 4096);
    nvme_memset((void *)nc->io_cq_phys, 0, 4096);
    nc->io_sq_tail  = 0;
    nc->io_cq_head  = 0;
    nc->io_cq_phase = 1;

    /* ---- Create I/O Completion Queue (QID=1) ---- */
    nvme_memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_CREATE_IOCQ;
    cmd.prp1 = (uint64_t)nc->io_cq_phys;
    cmd.cdw10 = ((uint32_t)(io_depth - 1) << 16) | 1;  /* size[31:16] | QID[15:0] */
    cmd.cdw11 = (1 << 0);  /* PC=1 (physically contiguous) */

    if (nvme_submit_admin_cmd(nc, &cmd, timeout_ms) != 0) {
        klog(LOG_ERROR, "nvme", "Create I/O CQ failed");
        POST16(POST16_NVME_IO_OK);
        return -1;
    }

    /* ---- Create I/O Submission Queue (QID=1, CQID=1) ---- */
    nvme_memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_CREATE_IOSQ;
    cmd.prp1 = (uint64_t)nc->io_sq_phys;
    cmd.cdw10 = ((uint32_t)(io_depth - 1) << 16) | 1;  /* size[31:16] | QID[15:0] */
    cmd.cdw11 = (1 << 16) | (1 << 0);  /* CQID=1[31:16] | PC=1[0] */

    if (nvme_submit_admin_cmd(nc, &cmd, timeout_ms) != 0) {
        klog(LOG_ERROR, "nvme", "Create I/O SQ failed");
        POST16(POST16_NVME_IO_OK);
        return -1;
    }

    nc->io_queue_active = 1;

    klog(LOG_INFO, "nvme", "I/O Queue created (QID=1, depth=%u, db_stride=%u)",
         (uint64_t)io_depth, (uint64_t)db_stride);

    /* ---- Verify: read sector 0 ---- */
    {
        uintptr_t test_phys = pmm_alloc_contiguous(1);
        if (test_phys) {
            struct nvme_sqe rd;
            uint8_t *test_buf = (uint8_t *)test_phys;
            nvme_memset(test_buf, 0, 512);

            nvme_memset(&rd, 0, sizeof(rd));
            rd.cdw0 = NVME_IO_READ;
            rd.nsid = 1;
            rd.prp1 = (uint64_t)test_phys;
            rd.cdw10 = 0;  /* LBA 0 */
            rd.cdw11 = 0;
            rd.cdw12 = 0;  /* 1 sector (0-based) */

            if (nvme_submit_io_cmd(nc, &rd, timeout_ms) == 0) {
                /* Check for GPT/MBR signature */
                if (test_buf[510] == 0x55 && test_buf[511] == 0xAA)
                    klog(LOG_INFO, "nvme", "sector 0 read OK (MBR signature found)");
                else if (test_buf[0] == 0xEB || test_buf[0] == 0xE9)
                    klog(LOG_INFO, "nvme", "sector 0 read OK (FAT boot jump)");
                else
                    klog(LOG_INFO, "nvme", "sector 0 read OK (sig=0x%x 0x%x)",
                         (uint64_t)test_buf[510], (uint64_t)test_buf[511]);
            } else {
                klog(LOG_WARN, "nvme", "sector 0 read failed");
            }
            pmm_free_frame(test_phys);
        }
    }

    POST16(POST16_NVME_IO_OK);
    return 0;
}

/* ---- I/O command submission ---- */

static int nvme_submit_io_cmd(struct nvme_controller *nc,
                              struct nvme_sqe *cmd,
                              uint32_t timeout_ms)
{
    volatile struct nvme_sqe *sqe;
    volatile struct nvme_cqe *cqe;
    uint32_t db_stride, elapsed;
    uint16_t status;

    db_stride = 4u << nc->dstrd;

    /* Write command into I/O SQ at tail */
    sqe = &nc->io_sq[nc->io_sq_tail];
    {
        const uint32_t *src = (const uint32_t *)cmd;
        volatile uint32_t *dst = (volatile uint32_t *)sqe;
        uint32_t i;
        for (i = 0; i < 16; i++)
            dst[i] = src[i];
    }

    /* Advance tail and ring I/O SQ 1 tail doorbell */
    nc->io_sq_tail = (nc->io_sq_tail + 1) % NVME_IO_QUEUE_DEPTH;
    wmb();  /* SQ entry visible before doorbell */
    /* SQ y tail doorbell: 0x1000 + (2y * db_stride), y=1 for I/O QID 1 */
    nvme_write32(nc->mmio_base, 0x1000 + (2 * db_stride), nc->io_sq_tail);

    /* Poll I/O CQ for completion */
    elapsed = 0;
    while (elapsed < timeout_ms) {
        rmb();  /* re-read CQE from RAM */
        cqe = &nc->io_cq[nc->io_cq_head];
        status = cqe->status;

        if ((status & 1) == nc->io_cq_phase)
            goto done;

        sleep_ms(1);
        elapsed++;
    }
    return -1;  /* timeout */

done:
    if ((status >> 1) != 0) {
        klog(LOG_ERROR, "nvme", "I/O cmd failed: status=0x%x",
             (uint64_t)(status >> 1));
    }

    /* Advance CQ head, flip phase on wrap */
    nc->io_cq_head = (nc->io_cq_head + 1) % NVME_IO_QUEUE_DEPTH;
    if (nc->io_cq_head == 0)
        nc->io_cq_phase ^= 1;

    /* Ring I/O CQ 1 head doorbell: 0x1000 + ((2*1+1) * db_stride) */
    nvme_write32(nc->mmio_base, 0x1000 + (3 * db_stride), nc->io_cq_head);

    return (status >> 1) != 0 ? -1 : 0;
}

/* ---- Controller init ---- */

static int nvme_init_controller(uint8_t bus, uint8_t dev, uint8_t func)
{
    struct nvme_controller *nc;
    uint32_t bar0, bar1, vs, cc, csts;
    uint64_t cap, mmio_phys;
    uint32_t timeout_ms, elapsed;

    if (num_controllers >= NVME_MAX_CONTROLLERS) {
        klog(LOG_WARN, "nvme", "Too many controllers (max %u)",
             (uint64_t)NVME_MAX_CONTROLLERS);
        return -1;
    }

    nc = &controllers[num_controllers];

    /* ---- Read 64-bit BAR0/BAR1 ---- */
    bar0 = pci_read32(bus, dev, func, PCI_BAR0);
    bar1 = pci_read32(bus, dev, func, PCI_BAR1);

    if (bar0 & 0x01) {
        klog(LOG_ERROR, "nvme", "BAR0 is I/O space (expected memory)");
        return -1;
    }
    if (((bar0 >> 1) & 0x03) != 0x02) {
        klog(LOG_WARN, "nvme", "BAR0 is not 64-bit (type=%u), trying 32-bit",
             (uint64_t)((bar0 >> 1) & 0x03));
        bar1 = 0;
    }

    mmio_phys = (uint64_t)(bar0 & 0xFFFFFFF0) | ((uint64_t)bar1 << 32);
    if (mmio_phys == 0) {
        klog(LOG_ERROR, "nvme", "BAR0 is zero — no MMIO base");
        return -1;
    }

    /* ---- Enable PCI command register ---- */
    {
        uint16_t cmd = pci_read16(bus, dev, func, PCI_COMMAND);
        cmd |= PCI_CMD_BUS_MASTER;
        cmd |= PCI_CMD_MEM_SPACE;
        cmd |= PCI_CMD_INT_DISABLE;
        pci_write16(bus, dev, func, PCI_COMMAND, cmd);
    }

    /* ---- Map MMIO region (16 KiB for NVMe controller registers) ---- */
    nc->mmio_phys = mmio_phys;
    nc->mmio_size = 0x4000;
    {
        void *va = vmm_map_mmio_uc(mmio_phys, nc->mmio_size);
        if (!va) {
            klog(LOG_ERROR, "nvme", "vmm_map_mmio_uc failed for 0x%llx",
                 (uint64_t)mmio_phys);
            return -1;
        }
        nc->mmio_base = (volatile uint8_t *)va;
    }

    /* ---- Read CAP register (64-bit) ---- */
    cap = nvme_read64(nc->mmio_base, NVME_REG_CAP);
    nc->mqes   = (uint16_t)(cap & NVME_CAP_MQES_MASK);
    nc->to     = (uint8_t)((cap >> NVME_CAP_TO_SHIFT) & NVME_CAP_TO_MASK);
    nc->dstrd  = (uint8_t)((cap >> NVME_CAP_DSTRD_SHIFT) & NVME_CAP_DSTRD_MASK);
    nc->css    = (uint8_t)((cap >> NVME_CAP_CSS_SHIFT) & NVME_CAP_CSS_MASK);
    nc->mpsmin = (uint8_t)((cap >> NVME_CAP_MPSMIN_SHIFT) & NVME_CAP_MPSMIN_MASK);
    nc->mpsmax = (uint8_t)((cap >> NVME_CAP_MPSMAX_SHIFT) & NVME_CAP_MPSMAX_MASK);

    /* ---- Read VS register ---- */
    vs = nvme_read32(nc->mmio_base, NVME_REG_VS);
    nc->ver_major = (uint16_t)(vs >> 16);
    nc->ver_minor = (uint8_t)((vs >> 8) & 0xFF);
    nc->ver_ter   = (uint8_t)(vs & 0xFF);

    klog(LOG_INFO, "nvme", "Controller v%u.%u.%u at PCI %u:%u.%u, BAR0=0x%llx",
         (uint64_t)nc->ver_major, (uint64_t)nc->ver_minor,
         (uint64_t)nc->ver_ter,
         (uint64_t)bus, (uint64_t)dev, (uint64_t)func,
         (uint64_t)mmio_phys);

    klog(LOG_DEBUG, "nvme", "CAP: MQES=%u DSTRD=%u TO=%u CSS=0x%x MPSMIN=%u MPSMAX=%u",
         (uint64_t)nc->mqes, (uint64_t)nc->dstrd, (uint64_t)nc->to,
         (uint64_t)nc->css, (uint64_t)nc->mpsmin, (uint64_t)nc->mpsmax);

    /* Validate NVM command set support */
    if (!(nc->css & NVME_CAP_CSS_NVM)) {
        klog(LOG_ERROR, "nvme", "NVM command set not supported (CSS=0x%x)",
             (uint64_t)nc->css);
        return -1;
    }

    /* Timeout: CAP.TO * 500ms, minimum 500ms */
    timeout_ms = (uint32_t)nc->to * 500;
    if (timeout_ms < 500)
        timeout_ms = 500;

    /* ---- Controller disable (if currently enabled) ---- */
    cc = nvme_read32(nc->mmio_base, NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        cc &= ~NVME_CC_EN;
        nvme_write32(nc->mmio_base, NVME_REG_CC, cc);

        /* Wait for CSTS.RDY == 0 */
        elapsed = 0;
        while (elapsed < timeout_ms) {
            csts = nvme_read32(nc->mmio_base, NVME_REG_CSTS);
            if (!(csts & NVME_CSTS_RDY))
                break;
            if (csts & NVME_CSTS_CFS) {
                klog(LOG_ERROR, "nvme", "controller fatal status during disable");
                return -1;
            }
            sleep_ms(1);
            elapsed++;
        }
        if (elapsed >= timeout_ms) {
            klog(LOG_ERROR, "nvme", "controller disable timeout (%u ms)",
                 (uint64_t)timeout_ms);
            return -1;
        }
    }

    /* ---- Admin queue allocation (must happen while CC.EN=0) ---- */
    nc->admin_sq_phys = pmm_alloc_contiguous(1);
    nc->admin_cq_phys = pmm_alloc_contiguous(1);
    if (!nc->admin_sq_phys || !nc->admin_cq_phys) {
        klog(LOG_ERROR, "nvme", "failed to allocate admin queues");
        return -1;
    }
    nc->admin_sq = (volatile struct nvme_sqe *)nc->admin_sq_phys;
    nc->admin_cq = (volatile struct nvme_cqe *)nc->admin_cq_phys;
    nvme_memset((void *)nc->admin_sq_phys, 0, 4096);
    nvme_memset((void *)nc->admin_cq_phys, 0, 4096);
    nc->admin_sq_tail  = 0;
    nc->admin_cq_head  = 0;
    nc->admin_cq_phase = 1;

    /* Write AQA: ACQS[27:16] = depth-1, ASQS[11:0] = depth-1 */
    nvme_write32(nc->mmio_base, NVME_REG_AQA,
                 ((uint32_t)(NVME_ADMIN_QUEUE_DEPTH - 1) << 16) |
                  (uint32_t)(NVME_ADMIN_QUEUE_DEPTH - 1));
    /* Write ASQ and ACQ base addresses (64-bit) */
    nvme_write64(nc->mmio_base, NVME_REG_ASQ, (uint64_t)nc->admin_sq_phys);
    nvme_write64(nc->mmio_base, NVME_REG_ACQ, (uint64_t)nc->admin_cq_phys);

    klog(LOG_DEBUG, "nvme", "Admin SQ=0x%llx CQ=0x%llx depth=%u",
         (uint64_t)nc->admin_sq_phys, (uint64_t)nc->admin_cq_phys,
         (uint64_t)NVME_ADMIN_QUEUE_DEPTH);

    /* ---- Controller enable ---- */
    cc = NVME_CC_EN
       | NVME_CC_CSS_NVM
       | (0 << NVME_CC_MPS_SHIFT)          /* 4 KiB pages */
       | (0 << NVME_CC_AMS_SHIFT)          /* Round Robin */
       | (6 << NVME_CC_IOSQES_SHIFT)       /* 64-byte SQ entries (2^6) */
       | (4 << NVME_CC_IOCQES_SHIFT);      /* 16-byte CQ entries (2^4) */
    nvme_write32(nc->mmio_base, NVME_REG_CC, cc);

    /* Wait for CSTS.RDY == 1 */
    elapsed = 0;
    while (elapsed < timeout_ms) {
        csts = nvme_read32(nc->mmio_base, NVME_REG_CSTS);
        if (csts & NVME_CSTS_RDY)
            break;
        if (csts & NVME_CSTS_CFS) {
            klog(LOG_ERROR, "nvme", "controller fatal status during enable");
            return -1;
        }
        sleep_ms(1);
        elapsed++;
    }
    if (elapsed >= timeout_ms) {
        klog(LOG_ERROR, "nvme", "controller enable timeout (%u ms)",
             (uint64_t)timeout_ms);
        return -1;
    }

    /* Final CFS check */
    csts = nvme_read32(nc->mmio_base, NVME_REG_CSTS);
    if (csts & NVME_CSTS_CFS) {
        klog(LOG_ERROR, "nvme", "controller fatal status after enable");
        return -1;
    }

    nc->bus  = bus;
    nc->dev  = dev;
    nc->func = func;
    nc->active = 1;
    num_controllers++;

    klog(LOG_INFO, "nvme", "Controller enabled (MQES=%u, doorbell stride=%u)",
         (uint64_t)(nc->mqes + 1), (uint64_t)(4 << nc->dstrd));

    /* ---- Identify Controller + Namespace ---- */
    nvme_identify(nc, timeout_ms);

    /* ---- I/O Queue creation ---- */
    nvme_create_io_queues(nc, timeout_ms);

    return 0;
}

/* ---- Public API ---- */

int nvme_init(void)
{
    uint8_t bus, dev, func, hdr;

    num_controllers = 0;

    for (bus = 0; bus < 8; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (func = 0; func < 8; func++) {
                uint16_t vid = pci_read16(bus, dev, func, PCI_VENDOR_ID);
                if (vid == 0xFFFF)
                    continue;

                /* Multi-function check */
                if (func == 0) {
                    hdr = pci_read8(bus, dev, func, PCI_HEADER_TYPE);
                    if (!(hdr & 0x80)) {
                        /* Single-function — check func 0 only, then break */
                        uint8_t cls = pci_read8(bus, dev, 0, PCI_CLASS);
                        uint8_t sub = pci_read8(bus, dev, 0, PCI_SUBCLASS);
                        uint8_t pi  = pci_read8(bus, dev, 0, PCI_PROG_IF);
                        if (cls == NVME_PCI_CLASS &&
                            sub == NVME_PCI_SUBCLASS &&
                            pi  == NVME_PCI_PROG_IF) {
                            nvme_init_controller(bus, dev, 0);
                        }
                        break;
                    }
                }

                {
                    uint8_t cls = pci_read8(bus, dev, func, PCI_CLASS);
                    uint8_t sub = pci_read8(bus, dev, func, PCI_SUBCLASS);
                    uint8_t pi  = pci_read8(bus, dev, func, PCI_PROG_IF);
                    if (cls == NVME_PCI_CLASS &&
                        sub == NVME_PCI_SUBCLASS &&
                        pi  == NVME_PCI_PROG_IF) {
                        nvme_init_controller(bus, dev, func);
                    }
                }
            }
        }
    }

    if (num_controllers == 0)
        klog(LOG_DEBUG, "nvme", "no controller found");

    return num_controllers;
}

int nvme_controller_count(void)
{
    return num_controllers;
}

struct nvme_controller *nvme_get_controller(int idx)
{
    if (idx < 0 || idx >= num_controllers)
        return (struct nvme_controller *)0;
    return &controllers[idx];
}

/* ---- Sector I/O ---- */

int nvme_read_sectors(int ctrl_idx, uint64_t lba, uint32_t count, void *buf)
{
    struct nvme_controller *nc;
    struct nvme_sqe cmd;
    uint32_t timeout_ms, sectors_done, chunk;
    uintptr_t dma_phys;
    uint8_t *dma_buf;
    uint8_t *dst = (uint8_t *)buf;

    nc = nvme_get_controller(ctrl_idx);
    if (!nc || !nc->io_queue_active)
        return -1;

    timeout_ms = (uint32_t)nc->to * 500;
    if (timeout_ms < 500)
        timeout_ms = 500;

    /* Allocate a single DMA page (4096 bytes) for transfers */
    dma_phys = pmm_alloc_contiguous(1);
    if (!dma_phys)
        return -1;
    dma_buf = (uint8_t *)dma_phys;

    sectors_done = 0;
    while (sectors_done < count) {
        /* Max sectors per 4 KiB page */
        chunk = 4096 / nc->ns_sector_size;
        if (chunk > (count - sectors_done))
            chunk = count - sectors_done;

        nvme_memset(&cmd, 0, sizeof(cmd));
        cmd.cdw0 = NVME_IO_READ;
        cmd.nsid = 1;
        cmd.prp1 = (uint64_t)dma_phys;
        /* CDW10-11: Starting LBA (64-bit) */
        cmd.cdw10 = (uint32_t)(lba + sectors_done);
        cmd.cdw11 = (uint32_t)((lba + sectors_done) >> 32);
        /* CDW12: Number of Logical Blocks (0-based) */
        cmd.cdw12 = chunk - 1;

        if (nvme_submit_io_cmd(nc, &cmd, timeout_ms) != 0) {
            klog(LOG_ERROR, "nvme", "read failed: LBA=%llu count=%u",
                 lba + sectors_done, (uint64_t)chunk);
            pmm_free_frame(dma_phys);
            return -1;
        }

        /* Copy from DMA buffer to caller's buffer */
        {
            uint32_t bytes = chunk * nc->ns_sector_size;
            uint32_t j;
            for (j = 0; j < bytes; j++)
                dst[j] = dma_buf[j];
        }

        dst += chunk * nc->ns_sector_size;
        sectors_done += chunk;
    }

    pmm_free_frame(dma_phys);
    return 0;
}

int nvme_write_sectors(int ctrl_idx, uint64_t lba, uint32_t count,
                       const void *buf)
{
    struct nvme_controller *nc;
    struct nvme_sqe cmd;
    uint32_t timeout_ms, sectors_done, chunk;
    uintptr_t dma_phys;
    uint8_t *dma_buf;
    const uint8_t *src = (const uint8_t *)buf;

    nc = nvme_get_controller(ctrl_idx);
    if (!nc || !nc->io_queue_active)
        return -1;

    timeout_ms = (uint32_t)nc->to * 500;
    if (timeout_ms < 500)
        timeout_ms = 500;

    /* Allocate a single DMA page for transfers */
    dma_phys = pmm_alloc_contiguous(1);
    if (!dma_phys)
        return -1;
    dma_buf = (uint8_t *)dma_phys;

    sectors_done = 0;
    while (sectors_done < count) {
        chunk = 4096 / nc->ns_sector_size;
        if (chunk > (count - sectors_done))
            chunk = count - sectors_done;

        /* Copy from caller's buffer to DMA buffer */
        {
            uint32_t bytes = chunk * nc->ns_sector_size;
            uint32_t j;
            for (j = 0; j < bytes; j++)
                dma_buf[j] = src[j];
        }

        nvme_memset(&cmd, 0, sizeof(cmd));
        cmd.cdw0 = NVME_IO_WRITE;
        cmd.nsid = 1;
        cmd.prp1 = (uint64_t)dma_phys;
        cmd.cdw10 = (uint32_t)(lba + sectors_done);
        cmd.cdw11 = (uint32_t)((lba + sectors_done) >> 32);
        cmd.cdw12 = chunk - 1;

        if (nvme_submit_io_cmd(nc, &cmd, timeout_ms) != 0) {
            klog(LOG_ERROR, "nvme", "write failed: LBA=%llu count=%u",
                 lba + sectors_done, (uint64_t)chunk);
            pmm_free_frame(dma_phys);
            return -1;
        }

        src += chunk * nc->ns_sector_size;
        sectors_done += chunk;
    }

    pmm_free_frame(dma_phys);
    return 0;
}
