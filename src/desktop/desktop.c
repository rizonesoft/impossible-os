/* ============================================================================
 * desktop.c — Desktop shell (wallpaper, taskbar, start menu)
 *
 * - Loads JPEG/PNG wallpaper from C:\ via image_load() + image_scale()
 * - Reads wallpaper path and fit mode from Registry (HKLM\SYSTEM\Theme)
 * - Draws a taskbar at the bottom with start button, window list, clock
 * - Draws a start menu popup with app launcher items
 * ============================================================================ */

#include "desktop/desktop.h"
#include "gfx.h"
#include "kernel/drivers/framebuffer.h"
#include "desktop/font.h"       /* bitmap font — kept for early boot fallback */
#include "font_mgr.h"            /* TrueType fonts — primary rendering */
#include "kernel/fs/vfs.h"
#include "kernel/timer.h"
#include "kernel/drivers/rtc.h"
#include "desktop/wm.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/sched/task.h"
#include "kernel/acpi.h"
#include "desktop/terminal.h"
#include "kernel/image.h"        /* runtime JPEG/PNG decoding + scaling */
#include "registry.h"           /* Registry for wallpaper settings */
#include "icon_store.h"          /* Color icon rendering from IRES */
#include "gfx.h"                 /* Alpha blending for icon compositing */
#include <kernel/os_logo.h>      /* Pre-built OS logo arrays (all sizes, no scaling needed) */

/* Integer square root — local copy matching gfx_core.c (isqrt is static) */
static uint32_t isqrt_u(uint32_t n)
{
    uint32_t x, x1;
    if (n == 0) return 0;
    x = n;
    x1 = (x + 1) / 2;
    while (x1 < x) { x = x1; x1 = (x + n / x) / 2; }
    return x;
}

/* ---- Wallpaper (decoded + scaled) ---- */
static image_t   wallpaper_img;       /* Scaled wallpaper (PMM or kmalloc) */
static uint8_t   wallpaper_loaded;    /* 1 if wallpaper was loaded successfully */

/* ---- Start Menu layout constants (Win7 two-column, Win11 dark style) ---- */
#define SM_LEFT_W          260  /* left column width (thinner)  */
#define SM_RIGHT_W         190  /* right column width (thinner) */
#define SM_TOTAL_W         (SM_LEFT_W + SM_RIGHT_W)
#define SM_ITEM_H          38   /* item row height (taller)    */
#define SM_SEARCH_H        42   /* search bar height           */
#define SM_ICON_SZ         16   /* icon size for left items (IRES native) */
#define SM_RIGHT_ICON_SZ   32   /* icon size for right column (IRES native) */
#define SM_BTN_SZ          36   /* bottom icon button size     */
#define SM_PAD             14   /* inner padding (more air)    */
#define SM_RADIUS          8    /* rounded corner radius       */
#define SM_ACRYLIC_TINT    0xFF202020  /* neutral dark gray (matches taskbar) */
#define SM_ACRYLIC_OP      200  /* acrylic opacity — matches taskbar (78% tint) */
#define SM_BG_RIGHT        0xE6282828  /* right column (slightly lighter gray) */
#define SM_DIVIDER         0xFF3A3A3A  /* column divider              */
#define SM_SEPARATOR       0xFF353535  /* thin separator line         */
#define SM_ACCENT          0xFF60CDFF  /* accent blue                 */
#define SM_TEXT_PRI        0xFFE8E8E8  /* primary text                */
#define SM_TEXT_SEC        0xFF909090  /* secondary/dimmed text       */
#define SM_TEXT_HEADING    0xFF707070  /* heading letter (A, B...)    */
#define SM_HOVER_BG        0xFF3A3A3A  /* hover highlight background  */
#define SM_SEARCH_BG       0xFF2A2A2A  /* search bar background       */
#define SM_SEARCH_BORDER   0xFF444444  /* search bar border           */
#define SM_SEARCH_TEXT     0xFF707070  /* search placeholder          */
#define SM_BTN_BG          0xFF333333  /* bottom button bg            */
#define SM_BTN_HOVER       0xFF444444  /* bottom button hover         */
#define SM_SHADOW_COLOR    GFX_RGBA(0, 0, 0, 140)

/* ---- Start menu state ---- */
static uint8_t start_menu_open;
static uint8_t prev_left;          /* Previous left-button state for edge */
static int32_t sm_hover_col;       /* 0=left, 1=right, 2=btn_settings, 3=btn_power, -1=none */
static int32_t sm_hover_idx;       /* hovered item index, -1=none */

/* ---- Left column: pinned programs (alphabetical) ---- */
typedef struct {
    char         letter;        /* heading letter (0 = no heading) */
    const char  *label;
    system_icon_t icon_id;
} sm_left_item_t;

static const sm_left_item_t sm_left_items[] = {
    { 'A', "About",          ICON_INFO        },
    { 0,   "All Programs",   ICON_CHEVRON_RIGHT },
    { 'T', "Terminal",       ICON_HOME        },
};
#define SM_LEFT_COUNT  (sizeof(sm_left_items) / sizeof(sm_left_items[0]))

/* ---- Right column: quick access ---- */
typedef struct {
    const char    *label;
    system_icon_t  icon_id;
} sm_right_item_t;

