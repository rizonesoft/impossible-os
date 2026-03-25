/* ============================================================================
 * boot_desktop.c -- Desktop environment initialization
 *
 * TrueType font manager, icon store, cursor manager, window manager,
 * desktop (wallpaper + taskbar), boot splash finish, demo window,
 * terminal, gallery, shell loader, heap stats.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/printk.h"
#include "kernel/klog.h"
#include "kernel/mm/heap.h"
#include "kernel/timer.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/sched/task.h"
#include "kernel/boot_splash.h"
#include "desktop/wm.h"
#include "desktop/font.h"
#include "font_mgr.h"
#include "icon_store.h"
#include "gfx.h"
#include "cursor.h"
#include "desktop/desktop.h"
#include "desktop/terminal.h"
#include "desktop/gallery.h"
#include "kernel/fs/vfs.h"
#include "kernel/elf.h"
#include "main/main_internal.h"

void boot_desktop_init(void)
{
    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Desktop ----------------------------------------------------------------");

    /* Flush boot log to disk */
    boot_splash_status("Flushing boot log...");
    klog_disk_flush();

#ifdef BSOD_TEST
    /* Test trigger: fire a deliberate panic to test the BSOD screen */
    {
        extern void panic_screen(struct interrupt_frame *frame,
                                 uint64_t error_code,
                                 const char *description,
                                 const char *file, uint32_t line);
        panic_screen((struct interrupt_frame *)0, 0xDEAD,
                     "BSOD_TEST: Deliberate panic for testing",
                     __FILE__, __LINE__);
    }
#endif


    boot_splash_tick();
    boot_splash_status("Loading fonts...");

    /* Initialize TrueType font manager */
    ttf_mgr_init();

    /* Initialize icon store */
    boot_splash_tick();
    boot_splash_status("Loading resources...");
    icon_store_init();

    /* Initialize cursor manager */
    cursor_init();

    klog(LOG_DEBUG, "boot", "--- Phase: desktop & WM ---");
    boot_splash_tick();
    boot_splash_status("Almost ready...");
    wm_init();

    /* Initialize desktop (wallpaper, taskbar) */
    desktop_init();

    /* Finish boot splash */
    boot_splash_finish();

    /* Boot complete timing marker */
    {
        uint64_t ms = system_get_ticks() * 10;
        klog(LOG_INFO, "boot",
             "Boot complete in %u.%03us (PIT uptime from interrupt init)",
             (uint64_t)(ms / 1000), (uint64_t)(ms % 1000));
    }

    /* Boot-time heap stats */
    {
        uint64_t h_used  = heap_get_used();
        uint64_t h_total = heap_get_total();
        uint64_t pct     = h_total ? (h_used * 100) / h_total : 0;
        printk("[OK] Heap: %u KB used / %u KB total (%u%%)\n",
               (h_used + 1023) / 1024,
               (h_total + 1023) / 1024,
               pct);
        if (pct > 75) {
            printk("[!!] Heap pressure: %u%% used -- risk of silent exhaustion\n",
                   pct);
        }
    }

    /* Create a demo window with icon toolbar */
    {
        int demo = wm_create_window("Welcome", 100, 80, 460, 300,
                                     WM_DEFAULT_FLAGS);
        if (demo >= 0) {
            uint32_t *fb = wm_get_framebuffer(demo);
            uint32_t cw = wm_get_client_width(demo);
            uint32_t ch = wm_get_client_height(demo);
            if (fb && cw && ch) {
                gfx_surface_t ws;
                ttf_font_t *fnt;
                gfx_surface_init(&ws, fb, cw, ch, cw);

                /* Dark background */
                wm_fill_rect(demo, 0, 0, cw, ch, 0xFF202020);

                /* Toolbar strip (8 icons, 20px, Filled variant) */
                {
                    static const system_icon_t toolbar_icons[] = {
                        ICON_CUT, ICON_COPY, ICON_PASTE,
                        ICON_UNDO, ICON_REDO, ICON_SAVE,
                        ICON_SEARCH, ICON_SETTINGS
                    };
                    int icon_count = 8;
                    int icon_size  = 20;
                    int padding    = 8;
                    int toolbar_y  = 8;
                    int ix;
                    uint32_t toolbar_bg = 0xFF2D2D2D;
                    uint32_t icon_color = 0xFFFFFFFF;

                    gfx_fill_rect(&ws, 0, 0, cw,
                                   icon_size + padding * 2,
                                   toolbar_bg);

                    for (ix = 0; ix < icon_count; ix++) {
                        icon_bitmap_t *bmp = icon_get_variant(
                            toolbar_icons[ix], icon_size,
                            icon_color, ICON_FONT_REGULAR);
                        if (bmp) {
                            int icon_x =
                                padding + ix * (icon_size + padding);
                            icon_draw(&ws, bmp, icon_x, toolbar_y);
                        }
                    }

                    gfx_fill_rect(&ws, 0,
                                   icon_size + padding * 2,
                                   cw, 1, 0xFF383838);
                }

                /* Greeting text below toolbar */
                fnt = ttf_get(FONT_UI_BOLD, 20);
                if (fnt)
                    ttf_draw_string(&ws, fnt, 20, 56,
                                    "Welcome to Impossible OS!",
                                    0xFF60CDFF);

                /* Subtitle */
                fnt = ttf_get(FONT_UI, 14);
                if (fnt)
                    ttf_draw_string(&ws, fnt, 20, 86,
                                    "Fluent System Icons loaded from TTF",
                                    0xFFB0B0B0);
            }
        }
    }

    terminal_open();
    gallery_open();

    /* Load cmd.exe synchronously during boot — before the compositor
     * starts and takes over PID 0.  This avoids depending on the
     * preemptive scheduler to run ShellLoader (which doesn't work
     * reliably because compositor's scheduler_disable/enable cycle
     * prevents quantum accumulation). */
    if (vfs_is_mounted('C')) {
        struct vfs_node *file = vfs_open("C:\\cmd.exe", VFS_O_READ);
        if (file) {
            uint8_t *buf = (uint8_t *)kmalloc(file->size);
            if (buf) {
                struct elf_load_result elf;
                vfs_read(file, 0, (uint32_t)file->size, buf);
                vfs_close(file);
                elf = elf_load(buf, file->size);
                kfree(buf);
                if (elf.success) {
                    task_create_user((task_entry_t)elf.entry, "cmd.exe");
                    klog(LOG_DEBUG, "boot",
                         "cmd.exe loaded: entry %p", elf.entry);
                } else {
                    klog(LOG_WARN, "boot", "cmd.exe ELF load failed");
                }
            } else {
                vfs_close(file);
                klog(LOG_ERROR, "boot", "Cannot allocate buffer for cmd.exe");
            }
        } else {
            klog(LOG_WARN, "boot", "cmd.exe not found on C:\\");
        }
    }

    scheduler_enable();

    /* Yield repeatedly to give cmd.exe (PID 6) CPU time to print its
     * banner and reach its readline() blocking point.  This runs once
     * during boot — before compositor_run() starts its tight event loop
     * which prevents the preemptive scheduler from ever switching to
     * PID 6 on fast systems like QEMU.  On each yield, PID 6 runs
     * until it blocks (AHCI I/O, SYS_READ), then control returns here.
     * Once PID 6 blocks on keyboard input, further yields are no-ops. */
    {
        int i;
        for (i = 0; i < 200; i++)
            yield();
    }
}
