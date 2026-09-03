/* ============================================================================
 * controls.h -- Common Controls library
 *
 * Reusable GUI controls (Button, Label, TextBox, ScrollBar) that draw into
 * per-window framebuffers via the window manager.  Modelled after Windows
 * Common Controls -- each control has a type, position, size, and state.
 *
 * Usage:
 *   1. Create a window via wm_create_window()
 *   2. Create controls with ctrl_create_button() etc., passing the WM handle
 *   3. Call ctrl_draw_all(handle) each frame to render controls
 *   4. Call ctrl_handle_mouse() / ctrl_handle_key() from your input handler
 *   5. Call ctrl_destroy_all(handle) when the window is destroyed
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Limits ---- */

#define CTRL_MAX_PER_WINDOW  32
#define CTRL_TEXT_MAX        128
#define CTRL_MAX_WINDOWS     32  /* matches WM_MAX_WINDOWS */

/* ---- Control types ---- */

enum ctrl_type {
    CTRL_NONE = 0,
    CTRL_BUTTON,
    CTRL_LABEL,
    CTRL_TEXTBOX,
    CTRL_SCROLLBAR
};

/* ---- Control state flags ---- */

#define CTRL_STATE_VISIBLE   0x01
#define CTRL_STATE_ENABLED   0x02
#define CTRL_STATE_FOCUSED   0x04
#define CTRL_STATE_HOVERED   0x08
#define CTRL_STATE_PRESSED   0x10

/* ---- Scrollbar orientation ---- */

#define CTRL_SCROLLBAR_VERT  0
#define CTRL_SCROLLBAR_HORIZ 1

/* ---- Callback type ---- */

typedef void (*ctrl_click_fn)(int ctrl_id, int window_handle);

/* ---- Control color scheme ---- */

/* Windows 11 Dark Theme -- neutral grays, accent blue */
#define CTRL_COLOR_BG          0xFF2D2D2D   /* button/control surface    */
#define CTRL_COLOR_BG_HOVER    0xFF3D3D3D   /* hover lift                */
#define CTRL_COLOR_BG_PRESSED  0xFF1A1A1A   /* pressed dim               */
#define CTRL_COLOR_BG_DISABLED 0xFF252525   /* disabled muted            */
#define CTRL_COLOR_BORDER      0xFF454545   /* control border (subtle)   */
#define CTRL_COLOR_TEXT        0xFFFFFFFF   /* primary text              */
#define CTRL_COLOR_TEXT_DIM    0xFF9B9B9B   /* secondary/disabled text   */
#define CTRL_COLOR_CURSOR      0xFF60CDFF   /* accent blue cursor        */
#define CTRL_COLOR_TEXTBOX_BG  0xFF1A1A1A   /* input field background    */
#define CTRL_COLOR_SCROLL_BG   0xFF252525   /* scrollbar track           */
#define CTRL_COLOR_SCROLL_THUMB 0xFF4A4A4A  /* scrollbar thumb           */
#define CTRL_COLOR_SCROLL_HOVER 0xFF5A5A5A  /* scrollbar thumb hover     */

/* ---- Control struct ---- */

struct control {
    enum ctrl_type type;
    int            id;              /* unique ID within the window */
    int            window_handle;   /* owning WM window handle */
    uint32_t       x, y, w, h;     /* position/size in client area */
    uint32_t       state;           /* CTRL_STATE_* flags */

    /* Type-specific data */
    union {
        /* Button */
        struct {
            char         text[CTRL_TEXT_MAX];
            ctrl_click_fn on_click;
        } button;

        /* Label */
        struct {
            char     text[CTRL_TEXT_MAX];
            uint32_t fg_color;
        } label;

        /* TextBox */
        struct {
            char     text[CTRL_TEXT_MAX];
            uint32_t cursor_pos;    /* cursor index in text[] */
            uint32_t text_len;      /* current string length */
            uint32_t scroll_offset; /* first visible char index */
        } textbox;

