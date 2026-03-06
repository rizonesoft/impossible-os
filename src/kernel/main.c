/* ============================================================================
 * kernel_main — Kernel entry point
 *
 * Called from entry.asm after Multiboot2 magic verification.
 * Parses boot info, prints system details to serial.
 * ============================================================================ */

#include "types.h"
#include "multiboot2.h"
#include "boot_info.h"

#define SERIAL_PORT 0x3F8

/* --- Port I/O helpers --- */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* --- Serial output --- */

static void serial_init(void)
{
    outb(SERIAL_PORT + 1, 0x00);    /* Disable interrupts */
    outb(SERIAL_PORT + 3, 0x80);    /* Enable DLAB */
    outb(SERIAL_PORT + 0, 0x03);    /* 38400 baud */
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);    /* 8N1 */
    outb(SERIAL_PORT + 2, 0xC7);    /* FIFO */
    outb(SERIAL_PORT + 4, 0x0B);    /* IRQs, RTS/DSR */
}

static void serial_putchar(char c)
{
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0)
        ;
    outb(SERIAL_PORT, (uint8_t)c);
}

static void serial_write(const char *str)
{
    while (*str) {
        if (*str == '\n')
            serial_putchar('\r');
        serial_putchar(*str++);
    }
}

/* Simple hex printer for 64-bit values */
static void serial_write_hex(uint64_t val)
{
    const char hex[] = "0123456789ABCDEF";
    char buf[19]; /* "0x" + 16 hex digits + null */
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[18] = '\0';
    serial_write(buf);
}

/* Simple decimal printer */
static void serial_write_dec(uint32_t val)
{
    char buf[12];
    int i = 0;

    if (val == 0) {
        serial_write("0");
        return;
    }

    while (val > 0) {
        buf[i++] = '0' + (char)(val % 10);
        val /= 10;
    }

    /* Reverse and print */
    while (i > 0) {
        serial_putchar(buf[--i]);
    }
}

/* Memory type to string */
static const char *mmap_type_str(uint32_t type)
{
    switch (type) {
    case 1:  return "Available";
    case 2:  return "Reserved";
    case 3:  return "ACPI Reclaimable";
    case 4:  return "NVS";
    case 5:  return "Bad RAM";
    default: return "Unknown";
    }
}

/* --- External: Multiboot2 parser --- */
extern void multiboot2_parse(uintptr_t mbi_addr);

/* --- Kernel entry point --- */

void kernel_main(uint64_t magic, uint64_t mbi)
{
    uint32_t i;
    uint64_t total_ram = 0;

    serial_init();

    serial_write("\n");
    serial_write("================================================\n");
    serial_write("  Impossible OS Kernel\n");
    serial_write("  Architecture: x86-64 (Long Mode)\n");
    serial_write("  Boot: UEFI via GRUB + Multiboot2\n");
    serial_write("================================================\n\n");

    /* Verify Multiboot2 magic (also checked in entry.asm) */
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        serial_write("[FAIL] Invalid Multiboot2 magic: ");
        serial_write_hex(magic);
        serial_write("\n");
        goto halt;
    }
    serial_write("[OK] Multiboot2 magic verified: ");
    serial_write_hex(magic);
    serial_write("\n");

    /* Parse Multiboot2 info structure */
    serial_write("[OK] Multiboot2 info at: ");
    serial_write_hex(mbi);
    serial_write("\n");

    /* Verify we're in 64-bit Long Mode */
    serial_write("[OK] Running in 64-bit Long Mode\n");

    multiboot2_parse((uintptr_t)mbi);

    /* --- Memory map --- */
    serial_write("\n--- Memory Map (");
    serial_write_dec(g_boot_info.mmap_count);
    serial_write(" entries) ---\n");

    for (i = 0; i < g_boot_info.mmap_count; i++) {
        serial_write("  ");
        serial_write_hex((uint32_t)g_boot_info.mmap[i].base_addr);
        serial_write(" - ");
        serial_write_hex((uint32_t)(g_boot_info.mmap[i].base_addr +
                                     g_boot_info.mmap[i].length - 1));
        serial_write("  [");
        serial_write(mmap_type_str(g_boot_info.mmap[i].type));
        serial_write("]  ");
        serial_write_dec((uint32_t)(g_boot_info.mmap[i].length / 1024));
        serial_write(" KiB\n");

        if (g_boot_info.mmap[i].type == 1) {
            total_ram += g_boot_info.mmap[i].length;
        }
    }

    serial_write("  Total available RAM: ");
    serial_write_dec((uint32_t)(total_ram / (1024 * 1024)));
    serial_write(" MiB\n");

    /* --- Framebuffer --- */
    serial_write("\n--- Framebuffer ---\n");
    if (g_boot_info.fb_available) {
        serial_write("  Address: ");
        serial_write_hex(g_boot_info.fb.addr);
        serial_write("\n  Resolution: ");
        serial_write_dec(g_boot_info.fb.width);
        serial_write("x");
        serial_write_dec(g_boot_info.fb.height);
        serial_write("x");
        serial_write_dec(g_boot_info.fb.bpp);
        serial_write("\n  Pitch: ");
        serial_write_dec(g_boot_info.fb.pitch);
        serial_write(" bytes/line\n");
        serial_write("  Type: ");
        serial_write_dec(g_boot_info.fb.type);
        serial_write(g_boot_info.fb.type == 1 ? " (RGB)\n" : "\n");
    } else {
        serial_write("  Not available\n");
    }

    /* --- ACPI --- */
    serial_write("\n--- ACPI ---\n");
    if (g_boot_info.acpi_available) {
        serial_write("  RSDP at: ");
        serial_write_hex(g_boot_info.acpi_rsdp_addr);
        serial_write("\n  Version: ");
        serial_write_dec(g_boot_info.acpi_version);
        serial_write("\n");
    } else {
        serial_write("  Not available\n");
    }

    serial_write("\n[OK] Boot info parsed successfully\n");
    serial_write("Halting.\n");

halt:
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
