/* ============================================================================
 * kernel_main — Kernel entry point
 *
 * Called from entry.asm in 64-bit Long Mode.
 * Initializes serial, framebuffer, parses boot info, prints system details.
 * ============================================================================ */

#include "types.h"
#include "multiboot2.h"
#include "boot_info.h"
#include "serial.h"
#include "framebuffer.h"
#include "printk.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"

/* External: Multiboot2 parser */
extern void multiboot2_parse(uintptr_t mbi_addr);

/* Kernel entry point */
void kernel_main(uint64_t magic, uint64_t mbi)
{
    uint32_t i;
    uint64_t total_ram = 0;

    /* Step 1: Initialize serial (always works, even without display) */
    serial_init();

    /* Step 2: Verify Multiboot2 magic */
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        serial_write("[FAIL] Invalid Multiboot2 magic!\n");
        goto halt;
    }

    /* Step 3: Parse Multiboot2 info structure */
    multiboot2_parse((uintptr_t)mbi);

    /* Step 4: Initialize physical memory manager (needs parsed memory map) */
    pmm_init();

    /* Step 5: Initialize framebuffer (needs parsed boot info) */
    fb_init();

    /* Step 5: Load proper GDT with kernel/user segments and TSS */
    gdt_init();

    /* Step 6: Load IDT with exception and IRQ handlers */
    idt_init();

    /* Step 7: Remap PIC so hardware IRQs don't conflict with CPU exceptions */
    pic_init();

    /* Step 8: Start PIT system timer */
    pit_init();

    /* Step 9: Initialize PS/2 keyboard */
    keyboard_init();

    /* Step 10: Enable interrupts */
    __asm__ volatile ("sti");

    /* Step 8: Print boot banner */
    fb_set_color(FB_COLOR_CYAN, FB_COLOR_BG_DEFAULT);
    printk("\n");
    printk("  ================================================\n");
    printk("      Impossible OS\n");
    printk("  ================================================\n");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("  Architecture: x86-64 (Long Mode)\n");
    printk("  Boot: UEFI via GRUB + Multiboot2\n");
    printk("\n");

    /* Step 6: Print status */
    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("[OK] ");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("Multiboot2 magic verified: %x\n", magic);

    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("[OK] ");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("Running in 64-bit Long Mode\n");

    if (g_boot_info.fb_available) {
        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("Framebuffer: %ux%ux%u at %p\n",
               (uint64_t)g_boot_info.fb.width,
               (uint64_t)g_boot_info.fb.height,
               (uint64_t)g_boot_info.fb.bpp,
               (uint64_t)g_boot_info.fb.addr);
    }

    /* Step 7: Memory map */
    printk("\n");
    fb_set_color(FB_COLOR_YELLOW, FB_COLOR_BG_DEFAULT);
    printk("--- Memory Map (%u entries) ---\n", (uint64_t)g_boot_info.mmap_count);
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);

    for (i = 0; i < g_boot_info.mmap_count; i++) {
        if (g_boot_info.mmap[i].type == 1) {
            total_ram += g_boot_info.mmap[i].length;
        }
    }
    printk("  Total available RAM: %u MiB\n",
           (uint64_t)(total_ram / (1024 * 1024)));

    /* Step 8: ACPI */
    if (g_boot_info.acpi_available) {
        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("ACPI RSDP v%u at %p\n",
               (uint64_t)g_boot_info.acpi_version,
               g_boot_info.acpi_rsdp_addr);
    }

    /* Timer verification */
    printk("\n  Timer test: sleeping 1 second...\n");
    sleep_ms(1000);
    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("[OK] ");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("Timer working! Ticks: %u, Uptime: %u sec\n",
           pit_get_ticks(), uptime());

    /* Keyboard echo loop */
    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("\n  Impossible OS kernel loaded successfully!\n");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("\n> ");
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n') {
            printk("\n> ");
        } else {
            printk("%c", (int)c);
        }
    }

halt:
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
