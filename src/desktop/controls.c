/* ============================================================================
 * controls.c — Common Controls library
 *
 * Implements reusable GUI controls: Button, Label, TextBox, ScrollBar.
 * Each control draws into a per-window framebuffer via the WM drawing API.
 *
 * Controls are stored per-window in a static array (CTRL_MAX_PER_WINDOW per
 * window).  Event routing dispatches mouse clicks and keyboard input to the
 * appropriate control based on hit-testing and focus tracking.
 * ============================================================================ */

#include "controls.h"
#include "wm.h"
#include "font.h"
#include "framebuffer.h"

/* ---- Per-window control storage ---- */

static struct ctrl_window ctrl_windows[CTRL_MAX_WINDOWS];

/* ---- Helpers ---- */

static uint32_t ctrl_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void ctrl_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Find or create a ctrl_window for a WM handle */
static struct ctrl_window *get_ctrl_window(int window_handle)
{
    uint32_t i;

    /* Look for existing slot */
    for (i = 0; i < CTRL_MAX_WINDOWS; i++) {
        if (ctrl_windows[i].active &&
            ctrl_windows[i].window_handle == window_handle)
            return &ctrl_windows[i];
    }

    return (struct ctrl_window *)0;
}

static struct ctrl_window *get_or_create_ctrl_window(int window_handle)
{
    uint32_t i;
    struct ctrl_window *cw;

    cw = get_ctrl_window(window_handle);
    if (cw)
        return cw;

    /* Allocate a new slot */
    for (i = 0; i < CTRL_MAX_WINDOWS; i++) {
        if (!ctrl_windows[i].active) {
            cw = &ctrl_windows[i];
            cw->active = 1;
            cw->window_handle = window_handle;
            cw->count = 0;
            cw->focused_id = -1;
            return cw;
        }
    }

    return (struct ctrl_window *)0;
}

/* Find a control by ID in a ctrl_window */
static struct control *find_control(struct ctrl_window *cw, int ctrl_id)
{
    int i;

    for (i = 0; i < cw->count; i++) {
        if (cw->controls[i].id == ctrl_id &&
            cw->controls[i].type != CTRL_NONE)
            return &cw->controls[i];
    }

    return (struct control *)0;
}

/* Allocate a new control slot */
static struct control *alloc_control(struct ctrl_window *cw, int window_handle)
{
    struct control *c;

    if (cw->count >= CTRL_MAX_PER_WINDOW)
        return (struct control *)0;

    c = &cw->controls[cw->count];
    c->id = cw->count;
    c->window_handle = window_handle;
    c->state = CTRL_STATE_VISIBLE | CTRL_STATE_ENABLED;
    cw->count++;

    return c;
}

/* Check if point (px, py) is inside control c */
static int ctrl_hit_test(const struct control *c, int32_t px, int32_t py)
{
    return px >= (int32_t)c->x && px < (int32_t)(c->x + c->w) &&
           py >= (int32_t)c->y && py < (int32_t)(c->y + c->h);
}

/* Draw text centered in a rectangle, clipped to bounds */
static void draw_text_centered(int handle, uint32_t rx, uint32_t ry,
                                uint32_t rw, uint32_t rh,
                                const char *text, uint32_t fg, uint32_t bg)
{
    uint32_t tw = ctrl_strlen(text) * FONT_WIDTH;
    uint32_t th = FONT_HEIGHT;
    uint32_t tx, ty;
    uint32_t i, max_chars;

    /* Center horizontally and vertically */
    tx = (tw < rw) ? rx + (rw - tw) / 2 : rx + 2;
    ty = (th < rh) ? ry + (rh - th) / 2 : ry;

    /* Clip to rectangle width */
    max_chars = (rw - 4) / FONT_WIDTH;
    if (max_chars > ctrl_strlen(text))
        max_chars = ctrl_strlen(text);

    for (i = 0; i < max_chars; i++) {
        /* Draw each character via per-pixel approach into window fb */
        const uint8_t *glyph = font_get_glyph(text[i]);
        uint32_t py, px;
        for (py = 0; py < FONT_HEIGHT; py++) {
            uint8_t row = glyph[py];
            for (px = 0; px < FONT_WIDTH; px++) {
                uint32_t color = (row & (0x80 >> px)) ? fg : bg;
                wm_put_pixel(handle,
                             tx + i * FONT_WIDTH + px,
                             ty + py, color);
            }
        }
    }
}