static const sm_right_item_t sm_right_items[] = {
    { "Computer",       ICON_DESKTOP_COMPUTER },
    { "Documents",      ICON_FOLDER_CLOSED    },
    { "Pictures",       ICON_FOLDER_CLOSED    },
    { "Music",          ICON_FOLDER_CLOSED    },
    { "Downloads",      ICON_DOWNLOAD         },
    { "Control Panel",  ICON_CONTROL_PANEL    },
    { "Help",           ICON_QUESTION         },
};
#define SM_RIGHT_COUNT  (sizeof(sm_right_items) / sizeof(sm_right_items[0]))

/* ---- Forward declarations ---- */
static void load_wallpaper(void);

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


/* ---- Initialization ---- */

void desktop_init(void)
{
    /* Load wallpaper from Registry path (JPEG/PNG, runtime decoded) */
    load_wallpaper();
}

/* ---- Simple string comparison ---- */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- Wallpaper loading (JPEG/PNG via image_load + image_scale) ---- */

static void load_wallpaper(void)
{
    HKEY hTheme = (HKEY)0;
    char wp_path[128];
    char wp_mode_str[16];
    image_fit_t fit_mode = IMAGE_FIT_STRETCH;
    image_t decoded;
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    /* Initialize output image to zero */
    wallpaper_img.pixels = (uint32_t *)0;
    wallpaper_img.width  = 0;
    wallpaper_img.height = 0;
    wallpaper_img.alloc_size = 0;
    wallpaper_img.from_pmm = 0;

    /* Read wallpaper path from Registry (fall back to default) */
    {
        long rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Theme",
                               0, KEY_READ, &hTheme);
        if (rc == ERROR_SUCCESS &&
            RegGetString(hTheme, "Wallpaper", wp_path, sizeof(wp_path))
            == ERROR_SUCCESS) {
            /* Got path from Registry */
        } else {
            /* Default path */
            const char *def = "C:\\Impossible\\Web\\Wallpaper\\default.jpg";
            uint32_t i;
            for (i = 0; def[i] && i < sizeof(wp_path) - 1; i++)
                wp_path[i] = def[i];
            wp_path[i] = '\0';
        }
    }

    /* Read fit mode from Registry */
    if (hTheme &&
        RegGetString(hTheme, "WallpaperMode", wp_mode_str,
                     sizeof(wp_mode_str)) == ERROR_SUCCESS) {
        if (str_eq(wp_mode_str, "fill"))        fit_mode = IMAGE_FIT_FILL;
        else if (str_eq(wp_mode_str, "fit"))    fit_mode = IMAGE_FIT_FIT;
        else if (str_eq(wp_mode_str, "center")) fit_mode = IMAGE_FIT_CENTER;
        else if (str_eq(wp_mode_str, "tile"))   fit_mode = IMAGE_FIT_TILE;
        else                                    fit_mode = IMAGE_FIT_STRETCH;
    }
    if (hTheme) RegCloseKey(hTheme);

    /* Decode the image file (JPEG, PNG, BMP, GIF, TGA) */
    if (image_load(&decoded, wp_path) < 0) {
        klog(LOG_ERROR, "DESKTOP", "wallpaper: failed to decode '%s'", wp_path);
        return;
    }

    /* Scale to screen resolution if needed */
    if (decoded.width == screen_w && decoded.height == screen_h &&
        fit_mode == IMAGE_FIT_STRETCH) {
        /* Already exact match — use decoded image directly */
        wallpaper_img = decoded;
    } else {
        if (image_scale(&wallpaper_img, &decoded,
                        screen_w, screen_h, fit_mode) < 0) {
            klog(LOG_ERROR, "DESKTOP", "wallpaper: scale failed");
            image_free(&decoded);
            return;
        }
        image_free(&decoded);  /* Free the unscaled original */
    }

    wallpaper_loaded = 1;
    klog(LOG_INFO, "DESKTOP", "wallpaper loaded (%ux%u, %u bytes)",
           (uint64_t)wallpaper_img.width, (uint64_t)wallpaper_img.height,
           (uint64_t)wallpaper_img.alloc_size);
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
    if (wallpaper_loaded && wallpaper_img.pixels) {
        fb_blit(0, 0, wallpaper_img.pixels,
                wallpaper_img.width, wallpaper_img.height,
                wallpaper_img.width);
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

void desktop_draw_wallpaper_rect(int32_t rx, int32_t ry, uint32_t rw, uint32_t rh)
{
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();

    /* Clamp to screen */
    if (rx < 0) { rw = (uint32_t)((int32_t)rw + rx); rx = 0; }
    if (ry < 0) { rh = (uint32_t)((int32_t)rh + ry); ry = 0; }
    if ((uint32_t)rx + rw > sw) rw = sw - (uint32_t)rx;
    if ((uint32_t)ry + rh > sh) rh = sh - (uint32_t)ry;
    if (rw == 0 || rh == 0) return;

    if (wallpaper_loaded && wallpaper_img.pixels) {
        /* Blit only the rectangular sub-region from the wallpaper */
        uint32_t row;
        for (row = 0; row < rh; row++) {
            const uint32_t *src_row = wallpaper_img.pixels +
                ((uint32_t)ry + row) * wallpaper_img.width + (uint32_t)rx;
            fb_blit((uint32_t)rx, (uint32_t)ry + row,
                    src_row, rw, 1, rw);
        }
    } else {
        /* Fallback: gradient fill for the dirty region */
        uint32_t y;
        for (y = (uint32_t)ry; y < (uint32_t)ry + rh; y++) {
            uint32_t r = 0x10 + (y * 0x10) / sh;
            uint32_t g = 0x10 + (y * 0x08) / sh;
            uint32_t b = 0x20 + (y * 0x20) / sh;
            uint32_t color = (r << 16) | (g << 8) | b;
            fb_fill_rect((uint32_t)rx, y, rw, 1, color);
        }
    }
}

/* ---- Desktop icon rendering ---- */

void desktop_copy_wallpaper_rect(uint32_t *dst, int32_t rx, int32_t ry,
                                  uint32_t rw, uint32_t rh)
{
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();
    uint32_t row, col;

    if (!dst || rw == 0 || rh == 0) return;

    for (row = 0; row < rh; row++) {
        int32_t sy = ry + (int32_t)row;
        for (col = 0; col < rw; col++) {
            int32_t sx = rx + (int32_t)col;
            uint32_t px;

            if (wallpaper_loaded && wallpaper_img.pixels &&
                sx >= 0 && (uint32_t)sx < sw &&
                sy >= 0 && (uint32_t)sy < sh) {
                px = wallpaper_img.pixels[(uint32_t)sy * wallpaper_img.width + (uint32_t)sx];
            } else {
                /* Fallback gradient */
                uint32_t r = 0x10 + ((uint32_t)sy * 0x10) / sh;
                uint32_t g = 0x10 + ((uint32_t)sy * 0x08) / sh;
                uint32_t b = 0x20 + ((uint32_t)sy * 0x20) / sh;
                px = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
            dst[row * rw + col] = px;
        }
    }
}

/* ---- Desktop icon rendering ---- */

int desktop_get_wallpaper_surface(gfx_surface_t *out)
{
    if (!wallpaper_loaded || !wallpaper_img.pixels || !out)
        return -1;
    gfx_surface_init(out, wallpaper_img.pixels,
                     wallpaper_img.width, wallpaper_img.height,
                     wallpaper_img.width);
    return 0;
}

/* ---- Desktop icon rendering ---- */

/* Desktop icon layout: column on right side, 48px icons with labels */
#define DESKTOP_ICON_SIZE   48
#define DESKTOP_ICON_PAD    16   /* Padding between icon grid cells */
#define DESKTOP_ICON_MARGIN 20   /* Margin from screen edge */
#define DESKTOP_LABEL_GAP    4   /* Gap between icon and label */

typedef struct {
    system_icon_t id;
    const char   *label;
} desktop_icon_item_t;

static const desktop_icon_item_t desktop_icon_items[] = {
    { ICON_DESKTOP_COMPUTER,  "Computer"     },
    { ICON_RECYCLE_BIN_EMPTY, "Recycle Bin"  },
    { ICON_CONTROL_PANEL,     "Control Panel" },
};
#define DESKTOP_ICON_COUNT (sizeof(desktop_icon_items) / sizeof(desktop_icon_items[0]))

void desktop_draw_icons(void)
{
    uint32_t sh = fb_get_height();
    uint32_t sw = fb_get_width();
    uint32_t cell_h = DESKTOP_ICON_SIZE + DESKTOP_LABEL_GAP + 16 + DESKTOP_ICON_PAD;
    int32_t  start_x = (int32_t)(sw - DESKTOP_ICON_SIZE - DESKTOP_ICON_MARGIN);
    int32_t  start_y = (int32_t)DESKTOP_ICON_MARGIN;
    uint32_t i;

    gfx_surface_t scr;
    gfx_surface_init(&scr, fb_get_backbuffer(),
                     sw, sh, fb_get_stride());

    for (i = 0; i < DESKTOP_ICON_COUNT; i++) {
        int32_t ix = start_x;
        int32_t iy = start_y + (int32_t)(i * cell_h);

        /* Get the color icon bitmap from IRES */
        icon_bitmap_t *bmp = icon_get_colored(desktop_icon_items[i].id,
                                               DESKTOP_ICON_SIZE, 0xFFFFFFFF);
        if (bmp && bmp->pixels) {
            /* Alpha-blend icon onto screen back buffer */
            int32_t py, px;
            for (py = 0; py < (int32_t)bmp->height && (iy + py) < (int32_t)sh; py++) {
                for (px = 0; px < (int32_t)bmp->width && (ix + px) < (int32_t)sw; px++) {
                    uint32_t src_px = bmp->pixels[py * bmp->width + px];
                    uint8_t  alpha  = (uint8_t)(src_px >> 24);
                    if (alpha == 0) continue;

                    if (alpha == 255) {
                        fb_put_pixel((uint32_t)(ix + px), (uint32_t)(iy + py),
                                     src_px & 0x00FFFFFF);
                    } else {
                        /* Simple alpha blend */
                        uint32_t dst_px = fb_read_pixel((uint32_t)(ix + px),
                                                       (uint32_t)(iy + py));
                        uint32_t sr = (src_px >> 16) & 0xFF;
                        uint32_t sg = (src_px >>  8) & 0xFF;
                        uint32_t sb = (src_px >>  0) & 0xFF;
                        uint32_t dr = (dst_px >> 16) & 0xFF;
                        uint32_t dg = (dst_px >>  8) & 0xFF;
                        uint32_t db = (dst_px >>  0) & 0xFF;
                        uint32_t a  = alpha;
                        uint32_t ia = 255 - a;
                        uint32_t rr = (sr * a + dr * ia) / 255;
                        uint32_t rg = (sg * a + dg * ia) / 255;
                        uint32_t rb = (sb * a + db * ia) / 255;
                        fb_put_pixel((uint32_t)(ix + px), (uint32_t)(iy + py),
                                     (rr << 16) | (rg << 8) | rb);
                    }
                }
            }
        }

        /* Draw label text centered below icon */
        {
            ttf_font_t *tf = ttf_get(0, 14);
            if (tf) {
                int tw = ttf_measure_width(tf, desktop_icon_items[i].label);
                int32_t tx = ix + (int32_t)DESKTOP_ICON_SIZE / 2 - tw / 2;
                int32_t ty = iy + (int32_t)DESKTOP_ICON_SIZE + DESKTOP_LABEL_GAP;

                /* Draw shadow for readability on wallpaper */
                ttf_draw_string(&scr, tf, tx + 1, ty + 1,
                                desktop_icon_items[i].label, 0x00000000);
                /* Draw white text */
                ttf_draw_string(&scr, tf, tx, ty,
                                desktop_icon_items[i].label, 0x00FFFFFF);
            }
        }
    }
}

void desktop_draw_taskbar(void)
{
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();
    uint32_t ty = sh - TASKBAR_HEIGHT;  /* taskbar top Y */
    uint32_t i;

    /* Taskbar: apply acrylic frosted glass directly over the wallpaper
     * pixels in the back buffer (no solid fill first — acrylic needs
     * the wallpaper to blur, then tints with TASKBAR_COLOR). */
    {
        gfx_surface_t scr;
        gfx_surface_init(&scr, fb_get_backbuffer(),
                         fb_get_width(), fb_get_height(), fb_get_stride());
        gfx_acrylic(&scr, 0, (int32_t)ty, sw, TASKBAR_HEIGHT,
                    TASKBAR_COLOR, 200, 3);  /* dark tint, 78% opacity, 3px blur */
    }

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
        fb_fill_rect(btn_x + 1, btn_y + 1, START_BTN_WIDTH - 2, 1, 0xFF454545);

        /* Draw OS logo — 1:1 pixel-perfect blit from the 32x32 prebuilt array */
        {
            uint32_t icon_x = btn_x + (START_BTN_WIDTH - START_ICON_SIZE) / 2;
            uint32_t icon_y = btn_y + (btn_h - START_ICON_SIZE) / 2;
            uint32_t ix, iy;

            for (iy = 0; iy < START_ICON_SIZE; iy++) {
                for (ix = 0; ix < START_ICON_SIZE; ix++) {
                    uint32_t pixel = os_logo_32_pixels[iy * START_ICON_SIZE + ix];
                    uint8_t a = (pixel >> 24) & 0xFF;
                    if (a == 0) continue;

                    uint32_t sx = icon_x + ix;
                    uint32_t sy = icon_y + iy;
                    if (sx >= sw || sy >= sh) continue;

                    if (a == 0xFF) {
                        fb_put_pixel(sx, sy, pixel & 0x00FFFFFF);
                    } else {
                        uint8_t sr = (pixel >> 16) & 0xFF;
                        uint8_t sg = (pixel >>  8) & 0xFF;
                        uint8_t sb =  pixel        & 0xFF;
                        uint8_t dr = (btn_color >> 16) & 0xFF;
                        uint8_t dg = (btn_color >>  8) & 0xFF;
                        uint8_t db =  btn_color        & 0xFF;
                        uint8_t or_ = (sr * a + dr * (255 - a)) / 255;
                        uint8_t og  = (sg * a + dg * (255 - a)) / 255;
                        uint8_t ob  = (sb * a + db * (255 - a)) / 255;
                        fb_put_pixel(sx, sy, ((uint32_t)or_ << 16) |
                                             ((uint32_t)og  <<  8) | ob);
                    }
                }
            }
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
                              ? 0xFF3D3D3D : 0xFF2D2D2D;
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
    /* Menu height: enough for right column content + bottom button row */
    uint32_t right_h = SM_RIGHT_COUNT * SM_ITEM_H + SM_PAD * 2 + SM_BTN_SZ + SM_PAD;
    uint32_t left_h = SM_SEARCH_H + SM_PAD + SM_LEFT_COUNT * SM_ITEM_H + SM_PAD * 2;
    uint32_t menu_h = right_h > left_h ? right_h : left_h;
    int32_t menu_x = 2;
    int32_t menu_y = (int32_t)(sh - TASKBAR_HEIGHT) - (int32_t)menu_h - 10;
    int32_t right_x = menu_x + (int32_t)SM_LEFT_W;
    gfx_surface_t scr;
    uint32_t i;

    if (!start_menu_open)
        return;

    gfx_surface_init(&scr, fb_get_backbuffer(),
                     fb_get_width(), fb_get_height(), fb_get_stride());

    /* ---- Save corner regions from backbuffer BEFORE acrylic ----
     * When a window is behind the start menu, the backbuffer contains
     * composited window content — not raw wallpaper.  We must restore
     * these pixels (not wallpaper) when clipping corners at the end. */
    uint32_t corner_save[4][SM_RADIUS * SM_RADIUS];  /* TL, TR, BL, BR */
    {
        int32_t r, c;
        for (r = 0; r < (int32_t)SM_RADIUS; r++) {
            for (c = 0; c < (int32_t)SM_RADIUS; c++) {
                int32_t tl_x = menu_x + c, tl_y = menu_y + r;
                int32_t tr_x = menu_x + (int32_t)SM_TOTAL_W - 1 - c, tr_y = menu_y + r;
                int32_t bl_x = menu_x + c, bl_y = menu_y + (int32_t)menu_h - 1 - r;
                int32_t br_x = menu_x + (int32_t)SM_TOTAL_W - 1 - c;
                int32_t br_y = menu_y + (int32_t)menu_h - 1 - r;
                uint32_t idx = (uint32_t)(r * (int32_t)SM_RADIUS + c);

                #define SAVE_PX(buf, PX, PY) do { \
                    if ((PX) >= 0 && (uint32_t)(PX) < scr.width && \
                        (PY) >= 0 && (uint32_t)(PY) < scr.height) \
                        (buf) = scr.pixels[(uint32_t)(PY) * scr.stride + (uint32_t)(PX)]; \
                    else (buf) = 0xFF000000; \
                } while (0)

                SAVE_PX(corner_save[0][idx], tl_x, tl_y);
                SAVE_PX(corner_save[1][idx], tr_x, tr_y);
                SAVE_PX(corner_save[2][idx], bl_x, bl_y);
                SAVE_PX(corner_save[3][idx], br_x, br_y);
                #undef SAVE_PX
            }
        }
    }

    /* ---- Acrylic: blur + tint (same as taskbar) ---- */
    gfx_acrylic(&scr, menu_x, menu_y, SM_TOTAL_W, menu_h,
                SM_ACRYLIC_TINT, SM_ACRYLIC_OP, 3);

    /* ---- Right column: darker overlay via alpha blend (NOT gfx_acrylic,
     *       which would add double noise on top of the main acrylic) ---- */
    {
        int32_t r, c;
        int32_t rx0 = right_x > 0 ? right_x : 0;
        int32_t ry0 = menu_y > 0 ? menu_y : 0;
        int32_t rx1 = right_x + (int32_t)SM_RIGHT_W;
        int32_t ry1 = menu_y + (int32_t)menu_h;
        if (rx1 > (int32_t)fb_get_width())  rx1 = (int32_t)fb_get_width();
        if (ry1 > (int32_t)fb_get_height()) ry1 = (int32_t)fb_get_height();
        for (r = ry0; r < ry1; r++) {
            uint32_t *dp = scr.pixels + (uint32_t)r * scr.stride;
            for (c = rx0; c < rx1; c++) {
                uint32_t d = dp[c];
                /* Blend 0xB0181818 (69% opacity dark) over acrylic */
                uint32_t sa = 0xB0;
                uint32_t ia = 255 - sa;
                uint32_t dr = (d >> 16) & 0xFF;
                uint32_t dg = (d >>  8) & 0xFF;
                uint32_t db =  d        & 0xFF;
                dr = (0x18 * sa + dr * ia + 127) / 255;
                dg = (0x18 * sa + dg * ia + 127) / 255;
                db = (0x18 * sa + db * ia + 127) / 255;
                dp[c] = (0xFFu << 24) | (dr << 16) | (dg << 8) | db;
            }
        }
    }

    /* ---- Column divider (1px) ---- */
    gfx_fill_rect(&scr, right_x, menu_y + SM_PAD,
                   1, menu_h - SM_PAD * 2, SM_DIVIDER);

    /* ================================================================
     *  LEFT COLUMN — Search bar + alphabetical pinned programs
     * ================================================================ */
    {
        int32_t y = menu_y + SM_PAD;

        /* Search bar */
        {
            int32_t sb_x = menu_x + SM_PAD;
            int32_t sb_w = SM_LEFT_W - SM_PAD * 2;
            icon_bitmap_t *search_ico = icon_get_colored(ICON_SEARCH, 16, SM_TEXT_SEC);

            gfx_fill_rounded_rect(&scr, sb_x, y, (uint32_t)sb_w, SM_SEARCH_H,
                                   4, SM_SEARCH_BG);
            gfx_draw_rounded_rect(&scr, sb_x, y, (uint32_t)sb_w, SM_SEARCH_H,
                                   4, 1, SM_SEARCH_BORDER);

            /* Search icon */
            if (search_ico)
                icon_draw(&scr, search_ico, sb_x + 10, y + (SM_SEARCH_H - 16) / 2);

            /* Placeholder text */
            ttf_screen_text(sb_x + 32, y + (SM_SEARCH_H - 14) / 2,
                            "Search programs and files", FONT_UI, 13,
                            SM_SEARCH_TEXT);
        }

        y += SM_SEARCH_H + SM_PAD;

        /* Pinned programs with letter headings */
        for (i = 0; i < SM_LEFT_COUNT; i++) {
            const sm_left_item_t *item = &sm_left_items[i];
            int32_t item_y = y + (int32_t)(i * SM_ITEM_H);
            int32_t text_y = item_y + (SM_ITEM_H - 14) / 2;

            /* Letter heading */
            if (item->letter) {
                char hdr[2] = { item->letter, '\0' };
                ttf_screen_text(menu_x + SM_PAD + 4, text_y,
                                hdr, FONT_UI_BOLD, 12, SM_TEXT_HEADING);
            }

            /* Hover highlight */
            if (sm_hover_col == 0 && sm_hover_idx == (int32_t)i) {
                gfx_fill_rect(&scr, menu_x + SM_PAD, item_y,
                               SM_LEFT_W - SM_PAD * 2, SM_ITEM_H, SM_HOVER_BG);
            }

            /* Icon from icon store */
            {
                icon_bitmap_t *ico = icon_get_colored(item->icon_id, SM_ICON_SZ,
                                                       SM_TEXT_PRI);
                if (ico)
                    icon_draw(&scr, ico, menu_x + SM_PAD + 20,
                              item_y + (SM_ITEM_H - (int32_t)SM_ICON_SZ) / 2);
            }

            /* Label */
            ttf_screen_text(menu_x + SM_PAD + 38, text_y,
                            item->label, FONT_UI, 14, SM_TEXT_PRI);
        }
    }

    /* ================================================================
     *  RIGHT COLUMN — Quick access links + bottom buttons
     * ================================================================ */
    {
        int32_t rx = right_x + 1; /* past divider */
        int32_t y = menu_y + SM_PAD;

        /* Quick access items */
        for (i = 0; i < SM_RIGHT_COUNT; i++) {
            const sm_right_item_t *item = &sm_right_items[i];
            int32_t item_y = y + (int32_t)(i * SM_ITEM_H);
            int32_t text_y = item_y + (SM_ITEM_H - 14) / 2;

            /* Hover highlight */
            if (sm_hover_col == 1 && sm_hover_idx == (int32_t)i) {
                gfx_fill_rect(&scr, rx + SM_PAD, item_y,
                               SM_RIGHT_W - SM_PAD * 2 - 1, SM_ITEM_H, SM_HOVER_BG);
            }

            /* Icon */
            {
                icon_bitmap_t *ico = icon_get_colored(item->icon_id, SM_RIGHT_ICON_SZ,
                                                       SM_TEXT_PRI);
                if (ico)
                    icon_draw(&scr, ico, rx + SM_PAD + 4,
                              item_y + (SM_ITEM_H - (int32_t)SM_RIGHT_ICON_SZ) / 2);
            }

            /* Label */
            ttf_screen_text(rx + SM_PAD + 40, text_y,
                            item->label, FONT_UI, 14, SM_TEXT_PRI);
        }

        /* Separator above bottom buttons */
        {
            int32_t sep_y = menu_y + (int32_t)menu_h - (int32_t)SM_BTN_SZ - SM_PAD - 4;
            gfx_fill_rect(&scr, rx + SM_PAD, sep_y,
                           SM_RIGHT_W - SM_PAD * 2 - 1, 1, SM_SEPARATOR);
        }

        /* Bottom icon buttons: Settings and Power */
        {
            int32_t btn_y = menu_y + (int32_t)menu_h - (int32_t)SM_BTN_SZ - SM_PAD;
            int32_t settings_x = rx + (int32_t)SM_RIGHT_W - SM_PAD - SM_BTN_SZ * 2 - SM_PAD - 1;
            int32_t power_x = rx + (int32_t)SM_RIGHT_W - SM_PAD - SM_BTN_SZ - 1;

            /* Settings button */
            {
                uint32_t btn_bg = (sm_hover_col == 2) ? SM_BTN_HOVER : SM_BTN_BG;
                gfx_fill_rounded_rect(&scr, settings_x, btn_y,
                                       SM_BTN_SZ, SM_BTN_SZ, 4, btn_bg);
                {
                    icon_bitmap_t *ico = icon_get_colored(ICON_SETTINGS, 20,
                                                           SM_TEXT_PRI);
                    if (ico)
                        icon_draw(&scr, ico, settings_x + (SM_BTN_SZ - 20) / 2,
                                  btn_y + (SM_BTN_SZ - 20) / 2);
                }
            }

            /* Power button */
            {
                uint32_t btn_bg = (sm_hover_col == 3) ? SM_BTN_HOVER : SM_BTN_BG;
                gfx_fill_rounded_rect(&scr, power_x, btn_y,
                                       SM_BTN_SZ, SM_BTN_SZ, 4, btn_bg);
                {
                    icon_bitmap_t *ico = icon_get_colored(ICON_POWER, 20,
                                                           SM_TEXT_PRI);
                    if (ico)
                        icon_draw(&scr, ico, power_x + (SM_BTN_SZ - 20) / 2,
                                  btn_y + (SM_BTN_SZ - 20) / 2);
                }
            }
        }
    }

    /* ---- Rounded border (drawn before corner clip so it gets clipped) ---- */
    gfx_draw_rounded_rect(&scr, menu_x, menu_y, SM_TOTAL_W, menu_h,
                           SM_RADIUS, 1, SM_DIVIDER);

    /* ---- Clip ALL content to rounded corners (LAST step) ----
     * Uses the same isqrt-based sub-pixel AA as gfx_fill_rounded_rect.
     * Per scanline: compute arc inset, restore saved backbuffer outside,
     * blend edge pixels at 16x sub-pixel precision. */
    {
        int32_t row;
        uint32_t cr = SM_RADIUS;

        for (row = 0; row < (int32_t)cr; row++) {
            /* Compute arc inset for this row (top corners) */
            uint32_t r2 = cr * 2;
            uint32_t dy2 = r2 - (uint32_t)row * 2 - 1;
            uint32_t r2_sq = r2 * r2;
            uint32_t dx2_sq = (r2_sq > dy2 * dy2) ? (r2_sq - dy2 * dy2) : 0;
            uint32_t dx2 = isqrt_u(dx2_sq);
            uint32_t inset = cr - dx2 / 2;

            /* Edge alpha at 16x precision (same as gfx_fill_rounded_rect) */
            uint32_t edge_alpha;
            {
                uint32_t dx16_sq = dx2_sq * 64;
                uint32_t dx16 = isqrt_u(dx16_sq);
                uint32_t dx_whole_16 = (dx2 / 2) * 16;
                uint32_t subfrac = dx16 - dx_whole_16;
                if (subfrac > 15) subfrac = 15;
                edge_alpha = subfrac;  /* 0..15 */
            }

            /* For each of the 4 corners, restore pixels outside the arc */
            /* Process both top row and corresponding bottom row */
            int32_t top_y = menu_y + row;
            int32_t bot_y = menu_y + (int32_t)menu_h - 1 - row;
            int32_t col;

            for (col = 0; col < (int32_t)inset; col++) {
                uint32_t save_idx = (uint32_t)(row * (int32_t)cr + col);

                /* Top-left */
                {
                    int32_t px = menu_x + col, py = top_y;
                    if (px >= 0 && (uint32_t)px < scr.width &&
                        py >= 0 && (uint32_t)py < scr.height)
                        scr.pixels[(uint32_t)py * scr.stride + (uint32_t)px] = corner_save[0][save_idx];
                }
                /* Top-right */
                {
                    int32_t px = menu_x + (int32_t)SM_TOTAL_W - 1 - col, py = top_y;
                    if (px >= 0 && (uint32_t)px < scr.width &&
                        py >= 0 && (uint32_t)py < scr.height)
                        scr.pixels[(uint32_t)py * scr.stride + (uint32_t)px] = corner_save[1][save_idx];
                }
                /* Bottom-left */
                {
                    int32_t px = menu_x + col, py = bot_y;
                    if (px >= 0 && (uint32_t)px < scr.width &&
                        py >= 0 && (uint32_t)py < scr.height)
                        scr.pixels[(uint32_t)py * scr.stride + (uint32_t)px] = corner_save[2][save_idx];
                }
                /* Bottom-right */
                {
                    int32_t px = menu_x + (int32_t)SM_TOTAL_W - 1 - col, py = bot_y;
                    if (px >= 0 && (uint32_t)px < scr.width &&
                        py >= 0 && (uint32_t)py < scr.height)
                        scr.pixels[(uint32_t)py * scr.stride + (uint32_t)px] = corner_save[3][save_idx];
                }
            }

            /* Anti-alias the edge pixel (at col == inset - 1, blend with save) */
            if (edge_alpha > 0 && edge_alpha < 16 && inset > 0) {
                uint32_t save_idx = (uint32_t)(row * (int32_t)cr + (int32_t)inset - 1);
                uint32_t menu_a = edge_alpha * 16;  /* 0..240 */
                uint32_t bg_a = 256 - menu_a;

                #define BLEND_EDGE(PX, PY, SAVED) do { \
                    if ((PX) >= 0 && (uint32_t)(PX) < scr.width && \
                        (PY) >= 0 && (uint32_t)(PY) < scr.height) { \
                        uint32_t _mp = scr.pixels[(uint32_t)(PY) * scr.stride + (uint32_t)(PX)]; \
                        uint32_t _bg = (SAVED); \
                        uint32_t _or = ((((_mp >> 16) & 0xFF) * menu_a + ((_bg >> 16) & 0xFF) * bg_a) >> 8); \
                        uint32_t _og = ((((_mp >> 8) & 0xFF) * menu_a + ((_bg >> 8) & 0xFF) * bg_a) >> 8); \
                        uint32_t _ob = (((_mp & 0xFF) * menu_a + (_bg & 0xFF) * bg_a) >> 8); \
                        scr.pixels[(uint32_t)(PY) * scr.stride + (uint32_t)(PX)] = \
                            (0xFFu << 24) | (_or << 16) | (_og << 8) | _ob; \
                    } \
                } while (0)

                BLEND_EDGE(menu_x + (int32_t)inset - 1, top_y, corner_save[0][save_idx]);
                BLEND_EDGE(menu_x + (int32_t)SM_TOTAL_W - (int32_t)inset, top_y, corner_save[1][save_idx]);
                BLEND_EDGE(menu_x + (int32_t)inset - 1, bot_y, corner_save[2][save_idx]);
                BLEND_EDGE(menu_x + (int32_t)SM_TOTAL_W - (int32_t)inset, bot_y, corner_save[3][save_idx]);
                #undef BLEND_EDGE
            }
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
        uint32_t right_h = SM_RIGHT_COUNT * SM_ITEM_H + SM_PAD * 2 + SM_BTN_SZ + SM_PAD;
        uint32_t left_h = SM_SEARCH_H + SM_PAD + SM_LEFT_COUNT * SM_ITEM_H + SM_PAD * 2;
        uint32_t menu_h = right_h > left_h ? right_h : left_h;
        int32_t menu_x = 2;
        int32_t menu_y = (int32_t)(sh - TASKBAR_HEIGHT) - (int32_t)menu_h - 10;

        /* Check if click is inside the start menu bounds */
        if (mx >= menu_x && mx < menu_x + (int32_t)SM_TOTAL_W &&
            my >= menu_y && my < (int32_t)(sh - TASKBAR_HEIGHT)) {

            int32_t rel_x = mx - menu_x;
            int32_t rel_y = my - menu_y;

            /* Left column click (programs) */
            if (rel_x < (int32_t)SM_LEFT_W) {
                int32_t items_y_start = SM_PAD + SM_SEARCH_H + SM_PAD;
                int32_t item_idx = (rel_y - items_y_start) / SM_ITEM_H;

                if (item_idx >= 0 && item_idx < (int32_t)SM_LEFT_COUNT) {
                    const sm_left_item_t *item = &sm_left_items[item_idx];
                    start_menu_open = 0;
                    wm_mark_dirty();

                    /* Dispatch by label name */
                    if (item->label[0] == 'T') { /* Terminal */
                        extern void shell_loader_func(void);
                        terminal_open();
                        task_create(shell_loader_func, "ShellLoader");
                    } else if (item->label[0] == 'A' && item->label[1] == 'b') { /* About */
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
                                wm_fill_rect(h, 0, 0, 350, 180, 0xFF202020);
                                gfx_surface_init(&ws, fb, cw, ch, cw);
                                for (ln = 0; ln < 7; ln++) {
                                    gfx_color_t fg_c = (ln == 0) ? 0xFF60CDFF
                                                                 : 0xFFC0C0C0;
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
                    }
                    /* "All Programs" — no-op for now */
                    return 1;
                }
            }

            /* Right column — power button click */
            if (rel_x >= (int32_t)SM_LEFT_W) {
                int32_t btn_y = (int32_t)menu_h - (int32_t)SM_BTN_SZ - SM_PAD;
                int32_t rx = (int32_t)SM_LEFT_W + 1;
                int32_t power_x = rx + (int32_t)SM_RIGHT_W - SM_PAD - SM_BTN_SZ - 1;
                int32_t settings_x = power_x - SM_BTN_SZ - SM_PAD;

                if (rel_y >= btn_y && rel_y < btn_y + (int32_t)SM_BTN_SZ) {
                    /* Power button */
                    if (rel_x >= power_x && rel_x < power_x + (int32_t)SM_BTN_SZ) {
                        start_menu_open = 0;
                        wm_mark_dirty();
                        acpi_shutdown();
                        return 1;
                    }
                    /* Settings button — placeholder */
                    if (rel_x >= settings_x && rel_x < settings_x + (int32_t)SM_BTN_SZ) {
                        start_menu_open = 0;
                        wm_mark_dirty();
                        return 1;
                    }
                }
            }

            /* Click inside menu but not on an action — consume it */
            return 1;
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

/* ============================================================================
 * Cursor context — determine cursor shape for desktop elements
 * ============================================================================ */

#include "cursor.h"

cursor_shape_t desktop_get_cursor_context(int32_t mx, int32_t my)
{
    uint32_t sh = fb_get_height();
    uint32_t ty = sh - TASKBAR_HEIGHT;

    /* Start menu items → hand */
    if (start_menu_open) {
        uint32_t right_h = SM_RIGHT_COUNT * SM_ITEM_H + SM_PAD * 2 + SM_BTN_SZ + SM_PAD;
        uint32_t left_h = SM_SEARCH_H + SM_PAD + SM_LEFT_COUNT * SM_ITEM_H + SM_PAD * 2;
        uint32_t menu_h = right_h > left_h ? right_h : left_h;
        uint32_t menu_y = sh - TASKBAR_HEIGHT - menu_h - 10;
        if (mx >= 0 && mx < (int32_t)SM_TOTAL_W + 2 &&
            my >= (int32_t)menu_y && my < (int32_t)(sh - TASKBAR_HEIGHT)) {
            /* Update hover tracking */
            int32_t rel_x = mx - 2;
            int32_t rel_y = my - (int32_t)menu_y;
            sm_hover_col = -1;
            sm_hover_idx = -1;
            if (rel_x < (int32_t)SM_LEFT_W) {
                int32_t items_start = SM_PAD + SM_SEARCH_H + SM_PAD;
                int32_t idx = (rel_y - items_start) / SM_ITEM_H;
                if (idx >= 0 && idx < (int32_t)SM_LEFT_COUNT) {
                    sm_hover_col = 0;
                    sm_hover_idx = idx;
                }
            } else {
                int32_t idx = (rel_y - SM_PAD) / SM_ITEM_H;
                if (idx >= 0 && idx < (int32_t)SM_RIGHT_COUNT) {
                    sm_hover_col = 1;
                    sm_hover_idx = idx;
                } else {
                    /* Check bottom buttons */
                    int32_t btn_y = (int32_t)menu_h - (int32_t)SM_BTN_SZ - SM_PAD;
                    int32_t rx = (int32_t)SM_LEFT_W + 1;
                    int32_t power_x = rx + (int32_t)SM_RIGHT_W - SM_PAD - SM_BTN_SZ - 1;
                    int32_t settings_x = power_x - SM_BTN_SZ - SM_PAD;
                    if (rel_y >= btn_y && rel_y < btn_y + (int32_t)SM_BTN_SZ) {
                        if (rel_x >= power_x)
                            sm_hover_col = 3;
                        else if (rel_x >= settings_x)
                            sm_hover_col = 2;
                    }
                }
            }
            return CURSOR_HAND;
        } else {
            sm_hover_col = -1;
            sm_hover_idx = -1;
        }
    }

    /* Taskbar area */
    if (my >= (int32_t)ty) {
        /* Start button → hand */
        if (mx >= 2 && mx < (int32_t)(2 + START_BTN_WIDTH))
            return CURSOR_HAND;

        /* Window list buttons → hand */
        uint32_t list_x = START_BTN_WIDTH + 10;
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
            extern struct wm_window windows[];
            if (!windows[i].active || !(windows[i].flags & 0x01))
                continue;
            uint32_t btn_w = 120;
            if (mx >= (int32_t)list_x && mx < (int32_t)(list_x + btn_w))
                return CURSOR_HAND;
            list_x += btn_w + 4;
        }

        return CURSOR_ARROW;  /* Taskbar but not on a button */
    }

    return CURSOR_ARROW;
}
