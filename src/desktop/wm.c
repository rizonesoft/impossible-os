/* ============================================================================
 * wm.c — Stacking window manager
 *
 * Manages overlapping windows with per-window framebuffers.  Each window has
 * its own pixel buffer for its client area.  The compositor paints all windows
 * onto the screen back buffer in z-order (painter's algorithm), then the
 * caller calls fb_swap() to present.
 *
 * Always-full-redraw compositing: every frame redraws the entire screen
 * (wallpaper → windows → taskbar → start menu).  VBE page flipping ensures
 * tear-free presentation.  This is correct by construction — no partial-
 * repaint edge cases that cause wallpaper bleed-through.
 *
 * Features:
 *   - Window creation/destruction with dynamic framebuffer allocation
 *   - Title bar decorations with close button
 *   - Mouse-driven window dragging via title bar
 *   - Z-order stacking with bring-to-front on click
 *   - Focus tracking (active window has highlighted title bar)
 *   - Dirty-flag compositing (only redraws when needed)
 * ============================================================================ */

#include "desktop/wm.h"
#include "kernel/drivers/framebuffer.h"
#include "desktop/font.h"
#include "font_mgr.h"
#include "gfx.h"
#include "kernel/drivers/mouse.h"
#include "kernel/mm/pmm.h"
#include "desktop/desktop.h"

/* ---- Internal state ---- */

struct wm_window windows[WM_MAX_WINDOWS];
static int focused_window = -1;
static uint8_t wm_ready = 0;

/* Previous mouse button state for edge detection */
static uint8_t prev_buttons = 0;

/* Dirty flag — when set, the compositor will redraw on next call */
static volatile uint8_t needs_redraw = 1;

/* ---- Helpers ---- */

static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Mark the screen as needing a redraw */
static void mark_dirty(void)
{
    needs_redraw = 1;
}

/* Free N contiguous pages starting at the given physical address */
static void pmm_free_pages(uintptr_t base, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        pmm_free_frame(base + i * 4096);
}

/* Public version for external callers (e.g., cursor movement) */
void wm_mark_dirty(void)
{
    needs_redraw = 1;
}

int wm_needs_redraw(void)
{
    return needs_redraw;
}

/* Get the total outer width/height including decorations */
static uint32_t outer_width(const struct wm_window *w)
{
    if (w->flags & WM_FLAG_DECORATED)
        return w->width + 2 * WM_BORDER_WIDTH;
    return w->width;
}

static uint32_t outer_height(const struct wm_window *w)
{
    if (w->flags & WM_FLAG_DECORATED)
        return w->height + WM_TITLEBAR_HEIGHT + 2 * WM_BORDER_WIDTH;
    return w->height;
}

/* Get client area origin relative to window outer position */
static int32_t client_x(const struct wm_window *w)
{
    if (w->flags & WM_FLAG_DECORATED)
        return w->x + (int32_t)WM_BORDER_WIDTH;
    return w->x;
}

static int32_t client_y(const struct wm_window *w)
{
    if (w->flags & WM_FLAG_DECORATED)
        return w->y + (int32_t)WM_TITLEBAR_HEIGHT + (int32_t)WM_BORDER_WIDTH;
    return w->y;
}

/* Check if a screen coordinate is in the title bar */
static int in_titlebar(const struct wm_window *w, int32_t mx, int32_t my)
{
    if (!(w->flags & WM_FLAG_DECORATED))
        return 0;
    return mx >= w->x && mx < w->x + (int32_t)outer_width(w) &&
           my >= w->y && my < w->y + (int32_t)WM_TITLEBAR_HEIGHT;
}

/* Check if a screen coordinate is in the close button (rightmost 46px) */
static int in_close_button(const struct wm_window *w, int32_t mx, int32_t my)
{
    if (!(w->flags & WM_FLAG_DECORATED))
        return 0;
    int32_t btn_x = w->x + (int32_t)outer_width(w) - (int32_t)WM_BTN_WIDTH;
    int32_t btn_y = w->y;
    return mx >= btn_x && mx < btn_x + (int32_t)WM_BTN_WIDTH &&
           my >= btn_y && my < btn_y + (int32_t)WM_BTN_HEIGHT;
}