/* Draw text left-aligned in a rectangle */
static void draw_text_left(int handle, uint32_t rx, uint32_t ry,
                            uint32_t rh, const char *text, uint32_t len,
                            uint32_t fg, uint32_t bg)
{
    uint32_t ty;
    uint32_t i;

    ty = (FONT_HEIGHT < rh) ? ry + (rh - FONT_HEIGHT) / 2 : ry;

    for (i = 0; i < len; i++) {
        const uint8_t *glyph = font_get_glyph(text[i]);
        uint32_t py, px;
        for (py = 0; py < FONT_HEIGHT; py++) {
            uint8_t row = glyph[py];
            for (px = 0; px < FONT_WIDTH; px++) {
                uint32_t color = (row & (0x80 >> px)) ? fg : bg;
                wm_put_pixel(handle,
                             rx + i * FONT_WIDTH + px,
                             ty + py, color);
            }
        }
    }
}

/* ---- Initialization ---- */

void ctrl_init(void)
{
    uint32_t i;
    for (i = 0; i < CTRL_MAX_WINDOWS; i++) {
        ctrl_windows[i].active = 0;
        ctrl_windows[i].count = 0;
        ctrl_windows[i].focused_id = -1;
    }
}

/* ============================================================================
 * Control creation
 * ============================================================================ */

int ctrl_create_button(int window_handle, uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h, const char *text,
                       ctrl_click_fn on_click)
{
    struct ctrl_window *cw = get_or_create_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return -1;

    c = alloc_control(cw, window_handle);
    if (!c)
        return -1;

    c->type = CTRL_BUTTON;
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    ctrl_strcpy(c->data.button.text, text ? text : "Button", CTRL_TEXT_MAX);
    c->data.button.on_click = on_click;

    return c->id;
}

int ctrl_create_label(int window_handle, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, const char *text,
                      uint32_t fg_color)
{
    struct ctrl_window *cw = get_or_create_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return -1;

    c = alloc_control(cw, window_handle);
    if (!c)
        return -1;

    c->type = CTRL_LABEL;
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    ctrl_strcpy(c->data.label.text, text ? text : "", CTRL_TEXT_MAX);
    c->data.label.fg_color = fg_color;

    return c->id;
}

int ctrl_create_textbox(int window_handle, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h)
{
    struct ctrl_window *cw = get_or_create_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return -1;

    c = alloc_control(cw, window_handle);
    if (!c)
        return -1;

    c->type = CTRL_TEXTBOX;
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->data.textbox.text[0] = '\0';
    c->data.textbox.cursor_pos = 0;
    c->data.textbox.text_len = 0;
    c->data.textbox.scroll_offset = 0;

    return c->id;
}

int ctrl_create_scrollbar(int window_handle, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint8_t orientation,
                          uint32_t max_value, uint32_t page_size)
{
    struct ctrl_window *cw = get_or_create_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return -1;

    c = alloc_control(cw, window_handle);
    if (!c)
        return -1;

    c->type = CTRL_SCROLLBAR;
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->data.scrollbar.orientation = orientation;
    c->data.scrollbar.position = 0;
    c->data.scrollbar.max_value = max_value;
    c->data.scrollbar.page_size = page_size;
    c->data.scrollbar.dragging = 0;
    c->data.scrollbar.drag_offset = 0;

    return c->id;
}

/* ============================================================================
 * Destruction
 * ============================================================================ */

void ctrl_destroy(int window_handle, int ctrl_id)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return;

    c = find_control(cw, ctrl_id);
    if (c)
        c->type = CTRL_NONE;
}

