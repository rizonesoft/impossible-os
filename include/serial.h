/* ============================================================================
 * serial.h — Serial port (COM1) driver
 * ============================================================================ */

#pragma once

#include "types.h"

/* Initialize COM1 serial port */
void serial_init(void);

/* Write a single character to serial */
void serial_putchar(char c);

/* Write a null-terminated string to serial */
void serial_write(const char *str);