/* Check if a screen coordinate is in the maximize button (2nd from right) */
static int in_max_button(const struct wm_window *w, int32_t mx, int32_t my)
{
    if (!(w->flags & WM_FLAG_DECORATED))
        return 0;
    int32_t btn_x = w->x + (int32_t)outer_width(w) - 2 * (int32_t)WM_BTN_WIDTH;
    int32_t btn_y = w->y;
    return mx >= btn_x && mx < btn_x + (int32_t)WM_BTN_WIDTH &&
           my >= btn_y && my < btn_y + (int32_t)WM_BTN_HEIGHT;
}

/* Check if a screen coordinate is in the minimize button (3rd from right) */
static int in_min_button(const struct wm_window *w, int32_t mx, int32_t my)
{
    if (!(w->flags & WM_FLAG_DECORATED))
        return 0;
    int32_t btn_x = w->x + (int32_t)outer_width(w) - 3 * (int32_t)WM_BTN_WIDTH;
    int32_t btn_y = w->y;
    return mx >= btn_x && mx < btn_x + (int32_t)WM_BTN_WIDTH &&
           my >= btn_y && my < btn_y + (int32_t)WM_BTN_HEIGHT;
}

/* Check if a screen coordinate is anywhere in the window (outer bounds) */
static int in_window(const struct wm_window *w, int32_t mx, int32_t my)
{
    return mx >= w->x && mx < w->x + (int32_t)outer_width(w) &&
           my >= w->y && my < w->y + (int32_t)outer_height(w);
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

void wm_init(void)
{
    uint32_t i;

    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        windows[i].active = 0;
        windows[i].framebuffer = (uint32_t *)0;
        windows[i].dragging = 0;
    }

    focused_window = -1;
    prev_buttons = 0;
    needs_redraw = 1;
    wm_ready = 1;

    /* Lock the framebuffer console — all text output now goes to serial only.
     * The compositor exclusively owns the back buffer from this point. */
    fb_lock_compositor();
}

/* ============================================================================
 * Window management
 * ============================================================================ */

int wm_create_window(const char *title, int32_t x, int32_t y,
                     uint32_t width, uint32_t height, uint32_t flags)
{
    uint32_t i;
    struct wm_window *w;
    int32_t max_z = -1;

    if (!wm_ready)
        return -1;

    /* Find a free slot */
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!windows[i].active)
            break;
    }
    if (i >= WM_MAX_WINDOWS)
        return -1;

    w = &windows[i];

    /* Allocate the client-area framebuffer from PMM (not kernel heap).
     * Window framebuffers are large (hundreds of KB to MB) and would
     * exhaust the small kernel heap.  PMM has 2041 MiB available. */
    {
        uint32_t fb_bytes = width * height * sizeof(uint32_t);
        uint32_t fb_pages = (fb_bytes + 4095) / 4096;
        uintptr_t fb_base = pmm_alloc_contiguous(fb_pages);
        if (!fb_base)
            return -1;
        w->framebuffer = (uint32_t *)fb_base;
    }

    /* Clear client area to background color */
    {
        uint32_t px;
        for (px = 0; px < width * height; px++)
            w->framebuffer[px] = WM_COLOR_CLIENT_BG;
    }

    w->x = x;
    w->y = y;
    w->width = width;
    w->height = height;
    w->fb_pitch = width;
    w->flags = flags;
    w->dragging = 0;
    w->active = 1;

    str_copy(w->title, title ? title : "Window", WM_TITLE_MAX);

    /* Place on top of all existing windows */
    {
        uint32_t j;
        for (j = 0; j < WM_MAX_WINDOWS; j++) {
            if (windows[j].active && windows[j].z_order > max_z)
                max_z = windows[j].z_order;
        }
    }
    w->z_order = max_z + 1;

    /* Auto-focus */
    wm_focus_window((int)i);

    mark_dirty();
    return (int)i;
}

void wm_destroy_window(int handle)
{
    struct wm_window *w;

    if (handle < 0 || handle >= WM_MAX_WINDOWS)
        return;

    w = &windows[handle];
    if (!w->active)
        return;

    if (w->framebuffer) {
        uint32_t fb_bytes = w->width * w->height * sizeof(uint32_t);
        uint32_t fb_pages = (fb_bytes + 4095) / 4096;
        pmm_free_pages((uintptr_t)w->framebuffer, fb_pages);
        w->framebuffer = (uint32_t *)0;
    }

    w->active = 0;

    if (focused_window == handle)
        focused_window = -1;

    mark_dirty();
}

