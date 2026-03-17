/* ============================================================================
 * klog_live.c — Live debug log to FAT32 X: partition
 *
 * When enabled (via DEBUG flag on X:), every klog() entry is immediately
 * written to X:\debug.log.  Since FAT32 vfs_write() is a full-file overwrite,
 * we maintain a growing PMM-backed buffer and rewrite the entire file on
 * each append.  This ensures that even if the system hangs, the last log
 * line before the hang is on disk.
 *
 * Buffer: 256 KB via pmm_alloc_contiguous (identity-mapped).
 * ============================================================================ */

#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/pmm.h"
#include "kernel/drivers/pit.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/rtl8139.h"
#include "kernel/boot_info.h"

/* ---- State ---- */

static uint8_t  *live_buf      = (void *)0;
static uint32_t  live_pos      = 0;
static uint32_t  live_buf_size = 0;
static int       live_enabled  = 0;
static int       live_flushing = 0;  /* reentrancy guard */

/* Level name strings */
static const char *lvl_str(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    case LOG_FATAL: return "FATAL";
    default:        return "?????";
    }
}

/* Simple uint32 → decimal into buf, returns chars written */
static int u32_dec(uint32_t val, char *buf, int max)
{
    char tmp[12];
    int i = 0, j, len;
    if (val == 0) { buf[0] = '0'; return 1; }
    while (val > 0 && i < 11) {
        tmp[i++] = '0' + (char)(val % 10);
        val /= 10;
    }
    len = i;
    if (len > max) len = max;
    for (j = 0; j < len; j++)
        buf[j] = tmp[len - 1 - j];
    return len;
}

/* Simple uint32 → hex into buf, returns chars written */
static int u32_hex(uint32_t val, char *buf, int max)
{
    const char hex[] = "0123456789ABCDEF";
    char tmp[12];
    int i = 0, j, len;
    if (val == 0) { buf[0] = '0'; return 1; }
    while (val > 0 && i < 8) {
        tmp[i++] = hex[val & 0xF];
        val >>= 4;
    }
    len = i;
    if (len > max) len = max;
    for (j = 0; j < len; j++)
        buf[j] = tmp[len - 1 - j];
    return len;
}

/* Simple uint64 → hex into buf, returns chars written */
static int u64_hex(uint64_t val, char *buf, int max)
{
    const char hex[] = "0123456789ABCDEF";
    char tmp[20];
    int i = 0, j, len;
    if (val == 0) { buf[0] = '0'; return 1; }
    while (val > 0 && i < 16) {
        tmp[i++] = hex[val & 0xF];
        val >>= 4;
    }
    len = i;
    if (len > max) len = max;
    for (j = 0; j < len; j++)
        buf[j] = tmp[len - 1 - j];
    return len;
}

/* ---- Append helpers ---- */

static void buf_putc(char c)
{
    if (live_pos < live_buf_size - 1)
        live_buf[live_pos++] = (uint8_t)c;
}

static void buf_puts(const char *s)
{
    while (*s && live_pos < live_buf_size - 1)
        live_buf[live_pos++] = (uint8_t)*s++;
}

static void buf_putu(uint32_t val)
{
    char tmp[12];
    int n = u32_dec(val, tmp, 12);
    int i;
    for (i = 0; i < n; i++)
        buf_putc(tmp[i]);
}

static void buf_puthex32(uint32_t val)
{
    buf_puts("0x");
    char tmp[12];
    int n = u32_hex(val, tmp, 8);
    int i;
    for (i = 0; i < n; i++)
        buf_putc(tmp[i]);
}

static void buf_puthex64(uint64_t val)
{
    buf_puts("0x");
    char tmp[20];
    int n = u64_hex(val, tmp, 16);
    int i;
    for (i = 0; i < n; i++)
        buf_putc(tmp[i]);
}

/* ---- Public API ---- */

