/* ============================================================================
 * desktop.c — Desktop shell (wallpaper, taskbar, start menu)
 *
 * - Loads wallpaper.raw from C:\ (IXFS system partition) via VFS
 * - Draws a taskbar at the bottom with start button, window list, clock
 * - Draws a start menu popup with app launcher items
 * - Copies background images to C:\Documents\backgrounds\ on IXFS
 * ============================================================================ */

#include "desktop/desktop.h"
#include "kernel/drivers/framebuffer.h"
#include "desktop/font.h"       /* bitmap font — kept for early boot fallback */
#include "font_mgr.h"            /* TrueType fonts — primary rendering */
#include "kernel/fs/vfs.h"
#include "kernel/drivers/pit.h"
#include "kernel/drivers/rtc.h"
#include "desktop/wm.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/printk.h"
#include "kernel/sched/task.h"
#include "kernel/acpi.h"
#include "desktop/terminal.h"

/* ---- Wallpaper pixel buffer ---- */
static uint32_t *wallpaper_buf;       /* PMM-allocated (identity-mapped) */
static uint8_t   wallpaper_loaded;    /* 1 if wallpaper was loaded successfully */

/* ---- Start icon ---- */
static uint32_t *start_icon_buf;      /* Heap-allocated from VFS read */
static uint8_t   start_icon_loaded;

/* ---- Start menu state ---- */
static uint8_t start_menu_open;
static uint8_t prev_left;          /* Previous left-button state for edge */

/* ---- Menu items ---- */
#define MENU_ITEM_COUNT 3

static const char *menu_items[MENU_ITEM_COUNT] = {
    "Terminal",
    "About",
    "Shutdown"
};

/* Icon characters for menu items (rendered as colored text) */
static const char menu_icons[MENU_ITEM_COUNT] = {
    '>', 'i', 'X'
};

static const uint32_t menu_icon_colors[MENU_ITEM_COUNT] = {
    0x0044FF88,  /* green for Terminal */
    0x004488FF,  /* blue for About */
    0x00FF5555,  /* red for Shutdown */
};

/* ---- Forward declarations ---- */
static void load_wallpaper(void);
static void load_start_icon(void);

/* TrueType text helpers — wrap screen back buffer as gfx_surface_t */
static void ttf_screen_text(int32_t x, int32_t y, const char *text,
                            int font_slot, int px_size, gfx_color_t color);
static int  ttf_screen_width(const char *text, int font_slot, int px_size);

/* ---- String helpers ---- */
static uint32_t slen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ---- VFS file loader (reads entire file into heap buffer) ---- */

static uint8_t *vfs_load_file(const char *path, uint32_t *out_size)
{
    struct vfs_node *f;
    uint8_t *buf;
    int n;

    f = vfs_open(path, VFS_O_READ);
    if (!f)
        return (uint8_t *)0;

    if (f->size == 0) {
        vfs_close(f);
        return (uint8_t *)0;
    }

    buf = (uint8_t *)kmalloc(f->size);
    if (!buf) {
        vfs_close(f);
        return (uint8_t *)0;
    }

    n = vfs_read(f, 0, (uint32_t)f->size, buf);
    vfs_close(f);

    if (n <= 0) {
        kfree(buf);
        return (uint8_t *)0;
    }

    *out_size = (uint32_t)n;
    return buf;
}

/* ---- Initialization ---- */

void desktop_init(void)
{
    wallpaper_buf    = (uint32_t *)0;
    wallpaper_loaded = 0;
    start_icon_buf   = (uint32_t *)0;
    start_icon_loaded = 0;
    start_menu_open  = 0;
    prev_left        = 0;

    /* Load the wallpaper image from C:\ (IXFS) */
    load_wallpaper();

    /* Load start button icon from C:\ */
    load_start_icon();
}

/* ---- Wallpaper loading (from C:\ via VFS) ---- */