/* ============================================================================
 * Manipulation
 * ============================================================================ */

void wm_move_window(int handle, int32_t x, int32_t y)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS)
        return;
    if (!windows[handle].active)
        return;

    windows[handle].x = x;
    windows[handle].y = y;
    mark_dirty();
}

void wm_resize_window(int handle, uint32_t width, uint32_t height)
{
    struct wm_window *w;
    uint32_t *new_fb;

    if (handle < 0 || handle >= WM_MAX_WINDOWS)
        return;

    w = &windows[handle];
    if (!w->active)
        return;

    {
        uint32_t new_bytes = width * height * sizeof(uint32_t);
        uint32_t new_pages = (new_bytes + 4095) / 4096;
        uintptr_t new_base = pmm_alloc_contiguous(new_pages);
        if (!new_base)
            return;
        new_fb = (uint32_t *)new_base;
    }

    /* Clear new buffer */
    {
        uint32_t px;
        for (px = 0; px < width * height; px++)
            new_fb[px] = WM_COLOR_CLIENT_BG;
    }

    /* Copy old content (limited to intersection) */
    {
        uint32_t copy_w = width < w->width ? width : w->width;
        uint32_t copy_h = height < w->height ? height : w->height;
        uint32_t row, col;
        for (row = 0; row < copy_h; row++) {
            for (col = 0; col < copy_w; col++) {
                new_fb[row * width + col] = w->framebuffer[row * w->fb_pitch + col];
            }
        }
    }

    {
        uint32_t old_bytes = w->width * w->height * sizeof(uint32_t);
        uint32_t old_pages = (old_bytes + 4095) / 4096;
        pmm_free_pages((uintptr_t)w->framebuffer, old_pages);
    }
    w->framebuffer = new_fb;
    w->width = width;
    w->height = height;
    w->fb_pitch = width;

    mark_dirty();
}

void wm_raise_window(int handle)
{
    int32_t max_z = -1;
    uint32_t i;

    if (handle < 0 || handle >= WM_MAX_WINDOWS)
        return;
    if (!windows[handle].active)
        return;

    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].active && windows[i].z_order > max_z)
            max_z = windows[i].z_order;
    }

    windows[handle].z_order = max_z + 1;
    mark_dirty();
}

void wm_focus_window(int handle)
{
    uint32_t i;

    /* Remove focus from all */
    for (i = 0; i < WM_MAX_WINDOWS; i++)
        windows[i].flags &= ~WM_FLAG_FOCUSED;

    if (handle >= 0 && handle < WM_MAX_WINDOWS && windows[handle].active) {
        windows[handle].flags |= WM_FLAG_FOCUSED;
        focused_window = handle;
    }

    mark_dirty();
}

/* ============================================================================
 * Client-area drawing helpers
 * ============================================================================ */

uint32_t *wm_get_framebuffer(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS)
        return (uint32_t *)0;
    if (!windows[handle].active)
        return (uint32_t *)0;
    return windows[handle].framebuffer;
}

uint32_t wm_get_client_width(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS || !windows[handle].active)
        return 0;
    return windows[handle].width;
}

uint32_t wm_get_client_height(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS || !windows[handle].active)
        return 0;
    return windows[handle].height;
}

void wm_put_pixel(int handle, uint32_t x, uint32_t y, uint32_t color)
{
    struct wm_window *w;

    if (handle < 0 || handle >= WM_MAX_WINDOWS)
        return;
    w = &windows[handle];
    if (!w->active || x >= w->width || y >= w->height)
        return;

    w->framebuffer[y * w->fb_pitch + x] = color;
}

void wm_fill_rect(int handle, uint32_t x, uint32_t y,
                  uint32_t w_size, uint32_t h_size, uint32_t color)
{
    struct wm_window *win;
    uint32_t row, col;
    uint32_t x1, y1;

    if (handle < 0 || handle >= WM_MAX_WINDOWS)
        return;
    win = &windows[handle];
    if (!win->active)
        return;

    x1 = (x + w_size > win->width)  ? win->width  : x + w_size;
    y1 = (y + h_size > win->height) ? win->height : y + h_size;

    for (row = y; row < y1; row++) {
        for (col = x; col < x1; col++) {
            win->framebuffer[row * win->fb_pitch + col] = color;
        }
    }
}

