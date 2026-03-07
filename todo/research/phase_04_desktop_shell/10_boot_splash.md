# 51 — Boot Splash & Boot Manager

## Overview

A graphical boot splash screen (logo + progress bar) shown during kernel init, plus boot options (safe mode, recovery).

---

## Boot splash

```
┌──────────────────────────────────────┐
│                                      │
│                                      │
│          ╔══════════════╗            │
│          ║ IMPOSSIBLE   ║            │
│          ║     OS       ║            │
│          ╚══════════════╝            │
│                                      │
│          ━━━━━━━━━━░░░░░             │
│             Loading...               │
│                                      │
└──────────────────────────────────────┘
```

Drawn directly to framebuffer during `kernel_main()` before desktop starts.

## Boot menu (hold F8 during boot)
| Option | Action |
|--------|--------|
| Normal boot | Default |
| Safe mode | Minimal drivers, no network |
| Recovery console | Text-mode shell only |
| Last known good | Restore previous Codex settings |

## Implementation
```c
void boot_splash_init(void);              /* show logo + progress bar */
void boot_splash_progress(uint8_t pct);   /* update bar: 0-100 */
void boot_splash_status(const char *msg); /* "Loading drivers..." */
void boot_splash_finish(void);            /* fade out, start desktop */
```

## Files: `src/kernel/boot_splash.c` (~150 lines)
## Implementation: 2-3 days