static void load_wallpaper(void)
{
    struct vfs_node *f;
    uint32_t expected_size;
    uint32_t pages_needed;
    uintptr_t base;

    expected_size = WALLPAPER_WIDTH * WALLPAPER_HEIGHT * 4;
    pages_needed = (expected_size + 4095) / 4096;

    /* Check if file exists */
    f = vfs_open("C:\\wallpaper.raw", VFS_O_READ);
    if (!f) {
        printk("[DESKTOP] wallpaper.raw not found on C:\\\n");
        return;
    }

    if (f->size < expected_size) {
        printk("[DESKTOP] wallpaper.raw too small (%u < %u)\n",
               (uint64_t)f->size, (uint64_t)expected_size);
        vfs_close(f);
        return;
    }

    /* Allocate contiguous physical frames (identity-mapped, bypasses heap) */
    base = pmm_alloc_contiguous(pages_needed);
    if (!base) {
        printk("[DESKTOP] wallpaper: cannot allocate %u contiguous frames\n",
               (uint64_t)pages_needed);
        vfs_close(f);
        return;
    }

    /* Read wallpaper from disk in chunks into PMM buffer */
    {
        uint32_t offset = 0;
        uint8_t *buf = (uint8_t *)base;
        while (offset < expected_size) {
            uint32_t chunk = expected_size - offset;
            int n;
            if (chunk > 4096) chunk = 4096;
            n = vfs_read(f, offset, chunk, buf + offset);
            if (n <= 0) break;
            offset += (uint32_t)n;
        }
    }
    vfs_close(f);

    wallpaper_buf = (uint32_t *)base;
    wallpaper_loaded = 1;
    printk("[OK] Desktop wallpaper loaded (%ux%u, %u bytes)\n",
           (uint64_t)WALLPAPER_WIDTH, (uint64_t)WALLPAPER_HEIGHT,
           (uint64_t)expected_size);
}

/* ---- Start icon loading (from C:\ via VFS) ---- */

static void load_start_icon(void)
{
    uint32_t icon_size = 0;
    uint8_t *data;
    uint32_t expected = START_ICON_SIZE * START_ICON_SIZE * 4;

    data = vfs_load_file("C:\\start_icon.raw", &icon_size);
    if (data && icon_size >= expected) {
        start_icon_buf = (uint32_t *)data;
        start_icon_loaded = 1;
        printk("[OK] Start icon loaded (%ux%u, %u bytes)\n",
               (uint64_t)START_ICON_SIZE, (uint64_t)START_ICON_SIZE,
               (uint64_t)icon_size);
    } else if (data) {
        kfree(data);
    }
}

/* ---- TrueType text helpers (screen framebuffer) ---- */

static void ttf_screen_text(int32_t x, int32_t y, const char *text,
                            int font_slot, int px_size, gfx_color_t color)
{
    gfx_surface_t scr;
    ttf_font_t *f = ttf_get(font_slot, px_size);
    if (!f) return;  /* TTF not loaded — silent skip */
    gfx_surface_init(&scr, fb_get_backbuffer(),
                     fb_get_width(), fb_get_height(), fb_get_stride());
    ttf_draw_string(&scr, f, x, y, text, color);
}

static int ttf_screen_width(const char *text, int font_slot, int px_size)
{
    ttf_font_t *f = ttf_get(font_slot, px_size);
    if (!f) return (int)(slen(text) * 8);  /* fallback estimate */
    return ttf_measure_width(f, text);
}

/* ---- Drawing functions ---- */

void desktop_draw_wallpaper(void)
{
    if (wallpaper_loaded && wallpaper_buf) {
        fb_blit(0, 0, wallpaper_buf,
                WALLPAPER_WIDTH, WALLPAPER_HEIGHT, WALLPAPER_WIDTH);
    } else {
        /* Fallback: gradient background */
        uint32_t y;
        uint32_t w = fb_get_width();
        uint32_t h = fb_get_height();
        for (y = 0; y < h; y++) {
            /* Dark blue to dark purple gradient */
            uint32_t r = 0x10 + (y * 0x10) / h;
            uint32_t g = 0x10 + (y * 0x08) / h;
            uint32_t b = 0x20 + (y * 0x20) / h;
            uint32_t color = (r << 16) | (g << 8) | b;
            fb_fill_rect(0, y, w, 1, color);
        }
    }
}

