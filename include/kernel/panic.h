/* ============================================================================
 * panic.h — Kernel panic screen
 *
 * Provides a styled graphical panic screen (blue screen of death) with:
 *   - Exception name and stop code
 *   - Faulting address and RIP
 *   - Source file + line (via __FILE__, __LINE__)
 *   - Full register dump (RAX–R15, RSP, RFLAGS, CR2, CR3)
 *   - Stack trace (RBP chain walk)
 *   - Auto-restart countdown (configurable via Registry)
 *   - Crash dump to C:\Impossible\System\crashdump.log
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward declaration */
struct interrupt_frame;

/* Display the styled panic screen and halt.
 * frame: interrupt frame snapshot (NULL if not from an exception)
 * error_code: exception error code or stop code
 * description: human-readable error description
 * file: source file (__FILE__)
 * line: source line (__LINE__) */
void panic_screen(struct interrupt_frame *frame, uint64_t error_code,
                  const char *description, const char *file, uint32_t line);

/* Convenience macro that captures file/line automatically.
 * Usage: KPANIC("something terrible happened"); */
#define KPANIC(msg) \
    panic_screen((void *)0, 0, (msg), __FILE__, __LINE__)

/* Panic with an interrupt frame (called from exception handlers).
 * Usage: KPANIC_FRAME(frame, "page fault in kernel"); */
#define KPANIC_FRAME(frame, msg) \
    panic_screen((frame), (frame)->err_code, (msg), __FILE__, __LINE__)