/* ============================================================================
 * Compositing
 * ============================================================================ */

/* Draw the title bar and border decorations directly to the screen fb.
 * Windows 11 style: rounded corners, Mica title bar, centered title,
 * right-aligned flat caption buttons (minimize, maximize, close). */
static void draw_decorations(const struct wm_window *w)
{
    gfx_surface_t scr;
    uint32_t ow = outer_width(w);
    uint32_t oh = outer_height(w);
    int focused = (w->flags & WM_FLAG_FOCUSED) != 0;

    /* Safety: skip if window position is off-screen */
    if (w->x < -2 || w->y < -2)
        return;

    gfx_surface_init(&scr, fb_get_backbuffer(),
                     fb_get_width(), fb_get_height(), fb_get_stride());

    /* ---- 1. Drop shadow ----
     * Soft multi-layer shadow around the window frame.
     * Only for focused windows to avoid visual clutter. */
    if (focused) {
        gfx_drop_shadow(&scr,
                        (uint32_t)w->x, (uint32_t)w->y,
                        ow, oh,
                        6,     /* blur radius */
                        0, 2,  /* offset x, y */
                        0x60000000);  /* 37% black */
    }

    /* ---- 2. Window body (rounded rect) ---- */
    {
        uint32_t body_color = focused ? WM_COLOR_TITLEBAR_ACTIVE
                                      : WM_COLOR_TITLEBAR_INACTIVE;
        gfx_fill_rounded_rect(&scr,
                              (uint32_t)w->x, (uint32_t)w->y,
                              ow, oh,
                              WM_CORNER_RADIUS, body_color);
    }

    /* ---- 3. Title bar background ----
     * Fill only the title bar region. The body fill above already set the
     * background, so we just apply Mica over the top portion.
     * For Mica we need the wallpaper surface — use it if available. */
    {
        uint32_t tb_color = focused ? WM_COLOR_TITLEBAR_ACTIVE
                                    : WM_COLOR_TITLEBAR_INACTIVE;
        /* Fill the title bar area (rounded top corners only) */
        gfx_fill_rounded_rect(&scr,
                              (uint32_t)w->x, (uint32_t)w->y,
                              ow, WM_TITLEBAR_HEIGHT + WM_CORNER_RADIUS,
                              WM_CORNER_RADIUS, tb_color);
        /* Overwrite the bottom part (below the corner radius) with a flat rect
         * so we get rounded top + flat bottom transition into the client area */
        if (WM_TITLEBAR_HEIGHT > WM_CORNER_RADIUS) {
            gfx_fill_rect(&scr,
                          (uint32_t)w->x, (uint32_t)(w->y + (int32_t)WM_CORNER_RADIUS),
                          ow, WM_TITLEBAR_HEIGHT - WM_CORNER_RADIUS,
                          tb_color);
        }
    }

    /* ---- 4. Client area background ---- */
    {
        uint32_t cx = (uint32_t)client_x(w);
        uint32_t cy = (uint32_t)client_y(w);
        gfx_fill_rect(&scr, cx, cy, w->width, w->height, WM_COLOR_CLIENT_BG);
    }

    /* ---- 5. Centered title text ---- */
    {
        ttf_font_t *tf = ttf_get(FONT_UI, 13);
        if (tf) {
            uint32_t title_color = focused ? WM_COLOR_TITLE_TEXT
                                           : WM_COLOR_TITLE_INACTIVE;
            int32_t tw = ttf_measure_width(tf, w->title);
            /* Center horizontally, but not over button area */
            int32_t avail = (int32_t)ow - 3 * WM_BTN_WIDTH;
            int32_t tx = w->x + (avail - tw) / 2;
            /* Clamp so title doesn't go left of window */
            if (tx < w->x + 12)
                tx = w->x + 12;
            int32_t ty = w->y + ((int32_t)WM_TITLEBAR_HEIGHT - 13) / 2;
            ttf_draw_string(&scr, tf, tx, ty, w->title, title_color);
        }
    }

    /* ---- 6. Caption buttons (right-aligned: minimize, maximize, close) ----
     * Windows 11 style: flat by default, highlight on hover.
     * Close button turns red on hover, others get a subtle white glow. */
    {
        int32_t btn_y = w->y;
        int32_t btn_h = (int32_t)WM_BTN_HEIGHT;
        uint32_t glyph_color = focused ? WM_COLOR_BTN_GLYPH
                                        : WM_COLOR_BTN_GLYPH_DIM;

        /* Close button (rightmost) */
        {
            int32_t bx = w->x + (int32_t)ow - (int32_t)WM_BTN_WIDTH;

            if (w->close_hover) {
                /* Red highlight background — fill close button zone.
                 * Round top-right corner to match window shape. */
                gfx_fill_rect(&scr, (uint32_t)bx, (uint32_t)btn_y,
                              WM_BTN_WIDTH, (uint32_t)btn_h,
                              WM_COLOR_CLOSE_HOVER_BG);
                glyph_color = 0x00FFFFFF;  /* white glyph on red */
            }

            /* Draw × glyph (10px wide, centered in button) */
            {
                int32_t cx = bx + ((int32_t)WM_BTN_WIDTH - 10) / 2;
                int32_t cy = btn_y + (btn_h - 10) / 2;
                int d;
                for (d = 0; d < 10; d++) {
                    fb_put_pixel((uint32_t)(cx + d), (uint32_t)(cy + d), glyph_color);
                    fb_put_pixel((uint32_t)(cx + 9 - d), (uint32_t)(cy + d), glyph_color);
                    /* Thicken slightly */
                    if (d > 0 && d < 9) {
                        fb_put_pixel((uint32_t)(cx + d - 1), (uint32_t)(cy + d), glyph_color);
                        fb_put_pixel((uint32_t)(cx + 10 - d), (uint32_t)(cy + d), glyph_color);
                    }
                }
            }
        }

        /* Reset glyph color for other buttons */
        glyph_color = focused ? WM_COLOR_BTN_GLYPH : WM_COLOR_BTN_GLYPH_DIM;

        /* Maximize button (second from right) */
        {
            int32_t bx = w->x + (int32_t)ow - 2 * (int32_t)WM_BTN_WIDTH;

            if (w->max_hover) {
                gfx_fill_rect_alpha(&scr, (uint32_t)bx, (uint32_t)btn_y,
                                    WM_BTN_WIDTH, (uint32_t)btn_h,
                                    WM_COLOR_BTN_HOVER_BG);
            }

            /* Draw □ glyph (10×10 outline, centered) */
            {
                int32_t cx = bx + ((int32_t)WM_BTN_WIDTH - 10) / 2;
                int32_t cy = btn_y + (btn_h - 10) / 2;
                int d;
                for (d = 0; d < 10; d++) {
                    fb_put_pixel((uint32_t)(cx + d), (uint32_t)cy, glyph_color);
                    fb_put_pixel((uint32_t)(cx + d), (uint32_t)(cy + 9), glyph_color);
                    fb_put_pixel((uint32_t)cx, (uint32_t)(cy + d), glyph_color);
                    fb_put_pixel((uint32_t)(cx + 9), (uint32_t)(cy + d), glyph_color);
                }
            }
        }

        /* Minimize button (third from right) */
        {
            int32_t bx = w->x + (int32_t)ow - 3 * (int32_t)WM_BTN_WIDTH;

            if (w->min_hover) {
                gfx_fill_rect_alpha(&scr, (uint32_t)bx, (uint32_t)btn_y,
                                    WM_BTN_WIDTH, (uint32_t)btn_h,
                                    WM_COLOR_BTN_HOVER_BG);
            }

            /* Draw − glyph (10px horizontal line, centered) */
            {
                int32_t cx = bx + ((int32_t)WM_BTN_WIDTH - 10) / 2;
                int32_t cy = btn_y + btn_h / 2;
                int d;
                for (d = 0; d < 10; d++) {
                    fb_put_pixel((uint32_t)(cx + d), (uint32_t)cy, glyph_color);
                }
            }
        }
    }

    /* ---- 7. Subtle window border (rounded rect outline) ---- */
    {
        uint32_t border_color = focused ? WM_COLOR_BORDER_ACTIVE
                                        : WM_COLOR_BORDER_INACTIVE;
        gfx_draw_rounded_rect(&scr,
                              (uint32_t)w->x, (uint32_t)w->y,
                              ow, oh,
                              WM_CORNER_RADIUS, 1, border_color);
    }
}

