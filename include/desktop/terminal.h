/* ============================================================================
 * terminal.h — Graphical terminal emulator window
 *
 * Provides a text-mode terminal inside a WM window.  Shell I/O is routed
 * through this module: sys_write calls terminal_puts() to render text,
 * sys_read calls terminal_trygetchar() to consume keyboard input.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Terminal dimensions (in characters) ---- */
#define TERM_COLS    80
#define TERM_ROWS    25
#define TERM_INPUT_BUF 256

/* ---- Lifecycle ---- */

/* Open a new terminal window on the desktop.
 * Returns 0 on success, -1 on failure. */
int terminal_open(void);

/* Close the terminal window and free resources. */
void terminal_close(void);

/* Is a terminal window currently open? */
int terminal_is_open(void);

/* ---- Output (called from sys_write) ---- */

/* Write a single character to the terminal (handles \n, \b, \r, scroll). */
void terminal_putchar(char c);

/* Write a string of given length to the terminal. */
void terminal_puts(const char *s, int len);

/* ---- Input (keyboard → terminal → sys_read) ---- */

/* Feed a keyboard character into the terminal's input ring buffer. */
void terminal_key_input(char c);

/* Try to consume one character from the input ring buffer.
 * Returns the character, or 0 if the buffer is empty. */
char terminal_trygetchar(void);

/* Get the WM window handle for focus checking. Returns -1 if not open. */
int terminal_get_handle(void);

/* Render the terminal contents to its WM window. Call once per frame. */
void terminal_render(void);
