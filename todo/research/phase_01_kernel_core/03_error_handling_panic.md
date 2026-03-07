# 39 — Error Handling & Kernel Panic Screen

## Overview

Graceful crash handling: catch kernel faults, display a diagnostic screen (like Windows BSOD), and optionally restart.

---

## Panic screen (styled for Impossible OS)

```
┌──────────────────────────────────────────────────┐
│                                                  │
│       :(                                         │
│                                                  │
│       Impossible OS ran into a problem           │
│       and needs to restart.                      │
│                                                  │
│       Error: PAGE_FAULT_IN_KERNEL_SPACE          │
│       Address: 0xFFFF800000123456                │
│       RIP: 0xFFFF800000100A42                    │
│       Code: kernel.c:248 in process_event()      │
│                                                  │
│       Collecting error info... 35%               │
│       The system will restart in 10 seconds.     │
│                                                  │
│       Stop code: 0x0000000E                      │
└──────────────────────────────────────────────────┘
```

## Exception handlers
```c
/* panic.h */
void kernel_panic(const char *msg, const char *file, int line);
void panic_screen(uint64_t error_code, uint64_t rip, uint64_t cr2, 
                  const char *description);

#define KPANIC(msg) kernel_panic(msg, __FILE__, __LINE__)
```

## Caught exceptions
| Vector | Exception | Description |
|--------|-----------|-------------|
| 0 | #DE | Division by zero |
| 6 | #UD | Invalid opcode |
| 8 | #DF | Double fault |
| 13 | #GP | General protection fault |
| 14 | #PF | Page fault |

## Features
- Register dump (RAX-R15, RIP, RSP, CR2, CR3, RFLAGS)
- Stack trace (walk RBP chain)
- Auto-restart after countdown (Codex: `System\Recovery\AutoRestart`)
- Optional: dump to `C:\Impossible\System\crashdump.log`

## Files: `src/kernel/panic.c` (~200 lines) — mostly already exists, needs UI upgrade
## Implementation: 2-3 days