/* Blit a window's client-area framebuffer to the screen */
static void blit_client(const struct wm_window *w)
{
    int32_t cx = client_x(w);
    int32_t cy = client_y(w);

    /* Safety: skip if client area starts at negative coordinates */
    if (cx < 0 || cy < 0)
        return;

    /* Use fb_blit for fast row-level copy */
    fb_blit((uint32_t)cx, (uint32_t)cy,
            w->framebuffer, w->width, w->height, w->fb_pitch);
}

/* Sort order for compositing — sort active windows by z_order, ascending */
static void get_sorted_order(int *order, int *count)
{
    int n = 0;
    uint32_t i;
    int j, k;

    /* Collect active window indices */
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].active && (windows[i].flags & WM_FLAG_VISIBLE))
            order[n++] = (int)i;
    }

    /* Insertion sort by z_order */
    for (j = 1; j < n; j++) {
        int key = order[j];
        int32_t key_z = windows[key].z_order;
        k = j - 1;
        while (k >= 0 && windows[order[k]].z_order > key_z) {
            order[k + 1] = order[k];
            k--;
        }
        order[k + 1] = key;
    }

    *count = n;
}

void wm_composite(void)
{
    int order[WM_MAX_WINDOWS];
    int count = 0;
    int i;

    if (!wm_ready)
        return;

    /* Only redraw if something changed */
    if (!needs_redraw)
        return;

    needs_redraw = 0;

    /* Get sorted windows (bottom to top) */
    get_sorted_order(order, &count);

    /* ---- Always full redraw ----
     * Repaint the entire screen every frame.  VBE page flipping ensures
     * tear-free presentation (the host display never reads a half-drawn
     * frame).  This avoids all the subtle edge cases of partial repaints
     * (wallpaper bleed-through, cursor residue, z-order overlap misses). */

    /* Draw desktop wallpaper (or fallback gradient) */
    desktop_draw_wallpaper();

    /* Draw desktop icons (Computer, Recycle Bin, Control Deck) */
    desktop_draw_icons();

    /* Paint each window (painter's algorithm — back to front) */
    for (i = 0; i < count; i++) {
        struct wm_window *w = &windows[order[i]];

        if (w->flags & WM_FLAG_DECORATED)
            draw_decorations(w);

        blit_client(w);
    }

    /* Draw taskbar on top of everything */
    desktop_draw_taskbar();

    /* Draw start menu if open (overlays taskbar) */
    desktop_draw_start_menu();
}

