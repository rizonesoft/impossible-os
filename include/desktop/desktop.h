/* ============================================================================
 * desktop.h — Desktop shell (wallpaper, taskbar, start menu)
 *
 * Manages the desktop background wallpaper, a taskbar at the bottom of the
 * screen with a start button, window list, and uptime clock, plus a simple
 * start menu for launching applications.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Constants ---- */

#define TASKBAR_HEIGHT     48
#define TASKBAR_COLOR      0x00202040
#define TASKBAR_BORDER     0x003A3A5C

#define START_ICON_SIZE    32
#define START_BTN_WIDTH    48
#define START_BTN_COLOR    0x003A3A5C
#define START_BTN_HOVER    0x004A4A6C
#define START_BTN_TEXT     0x0088DDFF

#define CLOCK_COLOR        0x00B0B0CC
#define WINLIST_COLOR      0x00D0D0E0
#define WINLIST_ACTIVE     0x0088DDFF

#define MENU_WIDTH         200
#define MENU_ITEM_HEIGHT   28
#define MENU_BG            0x00252545
#define MENU_HOVER         0x003A3A6C
#define MENU_TEXT           0x00E0E0F0
#define MENU_BORDER        0x004A4A7C

/* Wallpaper dimensions (must match framebuffer) */
#define WALLPAPER_WIDTH    1280
#define WALLPAPER_HEIGHT   720

/* ---- API ---- */

/* Initialize the desktop: load wallpaper from initrd, copy backgrounds
 * to IXFS Documents\backgrounds\ folder. Call after wm_init(). */
void desktop_init(void);

/* Draw the wallpaper to the back buffer (replaces fb_fill_rect background) */
void desktop_draw_wallpaper(void);

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