void desktop_draw_taskbar(void)
{
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();
    uint32_t ty = sh - TASKBAR_HEIGHT;  /* taskbar top Y */
    uint32_t i;

    /* Taskbar background */
    fb_fill_rect(0, ty, sw, TASKBAR_HEIGHT, TASKBAR_COLOR);

    /* Top border line */
    fb_fill_rect(0, ty, sw, 1, TASKBAR_BORDER);

    /* ---- Start button ---- */
    {
        uint32_t btn_x = 4;
        uint32_t btn_pad = 4;  /* padding inside taskbar (top/bottom) */
        uint32_t btn_h = TASKBAR_HEIGHT - btn_pad * 2;
        uint32_t btn_y = ty + btn_pad;
        uint32_t btn_color = start_menu_open ? START_BTN_HOVER : START_BTN_COLOR;

        /* Button background */
        fb_fill_rect(btn_x, btn_y, START_BTN_WIDTH, btn_h, btn_color);

        /* Button border */
        fb_draw_rect(btn_x, btn_y, START_BTN_WIDTH, btn_h, TASKBAR_BORDER);

        /* Subtle highlight on top edge for 3D effect */
        fb_fill_rect(btn_x + 1, btn_y + 1, START_BTN_WIDTH - 2, 1, 0x004A4A7C);

        if (start_icon_loaded && start_icon_buf) {
            /* Center the 32x32 icon inside the button */
            uint32_t icon_x = btn_x + (START_BTN_WIDTH - START_ICON_SIZE) / 2;
            uint32_t icon_y = btn_y + (btn_h - START_ICON_SIZE) / 2;
            uint32_t ix, iy;

            for (iy = 0; iy < START_ICON_SIZE; iy++) {
                for (ix = 0; ix < START_ICON_SIZE; ix++) {
                    uint32_t pixel = start_icon_buf[iy * START_ICON_SIZE + ix];
                    uint8_t a = (pixel >> 24) & 0xFF;
                    if (a == 0) continue;

                    uint32_t sx = icon_x + ix;
                    uint32_t sy = icon_y + iy;
                    if (sx >= sw || sy >= sh) continue;

                    if (a == 0xFF) {
                        fb_put_pixel(sx, sy, pixel & 0x00FFFFFF);
                    } else {
                        /* Alpha blend with button background */
                        uint8_t sr = (pixel >> 16) & 0xFF;
                        uint8_t sg = (pixel >> 8) & 0xFF;
                        uint8_t sb = pixel & 0xFF;
                        uint8_t dr = (btn_color >> 16) & 0xFF;
                        uint8_t dg = (btn_color >> 8) & 0xFF;
                        uint8_t db = btn_color & 0xFF;
                        uint8_t or_ = (sr * a + dr * (255 - a)) / 255;
                        uint8_t og  = (sg * a + dg * (255 - a)) / 255;
                        uint8_t ob  = (sb * a + db * (255 - a)) / 255;
                        fb_put_pixel(sx, sy, ((uint32_t)or_ << 16) |
                                             ((uint32_t)og << 8) | ob);
                    }
                }
            }
        } else {
            /* Fallback: text label */
            ttf_screen_text((int32_t)(btn_x + 8),
                            (int32_t)(btn_y + (btn_h - 14) / 2),
                            "Start", FONT_UI_BOLD, 14, START_BTN_TEXT);
        }
    }

    /* ---- Window list ---- */
    {
        uint32_t list_x = START_BTN_WIDTH + 10;
        uint32_t list_max_x = sw - 120;  /* Reserve space for clock */
        uint32_t btn_w = 100;
        extern struct wm_window windows[];  /* Declared in wm.c */

        for (i = 0; i < WM_MAX_WINDOWS && list_x + btn_w <= list_max_x; i++) {
            if (!windows[i].active)
                continue;

            /* Window button on taskbar */
            uint32_t btn_bg = (windows[i].flags & WM_FLAG_FOCUSED)
                              ? 0x003A3A6C : 0x002A2A4C;
            uint32_t btn_fg = (windows[i].flags & WM_FLAG_FOCUSED)
                              ? WINLIST_ACTIVE : WINLIST_COLOR;

            fb_fill_rect(list_x, ty + 3, btn_w, TASKBAR_HEIGHT - 6, btn_bg);
            fb_draw_rect(list_x, ty + 3, btn_w, TASKBAR_HEIGHT - 6, TASKBAR_BORDER);

            /* Draw window title with TrueType */
            ttf_screen_text((int32_t)(list_x + 4),
                            (int32_t)(ty + (TASKBAR_HEIGHT - 12) / 2),
                            windows[i].title, FONT_UI, 12, btn_fg);

            list_x += btn_w + 4;
        }
    }

    /* ---- Clock (RTC real time) ---- */
    {
        struct rtc_time rtc;
        rtc_read(&rtc);

        /* Format HH:MM */
        char clock_buf[6];
        clock_buf[0] = '0' + (char)(rtc.hour / 10);
        clock_buf[1] = '0' + (char)(rtc.hour % 10);
        clock_buf[2] = ':';
        clock_buf[3] = '0' + (char)(rtc.minute / 10);
        clock_buf[4] = '0' + (char)(rtc.minute % 10);
        clock_buf[5] = '\0';

        int clock_w = ttf_screen_width(clock_buf, FONT_UI, 14);
        uint32_t clock_x = sw - (uint32_t)clock_w - 12;

        ttf_screen_text((int32_t)clock_x,
                        (int32_t)(ty + (TASKBAR_HEIGHT - 14) / 2),
                        clock_buf, FONT_UI, 14, CLOCK_COLOR);
    }
}