/* ============================================================================
 * Input dispatching
 * ============================================================================ */

int wm_window_at(int32_t x, int32_t y)
{
    int order[WM_MAX_WINDOWS];
    int count = 0;
    int i;

    get_sorted_order(order, &count);

    /* Search from top to bottom (highest z-order first) */
    for (i = count - 1; i >= 0; i--) {
        if (in_window(&windows[order[i]], x, y))
            return order[i];
    }

    return -1;
}

void wm_handle_mouse(int32_t mx, int32_t my, uint8_t buttons)
{
    uint8_t left_pressed  = (buttons & MOUSE_BTN_LEFT) &&
                            !(prev_buttons & MOUSE_BTN_LEFT);
    uint8_t left_released = !(buttons & MOUSE_BTN_LEFT) &&
                            (prev_buttons & MOUSE_BTN_LEFT);
    uint8_t left_held     = (buttons & MOUSE_BTN_LEFT);
    uint32_t i;

    prev_buttons = buttons;

    /* ---- Update button hover state ----
     * Track which caption button the cursor is over (if any).
     * This drives the hover highlight rendering in draw_decorations(). */
    {
        int top = wm_window_at(mx, my);
        for (i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!windows[i].active)
                continue;
            uint8_t ch = ((int)i == top) && in_close_button(&windows[i], mx, my);
            uint8_t xh = ((int)i == top) && in_max_button(&windows[i], mx, my);
            uint8_t nh = ((int)i == top) && in_min_button(&windows[i], mx, my);
            if (ch != windows[i].close_hover ||
                xh != windows[i].max_hover ||
                nh != windows[i].min_hover) {
                windows[i].close_hover = ch;
                windows[i].max_hover   = xh;
                windows[i].min_hover   = nh;
                mark_dirty();
            }
        }
    }

    /* Handle active drag */
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].active && windows[i].dragging) {
            if (left_held) {
                int32_t new_x = mx - windows[i].drag_offset_x;
                int32_t new_y = my - windows[i].drag_offset_y;

                /* Clamp so the window stays fully on-screen */
                int32_t ow = (int32_t)outer_width(&windows[i]);
                int32_t oh = (int32_t)outer_height(&windows[i]);
                int32_t sw = (int32_t)fb_get_width();
                int32_t sh = (int32_t)fb_get_height();

                if (new_x < 0)             new_x = 0;
                if (new_y < 0)             new_y = 0;
                if (new_x + ow > sw)       new_x = sw - ow;
                if (new_y + oh > sh)       new_y = sh - oh;

                /* Move window */
                windows[i].x = new_x;
                windows[i].y = new_y;
                mark_dirty();
            }
            if (left_released) {
                windows[i].dragging = 0;
                mark_dirty();
            }
            return;
        }
    }

    /* Handle new clicks */
    if (left_pressed) {
        int handle = wm_window_at(mx, my);

        if (handle >= 0) {
            struct wm_window *w = &windows[handle];

            /* Raise and focus */
            wm_raise_window(handle);
            wm_focus_window(handle);

            /* Close button click */
            if (in_close_button(w, mx, my)) {
                wm_destroy_window(handle);
                return;
            }

            /* Minimize button click (no-op for now — will hide window) */
            if (in_min_button(w, mx, my)) {
                /* TODO: wm_minimize(handle); */
                return;
            }

            /* Maximize button click (no-op for now — will toggle maximize) */
            if (in_max_button(w, mx, my)) {
                /* TODO: wm_maximize(handle); */
                return;
            }

            /* Title bar drag (only if not on a button) */
            if (in_titlebar(w, mx, my) && (w->flags & WM_FLAG_MOVABLE)) {
                w->dragging = 1;
                w->drag_offset_x = mx - w->x;
                w->drag_offset_y = my - w->y;
                return;
            }
        }
    }
}