void klog_live_enable(void)
{
    uint32_t pages = 64;  /* 64 * 4KB = 256KB */
    live_buf = (uint8_t *)pmm_alloc_contiguous(pages);
    if (!live_buf) return;

    live_buf_size = pages * 4096;
    live_pos = 0;
    live_enabled = 1;

    /* Create debug.log on X: */
    struct vfs_node *x_root = vfs_get_drive_root('X');
    if (x_root && x_root->ops && x_root->ops->create)
        x_root->ops->create(x_root, "debug.log", VFS_FILE);
}

int klog_live_active(void)
{
    return live_enabled;
}

/* Append a formatted klog entry to the live buffer */
void klog_live_append(const klog_entry_t *e)
{
    if (!live_enabled || !live_buf)
        return;

    /* Format: [timestamp] LEVEL subsystem: message\n */
    buf_putc('[');
    buf_putu(e->timestamp);
    buf_puts("] ");
    buf_puts(lvl_str(e->level));
    buf_putc(' ');
    if (e->subsystem && e->subsystem[0]) {
        buf_puts(e->subsystem);
        buf_puts(": ");
    }
    buf_puts(e->message);
    buf_putc('\n');
}

/* Flush the entire live buffer to X:\debug.log */
void klog_live_flush(void)
{
    struct vfs_node *f;

    if (!live_enabled || !live_buf || live_pos == 0)
        return;

    /* Reentrancy guard: vfs_write → klog → klog_live_flush → infinite loop */
    if (live_flushing)
        return;
    live_flushing = 1;

    f = vfs_open("X:\\debug.log", VFS_O_WRITE);
    if (f) {
        vfs_write(f, 0, live_pos, live_buf);
        vfs_close(f);
    }

    live_flushing = 0;
}

/* ---- Hardware info dump ---- */

/* CPUID helper */
static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                   uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