void ctrl_destroy_all(int window_handle)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);

    if (!cw)
        return;

    cw->active = 0;
    cw->count = 0;
    cw->focused_id = -1;
}

/* ============================================================================
 * Drawing — each control type has its own draw function
 * ============================================================================ */

static void draw_button(struct control *c)
{
    uint32_t bg;
    int handle = c->window_handle;

    /* Choose background based on state */
    if (!(c->state & CTRL_STATE_ENABLED)) {
        bg = CTRL_COLOR_BG_DISABLED;
    } else if (c->state & CTRL_STATE_PRESSED) {
        bg = CTRL_COLOR_BG_PRESSED;
    } else if (c->state & CTRL_STATE_HOVERED) {
        bg = CTRL_COLOR_BG_HOVER;
    } else {
        bg = CTRL_COLOR_BG;
    }

    /* Background fill */
    wm_fill_rect(handle, c->x, c->y, c->w, c->h, bg);

    /* Border */
    {
        uint32_t bx, by;
        /* Top and bottom edges */
        for (bx = c->x; bx < c->x + c->w; bx++) {
            wm_put_pixel(handle, bx, c->y, CTRL_COLOR_BORDER);
            wm_put_pixel(handle, bx, c->y + c->h - 1, CTRL_COLOR_BORDER);
        }
        /* Left and right edges */
        for (by = c->y; by < c->y + c->h; by++) {
            wm_put_pixel(handle, c->x, by, CTRL_COLOR_BORDER);
            wm_put_pixel(handle, c->x + c->w - 1, by, CTRL_COLOR_BORDER);
        }
    }

    /* Subtle top highlight for 3D effect */
    {
        uint32_t hx;
        uint32_t highlight = (c->state & CTRL_STATE_PRESSED) ? 0x00101020
                                                              : 0x003A3A5C;
        for (hx = c->x + 1; hx < c->x + c->w - 1; hx++) {
            wm_put_pixel(handle, hx, c->y + 1, highlight);
        }
    }

    /* Centered text */
    {
        uint32_t fg = (c->state & CTRL_STATE_ENABLED) ? CTRL_COLOR_TEXT
                                                       : CTRL_COLOR_TEXT_DIM;
        draw_text_centered(handle, c->x, c->y, c->w, c->h,
                           c->data.button.text, fg, bg);
    }
}

static void draw_label(struct control *c)
{
    int handle = c->window_handle;
    uint32_t fg = c->data.label.fg_color;
    uint32_t len = ctrl_strlen(c->data.label.text);
    uint32_t max_chars;
    uint32_t ty;
    uint32_t i;

    /* Labels render transparently — only draw foreground pixels */
    ty = (FONT_HEIGHT < c->h) ? c->y + (c->h - FONT_HEIGHT) / 2 : c->y;

    /* Clip to control width */
    max_chars = c->w / FONT_WIDTH;
    if (max_chars > len)
        max_chars = len;

    for (i = 0; i < max_chars; i++) {
        const uint8_t *glyph = font_get_glyph(c->data.label.text[i]);
        uint32_t py, px;
        for (py = 0; py < FONT_HEIGHT; py++) {
            uint8_t row = glyph[py];
            for (px = 0; px < FONT_WIDTH; px++) {
                if (row & (0x80 >> px)) {
                    wm_put_pixel(handle,
                                 c->x + i * FONT_WIDTH + px,
                                 ty + py, fg);
                }
                /* Skip background pixels for transparency */
            }
        }
    }
}

