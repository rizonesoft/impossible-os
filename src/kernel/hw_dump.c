/* ============================================================================
 * hw_dump.c -- Structured hardware information dump
 *
 * Writes a hardware inventory to the kernel log via klog().
 * Extracted from klog_live.c to separate concerns.
 * ============================================================================ */

#include "kernel/hw_dump.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/cpuid.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/heap.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/rtl8139.h"
#include "kernel/boot_info.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/fs/vfs.h"
#include "kernel/smp.h"
#include "libc/string.h"

/* ---- Hex formatting helpers ---- */

static void mac_to_str(const uint8_t *mac, char *out)
{
    const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < 6; i++) {
        out[i * 3]     = hex[(mac[i] >> 4) & 0xF];
        out[i * 3 + 1] = hex[mac[i] & 0xF];
        out[i * 3 + 2] = (i < 5) ? ':' : '\0';
    }
    out[17] = '\0';
}

/* ---- Public API ---- */

void hw_dump_to_log(void)
{
    klog(LOG_INFO, "hwdump",
         "======================================================================");
    klog(LOG_INFO, "hwdump", "  HARDWARE INFO DUMP");
    klog(LOG_INFO, "hwdump",
         "======================================================================");

    /* ---- CPU (from centralized CPUID) ---- */
    {
        const struct cpu_features *cpu = cpuid_get();

        /* Skip leading spaces in brand */
        const char *brand = cpu->brand;
        while (*brand == ' ') brand++;

        klog(LOG_INFO, "hwdump", "CPU vendor: %s", cpu->vendor);
        if (brand[0])
            klog(LOG_INFO, "hwdump", "CPU brand: %s", brand);
        klog(LOG_INFO, "hwdump", "CPU family=%u model=%u stepping=%u",
             (uint64_t)cpu->family, (uint64_t)cpu->model,
             (uint64_t)cpu->stepping);

        klog(LOG_INFO, "hwdump",
             "CPU features: %s%s%s%s%s%s%s%s%s%s%s%s%s%s",
             cpu_has(CPU_FEATURE_FPU)    ? "FPU " : "",
             cpu_has(CPU_FEATURE_TSC)    ? "TSC " : "",
             cpu_has(CPU_FEATURE_APIC)   ? "APIC " : "",
             cpu_has(CPU_FEATURE_MMX)    ? "MMX " : "",
             cpu_has(CPU_FEATURE_SSE)    ? "SSE " : "",
             cpu_has(CPU_FEATURE_SSE2)   ? "SSE2 " : "",
             cpu_has(CPU_FEATURE_HTT)    ? "HTT " : "",
             cpu_has(CPU_FEATURE_SSE3)   ? "SSE3 " : "",
             cpu_has(CPU_FEATURE_SSSE3)  ? "SSSE3 " : "",
             cpu_has(CPU_FEATURE_SSE4_1) ? "SSE4.1 " : "",
             cpu_has(CPU_FEATURE_SSE4_2) ? "SSE4.2 " : "",
             cpu_has(CPU_FEATURE_AES)    ? "AES " : "",
             cpu_has(CPU_FEATURE_AVX)    ? "AVX " : "",
             "");

        klog(LOG_INFO, "hwdump", "CPU ext: %s%s%s%s%s",
             cpu_has(CPU_FEATURE_AVX2)   ? "AVX2 " : "",
             cpu_has(CPU_FEATURE_BMI1)   ? "BMI1 " : "",
             cpu_has(CPU_FEATURE_BMI2)   ? "BMI2 " : "",
             cpu_has(CPU_FEATURE_RDSEED) ? "RDSEED " : "",
             cpu_has(CPU_FEATURE_ADX)    ? "ADX " : "");
    }

    /* ---- Memory ---- */
    {
        uint64_t total = pmm_get_total_frames();
        uint64_t free  = pmm_get_free_frames();
        uint32_t total_mb = (uint32_t)((total * 4) / 1024);
        uint32_t free_mb  = (uint32_t)((free * 4) / 1024);
        klog(LOG_INFO, "hwdump", "Memory: %u MiB total, %u MiB free, %u frames",
             total_mb, free_mb, (uint32_t)total);
    }

    /* ---- PCI Devices ---- */
    {
        uint8_t bus, dev, func;
        uint16_t vendor, device;
        uint8_t cls, sub, hdr;
        int count = 0;

        klog(LOG_INFO, "hwdump", "--- PCI Devices ---");
        for (bus = 0; bus < 255; bus++) {
            for (dev = 0; dev < 32; dev++) {
                for (func = 0; func < 8; func++) {
                    vendor = pci_read16(bus, dev, func, 0x00);
                    if (vendor == 0xFFFF)
                        continue;

                    device = pci_read16(bus, dev, func, 0x02);
                    cls    = pci_read8(bus, dev, func, 0x0B);
                    sub    = pci_read8(bus, dev, func, 0x0A);

                    {
                        uint32_t bar0 = pci_read32(bus, dev, func, 0x10);
                        uint8_t irq   = pci_read8(bus, dev, func, 0x3C);
                        const char *type = "Other";
                        switch (cls) {
                        case 0x01:
                            type = (sub == 0x06) ? "SATA" :
                                   (sub == 0x01) ? "IDE" : "Storage";
                            break;
                        case 0x02: type = "Network"; break;
                        case 0x03: type = "Display"; break;
                        case 0x04: type = "Multimedia"; break;
                        case 0x06:
                            type = (sub == 0x00) ? "Host Bridge" :
                                   (sub == 0x01) ? "ISA Bridge" : "Bridge";
                            break;
                        case 0x0C: type = (sub == 0x03) ? "USB" : "SerialBus"; break;
                        }
                        klog(LOG_DEBUG, "hwdump",
                             "PCI %u:%u.%u 0x%x:%x class=0x%x/%x BAR0=0x%x IRQ=%u %s",
                             (uint32_t)bus, (uint32_t)dev, (uint32_t)func,
                             (uint32_t)vendor, (uint32_t)device,
                             (uint32_t)cls, (uint32_t)sub,
                             bar0, (uint32_t)irq, type);
                    }
                    count++;

                    if (func == 0) {
                        hdr = pci_read8(bus, dev, func, 0x0E);
                        if (!(hdr & 0x80))
                            break;
                    }
                }
            }
        }
        klog(LOG_INFO, "hwdump", "PCI: %u devices found", (uint32_t)count);
    }

    /* ---- ACPI ---- */
    {
        extern uint32_t acpi_get_cpu_count(void);
        extern uint32_t acpi_get_lapic_base(void);

        klog(LOG_INFO, "hwdump", "ACPI: %u CPUs, LAPIC base 0x%x",
             acpi_get_cpu_count(), acpi_get_lapic_base());
    }

    /* ---- AHCI / Storage ---- */
    {
        if (ahci_present()) {
            int drives = ahci_drive_count();
            int pi;
            klog(LOG_INFO, "hwdump", "AHCI: %u drive(s)", (uint32_t)drives);
            for (pi = 0; pi < drives && pi < 32; pi++) {
                uint64_t cap = ahci_capacity(pi);
                if (cap > 0) {
                    klog(LOG_INFO, "hwdump", "  Drive %u: %u MiB (%u sectors)",
                         (uint32_t)pi,
                         (uint32_t)(cap / 2048),
                         (uint32_t)cap);
                }
            }
        } else {
            klog(LOG_INFO, "hwdump", "AHCI: not present");
        }
    }

    /* ---- Display ---- */
    {
        extern struct boot_info g_boot_info;
        if (g_boot_info.fb_available) {
            klog(LOG_INFO, "hwdump", "Display: %ux%ux%u pitch=%u addr=0x%x",
                 g_boot_info.fb.width, g_boot_info.fb.height,
                 g_boot_info.fb.bpp, g_boot_info.fb.pitch,
                 (uint32_t)g_boot_info.fb.addr);
        } else {
            klog(LOG_INFO, "hwdump", "Display: no framebuffer");
        }
    }

    /* ---- Network ---- */
    {
        uint8_t mac[6];
        rtl8139_get_mac(mac);
        if (mac[0] || mac[1] || mac[2] || mac[3] || mac[4] || mac[5]) {
            char mac_str[18];
            mac_to_str(mac, mac_str);
            klog(LOG_INFO, "hwdump", "Network: RTL8139 MAC=%s", mac_str);
        } else {
            klog(LOG_INFO, "hwdump", "Network: no NIC detected");
        }
    }

    klog(LOG_INFO, "hwdump",
         "======================================================================");

    /* Flush immediately so hardware info is on disk */
    klog_disk_flush();
}

