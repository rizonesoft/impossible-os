/* ============================================================================
 * printk.h — Kernel printf
 * ============================================================================ */

#pragma once

/* Kernel printf — outputs to both framebuffer and serial */
void printk(const char *fmt, ...);