static void draw_textbox(struct control *c)
{
    int handle = c->window_handle;
    uint32_t focused = (c->state & CTRL_STATE_FOCUSED);
    uint32_t border_color = focused ? CTRL_COLOR_CURSOR : CTRL_COLOR_BORDER;
    uint32_t bg = CTRL_COLOR_TEXTBOX_BG;
    uint32_t visible_chars;
    uint32_t draw_len;

    /* Background fill */
    wm_fill_rect(handle, c->x, c->y, c->w, c->h, bg);

    /* Border */
    {
        uint32_t bx, by;
        for (bx = c->x; bx < c->x + c->w; bx++) {
            wm_put_pixel(handle, bx, c->y, border_color);
            wm_put_pixel(handle, bx, c->y + c->h - 1, border_color);
        }
        for (by = c->y; by < c->y + c->h; by++) {
            wm_put_pixel(handle, c->x, by, border_color);
            wm_put_pixel(handle, c->x + c->w - 1, by, border_color);
        }
    }

    /* Text content area: 4px padding on each side */
    visible_chars = (c->w - 8) / FONT_WIDTH;
    if (visible_chars == 0)
        visible_chars = 1;

    /* Adjust scroll_offset to keep cursor visible */
    if (c->data.textbox.cursor_pos < c->data.textbox.scroll_offset) {
        c->data.textbox.scroll_offset = c->data.textbox.cursor_pos;
    }
    if (c->data.textbox.cursor_pos >= c->data.textbox.scroll_offset + visible_chars) {
        c->data.textbox.scroll_offset =
            c->data.textbox.cursor_pos - visible_chars + 1;
    }

    /* Draw visible text */
    draw_len = c->data.textbox.text_len - c->data.textbox.scroll_offset;
    if (draw_len > visible_chars)
        draw_len = visible_chars;

    if (draw_len > 0) {
        draw_text_left(handle, c->x + 4, c->y, c->h,
                       c->data.textbox.text + c->data.textbox.scroll_offset,
                       draw_len, CTRL_COLOR_TEXT, bg);
    }

    /* Draw cursor (blinking simulated by always-on for now) */
    if (focused) {
        uint32_t cursor_x = c->x + 4 +
            (c->data.textbox.cursor_pos - c->data.textbox.scroll_offset)
            * FONT_WIDTH;
        uint32_t cursor_y = c->y + (c->h - FONT_HEIGHT) / 2;
        uint32_t cy;

        if (cursor_x < c->x + c->w - 4) {
            for (cy = cursor_y; cy < cursor_y + FONT_HEIGHT; cy++) {
                wm_put_pixel(handle, cursor_x, cy, CTRL_COLOR_CURSOR);
            }
        }
    }
}

static void draw_scrollbar(struct control *c)
{
    int handle = c->window_handle;
    uint32_t thumb_pos, thumb_len, track_len;
    uint32_t thumb_color;

    /* Track background */
    wm_fill_rect(handle, c->x, c->y, c->w, c->h, CTRL_COLOR_SCROLL_BG);

    /* Border */
    {
        uint32_t bx, by;
        for (bx = c->x; bx < c->x + c->w; bx++) {
            wm_put_pixel(handle, bx, c->y, CTRL_COLOR_BORDER);
            wm_put_pixel(handle, bx, c->y + c->h - 1, CTRL_COLOR_BORDER);
        }
        for (by = c->y; by < c->y + c->h; by++) {
            wm_put_pixel(handle, c->x, by, CTRL_COLOR_BORDER);
            wm_put_pixel(handle, c->x + c->w - 1, by, CTRL_COLOR_BORDER);
        }
    }

    /* Calculate thumb position and size */
    if (c->data.scrollbar.max_value == 0)
        return;

    thumb_color = (c->state & CTRL_STATE_HOVERED) || c->data.scrollbar.dragging
                  ? CTRL_COLOR_SCROLL_HOVER : CTRL_COLOR_SCROLL_THUMB;

    if (c->data.scrollbar.orientation == CTRL_SCROLLBAR_VERT) {
        /* Vertical scrollbar */
        track_len = c->h - 4;  /* 2px padding top/bottom */

        /* Thumb proportional to page_size / (max_value + page_size) */
        if (c->data.scrollbar.page_size > 0) {
            thumb_len = (track_len * c->data.scrollbar.page_size) /
                        (c->data.scrollbar.max_value + c->data.scrollbar.page_size);
        } else {
            thumb_len = track_len / 4;
        }
        if (thumb_len < 16)
            thumb_len = 16;
        if (thumb_len > track_len)
            thumb_len = track_len;

        /* Thumb position proportional to current position */
        if (c->data.scrollbar.max_value > 0) {
            thumb_pos = (c->data.scrollbar.position *
                         (track_len - thumb_len)) /
                        c->data.scrollbar.max_value;
        } else {
            thumb_pos = 0;
        }

        /* Draw thumb */
        wm_fill_rect(handle,
                     c->x + 2, c->y + 2 + thumb_pos,
                     c->w - 4, thumb_len,
                     thumb_color);
    } else {
        /* Horizontal scrollbar */
        track_len = c->w - 4;

        if (c->data.scrollbar.page_size > 0) {
            thumb_len = (track_len * c->data.scrollbar.page_size) /
                        (c->data.scrollbar.max_value + c->data.scrollbar.page_size);
        } else {
            thumb_len = track_len / 4;
        }
        if (thumb_len < 16)
            thumb_len = 16;
        if (thumb_len > track_len)
            thumb_len = track_len;

        if (c->data.scrollbar.max_value > 0) {
            thumb_pos = (c->data.scrollbar.position *
                         (track_len - thumb_len)) /
                        c->data.scrollbar.max_value;
        } else {
            thumb_pos = 0;
        }

        wm_fill_rect(handle,
                     c->x + 2 + thumb_pos, c->y + 2,
                     thumb_len, c->h - 4,
                     thumb_color);
    }
}