/* ---- Write standalone hwdump.txt to X:\Diag\ ---- */

/* Append a line to the buffer. Returns new position. */
static uint32_t hw_line(char *buf, uint32_t pos, uint32_t max,
                         const char *line)
{
    int i;
    for (i = 0; line[i] && pos < max - 2; i++)
        buf[pos++] = line[i];
    buf[pos++] = '\n';
    return pos;
}

void hw_dump_write_file(void)
{
    extern int klog_using_blackbox;
    extern const char *klog_dir;
    const char *diag_dir = klog_using_blackbox ? "X:\\Diag\\" : klog_dir;
    char path[64];
    int pi = 0, j;
    struct vfs_node *f;
    char *buf;
    uint32_t pos = 0;
    uint32_t max_sz = 4096;

    for (j = 0; diag_dir[j]; j++) path[pi++] = diag_dir[j];
    { const char *fn = "hwdump.txt";
      for (j = 0; fn[j]; j++) path[pi++] = fn[j]; }
    path[pi] = '\0';

    buf = (char *)kmalloc(max_sz);
    if (!buf) return;

    pos = hw_line(buf, pos, max_sz, "Impossible OS -- Hardware Inventory");
    pos = hw_line(buf, pos, max_sz, "===================================");

    /* CPU */
    {
        const struct cpu_features *cpu = cpuid_get();
        pos = hw_line(buf, pos, max_sz, "");
        pos = hw_line(buf, pos, max_sz, "[CPU]");
        pos = hw_line(buf, pos, max_sz, cpu->brand);
        {
            char line[80];
            /* Hardware INVENTORY, so the discovered slot count (TODO-10
             * S21): a dump that lists fewer logical CPUs after one parks is
             * describing the run, not the machine. */
            snprintf(line, sizeof(line), "Cores: %u, Logical: %u",
                     cpu->num_cores, smp_cpu_present_count());
            pos = hw_line(buf, pos, max_sz, line);
        }
        {
            char line[80];
            snprintf(line, sizeof(line), "Features: %s%s%s%s%s%s%s",
                     cpu_has(CPU_FEATURE_SSE2) ? "SSE2 " : "",
                     cpu_has(CPU_FEATURE_SSE4_2) ? "SSE4.2 " : "",
                     cpu_has(CPU_FEATURE_AVX) ? "AVX " : "",
                     cpu_has(CPU_FEATURE_AVX2) ? "AVX2 " : "",
                     cpu_has(CPU_FEATURE_AES) ? "AES " : "",
                     cpu_has(CPU_FEATURE_RDRAND) ? "RDRAND " : "",
                     cpu_has(CPU_FEATURE_XSAVE) ? "XSAVE" : "");
            pos = hw_line(buf, pos, max_sz, line);
        }
    }

    /* Memory */
    {
        char line[80];
        pos = hw_line(buf, pos, max_sz, "");
        pos = hw_line(buf, pos, max_sz, "[Memory]");
        snprintf(line, sizeof(line), "Total: %u MiB (%u frames)",
                 (uint32_t)(pmm_get_total_frames() * 4 / 1024),
                 (uint32_t)pmm_get_total_frames());
        pos = hw_line(buf, pos, max_sz, line);
    }

    /* Display */
    {
        char line[80];
        pos = hw_line(buf, pos, max_sz, "");
        pos = hw_line(buf, pos, max_sz, "[Display]");
        snprintf(line, sizeof(line), "Framebuffer: %ux%u %ubpp",
                 g_boot_info.fb.width, g_boot_info.fb.height,
                 g_boot_info.fb.bpp);
        pos = hw_line(buf, pos, max_sz, line);
    }

    /* Storage */
    {
        pos = hw_line(buf, pos, max_sz, "");
        pos = hw_line(buf, pos, max_sz, "[Storage]");
        {
            char line[80];
            snprintf(line, sizeof(line), "Block devices: %d",
                     blkdev_count());
            pos = hw_line(buf, pos, max_sz, line);
        }
    }

    /* Create file via parent dir to avoid FAT32 dir cache re-walk bug */
    {
        struct vfs_node *dir = vfs_open(diag_dir, VFS_O_READ);
        if (dir) {
            if (dir->ops && dir->ops->create)
                dir->ops->create(dir, "hwdump.txt", VFS_FILE);
            vfs_close(dir);   /* release the parent-dir ref (was leaked) */
        }
    }
    /* Truncate-on-open so a shorter inventory cannot leave stale tail bytes
     * from a prior longer boot's hwdump.txt. */
    f = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (f) {
        int wr = vfs_write(f, 0, pos, (const uint8_t *)buf);
        if (wr < 0 || (uint32_t)wr != pos || vfs_flush(f) != 0) {
            klog(LOG_WARN, "hwdump",
                 "hwdump.txt write/flush failed (%d of %u) -- artifact may be partial",
                 (int64_t)wr, (uint64_t)pos);
        } else {
            klog(LOG_INFO, "hwdump", "Hardware inventory written to %s", path);
        }
        vfs_close(f);
    } else {
        klog(LOG_WARN, "hwdump", "hwdump.txt: cannot open %s for write", path);
    }

    kfree(buf);
}