void klog_live_hw_dump(void)
{
    if (!live_enabled || !live_buf)
        return;

    buf_puts("\n");
    buf_puts("========================================================================\n");
    buf_puts("  HARDWARE INFO DUMP\n");
    buf_puts("========================================================================\n\n");

    /* ---- CPU ---- */
    {
        uint32_t eax, ebx, ecx, edx;
        char vendor[13];

        buf_puts("--- CPU ---\n");

        /* Vendor string */
        cpuid(0, &eax, &ebx, &ecx, &edx);
        {
            char *v = vendor;
            *(uint32_t *)(v + 0) = ebx;
            *(uint32_t *)(v + 4) = edx;
            *(uint32_t *)(v + 8) = ecx;
            v[12] = '\0';
        }
        buf_puts("  Vendor:  ");
        buf_puts(vendor);
        buf_putc('\n');

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
                buf_puts("  Brand:   ");
                /* Skip leading spaces */
                {
                    const char *p = brand;
                    while (*p == ' ') p++;
                    buf_puts(p);
                }
                buf_putc('\n');
            }
        }

        /* Feature flags */
        cpuid(1, &eax, &ebx, &ecx, &edx);
        buf_puts("  Family:  ");
        buf_putu(((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF));
        buf_puts("  Model: ");
        buf_putu(((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4));
        buf_puts("  Stepping: ");
        buf_putu(eax & 0xF);
        buf_putc('\n');
        buf_puts("  Features: ");
        if (edx & (1 << 0))  buf_puts("FPU ");
        if (edx & (1 << 4))  buf_puts("TSC ");
        if (edx & (1 << 5))  buf_puts("MSR ");
        if (edx & (1 << 6))  buf_puts("PAE ");
        if (edx & (1 << 9))  buf_puts("APIC ");
        if (edx & (1 << 23)) buf_puts("MMX ");
        if (edx & (1 << 25)) buf_puts("SSE ");
        if (edx & (1 << 26)) buf_puts("SSE2 ");
        if (edx & (1 << 28)) buf_puts("HTT ");
        if (ecx & (1 << 0))  buf_puts("SSE3 ");
        if (ecx & (1 << 9))  buf_puts("SSSE3 ");
        if (ecx & (1 << 19)) buf_puts("SSE4.1 ");
        if (ecx & (1 << 20)) buf_puts("SSE4.2 ");
        if (ecx & (1 << 25)) buf_puts("AES ");
        if (ecx & (1 << 28)) buf_puts("AVX ");
        buf_putc('\n');

        /* Extended features (AVX2, etc.) */
        cpuid(7, &eax, &ebx, &ecx, &edx);
        buf_puts("  Ext:     ");
        if (ebx & (1 << 5))  buf_puts("AVX2 ");
        if (ebx & (1 << 3))  buf_puts("BMI1 ");
        if (ebx & (1 << 8))  buf_puts("BMI2 ");
        if (ebx & (1 << 18)) buf_puts("RDSEED ");
        if (ebx & (1 << 19)) buf_puts("ADX ");
        buf_putc('\n');
    }

    buf_putc('\n');

    /* ---- Memory ---- */
    {
        uint64_t total = pmm_get_total_frames();
        uint64_t free  = pmm_get_free_frames();
        uint32_t total_mb = (uint32_t)((total * 4) / 1024);
        uint32_t free_mb  = (uint32_t)((free * 4) / 1024);

        buf_puts("--- Memory ---\n");
        buf_puts("  Total: ");
        buf_putu(total_mb);
        buf_puts(" MiB  Free: ");
        buf_putu(free_mb);
        buf_puts(" MiB  Frames: ");
        buf_putu((uint32_t)total);
        buf_putc('\n');
    }

    buf_putc('\n');

    /* ---- PCI Devices ---- */
    {
        uint8_t bus, dev, func;
        uint16_t vendor, device;
        uint8_t cls, sub, prog_if, irq, hdr;
        int count = 0;

        buf_puts("--- PCI Devices ---\n");
        buf_puts("  Bus:Dev.Fn  Vendor:Device  Class  Sub  PI  IRQ  BAR0             BAR1             Type\n");
        buf_puts("  " "---------- ------------- ------ ---- --- ---- ---------------- ---------------- ----------\n");

        for (bus = 0; bus < 255; bus++) {
            for (dev = 0; dev < 32; dev++) {
                for (func = 0; func < 8; func++) {
                    vendor = pci_read16(bus, dev, func, 0x00);
                    if (vendor == 0xFFFF)
                        continue;

                    device  = pci_read16(bus, dev, func, 0x02);
                    cls     = pci_read8(bus, dev, func, 0x0B);
                    sub     = pci_read8(bus, dev, func, 0x0A);
                    prog_if = pci_read8(bus, dev, func, 0x09);
                    irq     = pci_read8(bus, dev, func, 0x3C);

                    buf_puts("  ");
                    buf_putu(bus);   buf_putc(':');
                    buf_putu(dev);   buf_putc('.');
                    buf_putu(func);
                    /* Pad to 12 chars */
                    { int p = (bus > 99 ? 3 : bus > 9 ? 2 : 1) +
                              (dev > 9 ? 2 : 1) + 1 + 1;
                      while (p++ < 10) buf_putc(' '); }

                    buf_puthex32((uint32_t)vendor);  buf_putc(':');
                    buf_puthex32((uint32_t)device);  buf_puts("  ");

                    buf_puthex32(cls); buf_puts("   ");
                    buf_puthex32(sub); buf_puts(" ");
                    buf_puthex32(prog_if); buf_puts(" ");
                    buf_putu(irq);

                    /* BAR0 and BAR1 */
                    {
                        uint32_t bar0 = pci_read32(bus, dev, func, 0x10);
                        uint32_t bar1 = pci_read32(bus, dev, func, 0x14);
                        buf_puts("  ");
                        buf_puthex32(bar0);
                        buf_puts("  ");
                        buf_puthex32(bar1);
                    }

                    /* Class name */
                    buf_puts("  ");
                    switch (cls) {
                    case 0x01:
                        if (sub == 0x01) buf_puts("IDE");
                        else if (sub == 0x06) buf_puts("SATA");
                        else buf_puts("Storage");
                        break;
                    case 0x02: buf_puts("Network"); break;
                    case 0x03: buf_puts("Display"); break;
                    case 0x04: buf_puts("Multimedia"); break;
                    case 0x06:
                        if (sub == 0x00) buf_puts("Host Bridge");
                        else if (sub == 0x01) buf_puts("ISA Bridge");
                        else buf_puts("Bridge");
                        break;
                    case 0x0C:
                        if (sub == 0x03) buf_puts("USB");
                        else buf_puts("Serial Bus");
                        break;
                    default: buf_puts("Other"); break;
                    }
                    buf_putc('\n');
                    count++;

                    if (func == 0) {
                        hdr = pci_read8(bus, dev, func, 0x0E);
                        if (!(hdr & 0x80))
                            break;
                    }
                }
            }
        }
        buf_puts("  Total: ");
        buf_putu((uint32_t)count);
        buf_puts(" devices\n");
    }

    buf_putc('\n');

    /* ---- ACPI ---- */
    {
        extern uint32_t acpi_get_cpu_count(void);
        extern uint32_t acpi_get_lapic_base(void);

        buf_puts("--- ACPI ---\n");
        buf_puts("  CPUs:       ");
        buf_putu(acpi_get_cpu_count());
        buf_putc('\n');
        buf_puts("  LAPIC base: ");
        buf_puthex32(acpi_get_lapic_base());
        buf_putc('\n');
    }

    buf_putc('\n');

    /* ---- AHCI / Storage ---- */
    {
        buf_puts("--- Storage ---\n");
        if (ahci_present()) {
            int drives = ahci_drive_count();
            buf_puts("  AHCI: ");
            buf_putu((uint32_t)drives);
            buf_puts(" drive(s)\n");
            {
                int pi;
                for (pi = 0; pi < drives && pi < 32; pi++) {
                    uint64_t cap = ahci_capacity(pi);
                    if (cap > 0) {
                        buf_puts("    Drive ");
                        buf_putu((uint32_t)pi);
                        buf_puts(": ");
                        buf_putu((uint32_t)(cap / 2048));
                        buf_puts(" MiB (");
                        buf_putu((uint32_t)cap);
                        buf_puts(" sectors)\n");
                    }
                }
            }
        } else {
            buf_puts("  AHCI: not present\n");
        }
    }

    buf_putc('\n');

    /* ---- Display ---- */
    {
        extern struct boot_info g_boot_info;
        buf_puts("--- Display ---\n");
        if (g_boot_info.fb_available) {
            buf_puts("  Resolution: ");
            buf_putu(g_boot_info.fb.width);
            buf_putc('x');
            buf_putu(g_boot_info.fb.height);
            buf_putc('x');
            buf_putu(g_boot_info.fb.bpp);
            buf_putc('\n');
            buf_puts("  Framebuffer: ");
            buf_puthex64(g_boot_info.fb.addr);
            buf_puts("  Pitch: ");
            buf_putu(g_boot_info.fb.pitch);
            buf_putc('\n');
        } else {
            buf_puts("  No framebuffer available\n");
        }
    }

    buf_putc('\n');

    /* ---- Network ---- */
    {
        uint8_t mac[6];
        rtl8139_get_mac(mac);

        buf_puts("--- Network ---\n");
        /* If MAC is all zeros, NIC wasn't detected */
        if (mac[0] || mac[1] || mac[2] || mac[3] || mac[4] || mac[5]) {
            buf_puts("  RTL8139 MAC: ");
            {
                int mi;
                for (mi = 0; mi < 6; mi++) {
                    char h[3];
                    const char hex[] = "0123456789ABCDEF";
                    h[0] = hex[(mac[mi] >> 4) & 0xF];
                    h[1] = hex[mac[mi] & 0xF];
                    h[2] = '\0';
                    buf_puts(h);
                    if (mi < 5) buf_putc(':');
                }
            }
            buf_putc('\n');
        } else {
            buf_puts("  No NIC detected\n");
        }
    }

    buf_puts("\n========================================================================\n\n");

    /* Flush the hardware dump immediately */
    klog_live_flush();
}