/* ---- Public draw functions ---- */

void ctrl_draw(int window_handle, int ctrl_id)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return;

    c = find_control(cw, ctrl_id);
    if (!c || c->type == CTRL_NONE)
        return;
    if (!(c->state & CTRL_STATE_VISIBLE))
        return;

    switch (c->type) {
    case CTRL_BUTTON:    draw_button(c);    break;
    case CTRL_LABEL:     draw_label(c);     break;
    case CTRL_TEXTBOX:   draw_textbox(c);   break;
    case CTRL_SCROLLBAR: draw_scrollbar(c); break;
    default: break;
    }
}

void ctrl_draw_all(int window_handle)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    int i;

    if (!cw)
        return;

    for (i = 0; i < cw->count; i++) {
        if (cw->controls[i].type != CTRL_NONE &&
            (cw->controls[i].state & CTRL_STATE_VISIBLE)) {
            switch (cw->controls[i].type) {
            case CTRL_BUTTON:    draw_button(&cw->controls[i]);    break;
            case CTRL_LABEL:     draw_label(&cw->controls[i]);     break;
            case CTRL_TEXTBOX:   draw_textbox(&cw->controls[i]);   break;
            case CTRL_SCROLLBAR: draw_scrollbar(&cw->controls[i]); break;
            default: break;
            }
        }
    }
}

/* ============================================================================
 * Event routing
 * ============================================================================ */

/* Previous button state for edge detection */
static uint8_t ctrl_prev_buttons = 0;

