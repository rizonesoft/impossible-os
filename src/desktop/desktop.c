/* ============================================================================
 * desktop.c — Desktop shell (wallpaper, taskbar, start menu)
 *
 * - Loads wallpaper.raw from initrd (zero-copy) and blits as background
 * - Draws a taskbar at the bottom with start button, window list, clock
 * - Draws a start menu popup with app launcher items
 * - Copies background images to C:\Documents\backgrounds\ on IXFS
 * ============================================================================ */

#include "desktop/desktop.h"
#include "kernel/drivers/framebuffer.h"
#include "desktop/font.h"
#include "kernel/fs/vfs.h"
#include "kernel/drivers/pit.h"
#include "kernel/drivers/rtc.h"
#include "desktop/wm.h"
#include "kernel/mm/heap.h"
#include "kernel/printk.h"
#include "kernel/sched/task.h"
#include "kernel/fs/initrd.h"
#include "kernel/acpi.h"

/* ---- Wallpaper pixel buffer ---- */
static const uint32_t *wallpaper_buf; /* Points directly into initrd memory */
static uint8_t   wallpaper_loaded;    /* 1 if wallpaper was loaded successfully */

/* ---- Start icon ---- */
static const uint32_t *start_icon_buf; /* 48x48 BGRA from initrd */
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
static void copy_backgrounds_to_ixfs(void);
static void draw_text(uint32_t x, uint32_t y, const char *text,
                      uint32_t fg, uint32_t bg);
static uint32_t text_width(const char *text);

/* ---- String helpers ---- */
static uint32_t slen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ---- Initialization ---- */

void desktop_init(void)
{
    wallpaper_buf    = (const uint32_t *)0;
    wallpaper_loaded = 0;
    start_icon_buf   = (const uint32_t *)0;
    start_icon_loaded = 0;
    start_menu_open  = 0;
    prev_left        = 0;

    /* Load the wallpaper image from initrd (zero-copy) */
    load_wallpaper();

    /* Load start button icon from initrd (zero-copy) */
    {
        uint32_t icon_size = 0;
        const uint8_t *icon_data = initrd_get_file_data("start_icon.raw", &icon_size);
        if (icon_data && icon_size >= START_ICON_SIZE * START_ICON_SIZE * 4) {
            start_icon_buf = (const uint32_t *)icon_data;
            start_icon_loaded = 1;
            printk("[OK] Start icon loaded (%ux%u, zero-copy)\n",
                   (uint64_t)START_ICON_SIZE, (uint64_t)START_ICON_SIZE);
        }
    }

    /* Copy background files from initrd to IXFS */
    copy_backgrounds_to_ixfs();
}

/* ---- Wallpaper loading (zero-copy from initrd) ---- */

static void load_wallpaper(void)
{
    uint32_t file_size = 0;
    uint32_t expected_size;
    const uint8_t *data;

    expected_size = WALLPAPER_WIDTH * WALLPAPER_HEIGHT * 4;

    /* Get a direct pointer to the wallpaper data in initrd memory */
    data = initrd_get_file_data("wallpaper.raw", &file_size);
    if (!data) {
        printk("[DESKTOP] wallpaper.raw not found in initrd\n");
        return;
    }

    if (file_size < expected_size) {
        printk("[DESKTOP] wallpaper.raw too small (%u < %u)\n",
               (uint64_t)file_size, (uint64_t)expected_size);
        return;
    }

    /* Point directly at initrd memory — no copy needed */
    wallpaper_buf = (const uint32_t *)data;
    wallpaper_loaded = 1;
    printk("[OK] Desktop wallpaper loaded (%ux%u, zero-copy)\n",
           (uint64_t)WALLPAPER_WIDTH, (uint64_t)WALLPAPER_HEIGHT);
}

/* ---- Copy backgrounds to IXFS (zero-copy read from initrd) ---- */

static void copy_one_bg(const char *initrd_name, const char *ixfs_path)
{
    uint32_t file_size = 0;
    const uint8_t *data;
    struct vfs_node *dst;

    data = initrd_get_file_data(initrd_name, &file_size);
    if (!data || file_size == 0)
        return;

    /* Create and write directly from initrd memory — no heap buffer */
    vfs_create(ixfs_path, VFS_FILE);
    dst = vfs_open(ixfs_path, VFS_O_WRITE);
    if (dst) {
        vfs_write(dst, 0, file_size, data);
        vfs_close(dst);
    }
}