        /* ScrollBar */
        struct {
            uint8_t  orientation;   /* CTRL_SCROLLBAR_VERT / HORIZ */
            uint32_t position;      /* current scroll position */
            uint32_t max_value;     /* maximum scroll value */
            uint32_t page_size;     /* visible page size (for thumb sizing) */
            uint8_t  dragging;      /* 1 if thumb is being dragged */
            int32_t  drag_offset;   /* mouse offset from thumb top during drag */
        } scrollbar;
    } data;
};

/* ---- Per-window control storage ---- */

struct ctrl_window {
    struct control controls[CTRL_MAX_PER_WINDOW];
    int            count;           /* number of active controls */
    int            focused_id;      /* ID of focused control (-1 = none) */
    uint8_t        active;          /* 1 if this slot is in use */
    int            window_handle;   /* matching WM window handle */
};

/* ---- Lifecycle ---- */

/* Initialize the controls subsystem (call once at boot, after pmm_init).
 * The window pool is frame-backed (see TODO-33 s13); allocation failure
 * degrades rather than halts -- ctrl_ready() reports the outcome. */
void ctrl_init(void);

/* Returns 1 if the window pool allocated successfully and controls can be
 * created; 0 if ctrl_init() degraded (OOM) or has not run yet. Callers
 * that create UI at boot (e.g. gallery_open()) must check this before
 * treating control creation as authoritative -- ctrl_create_*() already
 * returns -1 per-call when degraded, but a caller that ignores every
 * individual return value needs one place to check readiness up front. */
int ctrl_ready(void);

#ifdef KERNEL_TESTS
/* Test-only: free the window pool and reset to uninitialized so a test can
 * drive ctrl_init()'s OOM path via pmm_alloc_fail_next(). */
void ctrl_test_reset_for_fault_injection(void);
#endif

/* Create controls -- returns control ID (>= 0) or -1 on failure */
int ctrl_create_button(int window_handle, uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h, const char *text,
                       ctrl_click_fn on_click);

int ctrl_create_label(int window_handle, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, const char *text,
                      uint32_t fg_color);

int ctrl_create_textbox(int window_handle, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h);

int ctrl_create_scrollbar(int window_handle, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint8_t orientation,
                          uint32_t max_value, uint32_t page_size);

/* Destroy a single control */
void ctrl_destroy(int window_handle, int ctrl_id);

/* Destroy all controls for a window */
void ctrl_destroy_all(int window_handle);

/* ---- Drawing ---- */

/* Draw a single control into its window's framebuffer */
void ctrl_draw(int window_handle, int ctrl_id);

/* Draw all controls for a window */
void ctrl_draw_all(int window_handle);

/* ---- Event routing ---- */

/* Handle a mouse event in the window's client area.
 * (cx, cy) are in client-area coordinates.
 * Returns 1 if a control consumed the event, 0 otherwise. */
int ctrl_handle_mouse(int window_handle, int32_t cx, int32_t cy,
                      uint8_t buttons);

/* Handle a keyboard event for the focused control in this window.
 * Returns 1 if a control consumed the key, 0 otherwise. */
int ctrl_handle_key(int window_handle, char key);

/* ---- Accessors ---- */

/* Get/set label text */
void ctrl_set_text(int window_handle, int ctrl_id, const char *text);
const char *ctrl_get_text(int window_handle, int ctrl_id);

/* Get/set scrollbar position */
void ctrl_set_scroll_pos(int window_handle, int ctrl_id, uint32_t pos);
uint32_t ctrl_get_scroll_pos(int window_handle, int ctrl_id);

/* Set focus to a specific control */
void ctrl_set_focus(int window_handle, int ctrl_id);

/* Enable or disable a control (0 = disabled, 1 = enabled) */
void ctrl_set_enabled(int window_handle, int ctrl_id, int enabled);