int ctrl_handle_mouse(int window_handle, int32_t cx, int32_t cy,
                      uint8_t buttons)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    uint8_t left_now = buttons & 0x01;
    uint8_t left_pressed = left_now && !(ctrl_prev_buttons & 0x01);
    uint8_t left_released = !left_now && (ctrl_prev_buttons & 0x01);
    int i;
    int consumed = 0;

    ctrl_prev_buttons = buttons;

    if (!cw)
        return 0;

    /* Handle scrollbar dragging first */
    for (i = 0; i < cw->count; i++) {
        struct control *c = &cw->controls[i];
        if (c->type != CTRL_SCROLLBAR || !c->data.scrollbar.dragging)
            continue;

        if (left_now) {
            /* Update position based on mouse */
            uint32_t track_len, thumb_len;
            int32_t pos_px;

            if (c->data.scrollbar.orientation == CTRL_SCROLLBAR_VERT) {
                track_len = c->h - 4;
                if (c->data.scrollbar.page_size > 0) {
                    thumb_len = (track_len * c->data.scrollbar.page_size) /
                                (c->data.scrollbar.max_value +
                                 c->data.scrollbar.page_size);
                } else {
                    thumb_len = track_len / 4;
                }
                if (thumb_len < 16) thumb_len = 16;
                if (thumb_len > track_len) thumb_len = track_len;

                pos_px = cy - (int32_t)(c->y + 2) - c->data.scrollbar.drag_offset;
            } else {
                track_len = c->w - 4;
                if (c->data.scrollbar.page_size > 0) {
                    thumb_len = (track_len * c->data.scrollbar.page_size) /
                                (c->data.scrollbar.max_value +
                                 c->data.scrollbar.page_size);
                } else {
                    thumb_len = track_len / 4;
                }
                if (thumb_len < 16) thumb_len = 16;
                if (thumb_len > track_len) thumb_len = track_len;

                pos_px = cx - (int32_t)(c->x + 2) - c->data.scrollbar.drag_offset;
            }

            /* Convert pixel position to scroll value */
            if (pos_px < 0) pos_px = 0;
            if ((uint32_t)pos_px > track_len - thumb_len)
                pos_px = (int32_t)(track_len - thumb_len);

            if (track_len > thumb_len) {
                c->data.scrollbar.position =
                    ((uint32_t)pos_px * c->data.scrollbar.max_value) /
                    (track_len - thumb_len);
            }

            wm_mark_dirty();
            return 1;
        }

        if (left_released) {
            c->data.scrollbar.dragging = 0;
            return 1;
        }
    }

    /* Update hover state and handle clicks */
    for (i = 0; i < cw->count; i++) {
        struct control *c = &cw->controls[i];
        if (c->type == CTRL_NONE || !(c->state & CTRL_STATE_VISIBLE))
            continue;

        if (ctrl_hit_test(c, cx, cy)) {
            /* Hover */
            if (!(c->state & CTRL_STATE_HOVERED)) {
                c->state |= CTRL_STATE_HOVERED;
                wm_mark_dirty();
            }

            if (left_pressed) {
                consumed = 1;

                /* Set focus */
                cw->focused_id = c->id;

                /* Update focus state for all controls */
                {
                    int j;
                    for (j = 0; j < cw->count; j++) {
                        if (cw->controls[j].id == c->id)
                            cw->controls[j].state |= CTRL_STATE_FOCUSED;
                        else
                            cw->controls[j].state &= ~CTRL_STATE_FOCUSED;
                    }
                }

                switch (c->type) {
                case CTRL_BUTTON:
                    c->state |= CTRL_STATE_PRESSED;
                    wm_mark_dirty();

                    /* Invoke callback */
                    if (c->data.button.on_click)
                        c->data.button.on_click(c->id, window_handle);

                    c->state &= ~CTRL_STATE_PRESSED;
                    wm_mark_dirty();
                    break;

                case CTRL_TEXTBOX:
                {
                    /* Click to position cursor */
                    int32_t rel_x = cx - (int32_t)(c->x + 4);
                    uint32_t char_pos;

                    if (rel_x < 0) rel_x = 0;
                    char_pos = c->data.textbox.scroll_offset +
                               (uint32_t)rel_x / FONT_WIDTH;
                    if (char_pos > c->data.textbox.text_len)
                        char_pos = c->data.textbox.text_len;
                    c->data.textbox.cursor_pos = char_pos;
                    wm_mark_dirty();
                    break;
                }

                case CTRL_SCROLLBAR:
                {
                    /* Start thumb drag or jump to position */
                    uint32_t track_len, thumb_len, thumb_pos;
                    int on_thumb = 0;

                    if (c->data.scrollbar.orientation == CTRL_SCROLLBAR_VERT) {
                        track_len = c->h - 4;
                        if (c->data.scrollbar.page_size > 0) {
                            thumb_len = (track_len *
                                         c->data.scrollbar.page_size) /
                                        (c->data.scrollbar.max_value +
                                         c->data.scrollbar.page_size);
                        } else {
                            thumb_len = track_len / 4;
                        }
                        if (thumb_len < 16) thumb_len = 16;
                        if (thumb_len > track_len) thumb_len = track_len;

                        if (c->data.scrollbar.max_value > 0) {
                            thumb_pos = (c->data.scrollbar.position *
                                         (track_len - thumb_len)) /
                                        c->data.scrollbar.max_value;
                        } else {
                            thumb_pos = 0;
                        }

                        /* Check if click is on thumb */
                        {
                            int32_t thumb_top = (int32_t)(c->y + 2 + thumb_pos);
                            if (cy >= thumb_top &&
                                cy < thumb_top + (int32_t)thumb_len) {
                                on_thumb = 1;
                                c->data.scrollbar.drag_offset =
                                    cy - thumb_top;
                            }
                        }
                    } else {
                        track_len = c->w - 4;
                        if (c->data.scrollbar.page_size > 0) {
                            thumb_len = (track_len *
                                         c->data.scrollbar.page_size) /
                                        (c->data.scrollbar.max_value +
                                         c->data.scrollbar.page_size);
                        } else {
                            thumb_len = track_len / 4;
                        }
                        if (thumb_len < 16) thumb_len = 16;
                        if (thumb_len > track_len) thumb_len = track_len;

                        if (c->data.scrollbar.max_value > 0) {
                            thumb_pos = (c->data.scrollbar.position *
                                         (track_len - thumb_len)) /
                                        c->data.scrollbar.max_value;
                        } else {
                            thumb_pos = 0;
                        }

                        {
                            int32_t thumb_left = (int32_t)(c->x + 2 + thumb_pos);
                            if (cx >= thumb_left &&
                                cx < thumb_left + (int32_t)thumb_len) {
                                on_thumb = 1;
                                c->data.scrollbar.drag_offset =
                                    cx - thumb_left;
                            }
                        }
                    }

                    if (on_thumb) {
                        c->data.scrollbar.dragging = 1;
                    } else {
                        /* Jump to click position */
                        uint32_t tl, thl;
                        int32_t p;

                        if (c->data.scrollbar.orientation ==
                            CTRL_SCROLLBAR_VERT) {
                            tl = c->h - 4;
                            thl = thumb_len;
                            p = cy - (int32_t)(c->y + 2) -
                                (int32_t)(thl / 2);
                        } else {
                            tl = c->w - 4;
                            thl = thumb_len;
                            p = cx - (int32_t)(c->x + 2) -
                                (int32_t)(thl / 2);
                        }

                        if (p < 0) p = 0;
                        if ((uint32_t)p > tl - thl)
                            p = (int32_t)(tl - thl);

                        if (tl > thl) {
                            c->data.scrollbar.position =
                                ((uint32_t)p * c->data.scrollbar.max_value) /
                                (tl - thl);
                        }
                    }

                    wm_mark_dirty();
                    break;
                }

                default:
                    break;
                }
            }
        } else {
            /* Mouse not over this control — clear hover */
            if (c->state & CTRL_STATE_HOVERED) {
                c->state &= ~CTRL_STATE_HOVERED;
                c->state &= ~CTRL_STATE_PRESSED;
                wm_mark_dirty();
            }
        }
    }

    return consumed;
}