void desktop_draw_start_menu(void)
{
    uint32_t sh = fb_get_height();
    uint32_t menu_h;
    uint32_t menu_x, menu_y;
    uint32_t i;

    if (!start_menu_open)
        return;

    menu_h = MENU_ITEM_COUNT * MENU_ITEM_HEIGHT + 8;  /* 4px padding top/bottom */
    menu_x = 2;
    menu_y = sh - TASKBAR_HEIGHT - menu_h;

    /* Menu background */
    fb_fill_rect(menu_x, menu_y, MENU_WIDTH, menu_h, MENU_BG);

    /* Border */
    fb_draw_rect(menu_x, menu_y, MENU_WIDTH, menu_h, MENU_BORDER);

    /* Draw each menu item */
    for (i = 0; i < MENU_ITEM_COUNT; i++) {
        uint32_t item_y = menu_y + 4 + i * MENU_ITEM_HEIGHT;

        /* Icon character */
        {
            char icon_str[2] = { menu_icons[i], '\0' };
            ttf_screen_text((int32_t)(menu_x + 12),
                            (int32_t)(item_y + (MENU_ITEM_HEIGHT - 14) / 2),
                            icon_str, FONT_UI_BOLD, 14,
                            menu_icon_colors[i]);
        }

        /* Label */
        ttf_screen_text((int32_t)(menu_x + 32),
                        (int32_t)(item_y + (MENU_ITEM_HEIGHT - 14) / 2),
                        menu_items[i], FONT_UI, 14, MENU_TEXT);

        /* Separator line (except after last item) */
        if (i < MENU_ITEM_COUNT - 1) {
            fb_fill_rect(menu_x + 8, item_y + MENU_ITEM_HEIGHT - 1,
                         MENU_WIDTH - 16, 1, MENU_BORDER);
        }
    }
}

/* ---- Click handling ---- */

