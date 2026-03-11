/* ============================================================================
 * desktop.h — Desktop shell (wallpaper, taskbar, start menu)
 *
 * Manages the desktop background wallpaper, a taskbar at the bottom of the
 * screen with a start button, window list, and uptime clock, plus a simple
 * start menu for launching applications.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "gfx.h"

/* ---- Constants ---- */

#define TASKBAR_HEIGHT     48
#define TASKBAR_COLOR      0xFF202020   /* taskbar background         */
#define TASKBAR_BORDER     0xFF383838   /* taskbar top border          */

#define START_ICON_SIZE    32
#define START_BTN_WIDTH    48
#define START_BTN_COLOR    0xFF2D2D2D   /* start button background     */
#define START_BTN_HOVER    0xFF3D3D3D   /* start button hover          */
#define START_BTN_TEXT     0xFF60CDFF   /* accent blue label           */

#define CLOCK_COLOR        0xFFB0B0B0   /* clock text (secondary)      */
#define WINLIST_COLOR      0xFFD0D0D0   /* window list text            */
#define WINLIST_ACTIVE     0xFF60CDFF   /* active window accent       */

#define MENU_WIDTH         200
#define MENU_ITEM_HEIGHT   28
#define MENU_BG            0xFF2D2D2D   /* menu background            */
#define MENU_HOVER         0xFF3D3D3D   /* menu hover highlight       */
#define MENU_TEXT           0xFFE0E0E0   /* menu text                  */
#define MENU_BORDER        0xFF454545   /* menu border                */

/* Wallpaper dimensions (must match framebuffer) */
#define WALLPAPER_WIDTH    1280
#define WALLPAPER_HEIGHT   720

/* ---- API ---- */

/* Initialize the desktop: load wallpaper from C:\, copy backgrounds
 * to IXFS Documents\backgrounds\ folder. Call after wm_init(). */
void desktop_init(void);

/* Draw the wallpaper to the back buffer (replaces fb_fill_rect background) */
void desktop_draw_wallpaper(void);
void desktop_draw_wallpaper_rect(int32_t rx, int32_t ry, uint32_t rw, uint32_t rh);

/* Copy wallpaper pixels into a destination buffer (for acrylic cache).
 * dst must be at least rw*rh uint32_t pixels. */
void desktop_copy_wallpaper_rect(uint32_t *dst, int32_t rx, int32_t ry,
                                  uint32_t rw, uint32_t rh);

/* Get a gfx_surface_t wrapping the wallpaper image (for gfx_mica).
 * Returns 0 on success, -1 if no wallpaper is loaded. */
int desktop_get_wallpaper_surface(gfx_surface_t *out);

/* Draw desktop icons (Computer, Recycle Bin, Control Deck) on the wallpaper */
void desktop_draw_icons(void);

/* Draw the taskbar at the bottom of the screen.
 * Call AFTER wm_composite() so it overlays windows. */
void desktop_draw_taskbar(void);

/* Check if the start menu is open and draw it if so */
void desktop_draw_start_menu(void);

/* Handle a mouse click at (x, y). Returns 1 if the desktop consumed
 * the click (taskbar/start menu), 0 if it should be passed to WM. */
int desktop_handle_click(int32_t mx, int32_t my, uint8_t buttons);

/* Check if a point is in the taskbar area */
int desktop_in_taskbar(int32_t my);

/* Get the usable desktop height (screen height minus taskbar) */
uint32_t desktop_get_usable_height(void);

/* Determine cursor shape for desktop areas (taskbar, start menu).
 * Returns CURSOR_ARROW if not in a clickable desktop element. */
#include "cursor.h"
cursor_shape_t desktop_get_cursor_context(int32_t mx, int32_t my);