/* ============================================================================
 * Cursor context — determine cursor shape from pointer position
 * ============================================================================ */

#include "cursor.h"

cursor_shape_t wm_get_cursor_context(int32_t mx, int32_t my)
{
    uint32_t i;

    /* If any window is being dragged, show move cursor */
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].active && windows[i].dragging)
            return CURSOR_MOVE;
    }

    /* Find topmost window under cursor */
    int handle = wm_window_at(mx, my);
    if (handle < 0)
        return CURSOR_ARROW;  /* Desktop/wallpaper */

    const struct wm_window *w = &windows[handle];
    int32_t ow = (int32_t)outer_width(w);
    int32_t oh = (int32_t)outer_height(w);

    /* Caption buttons → hand */
    if (in_close_button(w, mx, my) ||
        in_max_button(w, mx, my) ||
        in_min_button(w, mx, my))
        return CURSOR_HAND;

    /* Resize edges/corners (only for resizable windows) */
    if (w->flags & WM_FLAG_RESIZABLE) {
        int32_t M = WM_RESIZE_MARGIN;

        /* Distances from each edge */
        int near_left   = (mx >= w->x      && mx < w->x + M);
        int near_right  = (mx >= w->x + ow - M && mx < w->x + ow);
        int near_top    = (my >= w->y      && my < w->y + M);
        int near_bottom = (my >= w->y + oh - M && my < w->y + oh);

        /* Corners first (5×5 corner zones) */
        if (near_top && near_left)     return CURSOR_RESIZE_NWSE;
        if (near_bottom && near_right) return CURSOR_RESIZE_NWSE;
        if (near_top && near_right)    return CURSOR_RESIZE_NESW;
        if (near_bottom && near_left)  return CURSOR_RESIZE_NESW;

        /* Edges */
        if (near_top || near_bottom)   return CURSOR_RESIZE_NS;
        if (near_left || near_right)   return CURSOR_RESIZE_EW;
    }

    /* Title bar (not close button — already checked above) */
    if (in_titlebar(w, mx, my))
        return CURSOR_ARROW;

    /* Client area — default arrow (widgets can override in future) */
    return CURSOR_ARROW;
}