int ctrl_handle_key(int window_handle, char key)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    struct control *c;
    uint32_t i;

    if (!cw || cw->focused_id < 0)
        return 0;

    c = find_control(cw, cw->focused_id);
    if (!c)
        return 0;

    /* Only textboxes consume keyboard input */
    if (c->type != CTRL_TEXTBOX)
        return 0;

    switch (key) {
    case '\b':  /* Backspace */
        if (c->data.textbox.cursor_pos > 0) {
            /* Shift characters left */
            for (i = c->data.textbox.cursor_pos; i <= c->data.textbox.text_len; i++) {
                c->data.textbox.text[i - 1] = c->data.textbox.text[i];
            }
            c->data.textbox.cursor_pos--;
            c->data.textbox.text_len--;
            wm_mark_dirty();
        }
        return 1;

    case 0x7F:  /* Delete */
        if (c->data.textbox.cursor_pos < c->data.textbox.text_len) {
            for (i = c->data.textbox.cursor_pos; i < c->data.textbox.text_len; i++) {
                c->data.textbox.text[i] = c->data.textbox.text[i + 1];
            }
            c->data.textbox.text_len--;
            wm_mark_dirty();
        }
        return 1;

    default:
        /* Insert printable characters */
        if (key >= 32 && key <= 126) {
            if (c->data.textbox.text_len < CTRL_TEXT_MAX - 1) {
                /* Shift characters right to make room */
                for (i = c->data.textbox.text_len + 1;
                     i > c->data.textbox.cursor_pos; i--) {
                    c->data.textbox.text[i] = c->data.textbox.text[i - 1];
                }
                c->data.textbox.text[c->data.textbox.cursor_pos] = key;
                c->data.textbox.cursor_pos++;
                c->data.textbox.text_len++;
                c->data.textbox.text[c->data.textbox.text_len] = '\0';
                wm_mark_dirty();
            }
            return 1;
        }
        break;
    }

    return 0;
}

