/* ============================================================================
 * hw_dump.c — Structured hardware information dump
 *
 * Writes a hardware inventory to the kernel log via klog().
 * Extracted from klog_live.c to separate concerns.
 * ============================================================================ */

#include "kernel/hw_dump.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/rtl8139.h"
#include "kernel/boot_info.h"

/* ---- CPUID helper ---- */

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                   uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

/* ---- Hex formatting helpers ---- */

static void u32_to_hex(uint32_t val, char *out)
{
    const char hex[] = "0123456789ABCDEF";
    int i;
    out[0] = '0'; out[1] = 'x';
    for (i = 7; i >= 0; i--) {
        out[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    out[10] = '\0';
}

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

    /* ---- CPU ---- */
    {
        uint32_t eax, ebx, ecx, edx;
        char vendor[13];

        cpuid(0, &eax, &ebx, &ecx, &edx);
        {
            char *v = vendor;
            *(uint32_t *)(v + 0) = ebx;
            *(uint32_t *)(v + 4) = edx;
            *(uint32_t *)(v + 8) = ecx;
            v[12] = '\0';
        }
        klog(LOG_INFO, "hwdump", "CPU vendor: %s", vendor);

        /* Brand string (CPUID 0x80000002-0x80000004) */
        {
            uint32_t max_ext;
            cpuid(0x80000000, &max_ext, &ebx, &ecx, &edx);
            if (max_ext >= 0x80000004) {
                char brand[49];
                uint32_t *b = (uint32_t *)brand;
                uint32_t leaf;
                for (leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
                    cpuid(leaf, &eax, &ebx, &ecx, &edx);
                    *b++ = eax; *b++ = ebx; *b++ = ecx; *b++ = edx;
                }
                brand[48] = '\0';
                /* Skip leading spaces */
                {
                    const char *p = brand;
                    while (*p == ' ') p++;
                    klog(LOG_INFO, "hwdump", "CPU brand: %s", p);
                }
            }
        }

        /* Family/Model/Stepping */
        cpuid(1, &eax, &ebx, &ecx, &edx);
        klog(LOG_INFO, "hwdump", "CPU family=%u model=%u stepping=%u",
             ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF),
             ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4),
             eax & 0xF);

        /* Feature flags — log key features */
        klog(LOG_INFO, "hwdump",
             "CPU features: %s%s%s%s%s%s%s%s%s%s%s%s%s%s",
             (edx & (1 << 0))  ? "FPU " : "",
             (edx & (1 << 4))  ? "TSC " : "",
             (edx & (1 << 9))  ? "APIC " : "",
             (edx & (1 << 23)) ? "MMX " : "",
             (edx & (1 << 25)) ? "SSE " : "",
             (edx & (1 << 26)) ? "SSE2 " : "",
             (edx & (1 << 28)) ? "HTT " : "",
             (ecx & (1 << 0))  ? "SSE3 " : "",
             (ecx & (1 << 9))  ? "SSSE3 " : "",
             (ecx & (1 << 19)) ? "SSE4.1 " : "",
             (ecx & (1 << 20)) ? "SSE4.2 " : "",
             (ecx & (1 << 25)) ? "AES " : "",
             (ecx & (1 << 28)) ? "AVX " : "",
             "");

        /* Extended features */
        cpuid(7, &eax, &ebx, &ecx, &edx);
        klog(LOG_INFO, "hwdump", "CPU ext: %s%s%s%s%s",
             (ebx & (1 << 5))  ? "AVX2 " : "",
             (ebx & (1 << 3))  ? "BMI1 " : "",
             (ebx & (1 << 8))  ? "BMI2 " : "",
             (ebx & (1 << 18)) ? "RDSEED " : "",
             (ebx & (1 << 19)) ? "ADX " : "");
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
                             "PCI %u:%u.%u %x:%x class=%x/%x BAR0=%x IRQ=%u %s",
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

        klog(LOG_INFO, "hwdump", "ACPI: %u CPUs, LAPIC base %x",
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
            klog(LOG_INFO, "hwdump", "Display: %ux%ux%u pitch=%u addr=%x",
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

    (void)u32_to_hex;  /* suppress unused warning — kept for future use */
}