static void copy_backgrounds_to_ixfs(void)
{
    /* Create the Documents\backgrounds directory hierarchy */
    vfs_create("C:\\Documents", VFS_DIRECTORY);
    vfs_create("C:\\Documents\\backgrounds", VFS_DIRECTORY);

    /* Copy background directly from initrd memory to IXFS */
    copy_one_bg("bg.raw", "C:\\Documents\\backgrounds\\background.raw");

    printk("[OK] Background copied to C:\\Documents\\backgrounds\\\n");
}

/* ---- Text drawing helper ---- */

static void draw_text(uint32_t x, uint32_t y, const char *text,
                      uint32_t fg, uint32_t bg)
{
    uint32_t i = 0;
    while (text[i]) {
        font_draw_char(x + i * FONT_WIDTH, y, text[i], fg, bg);
        i++;
    }
}

static uint32_t text_width(const char *text)
{
    return slen(text) * FONT_WIDTH;
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
            /* Fallback: text */
            uint32_t txt_y = btn_y + (btn_h - FONT_HEIGHT) / 2;
            draw_text(btn_x + 8, txt_y, "Start", START_BTN_TEXT, btn_color);
        }
    }

    /* ---- Window list ---- */
    {
        uint32_t list_x = START_BTN_WIDTH + 10;
        uint32_t list_max_x = sw - 120;  /* Reserve space for clock */
        uint32_t btn_w = 100;
        uint32_t txt_y = ty + (TASKBAR_HEIGHT - FONT_HEIGHT) / 2;
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

            /* Truncate title to fit */
            uint32_t max_chars = (btn_w - 8) / FONT_WIDTH;
            uint32_t title_len = slen(windows[i].title);
            if (title_len > max_chars) title_len = max_chars;

            uint32_t c;
            for (c = 0; c < title_len; c++) {
                font_draw_char(list_x + 4 + c * FONT_WIDTH, txt_y,
                               windows[i].title[c], btn_fg, btn_bg);
            }

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

        uint32_t clock_w = text_width(clock_buf);
        uint32_t clock_x = sw - clock_w - 8;
        uint32_t txt_y = ty + (TASKBAR_HEIGHT - FONT_HEIGHT) / 2;

        draw_text(clock_x, txt_y, clock_buf, CLOCK_COLOR, TASKBAR_COLOR);
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
        uint32_t txt_y = item_y + (MENU_ITEM_HEIGHT - FONT_HEIGHT) / 2;

        /* Icon */
        font_draw_char(menu_x + 12, txt_y, menu_icons[i],
                        menu_icon_colors[i], MENU_BG);

        /* Label */
        draw_text(menu_x + 12 + FONT_WIDTH + 8, txt_y,
                  menu_items[i], MENU_TEXT, MENU_BG);

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
                case 0:  /* Terminal — launch shell */
                {
                    extern void shell_loader_func(void);
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
                            wm_fill_rect(h, 0, 0, 350, 180, 0x001E1E2E);
                            for (ln = 0; ln < 7; ln++) {
                                uint32_t c = 0;
                                while (lines[ln][c]) {
                                    const uint8_t *glyph =
                                        font_get_glyph(lines[ln][c]);
                                    uint32_t py, px;
                                    uint32_t fg_c = (ln == 0) ? 0x0088DDFF
                                                              : 0x00C0C0D0;
                                    for (py = 0; py < FONT_HEIGHT; py++) {
                                        uint8_t row = glyph[py];
                                        for (px = 0; px < FONT_WIDTH; px++) {
                                            uint32_t clr = (row & (0x80 >> px))
                                                ? fg_c : 0x001E1E2E;
                                            wm_put_pixel(h,
                                                c * FONT_WIDTH + px,
                                                ln * (FONT_HEIGHT + 4) + 12 + py,
                                                clr);
                                        }
                                    }
                                    c++;
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