/* ============================================================================
 * Accessors
 * ============================================================================ */

void ctrl_set_text(int window_handle, int ctrl_id, const char *text)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return;

    c = find_control(cw, ctrl_id);
    if (!c)
        return;

    switch (c->type) {
    case CTRL_BUTTON:
        ctrl_strcpy(c->data.button.text, text, CTRL_TEXT_MAX);
        break;
    case CTRL_LABEL:
        ctrl_strcpy(c->data.label.text, text, CTRL_TEXT_MAX);
        break;
    case CTRL_TEXTBOX:
        ctrl_strcpy(c->data.textbox.text, text, CTRL_TEXT_MAX);
        c->data.textbox.text_len = ctrl_strlen(c->data.textbox.text);
        c->data.textbox.cursor_pos = c->data.textbox.text_len;
        break;
    default:
        break;
    }

    wm_mark_dirty();
}

const char *ctrl_get_text(int window_handle, int ctrl_id)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return "";

    c = find_control(cw, ctrl_id);
    if (!c)
        return "";

    switch (c->type) {
    case CTRL_BUTTON:  return c->data.button.text;
    case CTRL_LABEL:   return c->data.label.text;
    case CTRL_TEXTBOX: return c->data.textbox.text;
    default:           return "";
    }
}

void ctrl_set_scroll_pos(int window_handle, int ctrl_id, uint32_t pos)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return;

    c = find_control(cw, ctrl_id);
    if (!c || c->type != CTRL_SCROLLBAR)
        return;

    if (pos > c->data.scrollbar.max_value)
        pos = c->data.scrollbar.max_value;
    c->data.scrollbar.position = pos;
    wm_mark_dirty();
}

uint32_t ctrl_get_scroll_pos(int window_handle, int ctrl_id)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    struct control *c;

    if (!cw)
        return 0;

    c = find_control(cw, ctrl_id);
    if (!c || c->type != CTRL_SCROLLBAR)
        return 0;

    return c->data.scrollbar.position;
}

void ctrl_set_focus(int window_handle, int ctrl_id)
{
    struct ctrl_window *cw = get_ctrl_window(window_handle);
    int i;

    if (!cw)
        return;

    cw->focused_id = ctrl_id;

    for (i = 0; i < cw->count; i++) {
        if (cw->controls[i].id == ctrl_id)
            cw->controls[i].state |= CTRL_STATE_FOCUSED;
        else
            cw->controls[i].state &= ~CTRL_STATE_FOCUSED;
    }

    wm_mark_dirty();
}
