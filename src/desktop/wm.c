/* ============================================================================
 * wm.c — Stacking window manager
 *
 * Manages overlapping windows with per-window framebuffers.  Each window has
 * its own pixel buffer for its client area.  The compositor paints all windows
 * onto the screen back buffer in z-order (painter's algorithm), then the
 * caller calls fb_swap() to present.
 *
 * Dirty-flag compositing: only redraws when something changed (mouse moved,
 * window moved/created/destroyed).  This eliminates flicker caused by QEMU's
 * host display thread catching a partially-drawn frame during the async read.
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
#include "kernel/drivers/mouse.h"
#include "kernel/mm/heap.h"
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

static uint32_t str_len(const char *s)
{
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

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

/* Public version for external callers (e.g., cursor movement) */
void wm_mark_dirty(void)
{
    needs_redraw = 1;
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

/* Check if a screen coordinate is in the close button */
static int in_close_button(const struct wm_window *w, int32_t mx, int32_t my)
{
    if (!(w->flags & WM_FLAG_DECORATED))
        return 0;
    int32_t btn_x = w->x + (int32_t)outer_width(w) - (int32_t)WM_TITLEBAR_HEIGHT;
    int32_t btn_y = w->y;
    return mx >= btn_x && mx < btn_x + (int32_t)WM_TITLEBAR_HEIGHT &&
           my >= btn_y && my < btn_y + (int32_t)WM_TITLEBAR_HEIGHT;
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

    /* Allocate the client-area framebuffer */
    w->framebuffer = (uint32_t *)kmalloc(width * height * sizeof(uint32_t));
    if (!w->framebuffer)
        return -1;

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
        kfree(w->framebuffer);
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

    new_fb = (uint32_t *)kmalloc(width * height * sizeof(uint32_t));
    if (!new_fb)
        return;

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

    kfree(w->framebuffer);
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

/* Draw the title bar and border decorations directly to the screen fb */
static void draw_decorations(const struct wm_window *w)
{
    uint32_t ow = outer_width(w);
    uint32_t tb_color;
    int32_t text_x, text_y;
    uint32_t title_len;
    uint32_t btn_size = WM_TITLEBAR_HEIGHT - 2 * 4;  /* button with padding */

    /* Safety: skip if window position is negative (would wrap uint32_t) */
    if (w->x < 0 || w->y < 0)
        return;

    /* Title bar color depends on focus */
    tb_color = (w->flags & WM_FLAG_FOCUSED) ? WM_COLOR_TITLEBAR_ACTIVE
                                             : WM_COLOR_TITLEBAR_INACTIVE;

    /* Draw title bar background using fb_fill_rect for speed */
    fb_fill_rect((uint32_t)w->x, (uint32_t)w->y, ow, WM_TITLEBAR_HEIGHT, tb_color);

    /* Draw title text (left-aligned with padding) */
    text_x = w->x + 8;
    text_y = w->y + (int32_t)(WM_TITLEBAR_HEIGHT / 2 - FONT_HEIGHT / 2);
    title_len = str_len(w->title);
    {
        uint32_t i;
        for (i = 0; i < title_len; i++) {
            font_draw_char((uint32_t)(text_x + (int32_t)(i * FONT_WIDTH)),
                           (uint32_t)text_y, w->title[i],
                           WM_COLOR_TITLE_TEXT, tb_color);
        }
    }

    /* Draw close button (red square with X) */
    {
        int32_t cbx = w->x + (int32_t)ow - (int32_t)WM_TITLEBAR_HEIGHT + 4;
        int32_t cby = w->y + 4;

        fb_fill_rect((uint32_t)cbx, (uint32_t)cby, btn_size, btn_size, WM_COLOR_CLOSE_BTN);

        /* Draw "×" on the button */
        {
            uint32_t d;
            for (d = 2; d < btn_size - 2; d++) {
                fb_put_pixel((uint32_t)(cbx + (int32_t)d),
                             (uint32_t)(cby + (int32_t)d), 0x00FFFFFF);
                fb_put_pixel((uint32_t)(cbx + (int32_t)(btn_size - 1 - d)),
                             (uint32_t)(cby + (int32_t)d), 0x00FFFFFF);
            }
        }
    }

    /* Draw minimize button (yellow) */
    {
        int32_t mbx = w->x + (int32_t)ow - 2 * (int32_t)WM_TITLEBAR_HEIGHT + 4;
        int32_t mby = w->y + 4;

        fb_fill_rect((uint32_t)mbx, (uint32_t)mby, btn_size, btn_size, WM_COLOR_MIN_BTN);

        /* Draw "−" line */
        {
            uint32_t lx;
            for (lx = 3; lx < btn_size - 3; lx++) {
                fb_put_pixel((uint32_t)(mbx + (int32_t)lx),
                             (uint32_t)(mby + (int32_t)(btn_size / 2)),
                             0x00FFFFFF);
            }
        }
    }

    /* Draw maximize button (green) */
    {
        int32_t gbx = w->x + (int32_t)ow - 3 * (int32_t)WM_TITLEBAR_HEIGHT + 4;
        int32_t gby = w->y + 4;
        uint32_t d;

        fb_fill_rect((uint32_t)gbx, (uint32_t)gby, btn_size, btn_size, WM_COLOR_MAX_BTN);

        /* Draw "□" outline */
        for (d = 3; d < btn_size - 3; d++) {
            fb_put_pixel((uint32_t)(gbx + (int32_t)d),
                         (uint32_t)(gby + 3), 0x00FFFFFF);
            fb_put_pixel((uint32_t)(gbx + (int32_t)d),
                         (uint32_t)(gby + (int32_t)(btn_size - 4)), 0x00FFFFFF);
            fb_put_pixel((uint32_t)(gbx + 3),
                         (uint32_t)(gby + (int32_t)d), 0x00FFFFFF);
            fb_put_pixel((uint32_t)(gbx + (int32_t)(btn_size - 4)),
                         (uint32_t)(gby + (int32_t)d), 0x00FFFFFF);
        }
    }

    /* Draw border */
    {
        uint32_t oh = outer_height(w);
        uint32_t px;

        /* Top and bottom */
        for (px = 0; px < ow; px++) {
            fb_put_pixel((uint32_t)(w->x + (int32_t)px),
                         (uint32_t)w->y, WM_COLOR_BORDER);
            fb_put_pixel((uint32_t)(w->x + (int32_t)px),
                         (uint32_t)(w->y + (int32_t)oh - 1), WM_COLOR_BORDER);
        }
        /* Left and right */
        for (px = 0; px < oh; px++) {
            fb_put_pixel((uint32_t)w->x,
                         (uint32_t)(w->y + (int32_t)px), WM_COLOR_BORDER);
            fb_put_pixel((uint32_t)(w->x + (int32_t)ow - 1),
                         (uint32_t)(w->y + (int32_t)px), WM_COLOR_BORDER);
        }
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

    /* Draw desktop wallpaper (or fallback gradient) */
    desktop_draw_wallpaper();

    /* Get sorted windows (bottom to top) */
    get_sorted_order(order, &count);

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

                windows[i].x = new_x;
                windows[i].y = new_y;
                mark_dirty();
            }
            if (left_released) {
                windows[i].dragging = 0;
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

            /* Title bar drag */
            if (in_titlebar(w, mx, my) && (w->flags & WM_FLAG_MOVABLE)) {
                w->dragging = 1;
                w->drag_offset_x = mx - w->x;
                w->drag_offset_y = my - w->y;
                return;
            }
        }
    }
}