int desktop_handle_click(int32_t mx, int32_t my, uint8_t buttons)
{
    uint32_t sh = fb_get_height();
    uint32_t ty = sh - TASKBAR_HEIGHT;
    uint8_t left_now = buttons & 0x01;
    uint8_t left_pressed = left_now && !prev_left;
    prev_left = left_now;

    if (!left_pressed)
        return 0;

    /* Click on start button? */
    if (my >= (int32_t)ty && mx >= 2 && mx < (int32_t)(2 + START_BTN_WIDTH)) {
        start_menu_open = !start_menu_open;
        wm_mark_dirty();
        return 1;
    }

    /* Click on start menu item? */
    if (start_menu_open) {
        uint32_t menu_h = MENU_ITEM_COUNT * MENU_ITEM_HEIGHT + 8;
        uint32_t menu_y = sh - TASKBAR_HEIGHT - menu_h;

        if (mx >= 2 && mx < (int32_t)(2 + MENU_WIDTH) &&
            my >= (int32_t)menu_y && my < (int32_t)(sh - TASKBAR_HEIGHT)) {

            uint32_t rel_y = (uint32_t)(my - (int32_t)menu_y - 4);
            uint32_t item = rel_y / MENU_ITEM_HEIGHT;

            if (item < MENU_ITEM_COUNT) {
                start_menu_open = 0;
                wm_mark_dirty();

                switch (item) {
                case 0:  /* Terminal — open terminal window + launch shell */
                {
                    extern void shell_loader_func(void);
                    terminal_open();
                    task_create(shell_loader_func, "ShellLoader");
                    break;
                }
                case 1:  /* About */
                {
                    /* Create an About window */
                    int h = wm_create_window("About Impossible OS",
                                             200, 150, 350, 180,
                                             WM_DEFAULT_FLAGS);
                    if (h >= 0) {
                        uint32_t *fb = wm_get_framebuffer(h);
                        if (fb) {
                            const char *lines[] = {
                                "  Impossible OS v0.1.0",
                                "",
                                "  Architecture: x86-64",
                                "  Boot: UEFI + GRUB",
                                "  Kernel: Monolithic",
                                "",
                                "  Built with love in 2026"
                            };
                            uint32_t ln;
                            uint32_t cw = wm_get_client_width(h);
                            uint32_t ch = wm_get_client_height(h);
                            gfx_surface_t ws;
                            wm_fill_rect(h, 0, 0, 350, 180, 0x001E1E2E);
                            gfx_surface_init(&ws, fb, cw, ch, cw);
                            for (ln = 0; ln < 7; ln++) {
                                gfx_color_t fg_c = (ln == 0) ? 0x0088DDFF
                                                             : 0x00C0C0D0;
                                int slot = (ln == 0) ? FONT_UI_BOLD : FONT_UI;
                                ttf_font_t *fnt = ttf_get(slot, 14);
                                if (fnt) {
                                    ttf_draw_string(&ws, fnt,
                                        8, (int32_t)(ln * 20 + 10),
                                        lines[ln], fg_c);
                                }
                            }
                        }
                        wm_raise_window(h);
                        wm_focus_window(h);
                    }
                    break;
                }
                case 2:  /* Shutdown */
                    acpi_shutdown();
                    break;
                }
                return 1;
            }
        }

        /* Clicked outside the menu — close it */
        start_menu_open = 0;
        wm_mark_dirty();
        /* Don't consume the click — let WM handle it */
        return 0;
    }

    /* Click on window list buttons in taskbar? */
    if (my >= (int32_t)ty) {
        uint32_t list_x = START_BTN_WIDTH + 10;
        uint32_t btn_w = 100;
        uint32_t i;
        extern struct wm_window windows[];

        for (i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!windows[i].active)
                continue;
            if (mx >= (int32_t)list_x &&
                mx < (int32_t)(list_x + btn_w)) {
                wm_raise_window((int)i);
                wm_focus_window((int)i);
                wm_mark_dirty();
                return 1;
            }
            list_x += btn_w + 4;
        }
        return 1;  /* Consume taskbar click even if no button hit */
    }

    return 0;
}

int desktop_in_taskbar(int32_t my)
{
    return my >= (int32_t)(fb_get_height() - TASKBAR_HEIGHT);
}

uint32_t desktop_get_usable_height(void)
{
    return fb_get_height() - TASKBAR_HEIGHT;
}
