# 98 — Telemetry & Diagnostics

## Overview
Collect anonymous usage stats and crash reports to improve the OS. Opt-in only, transparent data collection.

## Data collected (if opted in)
| Data | Purpose |
|------|---------|
| OS version, architecture | Know what's deployed |
| Crash dumps | Fix bugs |
| Boot time | Performance tracking |
| Hardware info | Driver priorities |
| Feature usage counts | What to improve |

## Privacy
- **OFF by default** — user must opt in during setup or in Settings
- No personal data (files, passwords, browsing)
- Data stored locally in `C:\Impossible\System\Diagnostics\`
- Upload only on user action: "Send diagnostic report"

## Codex: `System\Privacy\Telemetry = 0` (0=off, 1=basic, 2=full)
## Files: `src/kernel/telemetry.c` (~150 lines) | 2-3 days
## Priority: 🔴 Long-term
