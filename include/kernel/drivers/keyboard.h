/* ============================================================================
 * keyboard.h -- PS/2 Keyboard driver
 *
 * Handles IRQ 1, translates scan codes to ASCII (US QWERTY layout),
 * supports Shift, Caps Lock, Ctrl, Alt modifiers.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Special key codes (non-ASCII, returned by keyboard_getchar) */
#define KEY_NONE       0
#define KEY_ESCAPE     27
#define KEY_BACKSPACE  '\b'
#define KEY_TAB        '\t'
#define KEY_ENTER      '\n'

/* Arrow keys (non-ASCII, use high values) */
#define KEY_UP         0x80
#define KEY_DOWN       0x81
#define KEY_LEFT       0x82
#define KEY_RIGHT      0x83

/* Initialize the PS/2 keyboard driver (registers IRQ 1) */
void keyboard_init(void);

/* Blocking read: waits until a character is available, returns ASCII */
char keyboard_getchar(void);

/* Non-blocking read: returns character or 0 if buffer empty */
char keyboard_trygetchar(void);

/* Inject a PS/2-style scan code (used by Hyper-V synthetic keyboard).
 * Processes modifiers, lookup tables, and pushes into the ring buffer
 * exactly as if the scan code came from the PS/2 IRQ handler. */
void keyboard_inject_scancode(uint8_t scancode);

/* Reset all latched keyboard state: ring buffer plus every modifier
 * (shift / ctrl / alt / capslock) and any pending 0xE0 prefix. Used by
 * test infrastructure between injection sequences so a stuck modifier
 * or orphan prefix from a prior test cannot contaminate the next one.
 * Safe to call at any time; does not touch hardware registers. */
void keyboard_reset_state(void);
