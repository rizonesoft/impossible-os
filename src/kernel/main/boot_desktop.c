/* ============================================================================
 * boot_desktop.c -- Phase 3: User Platform
 *
 * Scheduler, IPC, exec loader, boot tests (debug-only), fonts, icons,
 * cursors, window manager, desktop, cmd.exe, compositor.
 * The kernel is fully operational before this phase; failures fall back
 * to a text console, not BSOD.
 *
 * Provides: boot_phase3() and the legacy boot_desktop_init() wrapper.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/mm/heap.h"
#include "kernel/timer.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/ahci.h"
#include "kernel/sched/task.h"
#include "kernel/sched/workqueue.h"
#include "kernel/sched/syscall.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_timing.h"
#include "kernel/boot_init.h"
#include "kernel/boot_info.h"
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

/* ---- Phase 3 ------------------------------------------------------------ */

void boot_phase3(void)
{
    /* --- Scheduler init --- */
    boot_splash_status("Initializing scheduler...");
    task_init();
    kernel_subsystem_set_ready(SUBSYS_SCHED, true);
    boot_progress(3, "SCHED", POSTCODE_SCHED_INIT);

    ahci_enable_events();  /* safe now: yield handler registered */

    /* Create the system work queue (needed by NIC driver) */
    {
        extern workqueue_t *sys_wq;
        scheduler_enable();
        sys_wq = workqueue_create("sys_wq");
        scheduler_disable();
        if (sys_wq)
            klog(LOG_DEBUG, "wq", "sys_wq created");
        else
            klog(LOG_ERROR, "wq", "sys_wq creation FAILED");
    }

    /* --- IPC init (pipe, shmem, signal) --- */
    /* IPC subsystem is initialized implicitly by the kernel;
     * pipe_create/shmem_create work after heap + sched are up.
     * Mark ready for dependency tracking. */
    kernel_subsystem_set_ready(SUBSYS_IPC, true);
    boot_progress(3, "IPC", POSTCODE_IPC_INIT);

    /* --- Syscall handler --- */
    boot_splash_status("Initializing syscalls...");
    syscall_init();

    /* --- Exec loader (ELF/PE format handlers) --- */
    kernel_subsystem_set_ready(SUBSYS_EXEC, true);
    boot_progress(3, "EXEC", POSTCODE_EXEC_INIT);

    /* --- Boot tests (debug=1 only) --- */
    boot_tests_run();

    /* --- Fonts, icons, cursors --- */
    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Desktop ----------------------------------------------------------------");

    boot_splash_status("Flushing boot log...");
    klog_disk_flush();

#ifdef BSOD_TEST
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
    ttf_mgr_init();

    boot_splash_tick();
    boot_splash_status("Loading resources...");
    icon_store_init();

    cursor_init();

    /* --- Window manager + desktop --- */
    klog(LOG_DEBUG, "boot", "--- Phase: desktop & WM ---");
    boot_splash_tick();
    boot_splash_status("Almost ready...");
    wm_init();
    desktop_init();

    /* Finish boot splash */
    boot_splash_finish();

    /* Boot complete timing */
    {
        uint64_t ms = system_get_ticks() * 10;
        klog(LOG_INFO, "boot",
             "Boot complete in %u.%03us (PIT uptime from interrupt init)",
             (uint64_t)(ms / 1000), (uint64_t)(ms % 1000));
    }

    boot_progress(3, "DESKTOP_READY", POSTCODE_DESKTOP_INIT);
    kernel_subsystem_set_ready(SUBSYS_DESKTOP, true);
    boot_timing_print_steps();
    boot_timing_write_report();

    /* Boot-time heap stats */
    {
        uint64_t h_used  = heap_get_used();
        uint64_t h_total = heap_get_total();
        uint64_t pct     = h_total ? (h_used * 100) / h_total : 0;
        klog(LOG_INFO, "heap", "Heap: %u KB used / %u KB total (%u%%)",
               (h_used + 1023) / 1024,
               (h_total + 1023) / 1024,
               pct);
        if (pct > 75)
            klog(LOG_WARN, "heap",
                 "Heap pressure: %u%% used -- risk of silent exhaustion", pct);
    }

    /* --- Demo window --- */
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

                wm_fill_rect(demo, 0, 0, cw, ch, 0xFF202020);

                /* Toolbar strip */
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

                fnt = ttf_get(FONT_UI_BOLD, 20);
                if (fnt)
                    ttf_draw_string(&ws, fnt, 20, 56,
                                    "Welcome to Impossible OS!",
                                    0xFF60CDFF);

                fnt = ttf_get(FONT_UI, 14);
                if (fnt)
                    ttf_draw_string(&ws, fnt, 20, 86,
                                    "Fluent System Icons loaded from TTF",
                                    0xFFB0B0B0);
            }
        }
    }

    /* --- Terminal + Gallery --- */
    terminal_open();
    gallery_open();

    /* --- Load cmd.exe --- */
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

    /* Yield to give cmd.exe CPU time to print its banner */
    {
        int i;
        for (i = 0; i < 200; i++)
            yield();
    }

    /* --- Compositor event loop (never returns) --- */
    compositor_run();

    /* Unreachable under normal operation */
    klog(LOG_FATAL, "boot", "compositor_run() returned -- halting");
    for (;;)
        __asm__ volatile ("hlt");
}

/* ---- Legacy wrapper ----------------------------------------------------- */

void boot_desktop_init(void)
{
    boot_phase3();
}
