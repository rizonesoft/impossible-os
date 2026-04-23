/* ============================================================================
 * terminal.h -- Graphical terminal emulator window
 *
 * Provides a text-mode terminal inside a WM window.  Shell I/O is routed
 * through this module: sys_write calls terminal_puts() to render text,
 * sys_read calls terminal_trygetchar() to consume keyboard input.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Terminal dimensions (in characters) ---- */
#define TERM_COLS    80
#define TERM_ROWS    20
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

/* ---- Introspection (desktop UI test framework hook) ---- */

/* Copy the terminal's text grid into dest as a flat TERM_ROWS*TERM_COLS
 * byte array (row-major, row N starts at dest[N*TERM_COLS]). No null
 * terminator is written; use explicit lengths. dest_capacity must be at
 * least TERM_ROWS*TERM_COLS. Returns the number of bytes written on
 * success, 0 when the terminal is not open, -1 when dest is NULL or
 * dest_capacity is too small.
 *
 * Concurrency: reads term_cells locklessly, so a concurrent
 * terminal_putchar on another thread can produce a torn snapshot. Safe
 * under either (a) single-threaded callers, as in the current kernel
 * test phase which runs before the desktop compositor starts, or (b) a
 * quiesced state where terminal writers have been paused. Richer locking
 * is scheduled in the desktop test-isolation layer; see the desktop UI
 * test framework TODO's isolation/artifact section for the concrete
 * hardening plan. */
int terminal_get_buffer(char *dest, int dest_capacity);

/* Return 1 if the terminal's text grid currently contains the NUL-
 * terminated substring `needle`, 0 otherwise. The search ignores row
 * boundaries: a needle that spans the end of one row and the start of
 * the next is found. Returns 0 if needle is NULL / empty / longer than
 * TERM_ROWS*TERM_COLS or when the terminal is not open. Same concurrency
 * caveat as terminal_get_buffer. */
int terminal_buffer_contains(const char *needle);

#ifdef KERNEL_TESTS
/* Test-only harness: initialize the terminal's internal state without
 * creating a WM window. Lets the kernel test phase (which runs before
 * the compositor starts and before wm_create_window works) exercise the
 * terminal_get_buffer / terminal_buffer_contains success paths against a
 * populated term_cells grid. Sets term_handle to a sentinel so
 * terminal_is_open() returns true; terminal_render must NOT be called
 * in this mode because there is no real window to render into.
 * Paired with terminal_test_force_close() which resets term_handle to
 * -1 without a WM call. */
void terminal_test_force_open(void);
void terminal_test_force_close(void);
#endif
